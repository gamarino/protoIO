// Child processes and the process environment.
#include "protoio/error.h"
#include "protoio/process.h"

#include <gtest/gtest.h>

#include <csignal>
#include <thread>
#include <vector>

#include <unistd.h>

using protoio::Error;
namespace process = protoio::process;

TEST(Process, RunCollectsOutputsAndExitCode) {
    auto r = process::run({"sh", "-c", "printf out; printf err >&2; exit 3"});
    EXPECT_EQ(r.exitCode, 3);
    EXPECT_EQ(r.out, "out");
    EXPECT_EQ(r.err, "err");
}

TEST(Process, RunFeedsInput) {
    auto r = process::run({"cat"}, std::string("hello\nworld\n"));
    EXPECT_EQ(r.exitCode, 0);
    EXPECT_EQ(r.out, "hello\nworld\n");
    // Without input, the child's stdin is closed at once.
    auto r2 = process::run({"cat"});
    EXPECT_EQ(r2.out, "");
}

TEST(Process, RunLargeOutputAndInputDoNotDeadlock) {
    const std::string big(3 * 1024 * 1024, 'q');
    auto r = process::run({"cat"}, big);
    EXPECT_EQ(r.exitCode, 0);
    EXPECT_EQ(r.out.size(), big.size());
}

TEST(Process, AChildThatIgnoresItsInputDoesNotKillTheCaller) {
    // `true` exits without reading: writing 2 MB to it would raise SIGPIPE.
    const std::string input(2 * 1024 * 1024, 'x');
    auto r = process::run({"true"}, input);
    EXPECT_EQ(r.exitCode, 0);
    sigset_t pending;
    sigpending(&pending);
    EXPECT_FALSE(sigismember(&pending, SIGPIPE));
}

TEST(Process, ASignalGivesExitCode128PlusSignal) {
    auto r = process::run({"sh", "-c", "kill -TERM $$"});
    EXPECT_EQ(r.exitCode, 128 + SIGTERM);
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
    int pid = process::spawn({"sh", "-c", "exit 7"});
    EXPECT_GT(pid, 0);
    EXPECT_EQ(process::wait(pid), 7);

    int sleeper = process::spawn({"sleep", "30"});
    process::kill(sleeper, SIGKILL);
    EXPECT_EQ(process::wait(sleeper), 128 + SIGKILL);

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
                auto r = process::run({"cat"}, in);
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
    auto r = process::run({"sh", "-c", "printf %s \"$PROTOIO_TEST_VAR\""});
    EXPECT_EQ(r.out, "v=1");
    process::setenv("PROTOIO_TEST_VAR", std::nullopt);
    EXPECT_EQ(process::getenv("PROTOIO_TEST_VAR"), std::nullopt);
}

TEST(Process, Identity) {
    EXPECT_EQ(process::pid(), ::getpid());
    EXPECT_FALSE(process::hostName().empty());
#if defined(__linux__)
    EXPECT_EQ(process::platform(), "linux");
#endif
}

TEST(Process, ExitEndsTheProcessWithTheCode) {
    EXPECT_EXIT(process::exit(0x105), ::testing::ExitedWithCode(5), "");
}
