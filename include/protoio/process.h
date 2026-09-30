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
    // child (the shell's convention).
    int exitCode = 0;
    std::string out;  // everything the child wrote to its standard output
    std::string err;  // everything the child wrote to its standard error
};

// Runs argv[0] (searched in PATH) with the arguments that follow, feeds it
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

// Sends `signal` to a process.
void kill(int pid, int signal);

// The environment. getenv answers std::nullopt when the variable is unset;
// setenv with std::nullopt unsets it.
std::optional<std::string> getenv(const std::string& name);
void setenv(const std::string& name, const std::optional<std::string>& value);
std::vector<std::pair<std::string, std::string>> environment();

int pid();
std::string hostName();
// "linux", "macos" or "unix".
std::string platform();

// Flushes the C stdio streams and ends the process at once with
// `code & 0xff`, without running destructors or atexit handlers.
[[noreturn]] void exit(int code);

} // namespace protoio::process
