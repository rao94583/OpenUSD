#include "mcp_socket.h"
#include "mcp.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace vela {

// Shared with the threads, which may outlive the server object.
struct McpSocketServer::State {
    std::mutex mutex;
    int listenFd = -1;
    int wakePipe[2] = {-1, -1};
    std::set<int> clients;
    bool stopped = false;
};

namespace {

bool MakeAddress(const std::string &path, sockaddr_un *address) {
    std::memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (path.size() >= sizeof(address->sun_path)) return false;
    std::memcpy(address->sun_path, path.c_str(), path.size() + 1);
    return true;
}

// Creates each missing folder of path's parent, the last one private.
bool MakeParent(const std::string &path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return true;
    const std::string parent = path.substr(0, slash);
    struct stat info;
    if (stat(parent.c_str(), &info) == 0) return S_ISDIR(info.st_mode);
    if (!MakeParent(parent)) return false;
    return mkdir(parent.c_str(), 0700) == 0 || errno == EEXIST;
}

bool WriteAll(int fd, const std::string &data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = write(fd, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += size_t(n);
    }
    return true;
}

void Serve(std::shared_ptr<McpSocketServer::State> state, int fd,
           McpSocketServer::Dispatch dispatch) {
    std::string buffer;
    char chunk[65536];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buffer.append(chunk, size_t(n));
        size_t newline;
        bool ok = true;
        while (ok && (newline = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            std::string response;
            dispatch([&] { response = mcp::Handle(line); });
            if (!response.empty()) ok = WriteAll(fd, response + "\n");
        }
        if (!ok) break;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->clients.erase(fd)) close(fd);
}

void Listen(std::shared_ptr<McpSocketServer::State> state, McpSocketServer::Dispatch dispatch) {
    const int listenFd = state->listenFd, wakeFd = state->wakePipe[0];
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stopped) return;
        }
        pollfd fds[2] = {{listenFd, POLLIN, 0}, {wakeFd, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents) return;
        if (!(fds[0].revents & POLLIN)) continue;
        const int fd = accept(listenFd, nullptr, nullptr);
        if (fd < 0) continue;
        // Only this user's processes; the folder's mode already says so.
        uid_t uid;
        gid_t gid;
        if (getpeereid(fd, &uid, &gid) != 0 || uid != geteuid()) {
            close(fd);
            continue;
        }
        const int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stopped) {
                close(fd);
                return;
            }
            state->clients.insert(fd);
        }
        std::thread(Serve, state, fd, dispatch).detach();
    }
}

// Accepts connections until stopped, then closes the listening socket.
void Accept(std::shared_ptr<McpSocketServer::State> state, McpSocketServer::Dispatch dispatch) {
    Listen(state, std::move(dispatch));
    close(state->listenFd);
    close(state->wakePipe[0]);
    close(state->wakePipe[1]);
}

} // namespace

McpSocketServer::McpSocketServer(std::string path, Dispatch dispatch)
    : _path(std::move(path)), _dispatch(std::move(dispatch)) {}

McpSocketServer::~McpSocketServer() { Stop(); }

bool McpSocketServer::IsListening() const { return _state && !_state->stopped; }

bool McpSocketServer::Start(std::string *error) {
    if (IsListening()) return true;
    auto fail = [&](const std::string &message, int fd = -1) {
        if (error) *error = message;
        if (fd >= 0) close(fd);
        return false;
    };
    sockaddr_un address;
    if (!MakeAddress(_path, &address)) return fail("The socket path is too long: " + _path);
    if (!MakeParent(_path)) return fail("Could not create the folder for " + _path);

    // A socket someone answers on belongs to another Vela; otherwise it is
    // left over from one that quit.
    struct stat info;
    if (lstat(_path.c_str(), &info) == 0) {
        if (!S_ISSOCK(info.st_mode)) return fail(_path + " exists and is not a socket");
        const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe >= 0 && connect(probe, (sockaddr *)&address, sizeof(address)) == 0) {
            return fail("Another Vela is already serving MCP at " + _path, probe);
        }
        if (probe >= 0) close(probe);
        unlink(_path.c_str());
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return fail(std::string("socket: ") + strerror(errno));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    const mode_t mask = umask(0177);
    const int bound = bind(fd, (sockaddr *)&address, sizeof(address));
    umask(mask);
    if (bound != 0) return fail(std::string("bind: ") + strerror(errno), fd);
    chmod(_path.c_str(), 0600);
    if (listen(fd, 8) != 0) {
        unlink(_path.c_str());
        return fail(std::string("listen: ") + strerror(errno), fd);
    }
    auto state = std::make_shared<State>();
    state->listenFd = fd;
    if (pipe(state->wakePipe) != 0) {
        unlink(_path.c_str());
        return fail(std::string("pipe: ") + strerror(errno), fd);
    }
    _state = state;
    std::thread(Accept, state, _dispatch).detach();
    return true;
}

void McpSocketServer::Stop() {
    if (!_state) return;
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (_state->stopped) return;
    _state->stopped = true;
    // Wake the accept thread, and end connections (their threads close
    // them).
    (void)!write(_state->wakePipe[1], "x", 1);
    for (int fd : _state->clients) shutdown(fd, SHUT_RDWR);
    // The accept thread closes the listening socket.
    unlink(_path.c_str());
}

} // namespace vela
