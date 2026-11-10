// Files and directories against a temporary directory.
#include "protoio/digest.h"
#include "protoio/error.h"
#include "protoio/file.h"
#include "protoio/stream.h"
#include "test_support.h"

#include "protoio/process.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <algorithm>
#include <filesystem>
#include <io.h>
#else
#include <unistd.h>
#endif

using protoio::Error;
namespace file = protoio::file;
using protoio_test::TempDir;

namespace {

template <typename F>
Error::Kind kindOf(F f) {
    try {
        f();
    } catch (const Error& e) {
        return e.kind;
    }
    ADD_FAILURE() << "no Error was thrown";
    return Error::Kind::InvalidArgument;
}

} // namespace

TEST(File, WriteReadAppend) {
    TempDir d;
    const std::string f = d / "a.txt";
    file::write(f, "héllo\n");
    file::append(f, "more");
    EXPECT_EQ(file::read(f), "héllo\nmore");
    file::write(f, "replaced");
    EXPECT_EQ(file::read(f), "replaced");
    file::write(f, std::string("\0\x01\xff", 3));
    EXPECT_EQ(file::read(f), std::string("\0\x01\xff", 3));
}

TEST(File, NonAsciiNamesAreUtf8) {
    TempDir d;
    const std::string name = "\xC3\xB1" "and\xC3\xBA-\xE2\x82\xAC.txt";  // "ñandú-€.txt"
    file::write(d / name, "x");
    EXPECT_EQ(file::read(d / name), "x");
    EXPECT_EQ(file::list(d.path), std::vector<std::string>{name});
    ASSERT_TRUE(file::stat(d / name));
    EXPECT_TRUE(file::stat(d / name)->isFile);
    file::move(d / name, d / ("dir-" + name));
    EXPECT_EQ(file::read(d / ("dir-" + name)), "x");
}

TEST(File, ErrorsMapToKinds) {
    TempDir d;
    EXPECT_EQ(kindOf([&] { file::read(d / "missing"); }), Error::Kind::FileNotFound);
    EXPECT_EQ(kindOf([&] { file::open(d / "missing", file::Mode::Read); }), Error::Kind::FileNotFound);
    EXPECT_EQ(kindOf([&] { file::mkdir(d.path); }), Error::Kind::FileExists);
    EXPECT_EQ(kindOf([&] { file::read(d.path); }), Error::Kind::FileSystem);  // a directory
    EXPECT_EQ(kindOf([&] { file::list(d / "missing"); }), Error::Kind::FileNotFound);
    EXPECT_EQ(kindOf([&] { file::move(d / "missing", d / "b"); }), Error::Kind::FileNotFound);
    EXPECT_EQ(kindOf([&] { file::chdir(d / "missing"); }), Error::Kind::FileNotFound);
    try {
        file::read(d / "missing");
    } catch (const Error& e) {
        EXPECT_EQ(e.sysErrno, ENOENT);
        EXPECT_NE(std::string(e.what()).find("missing"), std::string::npos);
    }
}

TEST(File, Stat) {
    TempDir d;
    EXPECT_EQ(file::stat(d / "missing"), std::nullopt);
    file::write(d / "f", "12345");
    auto s = file::stat(d / "f");
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->isFile);
    EXPECT_FALSE(s->isDirectory);
    EXPECT_EQ(s->size, 5);
    EXPECT_GT(s->modifiedMs, 1'600'000'000'000LL);
    EXPECT_TRUE(s->readable);
    EXPECT_TRUE(s->writable);
    auto sd = file::stat(d.path);
    ASSERT_TRUE(sd);
    EXPECT_TRUE(sd->isDirectory);
}

TEST(File, DirectoriesListRemoveCopyMove) {
    TempDir d;
    file::mkdir(d / "x/y/z", true);
    file::mkdir(d / "x/y/z", true);  // parents: existing is fine
    file::write(d / "x/b.txt", "b");
    file::write(d / "x/a.txt", "a");
    EXPECT_EQ(file::list(d / "x"), (std::vector<std::string>{"a.txt", "b.txt", "y"}));

    file::copy(d / "x", d / "copy");
    EXPECT_EQ(file::read(d / "copy/a.txt"), "a");
    EXPECT_TRUE(file::stat(d / "copy/y/z"));

    file::move(d / "copy/a.txt", d / "moved.txt");
    EXPECT_EQ(file::read(d / "moved.txt"), "a");
    EXPECT_FALSE(file::stat(d / "copy/a.txt"));

    EXPECT_TRUE(file::remove(d / "moved.txt"));
    EXPECT_FALSE(file::remove(d / "moved.txt"));
    EXPECT_EQ(kindOf([&] { file::remove(d / "x"); }), Error::Kind::FileSystem);  // not empty
    EXPECT_TRUE(file::remove(d / "x", true));
    EXPECT_FALSE(file::stat(d / "x"));
}

