#include "mcp.h"
#include "actions.h"
#include "document.h"
#include "log.h"
#include "python.h"
#include "session.h"
#include "image_jobs.h"
#include <set>

#include "pxr/base/js/json.h"

#include <algorithm>
#include <functional>
#include <map>
#include <sstream>

namespace vela {
namespace mcp {

namespace {

// Initialize-handshake versions, newest first.
const char *const LegacyVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

const char *const Instructions =
    "Vela is a USD scene viewer and editor. These tools act on the scenes "
    "open in it: the current document is the front window's unless a tool "
    "names another by id (get_app_state lists them). run_python runs code "
    "against the live stage with the whole pxr API and a vela module "
    "(vela.stage, vela.selection, vela.time, vela.run(action, **params)); "
    "edits appear in the window at once and each call is one undo step. "
    "query_python is the same for questions: it never edits. "
    "ui_action runs any menu command by name (list_actions describes them, "
    "e.g. undo, frame_selection, set_draw_mode). viewport_screenshot shows "
    "what the viewport renders; the export_image and export_movie actions "
    "write it to files (any size, png/jpg/tif/exr, or .mov/.mp4 movies "
    "through the view's camera), rendered offscreen.";

// JSON-RPC error codes.
constexpr int ParseError = -32700;
constexpr int InvalidRequest = -32600;
constexpr int MethodNotFound = -32601;
constexpr int InvalidParams = -32602;
constexpr int UnsupportedVersion = -32022;

JsValue Parse(const std::string &json) { return JsParseString(json); }

// JSON integers parse as uint64 when non-negative, int64 otherwise.
bool Integer(const JsValue &value, int64_t *result) {
    if (value.IsUInt64()) *result = int64_t(value.GetUInt64());
    else if (value.IsInt()) *result = value.GetInt64();
    else return false;
    return true;
}

JsObject Schema(const std::string &json) {
    const JsValue value = Parse(json);
    return value.IsObject() ? value.GetJsObject() : JsObject();
}

std::string Reply(const JsValue &id, JsObject result) {
    return Compact(JsValue(JsObject{
        {"jsonrpc", JsValue("2.0")}, {"id", id}, {"result", JsValue(std::move(result))}}));
}

std::string ErrorReply(const JsValue &id, int code, const std::string &message,
                       const JsValue &data = JsValue()) {
    JsObject error{{"code", JsValue(code)}, {"message", JsValue(message)}};
    if (!data.IsNull()) error["data"] = data;
    return Compact(JsValue(JsObject{
        {"jsonrpc", JsValue("2.0")}, {"id", id}, {"error", JsValue(error)}}));
}

JsValue Text(const std::string &text) {
    return JsValue(JsObject{{"type", JsValue("text")}, {"text", JsValue(text)}});
}

// A tool's result: text content, and the same value as structured content
// when it has one.
JsObject ToolResult(const std::string &text, bool isError = false,
                    const JsValue &structured = JsValue()) {
    JsObject result{{"content", JsValue(JsArray{Text(text)})}, {"isError", JsValue(isError)}};
    if (!structured.IsNull()) result["structuredContent"] = structured;
    return result;
}

JsObject JsonResult(const JsValue &value) {
    // Structured content must be an object for older clients.
    const JsValue structured =
        value.IsObject() ? value : JsValue(JsObject{{"result", value}});
    return ToolResult(JsWriteToString(value), false, structured);
}

// The named document (an "document" argument), or the current one.
const SessionDocument *TargetDocument(const JsObject &arguments, std::string *error) {
    auto it = arguments.find("document");
    if (it != arguments.end() && !it->second.IsNull()) {
        int64_t id = 0;
        if (!Integer(it->second, &id)) {
            *error = "document must be an integer id";
            return nullptr;
        }
        if (Session::Get().ScopedId() && id != Session::Get().ScopedId()) {
            *error = "This MCP connection is bound to another document";
            return nullptr;
        }
        const SessionDocument *entry = Session::Get().Find(int(id));
        if (!entry) *error = "No open document has id " + JsWriteToString(it->second);
        return entry;
    }
    const SessionDocument *entry = Session::Get().GetCurrent();
    if (!entry) *error = "No document is open in Vela. Open one with open_document.";
    return entry;
}

// run_python and query_python. A query's edits to the document's layers
// are reverted (and dropped from undo).
JsObject RunPythonTool(const JsObject &arguments, bool query) {
    auto code = arguments.find("code");
    if (code == arguments.end() || !code->second.IsString()) {
        return ToolResult("code must be a string", true);
    }
    std::string error;
    const SessionDocument *entry = TargetDocument(arguments, &error);
    if (!entry && arguments.count("document")) return ToolResult(error, true);
    const std::shared_ptr<Document> document = entry ? entry->document : nullptr;
    const uint64_t mark = query && document ? document->GetUndoStack().Mark() : 0;
    const PythonResult result = RunPython(code->second.GetString(), "mcp", entry ? entry->id : 0);
    std::string text = result.output;
    const std::string &tail = result.ok ? result.value : result.error;
    if (!tail.empty()) {
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += tail;
    }
    if (query && document && document->GetUndoStack().Discard(mark)) {
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += "query_python cannot edit the stage, so its edits were reverted. "
                "Use run_python to make changes.";
        return ToolResult(text, true);
    }
    if (text.empty()) text = "(no output)";
    return ToolResult(text, !result.ok);
}

JsObject ImageTool(const std::string &name, const JsObject &arguments) {
    using images::Text;
    auto &service = images::Service::Get();
    if (name == "list_image_models") {
        if (!arguments.empty()) return ToolResult("list_image_models takes no arguments",true);
        return JsonResult(JsValue(JsObject{{"models",JsValue(service.Models())}}));
    }
    std::string error;
    const auto *entry = TargetDocument(arguments,&error);
    if (!entry) return ToolResult(error,true);
    const int bound = Session::Get().ScopedId();
    if (bound && entry->id != bound) return ToolResult("Image tools are bound to this panel's document",true);
    JsObject input = arguments; input.erase("document");
    if (name == "generate_image") {
        auto r = service.Submit(entry->id,input);
        return r ? JsonResult(JsValue(r.value)) : ToolResult(r.error,true);
    }
    const std::set<std::string> fields = name == "capture_image_reference" ? std::set<std::string>{"width","height"} :
        name == "get_generated_image" ? std::set<std::string>{"artifact_id","include_preview"} :
        name == "list_generated_images" ? std::set<std::string>{} : std::set<std::string>{"job_id"};
    for (const auto &[k,v] : input) if (!fields.count(k)) return ToolResult("Unknown argument: " + k,true);
    if (name == "capture_image_reference") {
        int width=1024,height=0;
        for (const auto &[k,v] : input) {
            int64_t n=0; if (!Integer(v,&n) || n<16 || n>2048) return ToolResult("Capture dimensions must be 16–2048",true);
            if (k=="width") width=int(n); else height=int(n);
        }
        if (!entry->screenshot || !service.normalize) return ToolResult("Image capture unavailable",true);
        images::Bytes png;
        if (!entry->screenshot(width,height,&png,&error)) return ToolResult(error,true);
        auto o=service.normalize(png); if (!o.error.empty()) return ToolResult(o.error,true);
        auto r=service.AddReference(entry->id,std::move(o),JsObject{{"source",JsValue(DocumentState(*entry))}});
        return r ? JsonResult(JsValue(r.value)) : ToolResult(r.error,true);
    }
    if (name == "list_generated_images") return JsonResult(JsValue(JsObject{{"artifacts",JsValue(service.Artifacts(entry->id))}}));
    if (name == "get_generated_image") {
        auto r=service.Artifact(entry->id,Text(input,"artifact_id")); if (!r) return ToolResult(r.error,true);
        auto result=JsonResult(JsValue(r.value));
        if (auto it=input.find("include_preview");it!=input.end()) {
            if (!it->second.IsBool()) return ToolResult("include_preview must be boolean",true);
            if (it->second.GetBool()) {
                auto bytes=service.Read(entry->id,Text(input,"artifact_id"),true);
                if (bytes.empty()) return ToolResult("Preview is missing",true);
                auto content=result.at("content").GetJsArray();
                content.push_back(JsValue(JsObject{{"type",JsValue("image")},{"mimeType",JsValue("image/png")},{"data",JsValue(Base64(bytes.data(),bytes.size()))}}));
                result["content"]=JsValue(content);
            }
        }
        return result;
    }
    auto r=name=="cancel_image_job" ? service.Cancel(entry->id,Text(input,"job_id")) : service.Job(entry->id,Text(input,"job_id"));
    return r ? JsonResult(JsValue(r.value)) : ToolResult(r.error,true);
}

const char *const DocumentProperty =
    R"("document": {"type": "integer", "description": "A document id from get_app_state; the current document when omitted"})";

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    std::string schema;
    bool readOnly = false;
    std::function<JsObject(const JsObject &arguments)> call;
};

const std::vector<Tool> &ToolTable() {
    static const std::vector<Tool> tools = {
        {"list_image_models", "List Image Models",
         "Discover image provider connections, readiness, models and supported options.",
         R"({"type": "object", "properties": {}, "additionalProperties": false})", true,
         [](const JsObject &a) { return ImageTool("list_image_models",a); }},
        {"capture_image_reference", "Capture Image Reference",
         "Capture a local viewport reference for image editing. Does not upload it.",
         R"({"type": "object", "properties": {"width": {"type": "integer"}, "height": {"type": "integer"}, "document": {"type": "integer"}}, "additionalProperties": false})", true,
         [](const JsObject &a) { return ImageTool("capture_image_reference",a); }},
        {"generate_image", "Generate Image",
         "Submit an asynchronous image generation/edit job. Native approval controls uploads and paid calls. Poll sparingly; Generated Images updates independently.",
         R"({"type": "object", "properties": {"request_id": {"type": "string"}, "prompt": {"type": "string"}, "purpose": {"type": "string"}, "reference_ids": {"type": "array", "items": {"type": "string"}}, "connection_id": {"type": "string"}, "model": {"type": "string"}, "size": {"type": "string"}, "quality": {"type": "string"}, "background": {"type": "string"}, "provider_options": {"type": "object"}, "document": {"type": "integer"}}, "additionalProperties": false, "required": ["request_id", "prompt"]})", false,
         [](const JsObject &a) { return ImageTool("generate_image",a); }},
        {"get_image_job", "Get Image Job",
         "Query image job state.",
         R"({"type": "object", "properties": {"job_id": {"type": "string"}, "document": {"type": "integer"}}, "additionalProperties": false, "required": ["job_id"]})", true,
         [](const JsObject &a) { return ImageTool("get_image_job",a); }},
        {"cancel_image_job", "Cancel Image Job",
         "Cancel this image job; charges may already have occurred.",
         R"({"type": "object", "properties": {"job_id": {"type": "string"}, "document": {"type": "integer"}}, "additionalProperties": false, "required": ["job_id"]})", false,
         [](const JsObject &a) { return ImageTool("cancel_image_job",a); }},
        {"list_generated_images", "List Generated Images",
         "List saved image artifacts for this document.",
         R"({"type": "object", "properties": {"document": {"type": "integer"}}, "additionalProperties": false})", true,
         [](const JsObject &a) { return ImageTool("list_generated_images",a); }},
        {"get_generated_image", "Get Generated Image",
         "Read artifact metadata and optionally a bounded image preview.",
         R"({"type": "object", "properties": {"artifact_id": {"type": "string"}, "include_preview": {"type": "boolean"}, "document": {"type": "integer"}}, "additionalProperties": false, "required": ["artifact_id"]})", true,
         [](const JsObject &a) { return ImageTool("get_generated_image",a); }},
        {
            "get_app_state", "Get App State",
            "The open documents, and for the current (or named) document: "
            "selection, time and time range, edit target, layer stack, view "
            "camera, free camera, renderer and undo state.",
            std::string(R"({"type": "object", "properties": {)") + DocumentProperty + "}}",
            true,
            [](const JsObject &arguments) {
                JsObject state{{"documents", JsValue(DocumentList())}};
                std::string error;
                if (const SessionDocument *entry = TargetDocument(arguments, &error)) {
                    state["document"] = JsValue(DocumentState(*entry));
                } else if (arguments.count("document")) {
                    return ToolResult(error, true);
                }
                return JsonResult(JsValue(state));
            },
        },
        {
            "list_actions", "List Actions",
            "Every action ui_action can run (the app's menu commands): name, "
            "title, description, JSON schema of its parameters, menu and "
            "shortcut. query marks actions that only return information.",
            R"({"type": "object", "additionalProperties": false})",
            true,
            [](const JsObject &) {
                return JsonResult(JsValue(JsObject{
                    {"actions", ActionRegistry::Get().Describe()}}));
            },
        },
        {
            "ui_action", "Run Action",
            "Run one of the app's actions by name, as its menu item would, "
            "with parameters matching its schema (see list_actions). Edits "
            "are undoable. Returns the action's result.",
            std::string(R"({"type": "object", "properties": {
                "action": {"type": "string", "description": "An action name, e.g. frame_selection"},
                "params": {"type": "object", "description": "The action's parameters"},
                )") + DocumentProperty + R"(}, "required": ["action"]})",
            false,
            [](const JsObject &arguments) {
                auto action = arguments.find("action");
                if (action == arguments.end() || !action->second.IsString()) {
                    return ToolResult("action must be a string", true);
                }
                JsObject params;
                if (auto p = arguments.find("params"); p != arguments.end() && !p->second.IsNull()) {
                    if (!p->second.IsObject()) return ToolResult("params must be an object", true);
                    params = p->second.GetJsObject();
                }
                std::string error;
                const SessionDocument *entry = TargetDocument(arguments, &error);
                if (!entry) return ToolResult(error, true);
                const std::shared_ptr<Document> document = entry->document;
                const ActionResult result =
                    ActionRegistry::Get().Run(*document, action->second.GetString(), params);
                if (!result.ok) return ToolResult(result.error, true);
                return JsonResult(JsValue(JsObject{{"result", result.value}}));
            },
        },
        {
            "run_python", "Run Python",
            "Run Python 3.11 in Vela, against the live stage, to query or "
            "edit it. The namespace persists between calls and has the whole "
            "pxr package available, with vela, Usd, Sdf, Gf, Vt, Tf, UsdGeom, "
            "UsdShade, UsdLux, UsdUtils and Kind imported. vela.stage is the "
            "document's Usd.Stage (the one the window shows); vela.selection "
            "and vela.time can be read and assigned; vela.run(action, "
            "**params) runs an action. Returns what the code printed and the "
            "value of a final expression; edits are one undo step.",
            std::string(R"({"type": "object", "properties": {
                "code": {"type": "string", "description": "Python source"},
                )") + DocumentProperty + R"(}, "required": ["code"]})",
            false,
            [](const JsObject &arguments) { return RunPythonTool(arguments, false); },
        },
        {
            "query_python", "Query with Python",
            "Like run_python, with the same namespace, but only to read: "
            "any edit to the stage's layers is reverted and reported as an "
            "error. Prefer it for questions about the scene, since it never "
            "needs the user's approval.",
            std::string(R"({"type": "object", "properties": {
                "code": {"type": "string", "description": "Python source"},
                )") + DocumentProperty + R"(}, "required": ["code"]})",
            true,
            [](const JsObject &arguments) { return RunPythonTool(arguments, true); },
        },
        {
            "viewport_screenshot", "Viewport Screenshot",
            "Render the document's viewport, as it is now, to a PNG image. "
            "width and height default to the viewport's size in pixels; "
            "give one to keep the viewport's aspect ratio.",
            std::string(R"({"type": "object", "properties": {
                "width": {"type": "integer", "minimum": 16, "maximum": 4096},
                "height": {"type": "integer", "minimum": 16, "maximum": 4096},
                )") + DocumentProperty + "}}",
            true,
            [](const JsObject &arguments) {
                std::string error;
                const SessionDocument *entry = TargetDocument(arguments, &error);
                if (!entry) return ToolResult(error, true);
                if (!entry->screenshot) return ToolResult("The document has no viewport", true);
                auto size = [&](const char *key) {
                    auto it = arguments.find(key);
                    int64_t value = 0;
                    return it != arguments.end() && Integer(it->second, &value)
                        ? int(std::clamp<int64_t>(value, 16, 4096)) : 0;
                };
                std::vector<uint8_t> png;
                if (!entry->screenshot(size("width"), size("height"), &png, &error)) {
                    return ToolResult(error.empty() ? "Rendering failed" : error, true);
                }
                return JsObject{
                    {"content", JsValue(JsArray{JsValue(JsObject{
                        {"type", JsValue("image")},
                        {"mimeType", JsValue("image/png")},
                        {"data", JsValue(Base64(png.data(), png.size()))},
                    })})},
                    {"isError", JsValue(false)},
                };
            },
        },
        {
            "open_document", "Open Document",
            "Open a USD file (.usd, .usda, .usdc, .usdz) in a new Vela window "
            "and make it current. Returns its document id.",
            R"({"type": "object", "properties": {
                "path": {"type": "string", "description": "An absolute file path"}
            }, "required": ["path"]})",
            false,
            [](const JsObject &arguments) {
                auto path = arguments.find("path");
                if (path == arguments.end() || !path->second.IsString()) {
                    return ToolResult("path must be a string", true);
                }
                std::string error = "Vela cannot open files now";
                const int id = Session::Get().open
                    ? Session::Get().open(path->second.GetString(), &error) : 0;
                if (!id) return ToolResult(error, true);
                Session::Get().SetFront(id);
                return JsonResult(JsValue(JsObject{{"document", JsValue(id)}}));
            },
        },
    };
    return tools;
}

