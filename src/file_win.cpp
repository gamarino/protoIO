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
using detail::checkNoNul;
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
    if ((flags & _O_CREAT) && (flags & _O_EXCL)) disposition = CREATE_NEW;  // ERROR_FILE_EXISTS: EEXIST
    else if (flags & _O_CREAT) disposition = (flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
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
        if (w == 0) { ::_close(fd); fileError(path, EIO, "cannot write"); }  // no progress
        done += static_cast<std::size_t>(w);
    }
    if (::_close(fd) != 0) fileError(path, errno, "cannot write");
}

// The name a descriptor goes by in an error message.
std::string fdName(int fd) { return "descriptor " + std::to_string(fd); }

[[noreturn]] void invalid(const std::string& message) { throw Error(Error::Kind::InvalidArgument, message); }

void checkOffset(std::int64_t offset, const char* what) {
    if (offset < 0) invalid(std::string(what) + ": negative offset " + std::to_string(offset));
}

// The largest count one ReadFile/WriteFile is asked for (a DWORD).
constexpr std::size_t kMaxChunk = 1u << 30;

// The handle behind a C runtime descriptor; EBADF (FileSystem) when `fd` is
// not open, as on POSIX, rather than the C runtime's invalid-parameter abort.
HANDLE handleOf(int fd, const char* action) {
    HANDLE h;
    {
        win::QuietCrt quiet;
        h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    }
    if (h == INVALID_HANDLE_VALUE || h == nullptr) fileError(fdName(fd), EBADF, action);
    return h;
}

// An OVERLAPPED that names `offset`, for positioned I/O and the lock on a
// synchronous handle.
OVERLAPPED at(std::int64_t offset) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(static_cast<std::uint64_t>(offset) & 0xffffffffu);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32);
    return ov;
}

// Writes all of `data` at `offset` with positioned WriteFile calls, which do
// not depend on the file pointer. A call that reports no progress for a
// non-zero request throws FileSystem (EIO) rather than looping forever.
void writeAllAt(HANDLE h, int fd, std::int64_t offset, std::string_view data, const char* action) {
    std::size_t done = 0;
    while (done < data.size()) {
        OVERLAPPED ov = at(offset + static_cast<std::int64_t>(done));
        DWORD put = 0;
        if (!::WriteFile(h, data.data() + done, static_cast<DWORD>(std::min(data.size() - done, kMaxChunk)), &put, &ov))
            fileError(fdName(fd), win::errnoOfWin32(::GetLastError()), action);
        if (put == 0) fileError(fdName(fd), EIO, action);
        done += put;
    }
}

} // namespace

int open(const std::string& path, Mode mode) {
    checkNoNul(path, "file::open", "the path");
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
    if (mode == Mode::Append) win::markAppend(fd);  // writeAt refuses it
    return fd;
}

std::string read(const std::string& path) {
    checkNoNul(path, "file::read", "the path");
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
    checkNoNul(path, "file::remove", "the path");
    try {
        return recursive ? fs::remove_all(pathOf(path)) > 0 : fs::remove(pathOf(path));
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot delete");
    }
}

void move(const std::string& from, const std::string& to) {
    checkNoNul(from, "file::move", "the source path");
    checkNoNul(to, "file::move", "the target path");
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
    checkNoNul(from, "file::copy", "the source path");
    checkNoNul(to, "file::copy", "the target path");
    try {
        fs::copy(pathOf(from), pathOf(to), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    } catch (const fs::filesystem_error& e) {
        fsError(e, "cannot copy");
    }
}

void mkdir(const std::string& path, bool parents) {
    checkNoNul(path, "file::mkdir", "the path");
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
    checkNoNul(path, "file::list", "the path");
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
    checkNoNul(path, "file::absolute", "the path");
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
    checkNoNul(path, "file::chdir", "the path");
    if (::_wchdir(win::widen(path).c_str()) != 0) fileError(path, errno, "cannot change directory to");
}

// ------------------------------------------- positioned and durable operations

int open(const std::string& path, const OpenOptions& options) {
    checkNoNul(path, "file::open", "the path");
    const bool write = options.write || options.append;
    if (!options.read && !write) invalid("cannot open " + path + ": neither read nor write was asked for");
    if (options.exclusive && !options.create) invalid("cannot open " + path + ": exclusive needs create");
    if (options.truncate && !write) invalid("cannot open " + path + ": truncate needs write");
    int flags = options.read && write ? _O_RDWR : write ? _O_WRONLY : _O_RDONLY;
    if (options.create) flags |= _O_CREAT;
    if (options.exclusive) flags |= _O_EXCL;
    if (options.truncate) flags |= _O_TRUNC;
    if (options.append) flags |= _O_APPEND;
    // options.permissions has no Windows meaning (the file gets its
    // directory's inherited ACL), as for CPython's os.open.
    const int fd = openPath(path, flags);
    if (fd < 0) fileError(path, errno, "cannot open");
    protoio::forget(fd);  // a stale state for a reused number must not survive
    if (options.append) win::markAppend(fd);  // writeAt refuses it
    return fd;
}

std::string readAt(int fd, std::int64_t offset, std::size_t n) {
    checkOffset(offset, "cannot read at");
    const HANDLE h = handleOf(fd, "cannot read");
    // Allocate no more than a disk file holds from `offset` on, so a corrupt
    // length is neither zero-filled nor a std::length_error; fewer bytes are
    // answered, as at any end of the file.
    if (::GetFileType(h) == FILE_TYPE_DISK) {
        LARGE_INTEGER sz{};
        if (!::GetFileSizeEx(h, &sz)) fileError(fdName(fd), win::errnoOfWin32(::GetLastError()), "cannot read");
        const std::int64_t left = std::max<std::int64_t>(0, static_cast<std::int64_t>(sz.QuadPart) - offset);
        // Compare in 64 bits: narrowing first would wrap on a 32-bit size_t and cut
        // n below the true remainder, a short read before the end of the file.
        if (static_cast<std::uint64_t>(left) < static_cast<std::uint64_t>(n)) n = static_cast<std::size_t>(left);
    }
    std::string out(n, '\0');
    std::size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = at(offset + static_cast<std::int64_t>(done));
        DWORD got = 0;
        if (!::ReadFile(h, out.data() + done, static_cast<DWORD>(std::min(n - done, kMaxChunk)), &got, &ov)) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_HANDLE_EOF) break;  // the offset is at or past the end of the file
            fileError(fdName(fd), win::errnoOfWin32(e), "cannot read");
        }
        if (got == 0) break;  // the end of the file
        done += got;
    }
    out.resize(done);
    return out;
}