#ifdef _WIN32
// absolute answers native separators on Windows.
std::string native(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

TEST(File, AbsoluteIsLexicalAndKeepsSymlinks) {
    TempDir d;
    file::mkdir(d / "real/sub", true);
    // A symbolic link needs a privilege (or developer mode); a junction,
    // which absolute must not resolve either, does not.
    std::error_code ec;
    std::filesystem::create_directory_symlink(d / "real", d / "link", ec);  // ASCII paths
    if (ec) {
        auto r = protoio::process::run({"cmd", "/c", "mklink", "/J", native(d / "link"), native(d / "real")});
        ASSERT_EQ(r.exitCode, 0) << r.out << r.err;
    }
    // "link/sub/.." folds to "link", not to the link's target.
    EXPECT_EQ(file::absolute(d / "link/sub/.."), native(d / "link"));
    EXPECT_EQ(file::absolute(d / "link/./sub/"), native(d / "link/sub"));
    const std::string root = file::absolute("/");
    EXPECT_EQ(root.size(), 3u) << root;  // e.g. "C:\"
    EXPECT_EQ(root.substr(1), ":\\");
    const std::string here = file::cwd();
    EXPECT_EQ(file::absolute("rel/x"), here + "\\rel\\x");
}
#else
TEST(File, AbsoluteIsLexicalAndKeepsSymlinks) {
    TempDir d;
    file::mkdir(d / "real/sub", true);
    ASSERT_EQ(::symlink((d / "real").c_str(), (d / "link").c_str()), 0);
    // "link/sub/.." folds to "link", not to the link's target.
    EXPECT_EQ(file::absolute(d / "link/sub/.."), d / "link");
    EXPECT_EQ(file::absolute(d / "link/./sub/"), d / "link/sub");
    EXPECT_EQ(file::absolute("/"), "/");
    const std::string here = file::cwd();
    EXPECT_EQ(file::absolute("rel/x"), here + "/rel/x");
}
#endif

TEST(File, CwdAndChdir) {
    TempDir d;
    const std::string before = file::cwd();
    file::chdir(d.path);
#ifdef _WIN32
    EXPECT_EQ(file::cwd(), file::absolute(d.path));
#else
    // The temporary directory may itself sit behind a symbolic link.
    EXPECT_EQ(file::cwd(), [&] { char b[4096]; return std::string(::realpath(d.path.c_str(), b)); }());
#endif
    file::chdir(before);
    EXPECT_EQ(file::cwd(), before);
    EXPECT_FALSE(file::tempDir().empty());
}

TEST(File, OpenGivesAStreamDescriptor) {
    TempDir d;
    int w = file::open(d / "s.txt", file::Mode::Write);
    protoio::write(w, "first\nsecond\n");
    protoio::close(w);
    int a = file::open(d / "s.txt", file::Mode::Append);
    protoio::write(a, "third\n");
    protoio::close(a);
    int r = file::open(d / "s.txt", file::Mode::Read);
    EXPECT_EQ(protoio::readLine(r), "first");
    EXPECT_EQ(protoio::readAll(r), "second\nthird\n");
    protoio::close(r);
}

// A file a stream has open can still be deleted, renamed and replaced by a
// rename, as on POSIX: the stream keeps the file it opened. On Windows this
// needs every descriptor opened with FILE_SHARE_DELETE and a rename that
// replaces its target.
TEST(File, AnOpenFileCanBeDeletedRenamedAndReplaced) {
    TempDir d;
    // Deleted while open for reading: the name is gone, the stream still reads.
    file::write(d / "a", "alpha\n");
    const int r = file::open(d / "a", file::Mode::Read);
    EXPECT_TRUE(file::remove(d / "a"));
    EXPECT_FALSE(file::stat(d / "a"));
    EXPECT_EQ(protoio::readAll(r), "alpha\n");
    protoio::close(r);

    // Replaced by a rename (the atomic-save pattern) while open for reading.
    file::write(d / "t", "old\n");
    const int rt = file::open(d / "t", file::Mode::Read);
    file::write(d / "t.new", "new\n");
    file::move(d / "t.new", d / "t");
    EXPECT_EQ(file::read(d / "t"), "new\n");
    EXPECT_EQ(protoio::readAll(rt), "old\n");
    protoio::close(rt);

    // Renamed while open for writing: the stream writes on under the new name.
    const int w = file::open(d / "w", file::Mode::Write);
    protoio::write(w, "one\n");
    file::move(d / "w", d / "w2");
    protoio::write(w, "two\n");
    protoio::close(w);
    EXPECT_EQ(file::read(d / "w2"), "one\ntwo\n");

    // Deleted while open for writing, and the name is free for a new file at once.
    const int h = file::open(d / "h", file::Mode::Write);
    EXPECT_TRUE(file::remove(d / "h"));
    file::write(d / "h", "fresh");
    EXPECT_EQ(file::read(d / "h"), "fresh");
    protoio::close(h);
}

TEST(File, ReopenedNumberStartsWithAFreshBuffer) {
    TempDir d;
    file::write(d / "one", "1a\n1b\n");
    file::write(d / "two", "2a\n");
    int r = file::open(d / "one", file::Mode::Read);
    EXPECT_EQ(protoio::readLine(r), "1a");  // "1b" stays buffered
#ifdef _WIN32
    ::_close(r);  // closed behind the library's back: its state for `r` is stale
#else
    ::close(r);  // closed behind the library's back: its state for `r` is stale
#endif
    int r2 = file::open(d / "two", file::Mode::Read);
    ASSERT_EQ(r2, r) << "the lowest free number is reused";
    EXPECT_EQ(protoio::readLine(r2), "2a");
    EXPECT_EQ(protoio::readLine(r2), std::nullopt);
    protoio::close(r2);
}

// ------------------------------------------- positioned and durable operations

namespace {

file::OpenOptions readWriteCreate() {
    return {.read = true, .write = true, .create = true};
}

} // namespace

TEST(File, OpenOptions_ExclusiveRefusesExisting) {
    TempDir d;
    const file::OpenOptions excl{.write = true, .create = true, .exclusive = true};
    const int fd = file::open(d / "x", excl);
    protoio::close(fd);
    EXPECT_EQ(kindOf([&] { file::open(d / "x", excl); }), Error::Kind::FileExists);
}

TEST(File, OpenOptions_MissingFileWithoutCreate) {
    TempDir d;
    EXPECT_EQ(kindOf([&] { file::open(d / "none", file::OpenOptions{.read = true}); }), Error::Kind::FileNotFound);
}

TEST(File, OpenOptions_TruncateAndKeep) {
    TempDir d;
    file::write(d / "t", "0123456789");
    // Read-write without truncation keeps the contents.
    int fd = file::open(d / "t", readWriteCreate());
    EXPECT_EQ(file::size(fd), 10);
    protoio::close(fd);
    // With truncation it starts empty.
    fd = file::open(d / "t", file::OpenOptions{.write = true, .truncate = true});
    EXPECT_EQ(file::size(fd), 0);
    protoio::close(fd);
}

TEST(File, ReadAt_WriteAt_RoundTrip) {
    using namespace std::string_literals;
    TempDir d;
    const int fd = file::open(d / "rw", readWriteCreate());
    file::writeAt(fd, 10, "abc\0def"s);
    EXPECT_EQ(file::readAt(fd, 10, 7), "abc\0def"s);
    EXPECT_EQ(file::size(fd), 17);
    // The gap before offset 10 reads as zero bytes.
    EXPECT_EQ(file::readAt(fd, 0, 10), std::string(10, '\0'));
    protoio::close(fd);
}

TEST(File, ReadAt_PastEnd) {
    TempDir d;
    file::write(d / "p", "0123456789");
    const int fd = file::open(d / "p", file::OpenOptions{.read = true});
    const std::int64_t size = file::size(fd);
    EXPECT_EQ(file::readAt(fd, size, 10), "");
    EXPECT_EQ(file::readAt(fd, size + 100, 10), "");
    EXPECT_EQ(file::readAt(fd, size - 2, 10).size(), 2u);
    EXPECT_EQ(file::readAt(fd, size - 2, 10), "89");
    EXPECT_EQ(kindOf([&] { file::readAt(fd, -1, 1); }), Error::Kind::InvalidArgument);
    protoio::close(fd);
}

TEST(File, WriteAt_Large) {
    TempDir d;
    std::string data(64u << 20, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 131u + (i >> 12)) & 0xff);
    const int fd = file::open(d / "big", readWriteCreate());
    file::writeAt(fd, 4096, data);
    EXPECT_EQ(file::size(fd), static_cast<std::int64_t>(4096 + data.size()));
    EXPECT_TRUE(file::readAt(fd, 4096, data.size()) == data);
    protoio::close(fd);
}

