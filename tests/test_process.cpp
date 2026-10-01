// Child processes and the process environment.
#include "protoio/error.h"
#include "protoio/process.h"

#include <gtest/gtest.h>

#include <csignal>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using protoio::Error;
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
