// The Claude panel's adapter for the claude CLI (Claude Code): everything
// that depends on the CLI's command line and its stream-JSON protocol is
// here, so a change in the CLI touches one place.
//
// The CLI runs as `claude -p --input-format stream-json --output-format
// stream-json ...` with a persistent session. The host writes an
// initialize control request and then user messages as lines on stdin; the
// CLI writes events as lines on stdout, asks for approval with can_use_tool
// control requests, and ends each turn with a result.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <functional>
#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

namespace claude {

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool operator<(const Version &other) const;
    std::string ToString() const;
};
// The oldest CLI the panel was checked against.
inline constexpr Version MinimumVersion{2, 1, 296};
// Reads `claude --version` output, e.g. "2.1.296 (Claude Code)".
bool ParseVersion(const std::string &text, Version *version);

// Where to look for the CLI when no path is set, in order: the native
// installer's, Homebrew's, npm's usual prefixes and the old local install.
std::vector<std::string> CandidatePaths(const std::string &home);

// The MCP server name the panel gives the CLI; its tools are
// mcp__vela__<tool>.
inline const char *const McpServer = "vela";
std::string McpToolName(const std::string &tool);

struct LaunchOptions {
    std::string executable;
    std::string sessionId;  // a new conversation's id (a UUID)
    std::string resume;     // or the conversation to continue
    std::string model;      // "" for the CLI's default
    std::string effort;     // "" for the CLI's default
    // Folders Claude may read besides its working folder (the scene's).
    std::vector<std::string> addDirectories;
    // vela-mcp, and the socket it should connect to.
    std::string mcpHelper;
    std::string mcpSocket;
    std::string systemPrompt;  // appended to Claude Code's
    // A Claude Code plugin folder with Vela's skills (see claude_skills.h).
    std::string pluginDirectory;
};
// The command line, executable first.
std::vector<std::string> Arguments(const LaunchOptions &options);

// The choices the panel and Settings offer, as (value, title); the first
// is the CLI's default ("").
struct Choice {
    const char *value;
    const char *title;
};
const std::vector<Choice> &ModelChoices();
const std::vector<Choice> &EffortChoices();

// Messages to the CLI, one line each (without the newline).
std::string InitializeRequest(const std::string &requestId);
std::string InterruptRequest(const std::string &requestId);
// A user message: text and PNG images (base64).
std::string UserMessage(const std::string &text, const std::vector<std::string> &pngBase64 = {});
// Answers to a can_use_tool request.
std::string AllowTool(const std::string &requestId, const JsObject &input);
std::string DenyTool(const std::string &requestId, const std::string &message);

// What the CLI reports, decoded from its output lines.
struct Event {
    enum class Kind {
        Init,         // sessionId, model
        TextDelta,    // text: streamed while Claude writes
        Text,         // text: a finished block of Claude's reply
        Thinking,     // Claude is thinking (no text shown)
        ToolUse,      // toolId, toolName, input
        ToolResult,   // toolId, text, images, isError
        Permission,   // requestId, toolName, input, text (the CLI's description)
        Result,       // the turn ended: isError, text, costUsd, sessionId
        Other,        // anything the panel doesn't show
    };
    Kind kind = Kind::Other;
    std::string sessionId;
    std::string model;
    std::string text;
    std::string toolId;
    std::string toolName;
    JsObject input;
    std::string requestId;
    std::vector<std::string> images;  // base64 PNG or JPEG
    bool isError = false;
    double costUsd = 0.0;
};
// One line can hold several events (a message with several blocks); a line
// that isn't JSON gives none.
std::vector<Event> Parse(const std::string &line);

// Approvals: what the panel does with a can_use_tool request.
enum class Decision { Allow, Ask };
// Tools that only read run freely: the app's read-only tools, ui_action
// with a query action or one that asks the user itself (isQuery says
// which), and reading files. Edits, and
// writing files in writableFolder (plugins), ask only when
// asksBeforeEdits; anything else (writing elsewhere) always asks.
Decision Decide(const std::string &toolName, const JsObject &input, bool asksBeforeEdits,
                const std::function<bool(const std::string &action)> &isQuery,
                const std::string &writableFolder = std::string());
// Whether path is inside folder: absolute, with no "..".
bool IsInside(const std::string &path, const std::string &folder);

// A short title for a tool call, e.g. "Run Python", and a one-line summary
// of its input, e.g. the code's first line or the action's name.
std::string ToolTitle(const std::string &toolName);
std::string ToolSummary(const std::string &toolName, const JsObject &input);

} // namespace claude
} // namespace vela
