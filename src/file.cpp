// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Files and directories. Moved from protoST's src/primitives/io_prims.cpp.
#include "protoio/file.h"

#include "protoio/stream.h"
#include "internal.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace protoio::file {

namespace fs = std::filesystem;
using detail::fileError;
using detail::checkNoNul;

namespace {

[[noreturn]] void fsError(const fs::filesystem_error& e, const char* action) {
    fileError(e.path1().string(), e.code().value(), action);
}

void writeWhole(const std::string& path, const std::string& data, bool append) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) fileError(path, errno, "cannot write");
    SigpipeGuard noSigpipe;  // the path may name a FIFO
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = ::write(fd, data.data() + done, data.size() - done);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) { const int e = errno; ::close(fd); fileError(path, e, "cannot write"); }
        if (w == 0) { ::close(fd); fileError(path, EIO, "cannot write"); }  // no progress
        done += static_cast<std::size_t>(w);
    }
    if (::close(fd) != 0 && errno != EINTR) fileError(path, errno, "cannot write");
}

// The name a descriptor goes by in an error message.
std::string fdName(int fd) { return "descriptor " + std::to_string(fd); }

[[noreturn]] void invalid(const std::string& message) { throw Error(Error::Kind::InvalidArgument, message); }

void checkOffset(std::int64_t offset, const char* what) {
    if (offset < 0) invalid(std::string(what) + ": negative offset " + std::to_string(offset));
}

// The largest count one read(2)/write(2) is asked for: Linux moves at most
// 0x7ffff000 bytes per call anyway, and macOS refuses counts above INT_MAX.
constexpr std::size_t kMaxChunk = 1u << 30;

} // namespace

int open(const std::string& path, Mode mode) {
    checkNoNul(path, "file::open", "the path");
    int flags = O_CLOEXEC;
    switch (mode) {
        case Mode::Read: flags |= O_RDONLY; break;
        case Mode::Write: flags |= O_WRONLY | O_CREAT | O_TRUNC; break;
        case Mode::Append: flags |= O_WRONLY | O_CREAT | O_APPEND; break;
        case Mode::ReadWrite: flags |= O_RDWR | O_CREAT; break;
    }
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) fileError(path, errno, "cannot open");
    protoio::forget(fd);  // a stale state for a reused number must not survive
    return fd;
}

