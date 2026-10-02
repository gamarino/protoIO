// Buffered streams: reading lines, bytes and characters, limits, concurrent
// readers, close waking a blocked reader, and writes that never raise SIGPIPE.
#include "protoio/error.h"
#include "protoio/stream.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include "protoio/file.h"
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

using protoio::Error;
using protoio_test::TempDir;
using namespace std::chrono_literals;

namespace {

struct Pipe {
    int r = -1, w = -1;
    Pipe() {
        int p[2];
#ifdef _WIN32
        if (::_pipe(p, 65536, _O_BINARY | _O_NOINHERIT) != 0) throw std::runtime_error("pipe");
#else
        if (::pipe(p) != 0) throw std::runtime_error("pipe");
#endif
        r = p[0];
        w = p[1];
        protoio::forget(r);
        protoio::forget(w);
    }
    ~Pipe() {
        if (r >= 0) protoio::close(r);
        if (w >= 0) protoio::close(w);
    }
    void closeWriter() { protoio::close(w); w = -1; }
};

struct SocketPair {
    int a = -1, b = -1;
    SocketPair() {
        int s[2];
        protoio_test::socketPair(s);
        a = s[0];
        b = s[1];
    }
    ~SocketPair() {
        protoio::close(a);
        protoio::close(b);
    }
};

} // namespace

TEST(Stream, ReadLineStripsLfAndCrlfAndAnswersNulloptAtEnd) {
    Pipe p;
    protoio::write(p.w, "one\ntwo\r\n\nlast");
    p.closeWriter();
    EXPECT_EQ(protoio::readLine(p.r), "one");
    EXPECT_EQ(protoio::readLine(p.r), "two");
    EXPECT_EQ(protoio::readLine(p.r), "");
    EXPECT_EQ(protoio::readLine(p.r), "last");
    EXPECT_EQ(protoio::readLine(p.r), std::nullopt);
    EXPECT_EQ(protoio::readLine(p.r), std::nullopt);
}

TEST(Stream, ReadAllAnswersEverythingThenEmpty) {
    Pipe p;
    protoio::write(p.w, "line\nrest of it");
    p.closeWriter();
    EXPECT_EQ(protoio::readLine(p.r), "line");
    EXPECT_EQ(protoio::readAll(p.r), "rest of it");
    EXPECT_EQ(protoio::readAll(p.r), "");
    EXPECT_TRUE(protoio::atEnd(p.r));
}

TEST(Stream, ReadBytesAnswersUpToNThenNullopt) {
    Pipe p;
    protoio::write(p.w, "abcdefg");
    p.closeWriter();
    EXPECT_FALSE(protoio::atEnd(p.r));
    EXPECT_EQ(protoio::readBytes(p.r, 3), "abc");
    EXPECT_EQ(protoio::readBytes(p.r, 10), "defg");
    EXPECT_EQ(protoio::readBytes(p.r, 1), std::nullopt);
    EXPECT_EQ(protoio::readBytes(p.r, 0), "");
}

TEST(Stream, ReadCharsKeepsACharacterSplitAcrossReads) {
    Pipe p;
    // "aé€b": the é (C3 A9) is cut in two by the first write.
    protoio::write(p.w, "a\xC3");
    std::thread writer([&] {
        std::this_thread::sleep_for(100ms);
        protoio::write(p.w, "\xA9\xE2\x82\xAC" "b");
        p.closeWriter();
    });
    EXPECT_EQ(protoio::readChars(p.r, 2), "a\xC3\xA9");
    EXPECT_EQ(protoio::readChars(p.r, 1), "\xE2\x82\xAC");
    EXPECT_EQ(protoio::readChars(p.r, 5), "b");
    EXPECT_EQ(protoio::readChars(p.r, 1), std::nullopt);
    writer.join();
}

TEST(Stream, ReadCharsAnswersATruncatedSequenceAtEnd) {
    Pipe p;
    protoio::write(p.w, "x\xE2\x82");
    p.closeWriter();
    EXPECT_EQ(protoio::readChars(p.r, 5), "x\xE2\x82");
}

TEST(Stream, ReadLineOverMaxThrowsLineTooLong) {
    Pipe p;
    protoio::write(p.w, "12345\r\n" + std::string(100, 'x') + "\nnext\n");
    p.closeWriter();
    EXPECT_EQ(protoio::readLine(p.r, 5), "12345");  // exactly max, CRLF not counted
    try {
        protoio::readLine(p.r, 10);
        FAIL() << "expected LineTooLong";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::LineTooLong);
    }
}

TEST(Stream, ReadLineMaxStopsReadingAnEndlessLine) {
    SocketPair s;
    // The writer never sends a newline and never closes: the reader must give
    // up once it holds more than `max` bytes instead of waiting forever.
    protoio::write(s.b, std::string(200000, 'y'));
    try {
        protoio::readLine(s.a, 8192);
        FAIL() << "expected LineTooLong";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::LineTooLong);
    }
}

