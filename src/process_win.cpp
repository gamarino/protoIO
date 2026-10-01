// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Child processes and the process environment on Windows: the contracts of
// process.cpp on CreateProcessW.
//
//   * argv is joined into one command line with the quoting the Microsoft C
//     runtime (CommandLineToArgvW) undoes, so a child built with it sees the
//     same argv. argv[0] is searched as CreateProcess searches it (the
//     application's directory, the working directory, the system
//     directories, then PATH; ".exe" is appended when there is no
//     extension). A batch file needs an explicit "cmd /c".
//   * A child inherits exactly the handles it is given (a handle list), so
//     concurrent runs never hold each other's pipes open.
//   * kill accepts 0 (does the process exist?), SIGTERM (15) and SIGKILL (9);
//     the last two end the process with TerminateProcess and exit code
//     128 + signal, so wait and run answer what they answer on POSIX for a
//     child a signal ended. Any other signal throws Process (ENOSYS): Windows
//     has no way to deliver it.
#include "protoio/process.h"

#include "protoio/error.h"
#include "protoio/stream.h"
#include "internal.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <process.h>

namespace protoio::process {

namespace win = detail::win;

namespace {

[[noreturn]] void processError(const std::string& msg, int err) {
    throw Error(Error::Kind::Process, msg + ": " + detail::errorText(err), err);
}

[[noreturn]] void lastError(const std::string& msg) { processError(msg, win::errnoOfWin32(::GetLastError())); }

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        h = nullptr;
    }
};

// One argument quoted so that CommandLineToArgvW (and the C runtime's argv
// parser) give it back unchanged: backslashes are literal except before a
// double quote, where 2n (+1) backslashes stand for n (and a quote).
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
        } else {
            cmd.append(backslashes, L'\\');
        }
        cmd += *it;
    }
    cmd += L'"';
}

std::wstring commandLine(const std::vector<std::string>& argv, const char* who) {
    if (argv.empty()) throw Error(Error::Kind::InvalidArgument, std::string(who) + ": needs a command");
    std::wstring cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += L' ';
        appendQuoted(cmd, win::widen(argv[i]));
    }
    return cmd;
}

// Starts a child whose standard handles are `in`, `out` and `err` (which must
// be inheritable) and which inherits nothing else.
PROCESS_INFORMATION start(const std::vector<std::string>& argv, const char* who, HANDLE in, HANDLE out, HANDLE err) {
    std::wstring cmd = commandLine(argv, who);
    HANDLE list[3];
    DWORD n = 0;
    for (HANDLE h : {in, out, err}) {
        if (!h) continue;
        bool seen = false;
        for (DWORD i = 0; i < n; ++i) seen = seen || list[i] == h;
        if (!seen) list[n++] = h;
    }
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attrBuf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &size)) lastError("cannot run " + argv[0]);
    if (n > 0 && !::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, n * sizeof(HANDLE),
                                              nullptr, nullptr)) {
        const DWORD e = ::GetLastError();
        ::DeleteProcThreadAttributeList(attrs);
        processError("cannot run " + argv[0], win::errnoOfWin32(e));
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, n > 0, EXTENDED_STARTUPINFO_PRESENT,
                                     nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD e = ::GetLastError();
    ::DeleteProcThreadAttributeList(attrs);
    if (!ok) processError("cannot run " + argv[0], win::errnoOfWin32(e));
    ::CloseHandle(pi.hThread);
    return pi;
}

// An inheritable pipe: `child` is the end the child gets.
void makePipe(Handle& parent, Handle& child, bool childReads) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE r, w;
    if (!::CreatePipe(&r, &w, &sa, 0)) lastError("cannot create pipes");
    parent.h = childReads ? w : r;
    child.h = childReads ? r : w;
    ::SetHandleInformation(parent.h, HANDLE_FLAG_INHERIT, 0);
}

int exitCodeOf(HANDLE process) {
    DWORD code = 0;
    if (!::GetExitCodeProcess(process, &code)) return -1;
    return static_cast<int>(code);
}

void drain(HANDLE h, std::string& dst) {
    char chunk[65536];
    DWORD k = 0;
    while (::ReadFile(h, chunk, sizeof chunk, &k, nullptr) && k > 0) dst.append(chunk, k);
}

// Children started with spawn, by pid, until wait reaps them. The open
// handle keeps the pid from being reused meanwhile.
std::mutex g_childMutex;
std::unordered_map<int, HANDLE> g_children;

} // namespace

RunResult run(const std::vector<std::string>& argv, const std::optional<std::string>& input) {
    if (argv.empty()) throw Error(Error::Kind::InvalidArgument, "run: needs a command");
    Handle inW, inR, outR, outW, errR, errW;
    makePipe(inW, inR, true);
    makePipe(outR, outW, false);
    makePipe(errR, errW, false);
    PROCESS_INFORMATION pi = start(argv, "run", inR.h, outW.h, errW.h);
    Handle proc(pi.hProcess);
    inR.reset();
    outW.reset();
    errW.reset();

    RunResult res;
    // Anonymous pipes cannot be polled: the input is fed and the standard
    // error collected on threads of their own while this one reads the
    // output. A child that exits without reading its input makes the write
    // fail (ERROR_NO_DATA), which just ends the feeding.
    std::thread feeder;
    const std::string empty;
    const std::string& in = input ? *input : empty;
    if (in.empty()) {
        inW.reset();
    } else {
        feeder = std::thread([&] {
            std::size_t done = 0;
            while (done < in.size()) {
                DWORD w = 0;
                const DWORD want = static_cast<DWORD>(std::min<std::size_t>(in.size() - done, 1 << 20));
                if (!::WriteFile(inW.h, in.data() + done, want, &w, nullptr)) break;
                done += w;
            }
            inW.reset();
        });
    }
    std::thread errReader([&] { drain(errR.h, res.err); });
    drain(outR.h, res.out);
    errReader.join();
    if (feeder.joinable()) feeder.join();
    ::WaitForSingleObject(proc.h, INFINITE);
    res.exitCode = exitCodeOf(proc.h);
    return res;
}