TEST(File, AppendData_AnswersStartOffset) {
    TempDir d;
    const int fd = file::open(d / "log", file::OpenOptions{.read = true, .write = true, .create = true, .append = true});
    EXPECT_EQ(file::appendData(fd, "first"), 0);
    EXPECT_EQ(file::appendData(fd, "second"), 5);
    EXPECT_EQ(file::readAt(fd, 0, 100), "firstsecond");
    protoio::close(fd);
}

// writeAt names an offset, which an append descriptor cannot honour (Linux's
// pwrite ignores it under O_APPEND and appends): it is refused everywhere.
TEST(File, WriteAt_OnAppendDescriptor_Throws) {
    TempDir d;
    const int fd = file::open(d / "log", file::OpenOptions{.read = true, .write = true, .create = true, .append = true});
    file::appendData(fd, "0123456789");
    EXPECT_EQ(kindOf([&] { file::writeAt(fd, 2, "xy"); }), Error::Kind::InvalidArgument);
    EXPECT_EQ(file::readAt(fd, 0, 100), "0123456789");
    protoio::close(fd);
    const int m = file::open(d / "log", file::Mode::Append);
    EXPECT_EQ(kindOf([&] { file::writeAt(m, 0, "z"); }), Error::Kind::InvalidArgument);
    protoio::close(m);
    // The same number reopened without append writes at the offset again.
    const int rw = file::open(d / "log", readWriteCreate());
    file::writeAt(rw, 2, "xy");
    EXPECT_EQ(file::readAt(rw, 0, 100), "01xy456789");
    protoio::close(rw);
}

