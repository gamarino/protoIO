// Child processes and the process environment.
#include "protoio/error.h"
#include "protoio/file.h"
#include "protoio/process.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <optional>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using protoio::Error;
namespace file = protoio::file;
namespace process = protoio::process;

// The commands the tests run: POSIX ones, or on Windows the test child
// program (tests/testchild.cpp) standing in for them.
#ifdef _WIN32
const std::vector<std::string> kCat = {PROTOIO_TESTCHILD, "cat"};
const std::vector<std::string> kTrue = {PROTOIO_TESTCHILD, "true"};
const std::vector<std::string> kSleep30 = {PROTOIO_TESTCHILD, "sleep", "30000"};
constexpr int kSigKill = 9;
#else
const std::vector<std::string> kCat = {"cat"};
const std::vector<std::string> kTrue = {"true"};
const std::vector<std::string> kSleep30 = {"sleep", "30"};
constexpr int kSigKill = SIGKILL;
#endif

TEST(Process, RunCollectsOutputsAndExitCode) {
#ifdef _WIN32
    auto r = process::run({PROTOIO_TESTCHILD, "print", "out", "err", "3"});
#else
    auto r = process::run({"sh", "-c", "printf out; printf err >&2; exit 3"});
#endif
    EXPECT_EQ(r.exitCode, 3);
    EXPECT_EQ(r.out, "out");
    EXPECT_EQ(r.err, "err");
}

#ifdef _WIN32
TEST(Process, RunSearchesPathOnWindows) {
    // "cmd" is found without its directory or its ".exe".
    auto r = process::run({"cmd", "/c", "exit 3"});
    EXPECT_EQ(r.exitCode, 3);
}

TEST(Process, ArgumentsArriveUnchangedOnWindows) {
    // The command line is quoted so that the child's C runtime rebuilds the
    // same argv: spaces, quotes, backslashes before quotes and at the end.
    const std::vector<std::string> args = {"plain", "two words", "", "quo\"te", "back\\slash",
                                           "trail\\", "both \\\" x", "tab\there"};
    std::vector<std::string> argv = {PROTOIO_TESTCHILD, "args"};
    argv.insert(argv.end(), args.begin(), args.end());
    auto r = process::run(argv);
    std::string expected;
    for (const std::string& a : args) expected += "[" + a + "]\n";
    EXPECT_EQ(r.out, expected);
}

template <typename F>
Error errorOf(F f) {
    try {
        f();
    } catch (const Error& e) {
        return e;
    }
    ADD_FAILURE() << "no Error was thrown";
    return Error(Error::Kind::FileSystem, "none");
}

std::string nativePath(std::string s) {
    for (char& c : s) if (c == '/') c = '\\';
    return s;
}

// BatBadBut (CVE-2024-24576): CreateProcess runs a .bat or .cmd file through
// cmd.exe, which parses the command line again by its own rules, so an
// argument such as "a&echo pwned>marker" runs a second command whatever
// quoting the C runtime's rules give it. Such targets are refused, however
// the name is spelled; the marker file proves no second command ran.
TEST(Process, BatchFilesAreRefusedOnWindows) {
    protoio_test::TempDir d;
    const std::string marker = nativePath(d / "marker");
    file::write(d / "x.bat", "@echo off\r\necho %1\r\n");
    file::write(d / "y.CMD", "@echo off\r\necho %1\r\n");
    for (const std::string& target : {d / "x.bat", d / "y.CMD", d / "x.bat.", d / "x.BaT"}) {
        Error e = errorOf([&] { process::run({nativePath(target), "a&echo pwned>" + marker}); });
        EXPECT_EQ(e.kind, Error::Kind::InvalidArgument) << target << ": " << e.what();
        EXPECT_THROW(process::spawn({nativePath(target), "a&echo pwned>" + marker}), Error) << target;
    }
    EXPECT_FALSE(file::stat(d / "marker")) << "a batch file ran its argument as a command";
}

