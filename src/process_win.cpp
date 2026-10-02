// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Child processes and the process environment on Windows: the contracts of
// process.cpp on CreateProcessW.
//
//   * argv is joined into one command line with the quoting the Microsoft C
//     runtime (CommandLineToArgvW) undoes, so a child built with it sees the
//     same argv.
//   * argv[0] is resolved before CreateProcessW sees it (programPath): a
//     name with a directory is taken as it is; a bare name is searched in
//     the application's directory, the system directories and PATH -- never
//     in the working directory, where CreateProcess would look before the
//     system directories, so a planted cmd.exe or git.exe there never runs.
//     ".exe" is appended when the name has no extension.
//   * Batch files (.bat, .cmd) are refused with InvalidArgument: CreateProcess
//     runs them through cmd.exe, which parses the command line again by
//     rules no argument quoting can make safe (BatBadBut, CVE-2024-24576).
//   * shell runs the system directory's cmd.exe (never one found through
//     PATH, COMSPEC or the working directory) with the prebuilt command line
//     `cmd.exe /d /s /c "<command>"`, as CPython's subprocess does for
//     shell=True: /s makes cmd strip exactly the outer quotes, so the command
//     reaches it verbatim, and /d skips AutoRun. No argument quoting applies.
//   * A child inherits exactly the handles it is given (a handle list), so
//     concurrent runs never hold each other's pipes open. The handles are
//     created non-inheritable; inheritable duplicates exist only for the
//     length of the CreateProcessW call (see start).
//   * kill accepts 0 (does the process exist?), SIGTERM (15) and SIGKILL (9);
//     the last two end the process with TerminateProcess and exit code
//     128 + signal, so wait and run answer what they answer on POSIX for a
//     child a signal ended. Any other signal throws Process (ENOSYS): Windows
//     has no way to deliver it.
//   * A child that ends with an exception gets 128 + the POSIX signal for
//     the same fault (exitCodeOf).
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
#include <cwctype>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
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

// Calls `get(buffer, size)` (a Win32 call that answers the length it needs
// when the buffer is too small) until the answer fits; empty on failure.
template <typename Get>
std::wstring sized(Get get) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = get(buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::wstring();
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(n + 1);
    }
}

std::wstring fullPath(const std::wstring& path) {
    return sized([&](wchar_t* b, DWORD n) { return ::GetFullPathNameW(path.c_str(), n, b, nullptr); });
}

// The directories a bare program name is searched in, ';'-separated: the
// application's directory, the system directory, the Windows directory and
// PATH (each entry unquoted; empty entries skipped). Not the working
// directory.
std::wstring searchPath() {
    std::wstring dirs;
    auto add = [&](std::wstring d) {
        if (d.size() >= 2 && d.front() == L'"' && d.back() == L'"') d = d.substr(1, d.size() - 2);
        if (d.empty()) return;
        if (!dirs.empty()) dirs += L';';
        dirs += d;
    };
    std::wstring exe = sized([](wchar_t* b, DWORD n) {
        const DWORD r = ::GetModuleFileNameW(nullptr, b, n);
        return r == n ? n + 1 : r;  // a truncated answer has the buffer's size
    });
    const std::size_t slash = exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) add(exe.substr(0, slash));
    add(sized([](wchar_t* b, DWORD n) { return ::GetSystemDirectoryW(b, n); }));
    add(sized([](wchar_t* b, DWORD n) { return ::GetWindowsDirectoryW(b, n); }));
    const std::wstring path = sized([](wchar_t* b, DWORD n) { return ::GetEnvironmentVariableW(L"PATH", b, n); });
    for (std::size_t start = 0; start <= path.size();) {
        const std::size_t end = std::min(path.find(L';', start), path.size());
        add(path.substr(start, end - start));
        start = end + 1;
    }
    return dirs;
}

