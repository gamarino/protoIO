// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Child processes and the process environment. Moved from protoST's
// src/primitives/io_prims.cpp.
#include "protoio/process.h"

#include "protoio/error.h"
#include "protoio/stream.h"
#include "internal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace protoio::process {

namespace {

[[noreturn]] void processError(const std::string& msg, int err) {
    throw Error(Error::Kind::Process, msg + ": " + std::strerror(err), err);
}

int exitCodeOf(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

std::vector<char*> argvOf(const std::vector<std::string>& argv, const char* who) {
    if (argv.empty()) throw Error(Error::Kind::InvalidArgument, std::string(who) + ": needs a command");
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& s : argv) args.push_back(const_cast<char*>(s.c_str()));
    args.push_back(nullptr);
    return args;
}

} // namespace

RunResult run(const std::vector<std::string>& argv, const std::optional<std::string>& input) {
    std::vector<char*> args = argvOf(argv, "run");
    int inP[2] = {-1, -1}, outP[2] = {-1, -1}, errP[2] = {-1, -1};
    if (detail::newPipe(inP) || detail::newPipe(outP) || detail::newPipe(errP)) {
        const int e = errno;
        for (int f : {inP[0], inP[1], outP[0], outP[1], errP[0], errP[1]}) if (f >= 0) ::close(f);
        processError("cannot create pipes", e);
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inP[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outP[1], 1);
    posix_spawn_file_actions_adddup2(&fa, errP[1], 2);
    pid_t pid;
    const int rc = ::posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(inP[0]);
    ::close(outP[1]);
    ::close(errP[1]);
    if (rc != 0) {
        ::close(inP[1]);
        ::close(outP[0]);
        ::close(errP[0]);
        processError("cannot run " + argv[0], rc);
    }
    RunResult res;
    // A child that exits without reading all its input must not kill us.
    SigpipeGuard noSigpipe;
    std::size_t written = 0;
    const std::string empty;
    const std::string& in = input ? *input : empty;
    if (in.empty()) { ::close(inP[1]); inP[1] = -1; }
    else ::fcntl(inP[1], F_SETFL, O_NONBLOCK);
    int outFd = outP[0], errFd = errP[0];
    char chunk[65536];
    while (outFd >= 0 || errFd >= 0 || inP[1] >= 0) {
        pollfd p[3];
        int n = 0, iOut = -1, iErr = -1, iIn = -1;
        if (outFd >= 0) { iOut = n; p[n++] = {outFd, POLLIN, 0}; }
        if (errFd >= 0) { iErr = n; p[n++] = {errFd, POLLIN, 0}; }
        if (inP[1] >= 0) { iIn = n; p[n++] = {inP[1], POLLOUT, 0}; }
        if (::poll(p, static_cast<nfds_t>(n), -1) < 0) {
            if (errno == EINTR) continue;
            // Cannot wait any more: stop feeding and collecting, but still
            // reap the child below.
            for (int* f : {&outFd, &errFd, &inP[1]}) if (*f >= 0) { ::close(*f); *f = -1; }
            break;
        }
        auto drain = [&](int idx, int& fd, std::string& dst) {
            if (idx < 0 || !(p[idx].revents & (POLLIN | POLLHUP | POLLERR))) return;
            const ssize_t k = ::read(fd, chunk, sizeof chunk);
            if (k > 0) dst.append(chunk, static_cast<std::size_t>(k));
            else if (k == 0 || errno != EINTR) { ::close(fd); fd = -1; }
        };
        drain(iOut, outFd, res.out);
        drain(iErr, errFd, res.err);
        if (iIn >= 0 && (p[iIn].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const ssize_t w = ::write(inP[1], in.data() + written, in.size() - written);
            if (w > 0) written += static_cast<std::size_t>(w);
            // EPIPE (the child stopped reading) or another failure: stop feeding.
            if (w < 0 && errno != EAGAIN && errno != EINTR) written = in.size();
            if (written >= in.size()) { ::close(inP[1]); inP[1] = -1; }
        }
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    res.exitCode = exitCodeOf(status);
    return res;
}

RunResult shell(const std::string& command, const std::optional<std::string>& input) {
    if (command.empty()) throw Error(Error::Kind::InvalidArgument, "shell: needs a command");
    return run({"/bin/sh", "-c", command}, input);
}

int spawn(const std::vector<std::string>& argv) {
    std::vector<char*> args = argvOf(argv, "spawn");
    pid_t pid;
    std::fflush(nullptr);
    const int rc = ::posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ);
    if (rc != 0) processError("cannot run " + argv[0], rc);
    return pid;
}

int wait(int pid) {
    int status = 0, rc;
    while ((rc = ::waitpid(static_cast<pid_t>(pid), &status, 0)) < 0 && errno == EINTR) {}
    if (rc < 0) processError("cannot wait for process " + std::to_string(pid), errno);
    return exitCodeOf(status);
}

void kill(int pid, int signal) {
    if (::kill(static_cast<pid_t>(pid), signal) != 0)
        processError("cannot signal process " + std::to_string(pid), errno);
}

std::optional<std::string> getenv(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
}

void setenv(const std::string& name, const std::optional<std::string>& value) {
    const int rc = value ? ::setenv(name.c_str(), value->c_str(), 1) : ::unsetenv(name.c_str());
    if (rc != 0) throw Error(Error::Kind::InvalidArgument, "invalid environment variable name: " + name, errno);
}

std::vector<std::pair<std::string, std::string>> environment() {
    std::vector<std::pair<std::string, std::string>> out;
    for (char** e = environ; e && *e; ++e) {
        const std::string entry(*e);
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) out.emplace_back(entry, std::string());
        else out.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
    }
    return out;
}

int pid() { return static_cast<int>(::getpid()); }

std::string hostName() {
    char buf[256] = {0};
    ::gethostname(buf, sizeof buf - 1);
    return buf;
}

std::string platform() {
#if defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unix";
#endif
}

void exit(int code) {
    std::fflush(nullptr);
    ::_exit(code & 0xff);
}

} // namespace protoio::process