// A program named without a directory is not looked for in the working
// directory (where CreateProcess looks before the system directories), so a
// planted cmd.exe or git.exe there never runs.
TEST(Process, TheWorkingDirectoryIsNotSearchedOnWindows) {
    protoio_test::TempDir d;
    // The test child answers 100 to commands it does not know.
    file::copy(PROTOIO_TESTCHILD, d / "cmd.exe");
    file::copy(PROTOIO_TESTCHILD, d / "protoio-planted.exe");
    const std::string before = file::cwd();
    file::chdir(d.path);
    struct Back {
        std::string to;
        ~Back() { file::chdir(to); }
    } back{before};
    EXPECT_EQ(process::run({"cmd", "/c", "exit 3"}).exitCode, 3) << "the planted cmd.exe ran";
    Error e = errorOf([] { process::run({"protoio-planted"}); });
    EXPECT_EQ(e.kind, Error::Kind::Process);
    EXPECT_EQ(e.sysErrno, ENOENT);
    // A path that names the directory still runs the program there.
    EXPECT_EQ(process::run({".\\protoio-planted.exe", "print", "", "", "4"}).exitCode, 4);
}

// A child that ends with an exception (an NTSTATUS code such as 0xC0000005)
// gets 128 + the POSIX signal for the same fault, as the shell reports a
// child that signal ended; ordinary exit codes are answered unchanged.
TEST(Process, ACrashedChildGets128PlusTheMatchingSignalOnWindows) {
    EXPECT_EQ(process::run({PROTOIO_TESTCHILD, "crash"}).exitCode, 128 + 11);  // SIGSEGV
    const std::pair<const char*, int> codes[] = {
        {"0xC0000005", 128 + 11},  // access violation: SIGSEGV
        {"0xC00000FD", 128 + 11},  // stack overflow: SIGSEGV
        {"0xC000013A", 128 + 2},   // ended by Ctrl+C: SIGINT
        {"0xC0000094", 128 + 8},   // integer division by zero: SIGFPE
        {"0xC000001D", 128 + 4},   // illegal instruction: SIGILL
        {"0xC0000409", 128 + 6},   // fail-fast (abort, buffer overrun): SIGABRT
        {"0xC0000001", 128 + 6},   // any other NTSTATUS error: SIGABRT
        {"0x80000003", 128 + 5},   // breakpoint: SIGTRAP
        {"3", 3},
        {"300", 300},
    };
    for (const auto& [code, expected] : codes)
        EXPECT_EQ(process::run({PROTOIO_TESTCHILD, "exit", code}).exitCode, expected) << code;
}

TEST(Process, UnsupportedSignalsThrowOnWindows) {
    int pid = process::spawn(kSleep30);
    try {
        process::kill(pid, SIGINT);
        FAIL() << "expected an Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::Process);
        EXPECT_EQ(e.sysErrno, ENOSYS);
    }
    process::kill(pid, 0);  // still there
    process::kill(pid, kSigKill);
    EXPECT_EQ(process::wait(pid), 128 + kSigKill);
}
#endif

TEST(Process, RunFeedsInput) {
    auto r = process::run(kCat, std::string("hello\nworld\n"));
    EXPECT_EQ(r.exitCode, 0);
    EXPECT_EQ(r.out, "hello\nworld\n");
    // Without input, the child's stdin is closed at once.
    auto r2 = process::run(kCat);
    EXPECT_EQ(r2.out, "");
}

#ifndef _WIN32
// A child that exits without reading its input: writing the rest is EPIPE, and
// the caller gets the child's exit code, not a SIGPIPE (macOS could deliver it
// to a thread that does not block it).
TEST(Process, InputToAChildThatIgnoresItIsNotFatal) {
    const std::string big(4 * 1024 * 1024, 'q');
    auto r = process::run({"sh", "-c", "exit 0"}, big);
    EXPECT_EQ(r.exitCode, 0);
}
#endif

TEST(Process, RunLargeOutputAndInputDoNotDeadlock) {
    const std::string big(3 * 1024 * 1024, 'q');
    auto r = process::run(kCat, big);
    EXPECT_EQ(r.exitCode, 0);
    EXPECT_EQ(r.out.size(), big.size());
}

TEST(Process, AChildThatIgnoresItsInputDoesNotKillTheCaller) {
    // `true` exits without reading: writing 2 MB to it would raise SIGPIPE.
    const std::string input(2 * 1024 * 1024, 'x');
    auto r = process::run(kTrue, input);
    EXPECT_EQ(r.exitCode, 0);
#ifndef _WIN32
    sigset_t pending;
    sigpending(&pending);
    EXPECT_FALSE(sigismember(&pending, SIGPIPE));
#endif
}

