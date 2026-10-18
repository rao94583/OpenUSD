#include "claude_process.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace vela {

struct LineProcess::State {
    std::mutex mutex;
    pid_t pid = 0;
    int input = -1;
    bool exited = false;
    bool detached = false;  // Terminate was called: no more callbacks
    Callbacks callbacks;
};

namespace {

// The app's environment with extra's entries added or replacing.
std::vector<std::string> Environment(const std::vector<std::string> &extra) {
    std::map<std::string, std::string> entries;
    for (char **e = environ; *e; ++e) {
        const std::string entry = *e;
        entries[entry.substr(0, entry.find('='))] = entry;
    }
    for (const std::string &entry : extra) entries[entry.substr(0, entry.find('='))] = entry;
    std::vector<std::string> result;
    for (auto &[name, entry] : entries) result.push_back(entry);
    return result;
}

std::vector<char *> Pointers(std::vector<std::string> &strings) {
    std::vector<char *> pointers;
    for (std::string &s : strings) pointers.push_back(s.data());
    pointers.push_back(nullptr);
    return pointers;
}

// Spawns with stdin, stdout and stderr on pipes; returns the parent's ends.
bool Spawn(const std::vector<std::string> &args, const std::string &directory,
           const std::vector<std::string> &extra, pid_t *pid, int *in, int *out, int *err,
           std::string *error) {
    int toChild[2], fromChild[2], errors[2];
    if (pipe(toChild) != 0) {
        *error = std::string("pipe: ") + strerror(errno);
        return false;
    }
    if (pipe(fromChild) != 0 || pipe(errors) != 0) {
        *error = std::string("pipe: ") + strerror(errno);
        close(toChild[0]);
        close(toChild[1]);
        return false;
    }
    for (int fd : {toChild[1], fromChild[0], errors[0]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, toChild[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fromChild[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
    if (!directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Its own process group, so terminating reaches what it starts, and
    // the app's file descriptors stay closed.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawnattr_setpgroup(&attributes, 0);
    std::vector<std::string> argv = args, env = Environment(extra);
    std::vector<char *> argvp = Pointers(argv), envp = Pointers(env);
    const int result = posix_spawn(pid, argv[0].c_str(), &actions, &attributes, argvp.data(),
                                   envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(toChild[0]);
    close(fromChild[1]);
    close(errors[1]);
    if (result != 0) {
        *error = "Could not run " + args[0] + ": " + strerror(result);
        close(toChild[1]);
        close(fromChild[0]);
        close(errors[0]);
        return false;
    }
    *in = toChild[1];
    *out = fromChild[0];
    *err = errors[0];
    return true;
}

// Reads stdout into lines and stderr into a tail, until both close.
void Read(std::shared_ptr<LineProcess::State> state, int out, int err) {
    std::string buffer, errors;
    char chunk[65536];
    bool outOpen = true, errOpen = true;
    while (outOpen || errOpen) {
        pollfd fds[2] = {{outOpen ? out : -1, POLLIN, 0}, {errOpen ? err : -1, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (outOpen && fds[0].revents) {
            const ssize_t n = read(out, chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                outOpen = false;
            } else {
                buffer.append(chunk, size_t(n));
                size_t newline;
                while ((newline = buffer.find('\n')) != std::string::npos) {
                    const std::string line = buffer.substr(0, newline);
                    buffer.erase(0, newline + 1);
                    std::function<void(const std::string &)> callback;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        if (!state->detached) callback = state->callbacks.line;
                    }
                    if (callback && !line.empty()) callback(line);
                }
            }
        }
        if (errOpen && fds[1].revents) {
            const ssize_t n = read(err, chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                errOpen = false;
            } else {
                errors.append(chunk, size_t(n));
                if (errors.size() > 4096) errors.erase(0, errors.size() - 4096);
            }
        }
    }
    close(out);
    close(err);
    pid_t pid;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        pid = state->pid;
    }
    int status = -1;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    std::function<void(int, const std::string &)> callback;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->exited = true;
        if (state->input >= 0) {
            close(state->input);
            state->input = -1;
        }
        if (!state->detached) callback = state->callbacks.exited;
    }
    if (callback) callback(status, errors);
}

} // namespace

LineProcess::~LineProcess() { Terminate(); }

bool LineProcess::Start(const std::vector<std::string> &args, const std::string &directory,
                        const std::vector<std::string> &extra, Callbacks callbacks,
                        std::string *error) {
    Terminate();
    if (args.empty()) {
        *error = "No command to run";
        return false;
    }
    pid_t pid;
    int in, out, err;
    if (!Spawn(args, directory, extra, &pid, &in, &out, &err, error)) return false;
    auto state = std::make_shared<State>();
    state->pid = pid;
    state->input = in;
    state->callbacks = std::move(callbacks);
    _state = state;
    std::thread(Read, state, out, err).detach();
    return true;
}

bool LineProcess::IsRunning() const {
    if (!_state) return false;
    std::lock_guard<std::mutex> lock(_state->mutex);
    return !_state->exited;
}

bool LineProcess::Write(const std::string &line) {
    if (!_state) return false;
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (_state->exited || _state->input < 0) return false;
    const std::string data = line + "\n";
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = write(_state->input, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += size_t(n);
    }
    return true;
}

void LineProcess::Terminate() {
    std::shared_ptr<State> state = std::move(_state);
    if (!state) return;
    pid_t pid;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->detached = true;
        if (state->input >= 0) {
            close(state->input);
            state->input = -1;
        }
        if (state->exited) return;
        pid = state->pid;
    }
    // Closing stdin ends the CLI; if it lingers, end its process group.
    std::thread([state, pid] {
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->exited) return;
        }
        kill(-pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->exited) kill(-pid, SIGKILL);
    }).detach();
}

bool RunAndCapture(const std::vector<std::string> &args, std::string *output,
                   int timeoutSeconds) {
    pid_t pid;
    int in, out, err;
    std::string error;
    if (args.empty() || !Spawn(args, std::string(), {}, &pid, &in, &out, &err, &error)) {
        return false;
    }
    close(in);
    output->clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    char chunk[4096];
    bool outOpen = true, errOpen = true, timedOut = false;
    while (outOpen || errOpen) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        pollfd fds[2] = {{outOpen ? out : -1, POLLIN, 0}, {errOpen ? err : -1, POLLIN, 0}};
        const int ready = poll(fds, 2, int(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        for (int i = 0; i < 2; ++i) {
            if (!fds[i].revents) continue;
            const ssize_t n = read(fds[i].fd, chunk, sizeof(chunk));
            if (n > 0) {
                if (i == 0) output->append(chunk, size_t(n));
            } else if (n == 0 || errno != EINTR) {
                (i == 0 ? outOpen : errOpen) = false;
            }
        }
    }
    close(out);
    close(err);
    if (timedOut) kill(-pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return !timedOut && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace vela