std::string read(const std::string& path) {
    checkNoNul(path, "file::read", "the path");
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) fileError(path, errno, "cannot read");
    struct stat st{};
    if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { ::close(fd); fileError(path, EISDIR, "cannot read"); }
    std::string out;
    char chunk[65536];
    for (;;) {
        const ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { const int e = errno; ::close(fd); fileError(path, e, "cannot read"); }
        if (n == 0) break;
        out.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

void write(const std::string& path, const std::string& data) {
    checkNoNul(path, "file::write", "the path");  // `data` may hold NUL bytes
    writeWhole(path, data, false);
}

void append(const std::string& path, const std::string& data) {
    checkNoNul(path, "file::append", "the path");  // `data` may hold NUL bytes
    writeWhole(path, data, true);
}

std::optional<Stat> stat(const std::string& path) {
    // No file can have a name with a NUL byte (see file.h); answered without
    // a system call, which would read the path only up to that byte.
    if (path.find('\0') != std::string::npos) return std::nullopt;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
    Stat s;
    s.isFile = S_ISREG(st.st_mode);
    s.isDirectory = S_ISDIR(st.st_mode);
    s.size = static_cast<std::int64_t>(st.st_size);
    #if defined(__APPLE__)
    s.modifiedMs = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
    s.modifiedMs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
    s.readable = ::access(path.c_str(), R_OK) == 0;
    s.writable = ::access(path.c_str(), W_OK) == 0;
    return s;
}

bool remove(const std::string& path, bool recursive) {
    checkNoNul(path, "file::remove", "the path");
    try {
        return recursive ? fs::remove_all(path) > 0 : fs::remove(path);
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot delete");
    }
}

void move(const std::string& from, const std::string& to) {
    checkNoNul(from, "file::move", "the source path");
    checkNoNul(to, "file::move", "the target path");
    try {
        fs::rename(from, to);
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot move");
    }
}

void copy(const std::string& from, const std::string& to) {
    checkNoNul(from, "file::copy", "the source path");
    checkNoNul(to, "file::copy", "the target path");
    try {
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot copy");
    }
}

void mkdir(const std::string& path, bool parents) {
    checkNoNul(path, "file::mkdir", "the path");
    int err = 0;
    if (parents) {
        std::error_code ec;
        fs::create_directories(path, ec);
        if (ec) err = ec.value();
    } else if (::mkdir(path.c_str(), 0755) != 0) {
        err = errno;
    }
    if (err) fileError(path, err, "cannot create directory");
}

std::vector<std::string> list(const std::string& path) {
    checkNoNul(path, "file::list", "the path");
    std::vector<std::string> names;
    try {
        for (const auto& e : fs::directory_iterator(path)) names.push_back(e.path().filename().string());
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot list");
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string absolute(const std::string& path) {
    checkNoNul(path, "file::absolute", "the path");
    // Lexical only: "." and ".." are folded and symbolic links are kept, as
    // Pharo's fullName does.
    std::error_code ec;
    const fs::path p = fs::absolute(path, ec);
    if (ec) fileError(path, ec.value(), "cannot resolve");
    std::string s = p.lexically_normal().string();
    if (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string tempDir() {
    std::error_code ec;
    const fs::path p = fs::temp_directory_path(ec);
    if (ec) return std::string("/tmp");
    // macOS's $TMPDIR ends with a separator; Linux's /tmp does not. Drop it, so
    // `tempDir() + "/name"` is the same path everywhere.
    std::string s = p.string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string cwd() {
    std::error_code ec;
    const fs::path p = fs::current_path(ec);
    if (ec) fileError(".", ec.value(), "cannot read the working directory");
    return p.string();
}

void chdir(const std::string& path) {
    checkNoNul(path, "file::chdir", "the path");
    if (::chdir(path.c_str()) != 0) fileError(path, errno, "cannot change directory to");
}

// ------------------------------------------- positioned and durable operations

int open(const std::string& path, const OpenOptions& options) {
    checkNoNul(path, "file::open", "the path");
    const bool write = options.write || options.append;
    if (!options.read && !write) invalid("cannot open " + path + ": neither read nor write was asked for");
    if (options.exclusive && !options.create) invalid("cannot open " + path + ": exclusive needs create");
    if (options.truncate && !write) invalid("cannot open " + path + ": truncate needs write");
    int flags = O_CLOEXEC | (options.read && write ? O_RDWR : write ? O_WRONLY : O_RDONLY);
    if (options.create) flags |= O_CREAT;
    if (options.exclusive) flags |= O_EXCL;
    if (options.truncate) flags |= O_TRUNC;
    if (options.append) flags |= O_APPEND;
    int fd;
    do fd = ::open(path.c_str(), flags, static_cast<mode_t>(options.permissions));
    while (fd < 0 && errno == EINTR);
    if (fd < 0) fileError(path, errno, "cannot open");
    protoio::forget(fd);  // a stale state for a reused number must not survive
    return fd;
}

std::string readAt(int fd, std::int64_t offset, std::size_t n) {
    checkOffset(offset, "cannot read at");
    // Allocate no more than a regular file holds from `offset` on, so a
    // corrupt length is neither zero-filled nor a std::length_error; fewer
    // bytes are answered, as at any end of the file.
    struct stat st{};
    if (::fstat(fd, &st) != 0) fileError(fdName(fd), errno, "cannot read");
    if (S_ISREG(st.st_mode)) {
        const std::int64_t left = std::max<std::int64_t>(0, static_cast<std::int64_t>(st.st_size) - offset);
        // Compare in 64 bits: narrowing first would wrap on a 32-bit size_t and cut
        // n below the true remainder, a short read before the end of the file.
        if (static_cast<std::uint64_t>(left) < static_cast<std::uint64_t>(n)) n = static_cast<std::size_t>(left);
    }
    std::string out(n, '\0');
    std::size_t done = 0;
    while (done < n) {
        const ssize_t r = ::pread(fd, out.data() + done, std::min(n - done, kMaxChunk),
                                  static_cast<off_t>(offset + static_cast<std::int64_t>(done)));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) fileError(fdName(fd), errno, "cannot read");
        if (r == 0) break;  // the end of the file
        done += static_cast<std::size_t>(r);
    }
    out.resize(done);
    return out;
}

void writeAt(int fd, std::int64_t offset, std::string_view data) {
    checkOffset(offset, "cannot write at");
    // pwrite may append and ignore the offset on an O_APPEND descriptor
    // (Linux does; other systems may): refused on every platform.
    const int fl = ::fcntl(fd, F_GETFL);
    if (fl < 0) fileError(fdName(fd), errno, "cannot write");
    if (fl & O_APPEND) invalid("cannot write at an offset of " + fdName(fd) + ": it was opened to append");
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = ::pwrite(fd, data.data() + done, std::min(data.size() - done, kMaxChunk),
                                   static_cast<off_t>(offset + static_cast<std::int64_t>(done)));
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) fileError(fdName(fd), errno, "cannot write");
        if (w == 0) fileError(fdName(fd), EIO, "cannot write");  // no progress: never loop forever
        done += static_cast<std::size_t>(w);
    }
}

std::int64_t appendData(int fd, std::string_view data) {
    // Valid under the one-appender contract: nobody else moves the end
    // between this answer and the write. The write names its offset (pwrite)
    // and never uses the file position, which a stream read or write on the
    // same descriptor moves, as file_win.cpp does. On an O_APPEND descriptor
    // pwrite may append and ignore the offset (Linux does; other systems
    // may). Either way the bytes land at `start` + what was written so far:
    // under the contract that offset IS the end of the file, so appending
    // there and writing there are the same. The end is read with fstat, not
    // lseek(SEEK_END), so the file position is not moved either.
    struct stat st{};
    if (::fstat(fd, &st) != 0) fileError(fdName(fd), errno, "cannot append to");
    // Only a regular file has an end fstat reports: a block device reports
    // size 0 (the append would overwrite its first bytes), and a pipe, FIFO,
    // socket or terminal has no offsets at all. Refused before any write.
    if (!S_ISREG(st.st_mode))
        throw Error(Error::Kind::FileSystem,
                    "cannot append to " + fdName(fd) + ": not a regular file (appendData needs one)", ESPIPE);
    const off_t start = st.st_size;
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = ::pwrite(fd, data.data() + done, std::min(data.size() - done, kMaxChunk),
                                   static_cast<off_t>(start + static_cast<off_t>(done)));
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) fileError(fdName(fd), errno, "cannot append to");
        if (w == 0) fileError(fdName(fd), EIO, "cannot append to");  // no progress: never loop forever
        done += static_cast<std::size_t>(w);
    }
    return static_cast<std::int64_t>(start);
}

std::int64_t size(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) fileError(fdName(fd), errno, "cannot read the size of");
    return static_cast<std::int64_t>(st.st_size);
}

