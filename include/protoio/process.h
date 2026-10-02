// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// Other programs and the running process. Failures to run, wait for or signal
// a child throw protoio::Error with kind Process.
namespace protoio::process {

struct RunResult {
    // The exit status, or 128 + the signal number when a signal ended the
    // child (the shell's convention). On Windows a child that ended with an
    // exception (an NTSTATUS code such as 0xC0000005) gets 128 + the POSIX
    // signal for the same fault, e.g. 139 for an access violation; see
    // README.md, "Windows".
    int exitCode = 0;
    std::string out;  // everything the child wrote to its standard output
    std::string err;  // everything the child wrote to its standard error
};

// Runs argv[0] (searched in PATH; on Windows also in the application's and
// the system directories, never in the working directory, and a .bat or
// .cmd target throws InvalidArgument) with the arguments that follow, feeds it
// `input` on its standard input (closed at once when there is none), and
// collects both outputs until it exits. A child that exits without reading
// all its input does not raise SIGPIPE in the caller. Blocks until the child
// exits.
RunResult run(const std::vector<std::string>& argv,
              const std::optional<std::string>& input = std::nullopt);

// Starts a child that shares the caller's standard streams; answers its pid.
int spawn(const std::vector<std::string>& argv);

// Waits for a child started with spawn; answers its exit code as run does.
int wait(int pid);

// Sends `signal` to a process. On Windows only 0 (does the process exist?),
// SIGTERM (15) and SIGKILL (9) are supported: the last two end the process
// with exit code 128 + signal; any other signal throws Process (ENOSYS).
void kill(int pid, int signal);

// The environment. getenv answers std::nullopt when the variable is unset;
// setenv with std::nullopt unsets it.
std::optional<std::string> getenv(const std::string& name);
void setenv(const std::string& name, const std::optional<std::string>& value);
std::vector<std::pair<std::string, std::string>> environment();

int pid();
std::string hostName();
// "linux", "macos", "windows" or "unix".
std::string platform();

// Flushes the C stdio streams and ends the process at once with
// `code & 0xff`, without running destructors or atexit handlers.
[[noreturn]] void exit(int code);

} // namespace protoio::process