// The program CreateProcessW runs for argv[0], as a full path. Throws
// Process (ENOENT) when there is none, InvalidArgument for a batch file. A
// name with a relative directory is taken from `base` when there is one (the
// child's working directory), as exec takes it on POSIX.
std::wstring programPath(const std::string& argv0, const std::wstring& base = std::wstring()) {
    const std::wstring name = win::widen(argv0);
    const std::size_t sep = name.find_last_of(L"\\/:");
    std::wstring found;
    if (sep != std::wstring::npos) {
        // A directory (or a drive) is named: no search, as CreateProcess.
        std::wstring candidate = name;
        if (name.find(L'.', sep + 1) == std::wstring::npos) candidate += L".exe";
        const bool relative = !(name[0] == L'\\' || name[0] == L'/' || (name.size() >= 2 && name[1] == L':'));
        if (relative && !base.empty()) candidate = base + L"\\" + candidate;
        found = fullPath(candidate);
        const DWORD attrs = found.empty() ? INVALID_FILE_ATTRIBUTES : ::GetFileAttributesW(found.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) found.clear();
    } else if (!name.empty()) {
        const std::wstring dirs = searchPath();
        found = sized([&](wchar_t* b, DWORD n) { return ::SearchPathW(dirs.c_str(), name.c_str(), L".exe", n, b, nullptr); });
    }
    if (found.empty()) processError("cannot run " + argv0, ENOENT);
    // Normalised as CreateProcess will see it (GetFullPathName also drops
    // trailing dots and spaces, so "x.bat." is "x.bat").
    found = fullPath(found);
    const std::size_t dot = found.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : found.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    if (ext == L".bat" || ext == L".cmd")
        throw Error(Error::Kind::InvalidArgument,
                    "cannot run " + argv0 + ": batch files are refused (cmd.exe would interpret the arguments); "
                    "run cmd.exe /c explicitly, with arguments safe for cmd",
                    EINVAL);
    return found;
}

// Starts `program` (a full path) with the command line `cmd`; its standard
// handles are `in`, `out` and `err` (a null one stays null in the child) and
// it inherits nothing else. `name` names the program in error messages.
// `directory` (empty: the caller's) is the child's working directory and
// `environment` (null: the caller's) its Unicode environment block.
//
// The handles are not inheritable; inheritable duplicates are made here and
// closed as soon as CreateProcessW returns, and only those, listed in
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST, reach the child. A CreateProcess that
// another thread makes during that call with bInheritHandles and no handle
// list (system(), _popen, a library) can inherit the duplicates too: that is
// the residual window, the length of one CreateProcessW call.
PROCESS_INFORMATION start(const std::wstring& program, std::wstring cmd, const std::string& name, HANDLE in,
                          HANDLE out, HANDLE err, const std::wstring& directory = std::wstring(),
                          const std::vector<wchar_t>* environment = nullptr) {
    HANDLE std3[3] = {in, out, err};
    Handle dups[3];
    HANDLE list[3];
    DWORD n = 0;
    for (int i = 0; i < 3; ++i) {
        // One duplicate per distinct handle (out and err may be the same).
        if (!std3[i] || std3[i] == INVALID_HANDLE_VALUE) {
            std3[i] = nullptr;
            continue;
        }
        int same = -1;
        for (int j = 0; j < i; ++j)
            if (std3[j] == std3[i]) same = j;
        if (same >= 0) {
            std3[i] = std3[same];  // already duplicated
            continue;
        }
        HANDLE dup = nullptr;
        if (!::DuplicateHandle(::GetCurrentProcess(), std3[i], ::GetCurrentProcess(), &dup, 0, TRUE,
                               DUPLICATE_SAME_ACCESS))
            lastError("cannot run " + name);
        dups[i].h = dup;
        std3[i] = dup;
        list[n++] = dup;
    }
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attrBuf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &size)) lastError("cannot run " + name);
    if (n > 0 && !::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, n * sizeof(HANDLE),
                                              nullptr, nullptr)) {
        const DWORD e = ::GetLastError();
        ::DeleteProcThreadAttributeList(attrs);
        processError("cannot run " + name, win::errnoOfWin32(e));
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = std3[0];
    si.StartupInfo.hStdOutput = std3[1];
    si.StartupInfo.hStdError = std3[2];
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | (environment ? CREATE_UNICODE_ENVIRONMENT : 0);
    void* envBlock = environment ? const_cast<wchar_t*>(environment->data()) : nullptr;
    const BOOL ok = ::CreateProcessW(program.c_str(), cmd.data(), nullptr, nullptr, n > 0, flags, envBlock,
                                     directory.empty() ? nullptr : directory.c_str(), &si.StartupInfo, &pi);
    const DWORD e = ::GetLastError();
    ::DeleteProcThreadAttributeList(attrs);
    for (Handle& d : dups) d.reset();
    if (!ok) processError("cannot run " + name, win::errnoOfWin32(e));
    ::CloseHandle(pi.hThread);
    return pi;
}

