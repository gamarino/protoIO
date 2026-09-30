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
#include <sys/stat.h>
#include <unistd.h>

namespace protoio::file {

namespace fs = std::filesystem;
using detail::fileError;

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
        done += static_cast<std::size_t>(w);
    }
    if (::close(fd) != 0 && errno != EINTR) fileError(path, errno, "cannot write");
}

} // namespace

int open(const std::string& path, Mode mode) {
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

void write(const std::string& path, const std::string& data) { writeWhole(path, data, false); }

void append(const std::string& path, const std::string& data) { writeWhole(path, data, true); }

std::optional<Stat> stat(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
    Stat s;
    s.isFile = S_ISREG(st.st_mode);
    s.isDirectory = S_ISDIR(st.st_mode);
    s.size = static_cast<std::int64_t>(st.st_size);
    s.modifiedMs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
    s.readable = ::access(path.c_str(), R_OK) == 0;
    s.writable = ::access(path.c_str(), W_OK) == 0;
    return s;
}

bool remove(const std::string& path, bool recursive) {
    try {
        return recursive ? fs::remove_all(path) > 0 : fs::remove(path);
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot delete");
    }
}

void move(const std::string& from, const std::string& to) {
    try {
        fs::rename(from, to);
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot move");
    }
}

void copy(const std::string& from, const std::string& to) {
    try {
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot copy");
    }
}

void mkdir(const std::string& path, bool parents) {
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
    return ec ? std::string("/tmp") : p.string();
}

std::string cwd() {
    std::error_code ec;
    const fs::path p = fs::current_path(ec);
    if (ec) fileError(".", ec.value(), "cannot read the working directory");
    return p.string();
}

void chdir(const std::string& path) {
    if (::chdir(path.c_str()) != 0) fileError(path, errno, "cannot change directory to");
}

} // namespace protoio::file