void truncate(int fd, std::int64_t newSize) {
    if (newSize < 0) invalid("cannot truncate " + fdName(fd) + ": negative size " + std::to_string(newSize));
    int r;
    do r = ::ftruncate(fd, static_cast<off_t>(newSize));
    while (r != 0 && errno == EINTR);
    if (r != 0) fileError(fdName(fd), errno, "cannot truncate");
}

void sync(int fd) {
    int r;
    do r = ::fsync(fd);
    while (r != 0 && errno == EINTR);
    if (r != 0) fileError(fdName(fd), errno, "cannot sync");
}

void dataSync(int fd) {
    int r;
#if defined(__APPLE__)
    do r = ::fsync(fd);  // macOS has no fdatasync
    while (r != 0 && errno == EINTR);
#else
    do r = ::fdatasync(fd);
    while (r != 0 && errno == EINTR);
#endif
    if (r != 0) fileError(fdName(fd), errno, "cannot sync");
}

void syncDirectory(const std::string& path) {
    checkNoNul(path, "file::syncDirectory", "the path");
    int fd;
    do fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) fileError(path, errno, "cannot sync directory");
    int r;
    do r = ::fsync(fd);
    while (r != 0 && errno == EINTR);
    const int e = errno;
    ::close(fd);
    if (r != 0) fileError(path, e, "cannot sync directory");
}

void replace(const std::string& from, const std::string& to) {
    checkNoNul(from, "file::replace", "the source path");
    checkNoNul(to, "file::replace", "the target path");
    move(from, to);
}

bool tryLock(int fd) {
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
        if (errno == EINTR) continue;
        if (errno == EWOULDBLOCK) return false;
        fileError(fdName(fd), errno, "cannot lock");
    }
}

void lock(int fd) {
    int r;
    do r = ::flock(fd, LOCK_EX);
    while (r != 0 && errno == EINTR);
    if (r != 0) fileError(fdName(fd), errno, "cannot lock");
}

void unlock(int fd) {
    int r;
    do r = ::flock(fd, LOCK_UN);
    while (r != 0 && errno == EINTR);
    if (r != 0) fileError(fdName(fd), errno, "cannot unlock");
}

} // namespace protoio::file