// start for an argv: the command line quoted for the C runtime, argv[0]
// resolved by programPath (which refuses batch files).
PROCESS_INFORMATION start(const std::vector<std::string>& argv, const char* who, HANDLE in, HANDLE out, HANDLE err,
                          const std::wstring& directory = std::wstring(),
                          const std::vector<wchar_t>* environment = nullptr) {
    std::wstring cmd = commandLine(argv, who);
    const std::wstring program = programPath(argv[0], directory);
    return start(program, std::move(cmd), argv[0], in, out, err, directory, environment);
}

// The working directory a child gets for RunOptions::directory: the full
// path, which must name a directory (Process, ENOENT otherwise, as chdir
// answers on POSIX).
std::wstring childDirectory(const std::string& argv0, const std::string& dir) {
    const std::wstring full = fullPath(win::widen(dir));
    const DWORD attrs = full.empty() ? INVALID_FILE_ATTRIBUTES : ::GetFileAttributesW(full.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        processError("cannot run " + argv0 + " in " + dir, ENOENT);
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) processError("cannot run " + argv0 + " in " + dir, ENOTDIR);
    return full;
}

// The Unicode environment block for RunOptions::environment: "NAME=value"
// entries sorted by name without regard to case (as CreateProcess
// documents), each NUL-terminated, and a final NUL. SystemRoot is added from
// the caller's environment when it is not given: some system DLLs (Winsock's
// among them) fail to load without it, which is also why the JVM adds it.
std::vector<wchar_t> environmentBlock(const std::vector<std::pair<std::string, std::string>>& vars) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    bool systemRoot = false;
    for (const auto& [name, value] : vars) {
        if (name.empty() || name.find('=') != std::string::npos)
            throw Error(Error::Kind::InvalidArgument, "run: invalid environment variable name: " + name, EINVAL);
        std::wstring wname = win::widen(name);
        if (::_wcsicmp(wname.c_str(), L"SystemRoot") == 0) systemRoot = true;
        entries.emplace_back(std::move(wname), win::widen(value));
    }
    if (!systemRoot) {
        const std::wstring root =
            sized([](wchar_t* b, DWORD n) { return ::GetEnvironmentVariableW(L"SystemRoot", b, n); });
        if (!root.empty()) entries.emplace_back(L"SystemRoot", root);
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const auto& a, const auto& b) { return ::_wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& [name, value] : entries) {
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    // An empty block is two NULs.
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

// A pipe whose ends are both non-inheritable: `child` is the end the child
// gets (start passes it through an inheritable duplicate).
void makePipe(Handle& parent, Handle& child, bool childReads) {
    HANDLE r, w;
    if (!::CreatePipe(&r, &w, nullptr, 0)) lastError("cannot create pipes");
    parent.h = childReads ? w : r;
    child.h = childReads ? r : w;
}

// The exit code wait and run answer. A process that ended with an exception
// has the exception's NTSTATUS code as its exit code (0xC0000005 for an
// access violation, -1073741819 as an int); it is answered as 128 + the
// POSIX signal (Linux numbers) for the same fault, as the shell answers for
// a child that signal ended, so callers can test for a crash the same way on
// every platform. Any other code is answered as it is.
int exitCodeOf(DWORD code) {
    constexpr int kSigInt = 2, kSigIll = 4, kSigTrap = 5, kSigAbrt = 6, kSigBus = 7, kSigFpe = 8, kSigSegv = 11;
    switch (code) {
        case 0xC0000005:  // STATUS_ACCESS_VIOLATION
        case 0xC00000FD:  // STATUS_STACK_OVERFLOW
        case 0xC000008C:  // STATUS_ARRAY_BOUNDS_EXCEEDED
            return 128 + kSigSegv;
        case 0xC0000006:  // STATUS_IN_PAGE_ERROR
        case 0x80000002:  // STATUS_DATATYPE_MISALIGNMENT
            return 128 + kSigBus;
        case 0xC000013A:  // STATUS_CONTROL_C_EXIT
            return 128 + kSigInt;
        case 0xC000008D: case 0xC000008E: case 0xC000008F: case 0xC0000090:  // floating-point faults
        case 0xC0000091: case 0xC0000092: case 0xC0000093:
        case 0xC0000094:  // STATUS_INTEGER_DIVIDE_BY_ZERO
        case 0xC0000095:  // STATUS_INTEGER_OVERFLOW
        case 0xC00002B4: case 0xC00002B5:  // STATUS_FLOAT_MULTIPLE_FAULTS, _TRAPS
            return 128 + kSigFpe;
        case 0xC000001D:  // STATUS_ILLEGAL_INSTRUCTION
        case 0xC0000096:  // STATUS_PRIVILEGED_INSTRUCTION
            return 128 + kSigIll;
        case 0x80000003:  // STATUS_BREAKPOINT
        case 0x80000004:  // STATUS_SINGLE_STEP
            return 128 + kSigTrap;
        default:
            break;
    }
    // Any other NTSTATUS error (0xC0000409, fail-fast, which abort() and
    // buffer-overrun checks raise; 0xC0000374, heap corruption; ...): the
    // process died abnormally.
    if ((code & 0xC0000000u) == 0xC0000000u) return 128 + kSigAbrt;
    return static_cast<int>(code);
}

int exitCodeOf(HANDLE process) {
    DWORD code = 0;
    if (!::GetExitCodeProcess(process, &code)) return -1;
    return exitCodeOf(code);
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

// The body of run and shell: makes the pipes, calls `startChild(in, out,
// err)` to start the child on them, feeds it `input` and collects its outputs
// until it exits.
template <typename StartChild>
RunResult runWith(StartChild startChild, const std::optional<std::string>& input) {
    Handle inW, inR, outR, outW, errR, errW;
    makePipe(inW, inR, true);
    makePipe(outR, outW, false);
    makePipe(errR, errW, false);
    PROCESS_INFORMATION pi = startChild(inR.h, outW.h, errW.h);
    Handle proc(pi.hProcess);
    inR.reset();
    outW.reset();
    errW.reset();

    RunResult res;
    // Anonymous pipes cannot be polled: the standard error is collected and
    // the input fed on threads of their own while this one reads the output.
    // A child that exits without reading its input makes the write fail
    // (ERROR_NO_DATA), which just ends the feeding.
    const std::string empty;
    const std::string& in = input ? *input : empty;
    if (in.empty()) inW.reset();
    std::thread errReader;
    std::thread feeder;
    try {
        errReader = std::thread([&] { drain(errR.h, res.err); });
        if (!in.empty()) {
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
    } catch (...) {
        // A thread could not be started (std::system_error): run cannot go
        // on. The child is ended first, so its pipes close and a thread that
        // did start finishes; it is joined before the handles it uses go
        // away (a joinable std::thread destroyed would call terminate).
        ::TerminateProcess(proc.h, 128 + 9);
        if (errReader.joinable()) errReader.join();
        if (feeder.joinable()) feeder.join();
        ::WaitForSingleObject(proc.h, INFINITE);
        throw;
    }
    drain(outR.h, res.out);
    errReader.join();
    if (feeder.joinable()) feeder.join();
    ::WaitForSingleObject(proc.h, INFINITE);
    res.exitCode = exitCodeOf(proc.h);
    return res;
}

} // namespace

RunResult run(const std::vector<std::string>& argv, const std::optional<std::string>& input) {
    RunOptions options;
    options.input = input;
    return run(argv, options);
}

RunResult run(const std::vector<std::string>& argv, const RunOptions& options) {
    if (argv.empty()) throw Error(Error::Kind::InvalidArgument, "run: needs a command");
    const std::wstring directory = options.directory ? childDirectory(argv[0], *options.directory) : std::wstring();
    std::vector<wchar_t> environment;
    if (options.environment) environment = environmentBlock(*options.environment);
    const std::vector<wchar_t>* env = options.environment ? &environment : nullptr;
    return runWith([&](HANDLE in, HANDLE out, HANDLE err) { return start(argv, "run", in, out, err, directory, env); },
                   options.input);
}

RunResult shell(const std::string& command, const std::optional<std::string>& input) {
    if (command.empty()) throw Error(Error::Kind::InvalidArgument, "shell: needs a command");
    const std::wstring systemDir = sized([](wchar_t* b, DWORD n) { return ::GetSystemDirectoryW(b, n); });
    if (systemDir.empty()) lastError("cannot run cmd.exe");
    const std::wstring program = systemDir + L"\\cmd.exe";
    const std::wstring cmd = L"cmd.exe /d /s /c \"" + win::widen(command) + L"\"";
    return runWith([&](HANDLE in, HANDLE out, HANDLE err) { return start(program, cmd, "cmd.exe", in, out, err); },
                   input);
}

int spawn(const std::vector<std::string>& argv) {
    if (argv.empty()) throw Error(Error::Kind::InvalidArgument, "spawn: needs a command");
    std::fflush(nullptr);
    // The child shares the caller's standard streams (start passes them
    // through inheritable duplicates).
    PROCESS_INFORMATION pi = start(argv, "spawn", ::GetStdHandle(STD_INPUT_HANDLE), ::GetStdHandle(STD_OUTPUT_HANDLE),
                                   ::GetStdHandle(STD_ERROR_HANDLE));
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