int spawn(const std::vector<std::string>& argv) {
    if (argv.empty()) throw Error(Error::Kind::InvalidArgument, "spawn: needs a command");
    std::fflush(nullptr);
    // The child shares the caller's standard streams: inheritable duplicates
    // of them (they may not be inheritable themselves).
    Handle std3[3];
    const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; ++i) {
        const HANDLE h = ::GetStdHandle(ids[i]);
        if (!h || h == INVALID_HANDLE_VALUE) continue;
        HANDLE dup = nullptr;
        if (::DuplicateHandle(::GetCurrentProcess(), h, ::GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS))
            std3[i].h = dup;
    }
    PROCESS_INFORMATION pi = start(argv, "spawn", std3[0].h, std3[1].h, std3[2].h);
    const int id = static_cast<int>(pi.dwProcessId);
    std::lock_guard<std::mutex> lock(g_childMutex);
    g_children[id] = pi.hProcess;
    return id;
}

int wait(int pid) {
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(pid);
        if (it != g_children.end()) {
            h = it->second;
            g_children.erase(it);
        }
    }
    if (!h) processError("cannot wait for process " + std::to_string(pid), ECHILD);
    Handle proc(h);
    ::WaitForSingleObject(h, INFINITE);
    return exitCodeOf(h);
}

void kill(int pid, int signal) {
    constexpr int kSigKill = 9;
    if (signal != 0 && signal != SIGTERM && signal != kSigKill)
        processError("cannot signal process " + std::to_string(pid) + " with signal " + std::to_string(signal) +
                         " (not supported on Windows)",
                     ENOSYS);
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(pid);
        if (it != g_children.end()) {
            if (!::DuplicateHandle(::GetCurrentProcess(), it->second, ::GetCurrentProcess(), &h, 0, FALSE,
                                   DUPLICATE_SAME_ACCESS))
                h = nullptr;
        }
    }
    if (!h) h = ::OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        const DWORD e = ::GetLastError();
        // An unknown pid: ESRCH, as kill(2) answers.
        processError("cannot signal process " + std::to_string(pid),
                     e == ERROR_INVALID_PARAMETER ? ESRCH : win::errnoOfWin32(e));
    }
    Handle proc(h);
    if (signal == 0) return;
    if (!::TerminateProcess(h, static_cast<UINT>(128 + signal))) {
        const DWORD e = ::GetLastError();
        // Already exiting: nothing left to do.
        if (e == ERROR_ACCESS_DENIED && ::WaitForSingleObject(h, 0) == WAIT_OBJECT_0) return;
        processError("cannot signal process " + std::to_string(pid), win::errnoOfWin32(e));
    }
}

std::optional<std::string> getenv(const std::string& name) {
    const std::wstring w = win::widen(name);
    const DWORD n = ::GetEnvironmentVariableW(w.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring v(n, L'\0');
    const DWORD got = ::GetEnvironmentVariableW(w.c_str(), v.data(), n);
    v.resize(got);
    return win::narrow(v);
}

void setenv(const std::string& name, const std::optional<std::string>& value) {
    if (name.empty() || name.find('=') != std::string::npos)
        throw Error(Error::Kind::InvalidArgument, "invalid environment variable name: " + name, EINVAL);
    // _wputenv_s changes both the C runtime's copy and the process's.
    const errno_t rc = ::_wputenv_s(win::widen(name).c_str(), value ? win::widen(*value).c_str() : L"");
    if (rc != 0) throw Error(Error::Kind::InvalidArgument, "invalid environment variable name: " + name, rc);
}

std::vector<std::pair<std::string, std::string>> environment() {
    std::vector<std::pair<std::string, std::string>> out;
    wchar_t* block = ::GetEnvironmentStringsW();
    if (!block) return out;
    for (const wchar_t* e = block; *e; e += std::wcslen(e) + 1) {
        const std::wstring entry(e);
        // Entries such as "=C:=C:\dir" are the shell's per-drive directories,
        // not variables.
        if (entry[0] == L'=') continue;
        const std::size_t eq = entry.find(L'=');
        if (eq == std::wstring::npos) out.emplace_back(win::narrow(entry), std::string());
        else out.emplace_back(win::narrow(entry.substr(0, eq)), win::narrow(entry.substr(eq + 1)));
    }
    ::FreeEnvironmentStringsW(block);
    return out;
}

int pid() { return static_cast<int>(::GetCurrentProcessId()); }

std::string hostName() {
    wchar_t buf[256] = {0};
    DWORD n = 255;
    if (!::GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) return std::string();
    return win::narrow(std::wstring(buf, n));
}

std::string platform() { return "windows"; }

void exit(int code) {
    std::fflush(nullptr);
    ::_exit(code & 0xff);
}

} // namespace protoio::process
