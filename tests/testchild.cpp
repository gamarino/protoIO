// A child program for the process tests on Windows, which lacks the POSIX
// commands (sh, cat, true, sleep) the tests run elsewhere.
//
//   testchild cat               copies its input to its output, as bytes
//   testchild true              exits 0 without reading its input
//   testchild print OUT ERR N   writes OUT and ERR to its outputs, exits N
//   testchild env NAME          writes the value of NAME (nothing if unset)
//   testchild cwd               writes its working directory, as UTF-8
//   testchild sleep MS          sleeps MS milliseconds
//   testchild args A...         writes each argument as "[A]\n"
//   testchild wargs A...        the same, read with GetCommandLineW and
//                               CommandLineToArgvW and written as UTF-8
//   testchild wenv NAME         writes the value of NAME, read with
//                               _wgetenv, as UTF-8
//   testchild crash             dereferences a null pointer
//   testchild exit CODE         ends with ExitProcess(CODE); CODE may be hex
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>

namespace {
std::string utf8(const wchar_t* w) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<std::size_t>(n) : 1, '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.pop_back();  // the terminating NUL
    return s;
}
} // namespace
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (argc < 2) return 100;
    const std::string mode = argv[1];
    if (mode == "cat") {
        char buf[65536];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) std::fwrite(buf, 1, n, stdout);
        return 0;
    }
    if (mode == "true") return 0;
    if (mode == "print" && argc == 5) {
        std::fputs(argv[2], stdout);
        std::fputs(argv[3], stderr);
        return std::atoi(argv[4]);
    }
    if (mode == "env" && argc == 3) {
        if (const char* v = std::getenv(argv[2])) std::fputs(v, stdout);
        return 0;
    }
    if (mode == "sleep" && argc == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
        return 0;
    }
#ifdef _WIN32
    if (mode == "wargs") {
        int n = 0;
        wchar_t** w = ::CommandLineToArgvW(::GetCommandLineW(), &n);
        for (int i = 2; i < n; ++i) std::printf("[%s]\n", utf8(w[i]).c_str());
        ::LocalFree(w);
        return 0;
    }
    if (mode == "cwd") {
        const DWORD n = ::GetCurrentDirectoryW(0, nullptr);
        std::wstring dir(n, L'\0');
        dir.resize(::GetCurrentDirectoryW(n, dir.data()));
        std::fputs(utf8(dir.c_str()).c_str(), stdout);
        return 0;
    }
    if (mode == "wenv" && argc == 3) {
        int n = 0;
        wchar_t** w = ::CommandLineToArgvW(::GetCommandLineW(), &n);
        if (const wchar_t* v = ::_wgetenv(w[2])) std::fputs(utf8(v).c_str(), stdout);
        ::LocalFree(w);
        return 0;
    }
    if (mode == "crash") {
        // No error-reporting dialog: the process just ends.
        ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        volatile int* nowhere = reinterpret_cast<volatile int*>(static_cast<std::uintptr_t>(std::atoi("0")));
        *nowhere = 1;
        return 0;
    }
    if (mode == "exit" && argc == 3) {
        std::fflush(nullptr);
        ::ExitProcess(static_cast<UINT>(std::strtoul(argv[2], nullptr, 0)));
    }
#endif
    if (mode == "args") {
        for (int i = 2; i < argc; ++i) std::printf("[%s]\n", argv[i]);
        return 0;
    }
    return 100;
}