TEST(Process, ASignalGivesExitCode128PlusSignal) {
#ifdef _WIN32
    // No process can signal itself on Windows: the test kills a child.
    int pid = process::spawn(kSleep30);
    process::kill(pid, SIGTERM);
    EXPECT_EQ(process::wait(pid), 128 + SIGTERM);
#else
    auto r = process::run({"sh", "-c", "kill -TERM $$"});
    EXPECT_EQ(r.exitCode, 128 + SIGTERM);
#endif
}

TEST(Process, AMissingCommandThrowsProcess) {
    try {
        process::run({"protoio-no-such-command-xyz"});
        FAIL() << "expected an Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::Process);
        EXPECT_EQ(e.sysErrno, ENOENT);
    }
    EXPECT_THROW(process::run({}), Error);
    EXPECT_THROW(process::spawn({"protoio-no-such-command-xyz"}), Error);
}

// shell hands the command line to the system shell verbatim: /bin/sh -c on
// POSIX, cmd.exe /d /s /c "<command>" on Windows.
#ifdef _WIN32
constexpr const char* kNl = "\r\n";
#else
constexpr const char* kNl = "\n";
#endif

TEST(Process, ShellRunsTheCommandThroughTheSystemShell) {
    auto r = process::shell("echo hello");
    EXPECT_EQ(r.exitCode, 0);
    EXPECT_EQ(r.out, std::string("hello") + kNl);
    EXPECT_EQ(process::shell("exit 3").exitCode, 3);
}

TEST(Process, ShellFeedsInput) {
#ifdef _WIN32
    auto r = process::shell("findstr x", std::string("x\r\n"));
    EXPECT_EQ(r.out, "x\r\n");
#else
    auto r = process::shell("cat", std::string("hello\nworld\n"));
    EXPECT_EQ(r.out, "hello\nworld\n");
#endif
    EXPECT_EQ(r.exitCode, 0);
}

TEST(Process, ShellPassesTheCommandVerbatim) {
    auto r = process::shell("echo \"a b\"");
    EXPECT_EQ(r.exitCode, 0);
#ifdef _WIN32
    // cmd's echo prints the quotes: they reached cmd as written, not escaped
    // by the C runtime's rules.
    EXPECT_EQ(r.out, "\"a b\"\r\n");
    // The shell parses its own syntax: & separates two commands.
    EXPECT_EQ(process::shell("echo a&echo b").out, "a\r\nb\r\n");
#else
    EXPECT_EQ(r.out, "a b\n");
    EXPECT_EQ(process::shell("echo a; echo b").out, "a\nb\n");
#endif
}

TEST(Process, ShellNeedsACommand) {
    try {
        process::shell("");
        FAIL() << "expected an Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::InvalidArgument);
    }
}

#ifdef _WIN32
// The shell is cmd.exe from the system directory, never one planted in the
// working directory (or named by PATH or COMSPEC).
TEST(Process, ShellIgnoresAPlantedCmdOnWindows) {
    protoio_test::TempDir d;
    // The test child answers 100 to commands it does not know.
    file::copy(PROTOIO_TESTCHILD, d / "cmd.exe");
    const std::string before = file::cwd();
    file::chdir(d.path);
    struct Back {
        std::string to;
        ~Back() { file::chdir(to); }
    } back{before};
    const std::optional<std::string> comspec = process::getenv("COMSPEC");
    process::setenv("COMSPEC", nativePath(d / "cmd.exe"));
    struct RestoreComspec {
        std::optional<std::string> value;
        ~RestoreComspec() { process::setenv("COMSPEC", value); }
    } restore{comspec};
    EXPECT_EQ(process::shell("exit 3").exitCode, 3) << "the planted cmd.exe ran";
}
#endif

TEST(Process, SpawnWaitKill) {
#ifdef _WIN32
    int pid = process::spawn({"cmd", "/c", "exit 7"});
#else
    int pid = process::spawn({"sh", "-c", "exit 7"});
#endif
    EXPECT_GT(pid, 0);
    EXPECT_EQ(process::wait(pid), 7);

    int sleeper = process::spawn(kSleep30);
    process::kill(sleeper, kSigKill);
    EXPECT_EQ(process::wait(sleeper), 128 + kSigKill);

    try {
        process::wait(sleeper);  // already reaped
        FAIL() << "expected an Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::Process);
        EXPECT_EQ(e.sysErrno, ECHILD);
    }
}

