// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Files and directories on Windows: the contracts of file.cpp, with every
// path taken and answered as UTF-8 (converted to and from the UTF-16 the
// system uses) and every descriptor opened in binary mode, so the bytes are
// those of the file, as on POSIX.
#include "protoio/file.h"

#include "protoio/stream.h"
#include "internal.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <filesystem>
#include <system_error>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

namespace protoio::file {

namespace fs = std::filesystem;
using detail::fileError;
namespace win = detail::win;

namespace {

fs::path pathOf(const std::string& utf8) { return fs::path(win::widen(utf8)); }

std::string utf8Of(const fs::path& p) { return win::narrow(p.native()); }

// The errno value behind a std::filesystem error, which on Windows carries a
// Win32 code.
int errnoOf(const std::error_code& ec) {
    if (ec.category() == std::generic_category()) return ec.value();
    return win::errnoOfWin32(static_cast<unsigned long>(ec.value()));
}

[[noreturn]] void fsError(const fs::filesystem_error& e, const char* action) {
    fileError(utf8Of(e.path1()), errnoOf(e.code()), action);
}

bool isDirectory(const std::string& path) {
    const DWORD a = ::GetFileAttributesW(win::widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// _wopen; a directory answers EISDIR, as open(2) does for writing (the C
// runtime says EACCES).
int openPath(const std::string& path, int flags) {
    int fd = -1;
    const errno_t rc = ::_wsopen_s(&fd, win::widen(path).c_str(), flags | _O_BINARY | _O_NOINHERIT, _SH_DENYNO,
                                   _S_IREAD | _S_IWRITE);
    if (rc != 0) {
        errno = rc == EACCES && isDirectory(path) ? EISDIR : rc;
        return -1;
    }
    return fd;
}

void writeWhole(const std::string& path, const std::string& data, bool append) {
    const int fd = openPath(path, _O_WRONLY | _O_CREAT | (append ? _O_APPEND : _O_TRUNC));
    if (fd < 0) fileError(path, errno, "cannot write");
    std::size_t done = 0;
    while (done < data.size()) {
        const int w = ::_write(fd, data.data() + done, static_cast<unsigned>(std::min<std::size_t>(data.size() - done, INT_MAX)));
        if (w < 0) { const int e = errno; ::_close(fd); fileError(path, e, "cannot write"); }
        done += static_cast<std::size_t>(w);
    }
    if (::_close(fd) != 0) fileError(path, errno, "cannot write");
}

} // namespace

int open(const std::string& path, Mode mode) {
    int flags = 0;
    switch (mode) {
        case Mode::Read: flags = _O_RDONLY; break;
        case Mode::Write: flags = _O_WRONLY | _O_CREAT | _O_TRUNC; break;
        case Mode::Append: flags = _O_WRONLY | _O_CREAT | _O_APPEND; break;
        case Mode::ReadWrite: flags = _O_RDWR | _O_CREAT; break;
    }
    const int fd = openPath(path, flags);
    if (fd < 0) fileError(path, errno, "cannot open");
    protoio::forget(fd);  // a stale state for a reused number must not survive
    return fd;
}

std::string read(const std::string& path) {
    if (isDirectory(path)) fileError(path, EISDIR, "cannot read");
    const int fd = openPath(path, _O_RDONLY);
    if (fd < 0) fileError(path, errno, "cannot read");
    std::string out;
    char chunk[65536];
    for (;;) {
        const int n = ::_read(fd, chunk, sizeof chunk);
        if (n < 0) { const int e = errno; ::_close(fd); fileError(path, e, "cannot read"); }
        if (n == 0) break;
        out.append(chunk, static_cast<std::size_t>(n));
    }
    ::_close(fd);
    return out;
}

void write(const std::string& path, const std::string& data) { writeWhole(path, data, false); }

void append(const std::string& path, const std::string& data) { writeWhole(path, data, true); }

std::optional<Stat> stat(const std::string& path) {
    const std::wstring w = win::widen(path);
    // GetFileAttributesEx describes a symbolic link itself; opening the path
    // follows it, as stat(2) does.
    const HANDLE h = ::CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = ::GetFileInformationByHandle(h, &info);
    ::CloseHandle(h);
    if (!ok) return std::nullopt;
    Stat s;
    s.isDirectory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    s.isFile = !s.isDirectory && !(info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
    s.size = s.isDirectory ? 0 : static_cast<std::int64_t>((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
    // FILETIME counts 100 ns intervals since 1601-01-01.
    const std::uint64_t ft = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    s.modifiedMs = static_cast<std::int64_t>(ft / 10000) - 11644473600000LL;
    s.readable = ::_waccess(w.c_str(), 4) == 0;
    s.writable = ::_waccess(w.c_str(), 2) == 0;
    return s;
}

bool remove(const std::string& path, bool recursive) {
    try {
        return recursive ? fs::remove_all(pathOf(path)) > 0 : fs::remove(pathOf(path));
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot delete");
    }
}

void move(const std::string& from, const std::string& to) {
    try {
        fs::rename(pathOf(from), pathOf(to));
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot move");
    }
}

void copy(const std::string& from, const std::string& to) {
    try {
        fs::copy(pathOf(from), pathOf(to), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot copy");
    }
}

void mkdir(const std::string& path, bool parents) {
    int err = 0;
    if (parents) {
        std::error_code ec;
        fs::create_directories(pathOf(path), ec);
        if (ec) err = errnoOf(ec);
    } else if (::_wmkdir(win::widen(path).c_str()) != 0) {
        err = errno;
    }
    if (err) fileError(path, err, "cannot create directory");
}

std::vector<std::string> list(const std::string& path) {
    std::vector<std::string> names;
    try {
        for (const auto& e : fs::directory_iterator(pathOf(path))) names.push_back(utf8Of(e.path().filename()));
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot list");
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string absolute(const std::string& path) {
    // Lexical only: "." and ".." are folded and symbolic links are kept. The
    // answer uses the native separator, "\", and keeps it only at a root
    // ("C:\").
    std::error_code ec;
    const fs::path p = fs::absolute(pathOf(path), ec);
    if (ec) fileError(path, errnoOf(ec), "cannot resolve");
    const fs::path n = p.lexically_normal();
    std::wstring s = n.native();
    if (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/') && n != n.root_path()) s.pop_back();
    return win::narrow(s);
}

std::string tempDir() {
    std::error_code ec;
    const fs::path p = fs::temp_directory_path(ec);
    if (ec) return "C:\\Windows\\Temp";
    // GetTempPath ends in a separator; POSIX's answer does not.
    std::wstring s = p.native();
    if (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return win::narrow(s);
}

std::string cwd() {
    std::error_code ec;
    const fs::path p = fs::current_path(ec);
    if (ec) fileError(".", errnoOf(ec), "cannot read the working directory");
    return utf8Of(p);
}

void chdir(const std::string& path) {
    if (::_wchdir(win::widen(path).c_str()) != 0) fileError(path, errno, "cannot change directory to");
}

} // namespace protoio::file
