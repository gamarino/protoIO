// Files and directories against a temporary directory.
#include "protoio/error.h"
#include "protoio/file.h"
#include "protoio/stream.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <unistd.h>

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

TEST(File, CwdAndChdir) {
    TempDir d;
    const std::string before = file::cwd();
    file::chdir(d.path);
    // The temporary directory may itself sit behind a symbolic link.
    EXPECT_EQ(file::cwd(), [&] { char b[4096]; return std::string(::realpath(d.path.c_str(), b)); }());
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

TEST(File, ReopenedNumberStartsWithAFreshBuffer) {
    TempDir d;
    file::write(d / "one", "1a\n1b\n");
    file::write(d / "two", "2a\n");
    int r = file::open(d / "one", file::Mode::Read);
    EXPECT_EQ(protoio::readLine(r), "1a");  // "1b" stays buffered
    ::close(r);  // closed behind the library's back: its state for `r` is stale
    int r2 = file::open(d / "two", file::Mode::Read);
    ASSERT_EQ(r2, r) << "the lowest free number is reused";
    EXPECT_EQ(protoio::readLine(r2), "2a");
    EXPECT_EQ(protoio::readLine(r2), std::nullopt);
    protoio::close(r2);
}
