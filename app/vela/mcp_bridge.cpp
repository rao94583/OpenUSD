// vela-mcp: an MCP server on stdio for Claude Code and other clients, which
// forwards each message to the running Vela over its Unix socket and each
// reply back. Vela implements the protocol; this only moves lines. If Vela
// isn't running, the first message launches it (from the bundle this
// helper is in) and waits for it to listen.
//
//   claude mcp add vela -- /Applications/Vela.app/Contents/MacOS/vela-mcp
//
// VELA_MCP_SOCKET names another socket (tests); then nothing is launched.

#include "app/vela/core/mcp_path.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <mach-o/dyld.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

void Log(const std::string &message) {
    fprintf(stderr, "vela-mcp: %s\n", message.c_str());
}

int Connect(const std::string &path) {
    sockaddr_un address;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return -1;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    return fd;
}

// The .app this helper is in (…/Vela.app/Contents/MacOS/vela-mcp).
std::string BundlePath() {
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) return std::string();
    char resolved[PATH_MAX];
    if (!realpath(buffer, resolved)) return std::string();
    std::string path = resolved;
    for (int i = 0; i < 3; ++i) {
        const size_t slash = path.rfind('/');
        if (slash == std::string::npos) return std::string();
        path.resize(slash);
    }
    return path.size() > 4 && path.compare(path.size() - 4, 4, ".app") == 0 ? path : std::string();
}

// Opens Vela in the background, as Finder would.
bool Launch() {
    const std::string bundle = BundlePath();
    if (bundle.empty()) {
        Log("not inside Vela.app, so cannot launch Vela");
        return false;
    }
    Log("launching " + bundle);
    const char *argv[] = {"/usr/bin/open", "-g", bundle.c_str(), nullptr};
    pid_t pid;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, const_cast<char **>(argv), environ) != 0) {
        return false;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Connects, launching Vela if needed (and allowed) and waiting for it.
int Reach(const std::string &path, bool mayLaunch) {
    int fd = Connect(path);
    if (fd >= 0 || !mayLaunch || !Launch()) return fd;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        fd = Connect(path);
    }
    return fd;
}

bool WriteAll(int fd, const char *data, size_t size) {
    while (size) {
        const ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= size_t(n);
    }
    return true;
}

} // namespace

int main() {
    const bool customSocket = getenv("VELA_MCP_SOCKET") && *getenv("VELA_MCP_SOCKET");
    const std::string path = vela::DefaultMcpSocketPath();
    int app = -1;
    std::string input;  // stdin's unfinished line
    bool inputOpen = true;
    char chunk[65536];
    for (;;) {
        pollfd fds[2] = {{inputOpen ? STDIN_FILENO : -1, POLLIN, 0}, {app, POLLIN, 0}};
        if (!inputOpen && app < 0) return 0;
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (app >= 0 && fds[1].revents) {
            const ssize_t n = read(app, chunk, sizeof(chunk));
            if (n > 0) {
                if (!WriteAll(STDOUT_FILENO, chunk, size_t(n))) return 0;
            } else if (n == 0 || errno != EINTR) {
                // Vela quit: reconnect for the next message.
                close(app);
                app = -1;
                if (!inputOpen) return 0;
                Log("Vela closed the connection");
            }
        }
        if (inputOpen && fds[0].revents) {
            const ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                // The client is done: let Vela finish replying.
                inputOpen = false;
                if (app >= 0) shutdown(app, SHUT_WR);
                continue;
            }
            input.append(chunk, size_t(n));
            // Whole lines only, so a reconnection never splits a message.
            const size_t end = input.rfind('\n');
            if (end == std::string::npos) continue;
            if (app < 0) {
                app = Reach(path, !customSocket);
                if (app < 0) {
                    Log("could not reach Vela at " + path +
                        (customSocket ? "" : "; is it running, with MCP turned on in Settings?"));
                    return 1;
                }
            }
            if (!WriteAll(app, input.data(), end + 1)) {
                Log("lost the connection to Vela");
                return 1;
            }
            input.erase(0, end + 1);
        }
    }
}
