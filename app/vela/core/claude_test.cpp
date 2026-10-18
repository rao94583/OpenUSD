// The Claude panel's core: the CLI adapter (lines recorded from Claude Code
// 2.1.296), approvals, the child process and the conversation history.
#include "claude_cli.h"
#include "claude_history.h"
#include "claude_process.h"
#include "test_support.h"

#include "pxr/base/js/json.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

using namespace vela;
using namespace vela::claude;

namespace {

JsObject Json(const std::string &text) { return JsParseString(text).GetJsObject(); }

void TestVersions() {
    Version v;
    CHECK(ParseVersion("2.1.296 (Claude Code)\n", &v));
    CHECK(v.major == 2 && v.minor == 1 && v.patch == 296);
    CHECK(!(v < MinimumVersion));
    CHECK(ParseVersion("claude 2.0.5", &v) && v < MinimumVersion);
    CHECK((Version{2, 10, 0} < Version{3, 0, 0}) && !(Version{2, 2, 0} < Version{2, 1, 999}));
    CHECK(!ParseVersion("no version", &v));
    CHECK(MinimumVersion.ToString() == "2.1.296");
    CHECK(CandidatePaths("/Users/x").front() == "/Users/x/.local/bin/claude");
}

void TestArguments() {
    LaunchOptions options;
    options.executable = "/bin/claude";
    options.sessionId = "abc";
    options.mcpHelper = "/App/vela-mcp";
    options.mcpSocket = "/tmp/s.sock";
    options.model = "opus";
    std::vector<std::string> args = Arguments(options);
    auto after = [&](const std::string &flag) {
        for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == flag) return args[i + 1];
        return std::string("<missing>");
    };
    CHECK(args[0] == "/bin/claude" && args[1] == "-p");
    CHECK(after("--input-format") == "stream-json" && after("--output-format") == "stream-json");
    CHECK(after("--permission-prompt-tool") == "stdio");
    CHECK(after("--session-id") == "abc" && after("--resume") == "<missing>");
    CHECK(after("--model") == "opus");
    CHECK(after("--tools") == "Read,Glob,Grep");
    const JsObject config = Json(after("--mcp-config"));
    const JsObject &server = config.at("mcpServers").GetJsObject().at("vela").GetJsObject();
    CHECK(server.at("command").GetString() == "/App/vela-mcp");
    CHECK(server.at("env").GetJsObject().at("VELA_MCP_SOCKET").GetString() == "/tmp/s.sock");
    options.resume = "old";
    options.model.clear();
    args = Arguments(options);
    CHECK(after("--resume") == "old" && after("--session-id") == "<missing>");
    CHECK(after("--model") == "<missing>");
}

void TestMessages() {
    JsObject init = Json(InitializeRequest("r1"));
    CHECK(init.at("type").GetString() == "control_request");
    CHECK(init.at("request").GetJsObject().at("subtype").GetString() == "initialize");
    CHECK(Json(InterruptRequest("r2")).at("request").GetJsObject().at("subtype").GetString() ==
          "interrupt");
    const std::string user = UserMessage("Hello \"there\"\nline", {"iVBOR"});
    CHECK(user.find('\n') == std::string::npos);
    const JsArray content = Json(user).at("message").GetJsObject().at("content").GetJsArray();
    CHECK(content.size() == 2);
    CHECK(content[0].GetJsObject().at("type").GetString() == "image");
    CHECK(content[1].GetJsObject().at("text").GetString() == "Hello \"there\"\nline");
    const JsObject allow = Json(AllowTool("q", Json(R"j({"code": "1"})j")))
        .at("response").GetJsObject();
    CHECK(allow.at("request_id").GetString() == "q");
    CHECK(allow.at("response").GetJsObject().at("behavior").GetString() == "allow");
    CHECK(allow.at("response").GetJsObject().at("updatedInput").GetJsObject().at("code")
          .GetString() == "1");
    const JsObject deny = Json(DenyTool("q", "No")).at("response").GetJsObject()
        .at("response").GetJsObject();
    CHECK(deny.at("behavior").GetString() == "deny" && deny.at("message").GetString() == "No");
}