// A corrupt length must not allocate (or throw std::length_error): the
// answer is capped by what the file holds.
TEST(File, ReadAt_HugeCount_AnswersFileBytes) {
    TempDir d;
    file::write(d / "small", "0123456789");
    const int fd = file::open(d / "small", file::OpenOptions{.read = true});
    std::string got;
    EXPECT_NO_THROW(got = file::readAt(fd, 0, std::numeric_limits<std::size_t>::max() / 2));
    EXPECT_EQ(got, "0123456789");
    EXPECT_EQ(file::readAt(fd, 4, std::numeric_limits<std::size_t>::max()), "456789");
    EXPECT_EQ(file::readAt(fd, 100, std::numeric_limits<std::size_t>::max()), "");
    protoio::close(fd);
}

// A write that reports 0 bytes for a non-zero request (which no regular file
// does on demand, so it cannot be provoked portably here) throws FileSystem
// with sysErrno EIO instead of looping, as ProtoBase's _write_all does.

// The ProtoBase storage pattern: ONE appender and concurrent readers on one
// descriptor. Each record describes itself (its offset, its length and the
// CRC32 of its payload); readers check published records and read at random
// offsets while the appender runs. On Windows a positioned read moves the
// file pointer, so an append that relied on it could land mid-file.
TEST(File, AppendData_OneAppenderConcurrentReaders) {
    constexpr int kRecords = 2000;
    constexpr int kReaders = 4;
    constexpr std::size_t kHeader = 16;  // offset u64, payload length u32, crc32 u32
    TempDir d;
    const int fd = file::open(d / "segment", file::OpenOptions{.read = true, .write = true, .create = true, .append = true});

    auto put = [](std::string& s, std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    auto get = [](const std::string& s, std::size_t at, int bytes) {
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
        return v;
    };
    // An empty string when the record at `offset` verifies, else what is wrong.
    auto verify = [&](std::int64_t offset) -> std::string {
        const std::string head = file::readAt(fd, offset, kHeader);
        if (head.size() != kHeader) return "short header at " + std::to_string(offset);
        if (static_cast<std::int64_t>(get(head, 0, 8)) != offset)
            return "record at " + std::to_string(offset) + " names offset " + std::to_string(get(head, 0, 8));
        const std::size_t len = static_cast<std::size_t>(get(head, 8, 4));
        const std::string payload = file::readAt(fd, offset + static_cast<std::int64_t>(kHeader), len);
        if (payload.size() != len) return "short payload at " + std::to_string(offset);
        if (protoio::digest::crc32(payload) != static_cast<std::uint32_t>(get(head, 12, 4)))
            return "bad crc at " + std::to_string(offset);
        return {};
    };

    std::vector<std::int64_t> offsets(kRecords, -1);
    std::atomic<int> published{0};
    std::atomic<bool> done{false};
    std::atomic<int> readerFailures{0};
    std::vector<std::string> readerErrors(kReaders);

    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&, r] {
            std::mt19937_64 rng(static_cast<std::uint64_t>(r) * 7919 + 1);
            try {
                while (!done.load(std::memory_order_acquire)) {
                    const std::int64_t sz = file::size(fd);
                    if (sz > 0) {
                        // A read at a random offset (moves the Windows file pointer).
                        (void)file::readAt(fd, static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(sz)), 1 + rng() % 512);
                    }
                    const int n = published.load(std::memory_order_acquire);
                    if (n > 0) {
                        const std::string err = verify(offsets[static_cast<std::size_t>(rng() % static_cast<std::uint64_t>(n))]);
                        if (!err.empty()) {
                            readerErrors[static_cast<std::size_t>(r)] = err;
                            readerFailures.fetch_add(1);
                            return;
                        }
                    }
                }
            } catch (const Error& e) {
                readerErrors[static_cast<std::size_t>(r)] = e.what();
                readerFailures.fetch_add(1);
            }
        });
    }

    std::mt19937_64 rng(42);
    std::int64_t expected = 0;
    std::string appendError;
    try {
        for (int i = 0; i < kRecords; ++i) {
            std::string payload(1 + rng() % 2000, '\0');
            for (char& c : payload) c = static_cast<char>(rng() & 0xff);
            std::string rec;
            put(rec, static_cast<std::uint64_t>(expected), 8);  // the offset it must land at
            put(rec, payload.size(), 4);
            put(rec, protoio::digest::crc32(payload), 4);
            rec += payload;
            const std::int64_t at = file::appendData(fd, rec);
            if (at != expected) {
                appendError = "record " + std::to_string(i) + " answered " + std::to_string(at) + ", expected " +
                              std::to_string(expected);
                break;
            }
            offsets[static_cast<std::size_t>(i)] = at;
            published.store(i + 1, std::memory_order_release);
            expected += static_cast<std::int64_t>(rec.size());
        }
    } catch (const Error& e) {
        appendError = e.what();
    }
    done.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();

    EXPECT_EQ(appendError, "");
    for (int r = 0; r < kReaders; ++r) EXPECT_EQ(readerErrors[static_cast<std::size_t>(r)], "") << "reader " << r;
    EXPECT_EQ(readerFailures.load(), 0);
    EXPECT_EQ(file::size(fd), expected);
    const int n = published.load();
    EXPECT_EQ(n, kRecords);
    for (int i = 0; i < n; ++i) {
        const std::string err = verify(offsets[static_cast<std::size_t>(i)]);
        if (!err.empty()) {
            ADD_FAILURE() << "record " << i << ": " << err;
            break;
        }
    }
    protoio::close(fd);
}

