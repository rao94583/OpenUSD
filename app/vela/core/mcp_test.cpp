// The MCP server's protocol handling and tools, and its socket, without
// Python (run_python is checked by Vela --check-python and mcp_ui_test).
#include "mcp.h"
#include "mcp_socket.h"
#include "document.h"
#include "session.h"
#include "test_support.h"

#include "pxr/base/js/json.h"
#include "pxr/usd/usdGeom/xform.h"

#include <filesystem>
#include <cstring>
#include <mutex>
#include <string>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace vela;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

JsObject Call(const std::string &request) {
    const JsValue reply = JsParseString(mcp::Handle(request));
    return reply.IsObject() ? reply.GetJsObject() : JsObject();
}

JsObject Result(const std::string &request) {
    JsObject reply = Call(request);
    auto it = reply.find("result");
    return it != reply.end() && it->second.IsObject() ? it->second.GetJsObject() : JsObject();
}

int ErrorCode(const std::string &request) {
    JsObject reply = Call(request);
    auto it = reply.find("error");
    return it != reply.end() ? it->second.GetJsObject().at("code").GetInt() : 0;
}

std::string ToolCall(const std::string &name, const std::string &arguments) {
    return R"({"jsonrpc": "2.0", "id": 9, "method": "tools/call", "params": {"name": ")" +
           name + R"(", "arguments": )" + arguments + "}}";
}

std::string FirstText(const JsObject &result) {
    return result.at("content").GetJsArray()[0].GetJsObject().at("text").GetString();
}

void TestProtocol() {
    // Legacy: the initialize handshake, with the client's version if known.
    JsObject init = Result(R"({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "test", "version": "1"}}})");
    CHECK(init.at("protocolVersion").GetString() == "2025-06-18");
    CHECK(init.at("capabilities").GetJsObject().count("tools"));
    CHECK(init.at("serverInfo").GetJsObject().at("name").GetString() == "vela");
    CHECK(!init.count("resultType"));
    init = Result(R"({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                      "params": {"protocolVersion": "1999-01-01"}})");
    CHECK(init.at("protocolVersion").GetString() == "2025-11-25");
    // Notifications get no reply.
    CHECK(mcp::Handle(R"({"jsonrpc": "2.0", "method": "notifications/initialized"})").empty());
    CHECK(ErrorCode("not json") == -32700);
    CHECK(ErrorCode(R"({"jsonrpc": "2.0", "id": 2, "method": "nope"})") == -32601);

    // Modern: per-request _meta, server/discover, resultType.
    const std::string meta =
        R"("_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                     "io.modelcontextprotocol/clientInfo": {"name": "test", "version": "1"},
                     "io.modelcontextprotocol/clientCapabilities": {}})";
    JsObject discover = Result(R"({"jsonrpc": "2.0", "id": "d", "method": "server/discover",
                                   "params": {)" + meta + "}}");
    CHECK(discover.at("resultType").GetString() == "complete");
    CHECK(discover.at("supportedVersions").GetJsArray()[0].GetString() == "2026-07-28");
    CHECK(discover.at("_meta").GetJsObject().count("io.modelcontextprotocol/serverInfo"));
    CHECK(ErrorCode(R"({"jsonrpc": "2.0", "id": 3, "method": "tools/list", "params": {
        "_meta": {"io.modelcontextprotocol/protocolVersion": "1900-01-01"}}})") == -32022);
    JsObject list = Result(R"({"jsonrpc": "2.0", "id": 4, "method": "tools/list", "params": {)" +
                           meta + "}}");
    CHECK(list.at("resultType").GetString() == "complete");
    CHECK(list.at("ttlMs").GetInt() > 0 && list.at("cacheScope").GetString() == "public");
    CHECK(discover.at("ttlMs").GetInt() > 0 && discover.at("cacheScope").GetString() == "public");
    std::vector<std::string> names;
    for (const JsValue &tool : list.at("tools").GetJsArray()) {
        const JsObject &t = tool.GetJsObject();
        names.push_back(t.at("name").GetString());
        CHECK(t.at("inputSchema").GetJsObject().at("type").GetString() == "object");
    }
    CHECK((names == std::vector<std::string>{"get_app_state", "list_actions", "ui_action",
                                             "run_python", "query_python",
                                             "viewport_screenshot",
                                             "open_document"}));
    CHECK(ErrorCode(ToolCall("no_such_tool", "{}")) == -32602);
    CHECK(Result(R"({"jsonrpc": "2.0", "id": 5, "method": "ping"})").empty());
}