// A UTF-8 path as a std::filesystem::path.
std::filesystem::path fsPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// RunOptions::directory: the child starts in that directory.
TEST(Process, RunInADirectory) {
    protoio_test::TempDir d;
    process::RunOptions options;
    options.directory = d.path;
#ifdef _WIN32
    auto r = process::run({PROTOIO_TESTCHILD, "cwd"}, options);
#else
    auto r = process::run({"pwd", "-P"}, options);
#endif
    ASSERT_EQ(r.exitCode, 0) << r.err;
    while (!r.out.empty() && (r.out.back() == '\n' || r.out.back() == '\r')) r.out.pop_back();
    // Compared as files: the temporary directory may be reached through a
    // symbolic link (macOS) or an 8.3 short name (Windows).
    EXPECT_TRUE(std::filesystem::equivalent(fsPath(r.out), fsPath(d.path)))
        << r.out << " vs " << d.path;
    // The caller's own working directory is unchanged.
    EXPECT_FALSE(std::filesystem::equivalent(fsPath(file::cwd()), fsPath(d.path)));
}

// A program named with a relative directory is found from the child's
// working directory, as exec finds it after the child changed directory.
TEST(Process, ARelativeProgramPathIsResolvedInTheChildsDirectory) {
    protoio_test::TempDir d;
    file::mkdir(d / "bin");
#ifdef _WIN32
    file::copy(PROTOIO_TESTCHILD, d / "bin/child.exe");
    const std::vector<std::string> argv = {"bin\\child", "print", "here", "", "5"};
#else
    file::write(d / "bin/child", "#!/bin/sh\nprintf here\nexit 5\n");
    ASSERT_EQ(::chmod((d / "bin/child").c_str(), 0755), 0);
    const std::vector<std::string> argv = {"./bin/child"};
#endif
    process::RunOptions options;
    options.directory = d.path;
    auto r = process::run(argv, options);
    EXPECT_EQ(r.exitCode, 5) << r.err;
    EXPECT_EQ(r.out, "here");
}

TEST(Process, AMissingDirectoryThrowsProcess) {
    protoio_test::TempDir d;
    process::RunOptions options;
    options.directory = d / "missing";
    try {
        process::run(kTrue, options);
        FAIL() << "expected an Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::Process) << e.what();
        EXPECT_EQ(e.sysErrno, ENOENT) << e.what();
    }
}

// RunOptions::environment replaces the caller's environment; the program is
// still found through the caller's PATH, which the child does not get.
TEST(Process, RunWithAReplacedEnvironment) {
    process::RunOptions options;
    options.environment = std::vector<std::pair<std::string, std::string>>{{"PROTOIO_ONLY", "one two"}};
#ifdef _WIN32
    EXPECT_EQ(process::run({PROTOIO_TESTCHILD, "env", "PROTOIO_ONLY"}, options).out, "one two");
    EXPECT_EQ(process::run({"cmd", "/c", "exit 3"}, options).exitCode, 3);
    EXPECT_EQ(process::run({PROTOIO_TESTCHILD, "env", "PATH"}, options).out, "");
    // SystemRoot is the one variable added: some system DLLs need it.
    EXPECT_NE(process::run({PROTOIO_TESTCHILD, "env", "SystemRoot"}, options).out, "");
    EXPECT_EQ(process::run({PROTOIO_TESTCHILD, "wenv", "PROTOIO_ONLY"}, options).out, "one two");
#else
    auto r = process::run({"env"}, options);
    EXPECT_EQ(r.exitCode, 0) << r.err;
    EXPECT_EQ(r.out, "PROTOIO_ONLY=one two\n");
#endif
    // An empty environment is an environment too.
    options.environment = std::vector<std::pair<std::string, std::string>>{};
#ifdef _WIN32
    EXPECT_EQ(process::run({PROTOIO_TESTCHILD, "env", "PROTOIO_ONLY"}, options).out, "");
#else
    EXPECT_EQ(process::run({"env"}, options).out, "");
#endif
    options.environment = std::vector<std::pair<std::string, std::string>>{{"A=B", "c"}};
    try {
        process::run(kTrue, options);
        FAIL() << "expected an Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::InvalidArgument) << e.what();
    }
}

