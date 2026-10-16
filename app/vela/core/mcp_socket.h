// The app's end of the MCP connection: a Unix socket (mode 0600, in a
// 0700 folder) that the vela-mcp helper connects a client's stdio to, so no
// TCP port opens. Messages are lines of JSON, as on the stdio transport;
// each is handled by mcp::Handle on the main thread.
#pragma once

#include "mcp_path.h"

#include <functional>
#include <memory>
#include <string>

namespace vela {

class McpSocketServer {
public:
    // dispatch runs a function on the main thread and returns once it has
    // run.
    using Dispatch = std::function<void(const std::function<void()> &)>;

    McpSocketServer(std::string path, Dispatch dispatch);
    // Stops listening and removes the socket.
    ~McpSocketServer();
    McpSocketServer(const McpSocketServer &) = delete;
    McpSocketServer &operator=(const McpSocketServer &) = delete;

    // Listens on the path, replacing a stale socket. Fails if another
    // process is listening there.
    bool Start(std::string *error);
    void Stop();
    bool IsListening() const;
    const std::string &GetPath() const { return _path; }

    struct State;

private:
    std::string _path;
    Dispatch _dispatch;
    std::shared_ptr<State> _state;
};

} // namespace vela
