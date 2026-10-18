// A child process the app talks to in lines: the claude CLI for the Claude
// panel. Its stdin takes lines; a reader thread hands each stdout line to
// a callback, and reports the exit with the end of what it wrote to
// stderr.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vela {

class LineProcess {
public:
    struct Callbacks {
        // Called on the reader thread, in order.
        std::function<void(const std::string &line)> line;
        // Called once, on the reader thread, after the last line: the exit
        // status (as waitpid reports it, or -1) and stderr's last 4 KB.
        std::function<void(int status, const std::string &errors)> exited;
    };

    LineProcess() = default;
    // Ends the process (see Terminate) without calling exited.
    ~LineProcess();
    LineProcess(const LineProcess &) = delete;
    LineProcess &operator=(const LineProcess &) = delete;

    // Runs args[0] (an absolute path) in directory with the app's
    // environment plus extra ("NAME=value" entries replace the app's).
    bool Start(const std::vector<std::string> &args, const std::string &directory,
               const std::vector<std::string> &extra, Callbacks callbacks, std::string *error);
    bool IsRunning() const;
    // Writes line and a newline; false once the process has gone.
    bool Write(const std::string &line);
    // Closes stdin, and kills the process if it hasn't exited after a
    // moment. Callbacks are no longer called.
    void Terminate();

    struct State;

private:
    std::shared_ptr<State> _state;
};

// Runs a command to completion and returns its stdout (for `claude
// --version` and finding the CLI); false if it failed to run or exited
// with an error.
bool RunAndCapture(const std::vector<std::string> &args, std::string *output,
                   int timeoutSeconds = 10);

} // namespace vela