TEST(File, Truncate_Shrinks) {
    TempDir d;
    const int fd = file::open(d / "tr", readWriteCreate());
    file::writeAt(fd, 0, "0123456789");
    file::truncate(fd, 4);
    EXPECT_EQ(file::size(fd), 4);
    EXPECT_EQ(file::readAt(fd, 0, 100), "0123");
    protoio::close(fd);
    EXPECT_EQ(file::read(d / "tr"), "0123");
}

TEST(File, Sync_DataSync_SyncDirectory_DoNotThrow) {
    TempDir d;
    const int fd = file::open(d / "s", readWriteCreate());
    file::writeAt(fd, 0, "durable");
    EXPECT_NO_THROW(file::sync(fd));
    EXPECT_NO_THROW(file::dataSync(fd));
    EXPECT_NO_THROW(file::syncDirectory(d.path));
    protoio::close(fd);
}

#ifndef _WIN32
TEST(File, SyncDirectory_MissingPath) {
    TempDir d;
    EXPECT_EQ(kindOf([&] { file::syncDirectory(d / "missing"); }), Error::Kind::FileNotFound);
}
#endif

TEST(File, Replace_OverwritesTarget) {
    TempDir d;
    file::write(d / "target", "old");
    file::write(d / "source", "new");
    file::replace(d / "source", d / "target");
    EXPECT_EQ(file::read(d / "target"), "new");
    EXPECT_FALSE(file::stat(d / "source"));
}

TEST(File, TryLock_SecondDescriptorSameProcess_False) {
    TempDir d;
    const int a = file::open(d / "lock", readWriteCreate());
    const int b = file::open(d / "lock", readWriteCreate());
    EXPECT_TRUE(file::tryLock(a));
    EXPECT_FALSE(file::tryLock(b));
    file::unlock(a);
    EXPECT_TRUE(file::tryLock(b));
    file::unlock(b);
    // The blocking form takes a free lock at once.
    file::lock(a);
    EXPECT_FALSE(file::tryLock(b));
    file::unlock(a);
    protoio::close(a);
    protoio::close(b);
}

namespace {

// The command that runs a Python 3 interpreter, or an empty vector when none
// is found: python3, then python, then the Windows launcher's py -3.
std::vector<std::string> findPython3() {
    const std::vector<std::vector<std::string>> candidates = {{"python3"}, {"python"}, {"py", "-3"}};
    for (const auto& c : candidates) {
        std::vector<std::string> argv = c;
        argv.insert(argv.end(), {"-c", "import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)"});
        try {
            if (protoio::process::run(argv).exitCode == 0) return c;
        } catch (const Error&) {
            // Not installed under this name; try the next one.
        }
    }
    return {};
}

std::vector<std::string> withArgs(std::vector<std::string> argv, std::initializer_list<std::string> args) {
    argv.insert(argv.end(), args);
    return argv;
}

// Stops a lock peer (creates its stop file) and waits for it on every exit
// path of a test, so a failed assertion never leaves it holding the lock.
class PeerGuard {
public:
    PeerGuard(std::string stop, int pid) : stop_(std::move(stop)), pid_(pid) {}
    PeerGuard(const PeerGuard&) = delete;
    PeerGuard& operator=(const PeerGuard&) = delete;
    ~PeerGuard() {
        if (waited_) return;
        try {
            finish();
        } catch (const Error&) {
            // Nothing more can be done in a destructor.
        }
    }
    // Stops the peer and answers its exit code.
    int finish() {
        waited_ = true;
        file::write(stop_, "");
        return protoio::process::wait(pid_);
    }

private:
    std::string stop_;
    int pid_;
    bool waited_ = false;
};

} // namespace

// The lock must exclude ProtoBase's OwnerLock (proto_db/storage/ownerlock.py),
// which a Python peer takes exactly as ProtoBase does.
TEST(File, TryLock_ExcludesProtoBasePeer) {
    const std::vector<std::string> python = findPython3();
    if (python.empty()) GTEST_SKIP() << "no Python 3 found (python3, python, py -3): ProtoBase lock interop NOT verified";

    TempDir d;
    const std::string lockPath = d / "owner.lock";
    const std::string ready = d / "ready";
    const std::string stop = d / "stop";
    PeerGuard peer(stop, protoio::process::spawn(withArgs(python, {PROTOIO_OWNERLOCK_PEER, lockPath, ready, stop})));

    bool isReady = false;
    for (int i = 0; i < 200 && !isReady; ++i) {  // 10 s
        if (file::stat(ready)) isReady = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!isReady) FAIL() << "the ProtoBase lock peer did not report the lock within 10 s";

    const int fd = file::open(lockPath, readWriteCreate());
    EXPECT_FALSE(file::tryLock(fd)) << "the lock does not exclude ProtoBase's OwnerLock";
    EXPECT_EQ(peer.finish(), 0);
    EXPECT_TRUE(file::tryLock(fd));
    file::unlock(fd);
    protoio::close(fd);
}