void writeAt(int fd, std::int64_t offset, std::string_view data) {
    checkOffset(offset, "cannot write at");
    const HANDLE h = handleOf(fd, "cannot write");
    // As on POSIX, where an O_APPEND descriptor cannot write at an offset.
    if (win::isAppend(fd)) invalid("cannot write at an offset of " + fdName(fd) + ": it was opened to append");
    writeAllAt(h, fd, offset, data, "cannot write");
}

std::int64_t appendData(int fd, std::string_view data) {
    // Valid under the one-appender contract: nobody else moves the end
    // between this answer and the write. The write names its offset
    // (OVERLAPPED) and never uses the file pointer: a positioned read on
    // another thread moves that pointer, so the C runtime's append (a seek to
    // the end, then a write at the pointer) could land in the middle of the
    // file.
    const HANDLE h = handleOf(fd, "cannot append to");
    LARGE_INTEGER end{};
    if (!::GetFileSizeEx(h, &end)) fileError(fdName(fd), win::errnoOfWin32(::GetLastError()), "cannot append to");
    const std::int64_t start = static_cast<std::int64_t>(end.QuadPart);
    writeAllAt(h, fd, start, data, "cannot append to");
    return start;
}

std::int64_t size(int fd) {
    const HANDLE h = handleOf(fd, "cannot read the size of");
    LARGE_INTEGER n{};
    if (!::GetFileSizeEx(h, &n)) fileError(fdName(fd), win::errnoOfWin32(::GetLastError()), "cannot read the size of");
    return static_cast<std::int64_t>(n.QuadPart);
}

void truncate(int fd, std::int64_t newSize) {
    if (newSize < 0) invalid("cannot truncate " + fdName(fd) + ": negative size " + std::to_string(newSize));
    const HANDLE h = handleOf(fd, "cannot truncate");
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = newSize;
    if (!::SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof info))
        fileError(fdName(fd), win::errnoOfWin32(::GetLastError()), "cannot truncate");
}

void sync(int fd) {
    const HANDLE h = handleOf(fd, "cannot sync");
    if (!::FlushFileBuffers(h)) fileError(fdName(fd), win::errnoOfWin32(::GetLastError()), "cannot sync");
}

void dataSync(int fd) { sync(fd); }  // FlushFileBuffers: Windows has no data-only flush

void syncDirectory(const std::string& path) {
    checkNoNul(path, "file::syncDirectory", "the path");
    // A directory cannot be flushed on Windows; ProtoBase's fsync_dir skips
    // it there too.
}

void replace(const std::string& from, const std::string& to) {
    checkNoNul(from, "file::replace", "the source path");
    checkNoNul(to, "file::replace", "the target path");
    move(from, to);
}

// The lock is LockFileEx on byte 0, length 1: the region msvcrt.locking(fd,
// LK_NBLCK, 1) locks at offset 0, which is what ProtoBase's OwnerLock takes.

bool tryLock(int fd) {
    const HANDLE h = handleOf(fd, "cannot lock");
    OVERLAPPED ov = at(0);
    if (::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) return true;
    const DWORD e = ::GetLastError();
    // ERROR_IO_PENDING cannot happen on a synchronous handle with
    // LOCKFILE_FAIL_IMMEDIATELY; it is read as "held", defensively.
    if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) return false;
    fileError(fdName(fd), win::errnoOfWin32(e), "cannot lock");
}

void lock(int fd) {
    const HANDLE h = handleOf(fd, "cannot lock");
    OVERLAPPED ov = at(0);
    if (!::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov))
        fileError(fdName(fd), win::errnoOfWin32(::GetLastError()), "cannot lock");
}

void unlock(int fd) {
    const HANDLE h = handleOf(fd, "cannot unlock");
    OVERLAPPED ov = at(0);
    if (!::UnlockFileEx(h, 0, 1, 0, &ov)) fileError(fdName(fd), win::errnoOfWin32(::GetLastError()), "cannot unlock");
}

} // namespace protoio::file
