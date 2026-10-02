// Files and directories against a temporary directory.
#include "protoio/error.h"
#include "protoio/file.h"
#include "protoio/stream.h"
#include "test_support.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <algorithm>
#include <filesystem>
#include <io.h>
#include "protoio/process.h"
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