void TestParse() {
    std::vector<Event> e = Parse(R"j({"type": "system", "subtype": "init", "cwd": "/x", "session_id": "ee62", "tools": ["Read"], "model": "claude-haiku-5-5"})j");
    CHECK(e.size() == 1 && e[0].kind == Event::Kind::Init && e[0].sessionId == "ee62");
    CHECK(e[0].model == "claude-haiku-5-5");

    e = Parse(R"j({"type": "stream_event", "event": {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Hello there"}}, "session_id": "7605", "parent_tool_use_id": null})j");
    CHECK(e.size() == 1 && e[0].kind == Event::Kind::TextDelta && e[0].text == "Hello there");
    CHECK(Parse(R"j({"type": "stream_event", "event": {"type": "message_stop"}})j").empty());
    CHECK(Parse(R"j({"type": "stream_event", "event": {"type": "content_block_delta", "delta": {"type": "thinking_delta", "thinking": "hm"}}})j")[0].kind == Event::Kind::Thinking);

    e = Parse(R"j({"type": "assistant", "message": {"role": "assistant", "content": [{"type": "text", "text": "Let me look."}, {"type": "tool_use", "id": "toolu_1", "name": "mcp__vela__run_python", "input": {"code": "print(1)"}}]}, "parent_tool_use_id": null})j");
    CHECK(e.size() == 2);
    CHECK(e[0].kind == Event::Kind::Text && e[0].text == "Let me look.");
    CHECK(e[1].kind == Event::Kind::ToolUse && e[1].toolId == "toolu_1");
    CHECK(e[1].toolName == "mcp__vela__run_python");
    CHECK(e[1].input.at("code").GetString() == "print(1)");

    // A subagent's message is not the main conversation's.
    CHECK(Parse(R"j({"type": "assistant", "message": {"content": [{"type": "text", "text": "x"}]}, "parent_tool_use_id": "toolu_9"})j").empty());

    e = Parse(R"j({"type": "user", "message": {"role": "user", "content": [{"tool_use_id": "toolu_1", "type": "tool_result", "content": "(Bash completed with no output)", "is_error": false}]}, "parent_tool_use_id": null})j");
    CHECK(e.size() == 1 && e[0].kind == Event::Kind::ToolResult && e[0].toolId == "toolu_1");
    CHECK(e[0].text == "(Bash completed with no output)" && !e[0].isError);
    e = Parse(R"j({"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "t2", "is_error": true, "content": [{"type": "text", "text": "a"}, {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "QUJD"}}, {"type": "text", "text": "b"}]}]}})j");
    CHECK(e.size() == 1 && e[0].isError && e[0].text == "a\nb");
    CHECK(e[0].images.size() == 1 && e[0].images[0] == "QUJD");
    // The user's own messages echo nothing.
    CHECK(Parse(R"j({"type": "user", "message": {"content": [{"type": "text", "text": "hi"}]}})j").empty());

    e = Parse(R"j({"type": "control_request", "request_id": "5813", "request": {"subtype": "can_use_tool", "tool_name": "Bash", "display_name": "Bash", "input": {"command": "echo hello > hello.txt"}, "description": "Write hello"}})j");
    CHECK(e.size() == 1 && e[0].kind == Event::Kind::Permission && e[0].requestId == "5813");
    CHECK(e[0].toolName == "Bash" && e[0].text == "Write hello");
    CHECK(e[0].input.at("command").GetString() == "echo hello > hello.txt");

    e = Parse(R"j({"type": "result", "subtype": "success", "is_error": false, "duration_api_ms": 3299, "result": "done", "session_id": "ee62", "total_cost_usd": 0.0063})j");
    CHECK(e.size() == 1 && e[0].kind == Event::Kind::Result && !e[0].isError);
    CHECK(e[0].text == "done" && e[0].sessionId == "ee62" && e[0].costUsd > 0.006);
    e = Parse(R"j({"type": "result", "subtype": "error_during_execution", "is_error": true, "errors": ["Not logged in"]})j");
    CHECK(e[0].isError && e[0].text == "Not logged in");
    e = Parse(R"j({"type": "result", "subtype": "error_max_turns", "is_error": true})j");
    CHECK(e[0].text == "error_max_turns");

    CHECK(Parse("not json").empty());
    CHECK(Parse(R"j({"type": "rate_limit_event"})j").empty());
}