TEST(Stream, ConcurrentReadersGetEachLineExactlyOnceAndWhole) {
    Pipe p;
    constexpr int kLines = 20000;
    std::thread writer([&] {
        std::string chunk;
        for (int i = 0; i < kLines; ++i) {
            chunk += "line-" + std::to_string(i) + "-" + std::string(i % 37, 'z') + "\n";
            if (chunk.size() > 3000) { protoio::write(p.w, chunk); chunk.clear(); }
        }
        protoio::write(p.w, chunk);
        p.closeWriter();
    });
    std::mutex m;
    std::map<std::string, int> seen;
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            std::vector<std::string> mine;
            while (auto line = protoio::readLine(p.r)) mine.push_back(*line);
            std::lock_guard<std::mutex> lock(m);
            for (auto& l : mine) ++seen[l];
        });
    }
    writer.join();
    for (auto& r : readers) r.join();
    ASSERT_EQ(seen.size(), static_cast<size_t>(kLines));
    for (int i = 0; i < kLines; ++i) {
        const std::string expected = "line-" + std::to_string(i) + "-" + std::string(i % 37, 'z');
        auto it = seen.find(expected);
        ASSERT_NE(it, seen.end()) << expected;
        EXPECT_EQ(it->second, 1) << expected;
    }
}

TEST(Stream, CloseWakesAThreadBlockedReadingASocket) {
    SocketPair s;
    std::atomic<bool> done{false};
    std::optional<std::string> got = "unset";
    std::thread reader([&] {
        got = protoio::readLine(s.a);
        done = true;
    });
    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(done.load());
    const auto start = std::chrono::steady_clock::now();
    protoio::close(s.a);
    reader.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    EXPECT_EQ(got, std::nullopt);
}

TEST(Stream, AReaderAndAWriterOnOneSocketDoNotBlockEachOther) {
    SocketPair s;
    std::thread reader([&] { EXPECT_EQ(protoio::readLine(s.a), "late"); });
    std::this_thread::sleep_for(50ms);
    // The reader waits on `a` holding its read lock; a write on `a` proceeds.
    protoio::write(s.a, "hello\n");
    EXPECT_EQ(protoio::readLine(s.b), "hello");
    protoio::write(s.b, "late\n");
    reader.join();
}

TEST(Stream, WriteToAPipeWhoseReaderClosedThrowsInsteadOfSigpipe) {
    Pipe p;
    protoio::close(p.r);
    p.r = -1;
    try {
        protoio::write(p.w, "nobody reads this");
        FAIL() << "expected an Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::FileSystem);
        EXPECT_EQ(e.sysErrno, EPIPE);
    }
#ifndef _WIN32
    // Still alive, and no SIGPIPE left pending for this thread.
    sigset_t pending;
    sigpending(&pending);
    EXPECT_FALSE(sigismember(&pending, SIGPIPE));
#endif
}

TEST(Stream, WriteToASocketWhosePeerClosedThrowsNetwork) {
    SocketPair s;
    protoio::close(s.b);
    try {
#ifdef _WIN32
        // The peer is a TCP socket there (socketPair): the first write may
        // still be accepted, the peer's reset fails a later one.
        for (int i = 0; i < 100; ++i) {
            protoio::write(s.a, std::string(1 << 20, 'x'));
            std::this_thread::sleep_for(10ms);
        }
#else
        protoio::write(s.a, std::string(1 << 20, 'x'));
#endif
        FAIL() << "expected an Error";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::Network);
#ifdef _WIN32
        // Winsock reports the peer's reset rather than a broken pipe.
        EXPECT_TRUE(e.sysErrno == EPIPE || e.sysErrno == ECONNRESET || e.sysErrno == ECONNABORTED) << e.what();
#else
        EXPECT_EQ(e.sysErrno, EPIPE);
#endif
    }
}

TEST(Stream, TimeoutBoundsARead) {
    SocketPair s;
    protoio::setTimeout(s.a, 100);
    const auto start = std::chrono::steady_clock::now();
    try {
        protoio::readLine(s.a);
        FAIL() << "expected ConnectionTimedOut";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::ConnectionTimedOut);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    // The stream is still usable afterwards.
    protoio::write(s.b, "ok\n");
    EXPECT_EQ(protoio::readLine(s.a), "ok");
}

