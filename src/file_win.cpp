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
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

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

// FILE_RENAME_INFO as FileRenameInfoEx takes it (Windows 10 1607 and later),
// spelled out: older SDKs, and SDKs targeting an older Windows, lack the
// Flags member, the class and the flags.
struct RenameInfoEx {
    DWORD Flags;
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[1];
};
constexpr auto kFileRenameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
constexpr DWORD kRenameReplaceIfExists = 0x1;
constexpr DWORD kRenamePosixSemantics = 0x2;

// Opens `path` as open(2) does, answering a C runtime descriptor in binary
// mode, or -1 with errno set; a directory answers EISDIR, as open(2) does for
// writing (Windows says access denied).
//
// CreateFileW with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
// not _wsopen: the C runtime never asks for delete sharing, so a file a
// stream had open could not be deleted or renamed, nor replaced by a rename,
// which POSIX programs (and the atomic-save pattern) take for granted. The
// handle is not inheritable, like a descriptor opened with O_CLOEXEC.
int openPath(const std::string& path, int flags) {
    DWORD access = 0;
    switch (flags & (_O_RDONLY | _O_WRONLY | _O_RDWR)) {
        case _O_WRONLY: access = GENERIC_WRITE; break;
        case _O_RDWR: access = GENERIC_READ | GENERIC_WRITE; break;
        default: access = GENERIC_READ; break;
    }
    DWORD disposition = OPEN_EXISTING;
    if (flags & _O_CREAT) disposition = (flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (flags & _O_TRUNC) disposition = TRUNCATE_EXISTING;
    const HANDLE h = ::CreateFileW(win::widen(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        errno = e == ERROR_ACCESS_DENIED && isDirectory(path) ? EISDIR : win::errnoOfWin32(e);
        return -1;
    }
    // _O_APPEND: the C runtime moves to the end before each write.
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(h), flags & _O_APPEND);
    if (fd < 0) {
        const int e = errno;
        ::CloseHandle(h);
        errno = e;
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
    // As rename(2): an existing target is replaced, even while a stream has
    // it open, and the source may be open too. SetFileInformationByHandle
    // with POSIX semantics does that (Windows 10 1709 and later, NTFS);
    // elsewhere MoveFileExW replaces a target nobody has open.
    const std::wstring wfrom = win::widen(from);
    const std::wstring wto = win::widen(to);
    const HANDLE h = ::CreateFileW(wfrom.c_str(), DELETE | SYNCHRONIZE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) fileError(from, win::errnoOfWin32(::GetLastError()), "cannot move");
    // The new name as a full path, inside a FILE_RENAME_INFO of its size.
    std::wstring target(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetFullPathNameW(wto.c_str(), static_cast<DWORD>(target.size()), target.data(), nullptr);
        if (n == 0) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            fileError(to, win::errnoOfWin32(e), "cannot move");
        }
        if (n < target.size()) {
            target.resize(n);
            break;
        }
        target.resize(n + 1);
    }
    const std::size_t bytes = offsetof(RenameInfoEx, FileName) + (target.size() + 1) * sizeof(wchar_t);
    std::vector<unsigned char> buf(bytes, 0);
    auto* info = reinterpret_cast<RenameInfoEx*>(buf.data());
    info->Flags = kRenameReplaceIfExists | kRenamePosixSemantics;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, target.c_str(), (target.size() + 1) * sizeof(wchar_t));
    BOOL ok = ::SetFileInformationByHandle(h, kFileRenameInfoEx, info, static_cast<DWORD>(bytes));
    DWORD e = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(h);
    if (!ok && (e == ERROR_INVALID_PARAMETER || e == ERROR_NOT_SUPPORTED || e == ERROR_INVALID_FUNCTION)) {
        // No POSIX rename here (an older Windows, or a file system such as FAT).
        ok = ::MoveFileExW(wfrom.c_str(), wto.c_str(), MOVEFILE_REPLACE_EXISTING);
        e = ok ? ERROR_SUCCESS : ::GetLastError();
    }
    if (!ok) fileError(e == ERROR_FILE_NOT_FOUND ? from : to, e == ERROR_NOT_SAME_DEVICE ? EXDEV : win::errnoOfWin32(e),
                       "cannot move");
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