// The reverse direction: while protoIO holds the lock, ProtoBase's OwnerLock
// cannot be taken (the peer exits 2 when the lock is busy).
TEST(File, TryLock_ProtoBasePeerExcluded) {
    const std::vector<std::string> python = findPython3();
    if (python.empty()) GTEST_SKIP() << "no Python 3 found (python3, python, py -3): ProtoBase lock interop NOT verified";

    TempDir d;
    const std::string lockPath = d / "owner.lock";
    const std::string ready = d / "ready";
    const std::string stop = d / "stop";
    // The stop file exists up front, so a peer that wrongly gets the lock
    // releases it and exits 0 at once instead of holding it.
    file::write(stop, "");
    const int fd = file::open(lockPath, readWriteCreate());
    ASSERT_TRUE(file::tryLock(fd));
    const int busy = protoio::process::run(withArgs(python, {PROTOIO_OWNERLOCK_PEER, lockPath, ready, stop})).exitCode;
    EXPECT_EQ(busy, 2) << "ProtoBase's OwnerLock was not excluded by protoIO's lock";
    EXPECT_FALSE(file::stat(ready));
    file::unlock(fd);
    // Released, the peer takes it.
    EXPECT_EQ(protoio::process::run(withArgs(python, {PROTOIO_OWNERLOCK_PEER, lockPath, ready, stop})).exitCode, 0);
    EXPECT_TRUE(file::stat(ready));
    protoio::close(fd);
}

// ------------------------------------------------- NUL bytes in path arguments

namespace {

// `prefix` followed by a NUL byte and more: the system would read only `prefix`.
std::string withNul(const std::string& prefix) { return prefix + std::string("\0tail", 5); }

// The function throws InvalidArgument, naming itself and the argument.
template <typename F>
void expectNulRefused(F f, const std::string& message) {
    try {
        f();
        ADD_FAILURE() << "no Error was thrown; expected: " << message;
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::InvalidArgument) << e.what();
        EXPECT_EQ(std::string(e.what()), message);
    }
}

} // namespace

// A path with a NUL byte used to name its prefix ("a\0b" opened "a"). Each
// call is refused before any system call: nothing named by the prefix is
// created, read, changed or removed.
TEST(File, NulInPath_CreatesNothing) {
    TempDir d;
    const std::string pre = d / "pre";
    expectNulRefused([&] { file::open(withNul(pre), file::Mode::Write); }, "file::open: the path contains a NUL byte");
    expectNulRefused([&] { file::open(withNul(pre), readWriteCreate()); }, "file::open: the path contains a NUL byte");
    expectNulRefused([&] { file::write(withNul(pre), "data"); }, "file::write: the path contains a NUL byte");
    expectNulRefused([&] { file::append(withNul(pre), "data"); }, "file::append: the path contains a NUL byte");
    expectNulRefused([&] { file::mkdir(withNul(pre)); }, "file::mkdir: the path contains a NUL byte");
    expectNulRefused([&] { file::mkdir(withNul(pre), true); }, "file::mkdir: the path contains a NUL byte");
    EXPECT_FALSE(file::stat(pre).has_value());
    EXPECT_TRUE(file::list(d.path).empty());
}

TEST(File, NulInPath_TouchesNoExistingFile) {
    TempDir d;
    const std::string pre = d / "pre";
    file::write(pre, "original");
    expectNulRefused([&] { file::open(withNul(pre), file::Mode::Read); }, "file::open: the path contains a NUL byte");
    expectNulRefused([&] { file::read(withNul(pre)); }, "file::read: the path contains a NUL byte");
    expectNulRefused([&] { file::stat(withNul(pre)); }, "file::stat: the path contains a NUL byte");
    expectNulRefused([&] { file::remove(withNul(pre)); }, "file::remove: the path contains a NUL byte");
    expectNulRefused([&] { file::remove(withNul(d.path), true); }, "file::remove: the path contains a NUL byte");
    expectNulRefused([&] { file::list(withNul(d.path)); }, "file::list: the path contains a NUL byte");
    expectNulRefused([&] { file::absolute(withNul(pre)); }, "file::absolute: the path contains a NUL byte");
    expectNulRefused([&] { file::syncDirectory(withNul(d.path)); }, "file::syncDirectory: the path contains a NUL byte");
    const std::string other = d / "other";
    expectNulRefused([&] { file::move(withNul(pre), other); }, "file::move: the source path contains a NUL byte");
    expectNulRefused([&] { file::move(pre, withNul(other)); }, "file::move: the target path contains a NUL byte");
    expectNulRefused([&] { file::replace(withNul(pre), other); }, "file::replace: the source path contains a NUL byte");
    expectNulRefused([&] { file::replace(pre, withNul(other)); }, "file::replace: the target path contains a NUL byte");
    expectNulRefused([&] { file::copy(withNul(pre), other); }, "file::copy: the source path contains a NUL byte");
    expectNulRefused([&] { file::copy(pre, withNul(other)); }, "file::copy: the target path contains a NUL byte");
    EXPECT_EQ(file::read(pre), "original");
    EXPECT_EQ(file::list(d.path), std::vector<std::string>{"pre"});
}