TEST(Stream, TimeoutBoundsAPipeRead) {
    Pipe p;
    protoio::setTimeout(p.r, 100);
    const auto start = std::chrono::steady_clock::now();
    try {
        protoio::readLine(p.r);
        FAIL() << "expected a time-out";
    } catch (const Error& e) {
        // A pipe is not a socket: the kind is FileSystem, the cause ETIMEDOUT.
        EXPECT_EQ(e.kind, Error::Kind::FileSystem);
        EXPECT_EQ(e.sysErrno, ETIMEDOUT);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    protoio::write(p.w, "ok\n");
    EXPECT_EQ(protoio::readLine(p.r), "ok");
}

#ifdef _WIN32
// The library reads every descriptor as bytes, whatever the C runtime's mode
// for it: standard input starts in text mode, where the C runtime would turn
// CR LF into LF and stop at a Ctrl+Z (0x1A) byte.
TEST(Stream, TextModeDescriptorsReadAsBytesOnWindows) {
    Pipe p;
    ::_setmode(p.r, _O_TEXT);
    const std::string data("a\r\nb\x1A" "c\n", 7);
    protoio::write(p.w, data);
    p.closeWriter();
    EXPECT_EQ(protoio::readAll(p.r), data);
}

namespace {

// Puts `text` in the console input buffer as typed keys.
void typeKeys(HANDLE in, const std::wstring& text) {
    std::vector<INPUT_RECORD> keys;
    for (wchar_t c : text) {
        for (BOOL down : {TRUE, FALSE}) {
            INPUT_RECORD r{};
            r.EventType = KEY_EVENT;
            r.Event.KeyEvent.bKeyDown = down;
            r.Event.KeyEvent.wRepeatCount = 1;
            // VK_PACKET: a character typed by its code, as SendInput does.
            r.Event.KeyEvent.wVirtualKeyCode = c == L'\r' ? VK_RETURN : VK_PACKET;
            r.Event.KeyEvent.uChar.UnicodeChar = c;
            keys.push_back(r);
        }
    }
    DWORD written = 0;
    ::WriteConsoleInputW(in, keys.data(), static_cast<DWORD>(keys.size()), &written);
}

} // namespace

// Console input: a read timeout applies to it as to a pipe, the text arrives
// as UTF-8 whatever the console's code page, and Ctrl+Z at the start of a
// line is the end of the input. The test needs a console of its own, so it
// runs its checks in a child copy of this program started with a new
// (hidden) console, and reports what the child reported.
TEST(Stream, ConsoleInputHonoursTimeoutsAndIsUtf8OnWindows) {
    if (::GetEnvironmentVariableW(L"PROTOIO_CONSOLE_CHILD", nullptr, 0) > 0) {
        const HANDLE in = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (in == INVALID_HANDLE_VALUE) GTEST_SKIP() << "no console input: error " << ::GetLastError();
        const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(in), _O_RDONLY | _O_BINARY);
        ASSERT_GE(fd, 0);
        protoio::forget(fd);
        // Each step reports its own failure, so one report shows them all.
        auto line = [&](const char* step) -> std::optional<std::string> {
            try {
                return protoio::readLine(fd);
            } catch (const Error& e) {
                ADD_FAILURE() << step << ": " << e.what();
                return std::string("<error>");
            }
        };
        protoio::setTimeout(fd, 5000);
        typeKeys(in, L"ab\r");
        EXPECT_EQ(line("typed ASCII"), "ab");

        protoio::setTimeout(fd, 300);
        const auto start = std::chrono::steady_clock::now();
        try {
            protoio::readLine(fd);
            ADD_FAILURE() << "a console read with a timeout did not time out";
        } catch (const Error& e) {
            EXPECT_EQ(e.kind, Error::Kind::FileSystem);
            EXPECT_EQ(e.sysErrno, ETIMEDOUT);
        }
        EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);

        protoio::setTimeout(fd, 5000);
        typeKeys(in, L"h\u00E9\u20AC\r");
        EXPECT_EQ(line("typed after a timeout"), "h\xC3\xA9\xE2\x82\xAC");  // "hé€"
        typeKeys(in, L"\x1A\r");
        EXPECT_EQ(line("Ctrl+Z"), std::nullopt);
        protoio::close(fd);
        return;
    }
    TempDir d;
    const std::string log = d / "child.txt";
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    const HANDLE out = ::CreateFileW(std::filesystem::path(std::u8string(log.begin(), log.end())).c_str(),
                                     GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, 0, nullptr);
    ASSERT_NE(out, INVALID_HANDLE_VALUE);
    wchar_t exe[MAX_PATH];
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) +
                       L"\" --gtest_filter=Stream.ConsoleInputHonoursTimeoutsAndIsUtf8OnWindows";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi{};
    ::SetEnvironmentVariableW(L"PROTOIO_CONSOLE_CHILD", L"1");
    const BOOL started = ::CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_NEW_CONSOLE, nullptr,
                                          nullptr, &si, &pi);
    ::SetEnvironmentVariableW(L"PROTOIO_CONSOLE_CHILD", nullptr);
    ::CloseHandle(out);
    ASSERT_TRUE(started) << "error " << ::GetLastError();
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 30000);
    if (waited != WAIT_OBJECT_0) ::TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    const std::string report = protoio::file::read(log);
    ASSERT_EQ(waited, WAIT_OBJECT_0) << "the console child hung:\n" << report;
    if (report.find("[  SKIPPED ]") != std::string::npos) GTEST_SKIP() << report;
    EXPECT_EQ(code, 0u) << report;
}
#endif

TEST(Stream, LargeTransferThroughAPipe) {
    Pipe p;
    std::string data;
    for (int i = 0; i < 300000; ++i) data.push_back(static_cast<char>('a' + i % 26));
    std::thread writer([&] { protoio::write(p.w, data); p.closeWriter(); });
    EXPECT_EQ(protoio::readAll(p.r), data);
    writer.join();
}

TEST(Stream, CloseTwiceIsHarmless) {
    Pipe p;
    protoio::close(p.w);
    EXPECT_NO_THROW(protoio::close(p.w));
    p.w = -1;
}