void TestTools() {
    // No document yet.
    JsObject result = Result(ToolCall("ui_action", R"({"action": "frame_all"})"));
    CHECK(result.at("isError").GetBool());

    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomXform::Define(stage, SdfPath("/World"));
    auto document = std::make_shared<Document>(stage);
    SessionDocument entry;
    entry.name = "scene";
    entry.document = document;
    entry.screenshot = [](int width, int height, std::vector<uint8_t> *png, std::string *) {
        png->assign({1, 2, 3});
        return width == 64 && height == 0;
    };
    const int id = Session::Get().Add(entry);
    CHECK(Session::Get().Find(id)->stageCacheId != 0);

    result = Result(ToolCall("ui_action", R"({"action": "select", "params": {"paths": ["/World"]}})"));
    CHECK(!result.at("isError").GetBool());
    CHECK(document->GetSelection() == SdfPathVector{SdfPath("/World")});
    result = Result(ToolCall("ui_action", R"({"action": "select", "params": {"paths": ["/Nope"]}})"));
    CHECK(result.at("isError").GetBool() && FirstText(result) == "No prim at '/Nope'");
    result = Result(ToolCall("ui_action", R"({"action": "set_time", "params": {}})"));
    CHECK(result.at("isError").GetBool());

    result = Result(ToolCall("get_app_state", "{}"));
    const JsObject &state = result.at("structuredContent").GetJsObject();
    CHECK(state.at("documents").GetJsArray().size() == 1);
    const JsObject &current = state.at("document").GetJsObject();
    CHECK(current.at("id").GetInt() == id);
    CHECK(current.at("selection").GetJsArray()[0].GetString() == "/World");
    CHECK(current.at("freeCamera").GetJsObject().at("eye").GetJsArray().size() == 3);
    result = Result(ToolCall("get_app_state", R"({"document": 999})"));
    CHECK(result.at("isError").GetBool());

    result = Result(ToolCall("list_actions", "{}"));
    bool sawQuery = false;
    for (const JsValue &action :
         result.at("structuredContent").GetJsObject().at("actions").GetJsArray()) {
        const JsObject &a = action.GetJsObject();
        if (a.at("name").GetString() == "get_settings") sawQuery = a.at("query").GetBool();
    }
    CHECK(sawQuery);

    result = Result(ToolCall("viewport_screenshot", R"({"width": 64})"));
    CHECK(!result.at("isError").GetBool());
    const JsObject &image = result.at("content").GetJsArray()[0].GetJsObject();
    CHECK(image.at("type").GetString() == "image" && image.at("data").GetString() == "AQID");

    // Python isn't started in this test.
    result = Result(ToolCall("run_python", R"({"code": "1"})"));
    CHECK(result.at("isError").GetBool());

    Session::Get().Remove(id);
    CHECK(Session::Get().GetCurrent() == nullptr);
}

void TestBase64() {
    const unsigned char data[] = {'M', 'a', 'n', 'y'};
    CHECK(mcp::Base64(data, 4) == "TWFueQ==");
    CHECK(mcp::Base64(data, 3) == "TWFu");
    CHECK(mcp::Base64(data, 2) == "TWE=");
}

// Lines in, replies out, through the socket, with a stale socket file
// replaced and a second server refused.
void TestSocket() {
    const std::string dir = std::filesystem::temp_directory_path() /
        ("vela-mcp-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    const std::string path = dir + "/sub/mcp.sock";
    std::mutex mutex;  // stands in for the main thread
    auto dispatch = [&](const std::function<void()> &f) {
        std::lock_guard<std::mutex> lock(mutex);
        f();
    };
    McpSocketServer server(path, dispatch);
    std::string error;
    CHECK(server.Start(&error));
    struct stat info;
    CHECK(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    CHECK(stat((dir + "/sub").c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
    McpSocketServer second(path, dispatch);
    CHECK(!second.Start(&error) && error.find("Another Vela") != std::string::npos);

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path.c_str());
    CHECK(connect(fd, (sockaddr *)&address, sizeof(address)) == 0);
    const std::string requests =
        R"({"jsonrpc": "2.0", "method": "notifications/initialized"})" "\n"
        R"({"jsonrpc": "2.0", "id": 1, "method": "ping"})" "\n\n"
        R"({"jsonrpc": "2.0", "id": 2, "method": "ping"})" "\n";
    CHECK(write(fd, requests.data(), requests.size()) == ssize_t(requests.size()));
    shutdown(fd, SHUT_WR);
    std::string replies;
    char buffer[4096];
    for (ssize_t n; (n = read(fd, buffer, sizeof(buffer))) > 0;) replies.append(buffer, size_t(n));
    close(fd);
    CHECK(replies == R"({"id":1,"jsonrpc":"2.0","result":{}})" "\n"
                     R"({"id":2,"jsonrpc":"2.0","result":{}})" "\n");

    server.Stop();
    CHECK(!std::filesystem::exists(path));
    // A socket file nobody answers on is replaced.
    const int stale = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(stale, (sockaddr *)&address, sizeof(address)) == 0);
    close(stale);
    McpSocketServer third(path, dispatch);
    CHECK(third.Start(&error));
    third.Stop();
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    test::RegisterPlugins();
    TestProtocol();
    TestTools();
    TestBase64();
    TestSocket();
    return test::Finish("MCP");
}
