// Vela's MCP server: JSON-RPC messages from an MCP client (Claude Code,
// through the vela-mcp helper and the app's socket) to tools that query and
// edit the session's documents. Both protocol eras are served: the
// stateless one (2026-07-28: per-request _meta, server/discover) and the
// legacy initialize handshake (2025-11-25 and earlier).
//
// Tools: get_app_state, list_actions, ui_action, run_python,
// viewport_screenshot, open_document. Handle runs them on the calling
// thread, which must be the main thread.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <string>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

namespace mcp {

inline const char *const ModernVersion = "2026-07-28";

// Handles one message (a line of the stdio transport) and returns the
// response to send, or "" for notifications and responses.
std::string Handle(const std::string &message);

// The tool definitions tools/list returns.
JsArray Tools();

// JSON on one line, as the stdio transport needs (JsWriteToString
// pretty-prints).
std::string Compact(const JsValue &value);

// Standard base64, for image content.
std::string Base64(const unsigned char *data, size_t size);

} // namespace mcp

} // namespace vela