void TestDecisions() {
    auto isQuery = [](const std::string &action) { return action == "get_settings"; };
    const JsObject none;
    for (bool ask : {false, true}) {
        CHECK(Decide("mcp__vela__get_app_state", none, ask, isQuery) == Decision::Allow);
        CHECK(Decide("mcp__vela__query_python", none, ask, isQuery) == Decision::Allow);
        CHECK(Decide("mcp__vela__viewport_screenshot", none, ask, isQuery) == Decision::Allow);
        CHECK(Decide("Read", none, ask, isQuery) == Decision::Allow);
        CHECK(Decide("mcp__vela__ui_action", Json(R"j({"action": "get_settings"})j"), ask,
                     isQuery) == Decision::Allow);
        // Anything outside the app always asks.
        CHECK(Decide("Bash", none, ask, isQuery) == Decision::Ask);
        CHECK(Decide("mcp__other__run_python", none, ask, isQuery) == Decision::Ask);
    }
    for (const char *tool : {"mcp__vela__run_python", "mcp__vela__open_document"}) {
        CHECK(Decide(tool, none, false, isQuery) == Decision::Allow);
        CHECK(Decide(tool, none, true, isQuery) == Decision::Ask);
    }
    const JsObject edit = Json(R"j({"action": "set_visibility"})j");
    CHECK(Decide("mcp__vela__ui_action", edit, false, isQuery) == Decision::Allow);
    CHECK(Decide("mcp__vela__ui_action", edit, true, isQuery) == Decision::Ask);

    CHECK(ToolTitle("mcp__vela__run_python") == "Run Python");
    CHECK(ToolTitle("Read") == "Read");
    CHECK(ToolTitle("mcp__vela__new_tool") == "new_tool");
    CHECK(ToolSummary("mcp__vela__run_python", Json(R"j({"code": "\n  x = 1\ny = 2"})j")) == "x = 1 …");
    CHECK(ToolSummary("mcp__vela__ui_action", Json(R"j({"action": "select", "params": {"paths": ["/A"]}})j")) ==
          R"j(select {"paths":["/A"]})j");
    CHECK(ToolSummary("Read", Json(R"j({"file_path": "/a/b.usda"})j")) == "/a/b.usda");
}

// Waits for a condition the reader thread sets.
struct Waiter {
    std::mutex mutex;
    std::condition_variable changed;
    template <class F> bool Wait(F done) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), done);
    }
};

