// Where the app listens for MCP connections, shared with the vela-mcp
// helper (which links nothing else of Vela's).
#pragma once

#include <cstdlib>
#include <string>

namespace vela {

// ~/Library/Application Support/Vela/mcp.sock, or VELA_MCP_SOCKET when set
// (for tests).
inline std::string DefaultMcpSocketPath() {
    if (const char *path = getenv("VELA_MCP_SOCKET"); path && *path) return path;
    const char *home = getenv("HOME");
    return std::string(home ? home : "") + "/Library/Application Support/Vela/mcp.sock";
}

} // namespace vela