TEST(File, NulInPath_ChdirKeepsTheWorkingDirectory) {
    TempDir d;
    const std::string before = file::cwd();
    expectNulRefused([&] { file::chdir(withNul(d.path)); }, "file::chdir: the path contains a NUL byte");
    EXPECT_EQ(file::cwd(), before);
}

// File contents are data: NUL bytes in them are written as they are.
TEST(File, NulInData_IsWritten) {
    TempDir d;
    const std::string data("a\0b", 3);
    file::write(d / "w", data);
    file::append(d / "w", data);
    EXPECT_EQ(file::read(d / "w"), data + data);
    const int fd = file::open(d / "p", readWriteCreate());
    file::EndGuard guard;
    file::writeAt(fd, 0, data);
    file::writeAt(fd, guard, 3, data);
    EXPECT_EQ(file::appendData(fd, guard, data), 6);
    EXPECT_EQ(file::readAt(fd, 0, 100), data + data + data);
    protoio::close(fd);
}

// ------------------------------------------------------------ the end guard
//
// The tests hold a claim with EndGuard::Claim, so the conflicting call meets
// it deterministically: no second thread, no timing.

namespace {

Error conflictOf(const std::function<void()>& f) {
    try {
        f();
    } catch (const Error& e) {
        return e;
    }
    ADD_FAILURE() << "no Error was thrown";
    return Error(Error::Kind::Internal, "none");
}

} // namespace

TEST(File, Conflict_HasAName) {
    EXPECT_STREQ(protoio::kindName(Error::Kind::Conflict), "Conflict");
}

TEST(File, EndGuard_States) {
    file::EndGuard g;
    EXPECT_EQ(g.state(), 0);
    {
        file::EndGuard::Claim a(g, file::EndGuard::Mode::Shared, "test");
        EXPECT_EQ(g.state(), 1);
        file::EndGuard::Claim b(g, file::EndGuard::Mode::Shared, "test");
        EXPECT_EQ(g.state(), 2);
        // Exclusive needs the guard idle.
        const Error e = conflictOf([&] { file::EndGuard::Claim c(g, file::EndGuard::Mode::Exclusive, "mine"); });
        EXPECT_EQ(e.kind, Error::Kind::Conflict);
        EXPECT_EQ(std::string(e.what()).rfind("mine: ", 0), 0u) << e.what();
        EXPECT_EQ(g.state(), 2);
    }
    EXPECT_EQ(g.state(), 0);
    {
        file::EndGuard::Claim x(g, file::EndGuard::Mode::Exclusive, "test");
        EXPECT_EQ(g.state(), -1);
        EXPECT_EQ(conflictOf([&] { file::EndGuard::Claim c(g, file::EndGuard::Mode::Shared, "t"); }).kind,
                  Error::Kind::Conflict);
        EXPECT_EQ(conflictOf([&] { file::EndGuard::Claim c(g, file::EndGuard::Mode::Exclusive, "t"); }).kind,
                  Error::Kind::Conflict);
        EXPECT_EQ(g.state(), -1);
    }
    EXPECT_EQ(g.state(), 0);
}

TEST(File, EndGuard_GuardedCallsWhenIdle) {
    TempDir d;
    const int fd = file::open(d / "g", readWriteCreate());
    file::EndGuard g;
    EXPECT_EQ(file::appendData(fd, g, "0123"), 0);
    file::writeAt(fd, g, 4, "4567");  // extends
    file::writeAt(fd, g, 0, "ab");    // inside
    EXPECT_EQ(file::appendData(fd, g, "89"), 8);
    file::truncate(fd, g, 6);
    EXPECT_EQ(file::readAt(fd, 0, 100), "ab2345");
    EXPECT_EQ(g.state(), 0);
    protoio::close(fd);
}

// An append or a truncate in progress: appendData, truncate and an extending
// writeAt are refused and change nothing; a writeAt inside the file goes on.
TEST(File, EndGuard_ExclusiveHeld_RefusesWhatMovesTheEnd) {
    TempDir d;
    const int fd = file::open(d / "g", readWriteCreate());
    file::EndGuard g;
    file::writeAt(fd, g, 0, "0123456789");
    {
        file::EndGuard::Claim held(g, file::EndGuard::Mode::Exclusive, "test");
        const Error a = conflictOf([&] { file::appendData(fd, g, "xyz"); });
        EXPECT_EQ(a.kind, Error::Kind::Conflict);
        EXPECT_STREQ(a.what(), "appendData: another operation that moves the end of the file is in progress "
                               "(append and truncate need it alone)");
        const Error t = conflictOf([&] { file::truncate(fd, g, 2); });
        EXPECT_EQ(t.kind, Error::Kind::Conflict);
        EXPECT_STREQ(t.what(), "truncate: another operation that moves the end of the file is in progress "
                               "(append and truncate need it alone)");
        const Error w = conflictOf([&] { file::writeAt(fd, g, 8, "abcd"); });
        EXPECT_EQ(w.kind, Error::Kind::Conflict);
        EXPECT_STREQ(w.what(), "writeAt: an append or a truncate is in progress on the file "
                               "(it needs the end of the file alone)");
        EXPECT_EQ(conflictOf([&] { file::writeAt(fd, g, 20, "x"); }).kind, Error::Kind::Conflict);
        file::writeAt(fd, g, 6, "abcd");  // ends exactly at the end: inside
        EXPECT_EQ(g.state(), -1);
    }
    EXPECT_EQ(file::readAt(fd, 0, 100), "012345abcd");
    EXPECT_EQ(g.state(), 0);
    protoio::close(fd);
}