void TestProcess() {
    Waiter waiter;
    std::vector<std::string> lines;
    int status = -2;
    std::string errors;
    LineProcess process;
    LineProcess::Callbacks callbacks;
    callbacks.line = [&](const std::string &line) {
        std::lock_guard<std::mutex> lock(waiter.mutex);
        lines.push_back(line);
        waiter.changed.notify_all();
    };
    callbacks.exited = [&](int s, const std::string &e) {
        std::lock_guard<std::mutex> lock(waiter.mutex);
        status = s;
        errors = e;
        waiter.changed.notify_all();
    };
    std::string error;
    // Echoes each line back with the directory and an environment value,
    // then writes to stderr and exits 3 when stdin closes.
    CHECK(process.Start({"/bin/sh", "-c",
                         "while read l; do echo \"$l $(pwd) $VELA_TEST\"; done; echo oops >&2; exit 3"},
                        "/tmp", {"VELA_TEST=yes"}, callbacks, &error));
    CHECK(process.IsRunning());
    CHECK(process.Write("one") && process.Write("two"));
    CHECK(waiter.Wait([&] { return lines.size() == 2; }));
    CHECK(lines.size() == 2 && lines[0].rfind("one ", 0) == 0 && lines[1].find(" yes") != std::string::npos);
    CHECK(lines[0].find("/tmp") != std::string::npos);
    // Closing stdin (as Terminate does, but keeping callbacks) ends it.
    const std::vector<std::string> args = {"/bin/sh", "-c", "read l; echo got $l; echo bad >&2; exit 3"};
    LineProcess second;
    lines.clear();
    CHECK(second.Start(args, "", {}, callbacks, &error));
    CHECK(second.Write("x"));
    CHECK(waiter.Wait([&] { return status != -2; }));
    CHECK(lines.size() == 1 && lines[0] == "got x");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 3 && errors == "bad\n");
    CHECK(!second.IsRunning() && !second.Write("late"));
    // Terminate silences callbacks and ends a process that ignores stdin.
    LineProcess third;
    status = -2;
    CHECK(third.Start({"/bin/sleep", "30"}, "", {}, callbacks, &error));
    third.Terminate();
    CHECK(!third.IsRunning());
    process.Terminate();

    CHECK(!LineProcess().Start({"/no/such/program"}, "", {}, callbacks, &error));
    CHECK(error.find("/no/such/program") != std::string::npos);

    std::string output;
    CHECK(RunAndCapture({"/bin/echo", "2.1.296 (Claude Code)"}, &output));
    CHECK(output == "2.1.296 (Claude Code)\n");
    CHECK(!RunAndCapture({"/bin/sh", "-c", "exit 1"}, &output));
    CHECK(!RunAndCapture({"/bin/sleep", "5"}, &output, 1));
}

void TestHistory() {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("vela-claude-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(folder);
    const std::string dir = (folder / "Claude").string();
    CHECK(ListConversations(dir).empty());
    Conversation a{"aaaa-1", "Make the lights warmer", "/scenes/room.usda", 100.0};
    Conversation b{"bbbb-2", "Which prims have no material?", "/scenes/other.usda", 200.0};
    CHECK(CreateConversation(dir, a));
    CHECK(AppendEntry(dir, a.id, JsObject{{"kind", JsValue("user")}, {"text", JsValue("hi\nthere")}}));
    CHECK(AppendEntry(dir, a.id, JsObject{{"kind", JsValue("assistant")}, {"text", JsValue("done")}}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(CreateConversation(dir, b));
    CHECK(CreateConversation(dir, b));  // already there: kept
    const std::vector<JsObject> entries = LoadEntries(dir, a.id);
    CHECK(entries.size() == 2 && entries[0].at("text").GetString() == "hi\nthere");
    std::vector<Conversation> all = ListConversations(dir);
    CHECK(all.size() == 2 && all[0].id == "bbbb-2" && all[1].title == "Make the lights warmer");
    CHECK(all[1].document == "/scenes/room.usda" && all[1].created == 100.0 && all[1].updated > 0);
    all = ListConversations(dir, "/scenes/room.usda");
    CHECK(all.size() == 1 && all[0].id == "aaaa-1");
    // Ids can't name other files.
    CHECK(!CreateConversation(dir, Conversation{"../escape", "", "", 0}));
    CHECK(!AppendEntry(dir, "../escape", JsObject()));
    CHECK(LoadEntries(dir, "nope").empty());
    CHECK(DeleteConversation(dir, a.id) && ListConversations(dir).size() == 1);
    std::filesystem::remove_all(folder);

    CHECK(TitleFor("  make every light\n\n in this room 20% warmer ") ==
          "make every light in this room 20% warmer");
    const std::string longTitle = TitleFor(std::string(30, 'a') + " " + std::string(40, 'b'));
    CHECK(longTitle == std::string(30, 'a') + "…");
}

} // namespace

int main() {
    TestVersions();
    TestArguments();
    TestMessages();
    TestParse();
    TestDecisions();
    TestProcess();
    TestHistory();
    return test::Finish("claude");
}