TEST(Process, RunOptionsFeedInputWithADirectoryAndAnEnvironment) {
    protoio_test::TempDir d;
    process::RunOptions options;
    options.input = std::string("fed\n");
    options.directory = d.path;
    options.environment = std::vector<std::pair<std::string, std::string>>{{"X", "1"}};
    auto r = process::run(kCat, options);
    EXPECT_EQ(r.exitCode, 0) << r.err;
    EXPECT_EQ(r.out, "fed\n");
}

TEST(Process, ConcurrentRuns) {
    std::vector<std::thread> ts;
    std::atomic<int> ok{0};
    for (int i = 0; i < 4; ++i) {
        ts.emplace_back([&, i] {
            for (int k = 0; k < 5; ++k) {
                const std::string in = "t" + std::to_string(i) + "-" + std::to_string(k);
                auto r = process::run(kCat, in);
                if (r.exitCode == 0 && r.out == in) ++ok;
            }
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(ok.load(), 20);
}

// Arguments and environment strings beyond ASCII reach the child unchanged
// (as UTF-16 on Windows, read there with the wide APIs).
TEST(Process, UnicodeArgumentsAndEnvironmentRoundTrip) {
    const std::vector<std::string> args = {"\xC3\xB1" "and\xC3\xBA",          // ñandú
                                           "\xE2\x82\xAC 5",                     // € 5
                                           "\xE6\x97\xA5\xE6\x9C\xAC",           // 日本
                                           "\xF0\x9F\x98\x80 \"q\""};          // 😀 "q"
    std::string expected;
    for (const std::string& a : args) expected += "[" + a + "]\n";
#ifdef _WIN32
    std::vector<std::string> argv = {PROTOIO_TESTCHILD, "wargs"};
#else
    std::vector<std::string> argv = {"printf", "[%s]\\n"};
#endif
    argv.insert(argv.end(), args.begin(), args.end());
    EXPECT_EQ(process::run(argv).out, expected);

    const std::string value = "\xC3\xB1\xE2\x82\xAC\xF0\x9F\x98\x80";
    process::setenv("PROTOIO_UNICODE", value);
#ifdef _WIN32
    auto r = process::run({PROTOIO_TESTCHILD, "wenv", "PROTOIO_UNICODE"});
#else
    auto r = process::run({"sh", "-c", "printf %s \"$PROTOIO_UNICODE\""});
#endif
    EXPECT_EQ(r.out, value);
    process::setenv("PROTOIO_UNICODE", std::nullopt);
}

TEST(Process, Environment) {
    process::setenv("PROTOIO_TEST_VAR", std::string("v=1"));
    EXPECT_EQ(process::getenv("PROTOIO_TEST_VAR"), "v=1");
    bool found = false;
    for (auto& [k, v] : process::environment())
        if (k == "PROTOIO_TEST_VAR") { found = true; EXPECT_EQ(v, "v=1"); }
    EXPECT_TRUE(found);
#ifdef _WIN32
    auto r = process::run({PROTOIO_TESTCHILD, "env", "PROTOIO_TEST_VAR"});
#else
    auto r = process::run({"sh", "-c", "printf %s \"$PROTOIO_TEST_VAR\""});
#endif
    EXPECT_EQ(r.out, "v=1");
    process::setenv("PROTOIO_TEST_VAR", std::nullopt);
    EXPECT_EQ(process::getenv("PROTOIO_TEST_VAR"), std::nullopt);
}

TEST(Process, Identity) {
#ifdef _WIN32
    EXPECT_EQ(process::pid(), ::_getpid());
#else
    EXPECT_EQ(process::pid(), ::getpid());
#endif
    EXPECT_FALSE(process::hostName().empty());
#if defined(__linux__)
    EXPECT_EQ(process::platform(), "linux");
#elif defined(_WIN32)
    EXPECT_EQ(process::platform(), "windows");
#endif
}

TEST(Process, ExitEndsTheProcessWithTheCode) {
    EXPECT_EXIT(process::exit(0x105), ::testing::ExitedWithCode(5), "");
}