// Extending writes in progress: more extending writes join them; appendData
// and truncate are refused.
TEST(File, EndGuard_SharedHeld_RefusesAppendAndTruncate) {
    TempDir d;
    const int fd = file::open(d / "g", readWriteCreate());
    file::EndGuard g;
    file::writeAt(fd, g, 0, "0123");
    {
        file::EndGuard::Claim held(g, file::EndGuard::Mode::Shared, "test");
        EXPECT_EQ(conflictOf([&] { file::appendData(fd, g, "x"); }).kind, Error::Kind::Conflict);
        EXPECT_EQ(conflictOf([&] { file::truncate(fd, g, 0); }).kind, Error::Kind::Conflict);
        file::writeAt(fd, g, 4, "4567");
        EXPECT_EQ(g.state(), 1);
    }
    EXPECT_EQ(file::readAt(fd, 0, 100), "01234567");
    EXPECT_EQ(file::size(fd), 8);
    protoio::close(fd);
}

// A claim is released when the guarded call fails too, and arguments are
// validated before any claim.
TEST(File, EndGuard_ReleasedOnFailure) {
    TempDir d;
    file::EndGuard g;
    const int ap = file::open(d / "a", file::OpenOptions{.read = true, .write = true, .create = true, .append = true});
    EXPECT_EQ(kindOf([&] { file::writeAt(ap, g, 0, "x"); }), Error::Kind::InvalidArgument);  // append descriptor
    EXPECT_EQ(g.state(), 0);
    protoio::close(ap);
    const int ro = file::open(d / "a", file::Mode::Read);
    EXPECT_NE(kindOf([&] { file::appendData(ro, g, "x"); }), Error::Kind::Conflict);  // not writable
    EXPECT_EQ(g.state(), 0);
    EXPECT_NE(kindOf([&] { file::truncate(ro, g, 0); }), Error::Kind::Conflict);
    EXPECT_EQ(g.state(), 0);
    {
        file::EndGuard::Claim held(g, file::EndGuard::Mode::Exclusive, "test");
        EXPECT_EQ(kindOf([&] { file::writeAt(ro, g, -1, "x"); }), Error::Kind::InvalidArgument);
        EXPECT_EQ(kindOf([&] { file::truncate(ro, g, -1); }), Error::Kind::InvalidArgument);
    }
    EXPECT_EQ(g.state(), 0);
    protoio::close(ro);
}

// The guard is per EndGuard object: another guard on the same descriptor,
// and the unguarded calls, do not see it.
TEST(File, EndGuard_GuardsOnlyItsOwnCallers) {
    TempDir d;
    const int fd = file::open(d / "g", readWriteCreate());
    file::EndGuard g, other;
    file::EndGuard::Claim held(g, file::EndGuard::Mode::Exclusive, "test");
    EXPECT_EQ(file::appendData(fd, other, "ab"), 0);
    EXPECT_EQ(file::appendData(fd, "cd"), 2);
    file::truncate(fd, 3);
    EXPECT_EQ(file::readAt(fd, 0, 100), "abc");
    protoio::close(fd);
}

// Racing appenders on one guard: whatever the interleaving, each call either
// lands its record alone or is refused with Conflict and writes nothing, so
// every answered offset holds exactly its own record and the file holds
// nothing else. (No timing: the assertions hold for every interleaving.)
TEST(File, EndGuard_RacingAppenders_NeverShareAnOffset) {
    TempDir d;
    const int fd = file::open(d / "race", readWriteCreate());
    file::EndGuard g;
    constexpr int kThreads = 4, kCalls = 300;
    std::vector<std::vector<std::pair<std::int64_t, std::string>>> landed(kThreads);
    std::atomic<int> refused{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kCalls; ++i) {
                char rec[9];
                std::snprintf(rec, sizeof rec, "%c%07d", 'a' + t, i);
                try {
                    landed[t].emplace_back(file::appendData(fd, g, std::string_view(rec, 8)), std::string(rec, 8));
                } catch (const Error& e) {
                    EXPECT_EQ(e.kind, Error::Kind::Conflict);
                    ++refused;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    std::size_t total = 0;
    for (const auto& v : landed) {
        for (const auto& [offset, rec] : v) {
            EXPECT_EQ(offset % 8, 0);
            EXPECT_EQ(file::readAt(fd, offset, 8), rec);
        }
        total += v.size();
    }
    EXPECT_EQ(total + static_cast<std::size_t>(refused.load()), static_cast<std::size_t>(kThreads * kCalls));
    EXPECT_EQ(file::size(fd), static_cast<std::int64_t>(total * 8));
    EXPECT_EQ(g.state(), 0);
    protoio::close(fd);
}
