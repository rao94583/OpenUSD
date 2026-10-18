#include "claude_cli.h"
#include "mcp.h"

#include "pxr/base/js/json.h"

#include <cstdio>
#include <set>
#include <tuple>

namespace vela {
namespace claude {

namespace {

const JsValue *Get(const JsObject &object, const char *key) {
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

std::string String(const JsObject &object, const char *key) {
    const JsValue *value = Get(object, key);
    return value && value->IsString() ? value->GetString() : std::string();
}

const JsObject *Object(const JsObject &object, const char *key) {
    const JsValue *value = Get(object, key);
    return value && value->IsObject() ? &value->GetJsObject() : nullptr;
}

const JsArray *Array(const JsObject &object, const char *key) {
    const JsValue *value = Get(object, key);
    return value && value->IsArray() ? &value->GetJsArray() : nullptr;
}

bool Bool(const JsObject &object, const char *key) {
    const JsValue *value = Get(object, key);
    return value && value->IsBool() && value->GetBool();
}

double Number(const JsObject &object, const char *key) {
    const JsValue *value = Get(object, key);
    if (!value) return 0.0;
    if (value->IsReal()) return value->GetReal();
    if (value->IsUInt64()) return double(value->GetUInt64());
    if (value->IsInt()) return double(value->GetInt64());
    return 0.0;
}

std::string Line(const JsObject &object) { return mcp::Compact(JsValue(object)); }

// A tool result's content: a string, or text and image blocks.
void ReadToolContent(const JsValue &content, Event *event) {
    if (content.IsString()) {
        event->text = content.GetString();
        return;
    }
    if (!content.IsArray()) return;
    for (const JsValue &block : content.GetJsArray()) {
        if (!block.IsObject()) continue;
        const JsObject &b = block.GetJsObject();
        const std::string type = String(b, "type");
        if (type == "text") {
            if (!event->text.empty()) event->text += "\n";
            event->text += String(b, "text");
        } else if (type == "image") {
            if (const JsObject *source = Object(b, "source")) {
                const std::string data = String(*source, "data");
                if (!data.empty()) event->images.push_back(data);
            }
        }
    }
}

const std::string Prefix = std::string("mcp__") + McpServer + "__";

// The app tool a CLI tool name stands for, or "".
std::string AppTool(const std::string &toolName) {
    return toolName.compare(0, Prefix.size(), Prefix) == 0 ? toolName.substr(Prefix.size())
                                                           : std::string();
}

std::string FirstLine(const std::string &text, size_t limit = 80) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    size_t end = text.find('\n', start);
    std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (line.size() > limit) line = line.substr(0, limit - 1) + "…";
    else if (end != std::string::npos && text.find_first_not_of(" \t\r\n", end) != std::string::npos) {
        line += " …";
    }
    return line;
}

} // namespace

bool Version::operator<(const Version &other) const {
    return std::tie(major, minor, patch) < std::tie(other.major, other.minor, other.patch);
}

std::string Version::ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

bool ParseVersion(const std::string &text, Version *version) {
    const size_t start = text.find_first_of("0123456789");
    if (start == std::string::npos) return false;
    Version v;
    int consumed = 0;
    if (std::sscanf(text.c_str() + start, "%d.%d.%d%n", &v.major, &v.minor, &v.patch,
                    &consumed) != 3) {
        return false;
    }
    *version = v;
    return true;
}

std::vector<std::string> CandidatePaths(const std::string &home) {
    return {
        home + "/.local/bin/claude",
        "/opt/homebrew/bin/claude",
        "/usr/local/bin/claude",
        home + "/.npm-global/bin/claude",
        home + "/.claude/local/claude",
    };
}

std::string McpToolName(const std::string &tool) { return Prefix + tool; }

std::vector<std::string> Arguments(const LaunchOptions &options) {
    std::vector<std::string> args = {
        options.executable,
        "-p",
        "--input-format", "stream-json",
        "--output-format", "stream-json",
        "--verbose",
        "--include-partial-messages",
        // Approvals come to the panel as can_use_tool requests.
        "--permission-prompt-tool", "stdio",
        // Only the app's tools and reading files: the scene is edited
        // through Vela, where every edit can be undone.
        "--tools", "Read,Glob,Grep",
        "--strict-mcp-config",
    };
    const JsObject server{
        {"type", JsValue("stdio")},
        {"command", JsValue(options.mcpHelper)},
        {"args", JsValue(JsArray())},
        {"env", JsValue(JsObject{{"VELA_MCP_SOCKET", JsValue(options.mcpSocket)}})},
    };
    args.push_back("--mcp-config");
    args.push_back(Line(JsObject{{"mcpServers", JsValue(JsObject{{McpServer, JsValue(server)}})}}));
    if (!options.resume.empty()) {
        args.push_back("--resume");
        args.push_back(options.resume);
    } else if (!options.sessionId.empty()) {
        args.push_back("--session-id");
        args.push_back(options.sessionId);
    }
    if (!options.model.empty()) {
        args.push_back("--model");
        args.push_back(options.model);
    }
    if (!options.effort.empty()) {
        args.push_back("--effort");
        args.push_back(options.effort);
    }
    if (!options.systemPrompt.empty()) {
        args.push_back("--append-system-prompt");
        args.push_back(options.systemPrompt);
    }
    return args;
}

const std::vector<Choice> &ModelChoices() {
    static const std::vector<Choice> choices = {
        {"", "Default model"}, {"opus", "Opus"}, {"sonnet", "Sonnet"},
        {"haiku", "Haiku"}, {"fable", "Fable"}};
    return choices;
}

const std::vector<Choice> &EffortChoices() {
    static const std::vector<Choice> choices = {
        {"", "Default effort"}, {"low", "Low effort"}, {"medium", "Medium effort"},
        {"high", "High effort"}, {"xhigh", "Extra high effort"}, {"max", "Max effort"}};
    return choices;
}

std::string InitializeRequest(const std::string &requestId) {
    return Line(JsObject{
        {"type", JsValue("control_request")},
        {"request_id", JsValue(requestId)},
        {"request", JsValue(JsObject{{"subtype", JsValue("initialize")}})},
    });
}

std::string InterruptRequest(const std::string &requestId) {
    return Line(JsObject{
        {"type", JsValue("control_request")},
        {"request_id", JsValue(requestId)},
        {"request", JsValue(JsObject{{"subtype", JsValue("interrupt")}})},
    });
}

std::string UserMessage(const std::string &text, const std::vector<std::string> &pngBase64) {
    JsArray content;
    for (const std::string &image : pngBase64) {
        content.push_back(JsValue(JsObject{
            {"type", JsValue("image")},
            {"source", JsValue(JsObject{
                {"type", JsValue("base64")},
                {"media_type", JsValue("image/png")},
                {"data", JsValue(image)},
            })},
        }));
    }
    content.push_back(JsValue(JsObject{{"type", JsValue("text")}, {"text", JsValue(text)}}));
    return Line(JsObject{
        {"type", JsValue("user")},
        {"message", JsValue(JsObject{{"role", JsValue("user")}, {"content", JsValue(content)}})},
        {"parent_tool_use_id", JsValue()},
        {"session_id", JsValue("")},
    });
}

namespace {

std::string PermissionResponse(const std::string &requestId, JsObject response) {
    return Line(JsObject{
        {"type", JsValue("control_response")},
        {"response", JsValue(JsObject{
            {"subtype", JsValue("success")},
            {"request_id", JsValue(requestId)},
            {"response", JsValue(std::move(response))},
        })},
    });
}

} // namespace

std::string AllowTool(const std::string &requestId, const JsObject &input) {
    return PermissionResponse(requestId, JsObject{
        {"behavior", JsValue("allow")}, {"updatedInput", JsValue(input)}});
}

std::string DenyTool(const std::string &requestId, const std::string &message) {
    return PermissionResponse(requestId, JsObject{
        {"behavior", JsValue("deny")}, {"message", JsValue(message)}});
}

std::vector<Event> Parse(const std::string &line) {
    std::vector<Event> events;
    const JsValue parsed = JsParseString(line);
    if (!parsed.IsObject()) return events;
    const JsObject &message = parsed.GetJsObject();
    const std::string type = String(message, "type");
    // Subagents' messages carry their parent tool call; the panel shows
    // only the main conversation.
    if (const JsValue *parent = Get(message, "parent_tool_use_id"); parent && parent->IsString()) {
        return events;
    }

    if (type == "system" && String(message, "subtype") == "init") {
        Event event;
        event.kind = Event::Kind::Init;
        event.sessionId = String(message, "session_id");
        event.model = String(message, "model");
        events.push_back(event);
    } else if (type == "stream_event") {
        const JsObject *e = Object(message, "event");
        if (!e || String(*e, "type") != "content_block_delta") return events;
        const JsObject *delta = Object(*e, "delta");
        if (!delta) return events;
        const std::string deltaType = String(*delta, "type");
        Event event;
        if (deltaType == "text_delta") {
            event.kind = Event::Kind::TextDelta;
            event.text = String(*delta, "text");
        } else if (deltaType == "thinking_delta") {
            event.kind = Event::Kind::Thinking;
        } else {
            return events;
        }
        events.push_back(event);
    } else if (type == "assistant" || type == "user") {
        const JsObject *body = Object(message, "message");
        const JsArray *content = body ? Array(*body, "content") : nullptr;
        if (!content) return events;
        for (const JsValue &block : *content) {
            if (!block.IsObject()) continue;
            const JsObject &b = block.GetJsObject();
            const std::string blockType = String(b, "type");
            Event event;
            if (type == "assistant" && blockType == "text") {
                event.kind = Event::Kind::Text;
                event.text = String(b, "text");
            } else if (type == "assistant" && blockType == "tool_use") {
                event.kind = Event::Kind::ToolUse;
                event.toolId = String(b, "id");
                event.toolName = String(b, "name");
                if (const JsObject *input = Object(b, "input")) event.input = *input;
            } else if (type == "user" && blockType == "tool_result") {
                event.kind = Event::Kind::ToolResult;
                event.toolId = String(b, "tool_use_id");
                event.isError = Bool(b, "is_error");
                if (const JsValue *c = Get(b, "content")) ReadToolContent(*c, &event);
            } else {
                continue;
            }
            events.push_back(event);
        }
    } else if (type == "control_request") {
        const JsObject *request = Object(message, "request");
        if (!request || String(*request, "subtype") != "can_use_tool") return events;
        Event event;
        event.kind = Event::Kind::Permission;
        event.requestId = String(message, "request_id");
        event.toolName = String(*request, "tool_name");
        event.text = String(*request, "description");
        if (const JsObject *input = Object(*request, "input")) event.input = *input;
        events.push_back(event);
    } else if (type == "result") {
        Event event;
        event.kind = Event::Kind::Result;
        event.sessionId = String(message, "session_id");
        event.isError = Bool(message, "is_error");
        event.text = String(message, "result");
        if (event.isError && event.text.empty()) {
            if (const JsArray *errors = Array(message, "errors")) {
                for (const JsValue &error : *errors) {
                    if (!error.IsString()) continue;
                    if (!event.text.empty()) event.text += "\n";
                    event.text += error.GetString();
                }
            }
            if (event.text.empty()) event.text = String(message, "subtype");
        }
        event.costUsd = Number(message, "total_cost_usd");
        events.push_back(event);
    }
    return events;
}

Decision Decide(const std::string &toolName, const JsObject &input, bool asksBeforeEdits,
                const std::function<bool(const std::string &action)> &isQuery) {
    static const std::set<std::string> reading = {"Read", "Glob", "Grep"};
    if (reading.count(toolName)) return Decision::Allow;
    const std::string tool = AppTool(toolName);
    if (tool.empty()) return Decision::Ask;
    static const std::set<std::string> readOnly = {
        "get_app_state", "list_actions", "query_python", "viewport_screenshot"};
    if (readOnly.count(tool)) return Decision::Allow;
    // Image submission only creates a job; the service enforces its own
    // paid-request/upload policy for every caller.
    static const std::set<std::string> imageTools = {
        "list_image_models", "capture_image_reference", "generate_image",
        "get_image_job", "cancel_image_job", "list_generated_images", "get_generated_image"};
    if (imageTools.count(tool)) return Decision::Allow;
    if (tool == "ui_action") {
        const std::string action = String(input, "action");
        if (isQuery && isQuery(action)) return Decision::Allow;
    }
    // run_python, ui_action, open_document and any tool added later.
    return asksBeforeEdits ? Decision::Ask : Decision::Allow;
}

std::string ToolTitle(const std::string &toolName) {
    static const std::pair<const char *, const char *> titles[] = {
        {"get_app_state", "Get App State"},
        {"list_actions", "List Actions"},
        {"ui_action", "Run Action"},
        {"run_python", "Run Python"},
        {"query_python", "Query with Python"},
        {"viewport_screenshot", "Viewport Screenshot"},
        {"open_document", "Open Document"},
    };
    const std::string tool = AppTool(toolName);
    for (const auto &[name, title] : titles) {
        if (tool == name) return title;
    }
    if (!tool.empty()) return tool;
    if (toolName.compare(0, 5, "mcp__") == 0) return toolName.substr(5);
    return toolName;
}

std::string ToolSummary(const std::string &toolName, const JsObject &input) {
    const std::string tool = AppTool(toolName);
    if (tool == "run_python" || tool == "query_python") return FirstLine(String(input, "code"));
    if (tool == "ui_action") {
        std::string summary = String(input, "action");
        if (const JsObject *params = Object(input, "params"); params && !params->empty()) {
            summary += " " + Line(*params);
        }
        return FirstLine(summary, 100);
    }
    if (tool == "open_document") return String(input, "path");
    for (const char *key : {"file_path", "pattern", "path", "command", "url"}) {
        const std::string value = String(input, key);
        if (!value.empty()) return FirstLine(value, 100);
    }
    return std::string();
}

} // namespace claude
} // namespace vela