JsObject Capabilities() { return JsObject{{"tools", JsValue(JsObject())}}; }

JsObject ServerInfo() {
    return JsObject{{"name", JsValue("vela")}, {"title", JsValue("Vela")},
                    {"version", JsValue("0.6")}};
}

JsArray SupportedVersions() {
    JsArray versions{JsValue(ModernVersion)};
    for (const char *version : LegacyVersions) versions.push_back(JsValue(version));
    return versions;
}

} // namespace

JsArray Tools() {
    JsArray tools;
    for (const Tool &tool : ToolTable()) {
        JsObject entry{
            {"name", JsValue(tool.name)},
            {"title", JsValue(tool.title)},
            {"description", JsValue(tool.description)},
            {"inputSchema", JsValue(Schema(tool.schema))},
        };
        if (tool.readOnly) {
            entry["annotations"] = JsValue(JsObject{{"readOnlyHint", JsValue(true)}});
        }
        tools.push_back(JsValue(entry));
    }
    return tools;
}

std::string Handle(const std::string &message) {
    const JsValue parsed = Parse(message);
    if (!parsed.IsObject()) {
        return ErrorReply(JsValue(), ParseError, "Parse error: expected a JSON-RPC object");
    }
    const JsObject &request = parsed.GetJsObject();
    auto methodIt = request.find("method");
    auto idIt = request.find("id");
    const bool hasId = idIt != request.end();
    const JsValue id = hasId ? idIt->second : JsValue();
    if (methodIt == request.end() || !methodIt->second.IsString()) {
        // A response to a request we never send, or garbage.
        return hasId && !request.count("result") && !request.count("error")
            ? ErrorReply(id, InvalidRequest, "Invalid request: no method") : std::string();
    }
    const std::string &method = methodIt->second.GetString();
    if (!hasId) return std::string();  // notifications need no reply

    JsObject params;
    if (auto p = request.find("params"); p != request.end() && p->second.IsObject()) {
        params = p->second.GetJsObject();
    }
    // Modern requests name their version in _meta; results say they are
    // complete.
    bool modern = false;
    if (auto meta = params.find("_meta"); meta != params.end() && meta->second.IsObject()) {
        const JsObject &fields = meta->second.GetJsObject();
        auto version = fields.find("io.modelcontextprotocol/protocolVersion");
        if (version != fields.end()) {
            if (!version->second.IsString() || version->second.GetString() != ModernVersion) {
                return ErrorReply(id, UnsupportedVersion, "Unsupported protocol version",
                    JsValue(JsObject{{"supported", JsValue(SupportedVersions())},
                                     {"requested", version->second}}));
            }
            modern = true;
        }
    }
    auto reply = [&](JsObject result) {
        if (modern) result["resultType"] = JsValue("complete");
        return Reply(id, std::move(result));
    };
    // Modern list and discovery results must say how long they keep: the
    // tools never change while the app runs, and are the same for everyone.
    auto cacheable = [&](JsObject result) {
        if (modern) {
            result["ttlMs"] = JsValue(3600000);
            result["cacheScope"] = JsValue("public");
        }
        return reply(std::move(result));
    };

    if (method == "initialize") {
        std::string version = LegacyVersions[0];
        if (auto v = params.find("protocolVersion"); v != params.end() && v->second.IsString()) {
            for (const char *known : LegacyVersions) {
                if (v->second.GetString() == known) version = known;
            }
        }
        return Reply(id, JsObject{
            {"protocolVersion", JsValue(version)},
            {"capabilities", JsValue(Capabilities())},
            {"serverInfo", JsValue(ServerInfo())},
            {"instructions", JsValue(Instructions)},
        });
    }
    if (method == "server/discover") {
        return cacheable(JsObject{
            {"supportedVersions", JsValue(SupportedVersions())},
            {"capabilities", JsValue(Capabilities())},
            {"_meta", JsValue(JsObject{
                {"io.modelcontextprotocol/serverInfo", JsValue(ServerInfo())}})},
            {"instructions", JsValue(Instructions)},
        });
    }
    if (method == "ping") return reply(JsObject());
    if (method == "tools/list") return cacheable(JsObject{{"tools", JsValue(Tools())}});
    if (method == "tools/call") {
        auto name = params.find("name");
        if (name == params.end() || !name->second.IsString()) {
            return ErrorReply(id, InvalidParams, "tools/call needs a tool name");
        }
        JsObject arguments;
        if (auto a = params.find("arguments"); a != params.end() && a->second.IsObject()) {
            arguments = a->second.GetJsObject();
        }
        for (const Tool &tool : ToolTable()) {
            if (tool.name != name->second.GetString()) continue;
            // The Log tab shows what clients do, and what failed.
            std::string summary = tool.name;
            if (auto code = arguments.find("code"); code != arguments.end() && code->second.IsString()) {
                summary += ":\n" + code->second.GetString();
            } else if (!arguments.empty()) {
                summary += " " + Compact(JsValue(arguments));
            }
            Log::Get().Add(LogEntry::Level::Info, "MCP", summary);
            JsObject result = tool.call(arguments);
            if (result["isError"].IsBool() && result["isError"].GetBool()) {
                const JsArray &content = result["content"].GetJsArray();
                const std::string text = !content.empty() && content[0].IsObject() &&
                    content[0].GetJsObject().count("text")
                    ? content[0].GetJsObject().at("text").GetString() : "failed";
                Log::Get().Add(LogEntry::Level::Warning, "MCP", tool.name + " failed: " + text);
            }
            return reply(std::move(result));
        }
        return ErrorReply(id, InvalidParams, "Unknown tool: " + name->second.GetString());
    }
    return ErrorReply(id, MethodNotFound, "Method not found: " + method);
}

std::string Compact(const JsValue &value) {
    std::ostringstream stream;
    JsWriter writer(stream, JsWriter::Style::Compact);
    JsWriteValue(&writer, value);
    return stream.str();
}

std::string Base64(const unsigned char *data, size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned n = unsigned(data[i]) << 16 |
            (i + 1 < size ? unsigned(data[i + 1]) << 8 : 0u) |
            (i + 2 < size ? unsigned(data[i + 2]) : 0u);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += i + 1 < size ? alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < size ? alphabet[n & 63] : '=';
    }
    return out;
}

} // namespace mcp
} // namespace vela
