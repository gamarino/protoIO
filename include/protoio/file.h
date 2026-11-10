// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Files and directories. Every function may block. Failures throw
// protoio::Error with kind FileNotFound (ENOENT), FileExists (EEXIST) or
// FileSystem (anything else); the message names the path. A path that holds
// a NUL byte throws InvalidArgument before any system call (the system would
// read the path only up to that byte): "file::open: the path contains a NUL
// byte". File contents (the `data` of write, append, writeAt, appendData)
// may hold NUL bytes.
namespace protoio::file {

enum class Mode {
    Read,       // O_RDONLY
    Write,      // O_WRONLY | O_CREAT | O_TRUNC
    Append,     // O_WRONLY | O_CREAT | O_APPEND
    ReadWrite,  // O_RDWR | O_CREAT
};

// Opens a file (close-on-exec, created 0644) and answers its descriptor, for
// use with the stream functions of protoio/stream.h.
int open(const std::string& path, Mode mode);

// The whole file. Reading a directory throws FileSystem (EISDIR).
std::string read(const std::string& path);

// Replaces the file's contents (creating it).
void write(const std::string& path, const std::string& data);

// Appends to the file (creating it).
void append(const std::string& path, const std::string& data);

struct Stat {
    bool isFile = false;
    bool isDirectory = false;
    std::int64_t size = 0;
    std::int64_t modifiedMs = 0;  // modification time, milliseconds since the epoch
    bool readable = false;        // access(R_OK) for this process
    bool writable = false;        // access(W_OK) for this process
};

// std::nullopt when nothing exists at `path` (symbolic links are followed).
std::optional<Stat> stat(const std::string& path);

// Removes a file or an empty directory, or with `recursive` a whole tree.
// Answers whether something was removed (false when nothing was there).
bool remove(const std::string& path, bool recursive = false);

// Renames `from` to `to` (rename(2): same file system).
void move(const std::string& from, const std::string& to);

// Copies a file, or a whole directory tree, overwriting existing files.
void copy(const std::string& from, const std::string& to);

// Creates a directory; with `parents`, also the missing parents, and an
// existing directory is not an error.
void mkdir(const std::string& path, bool parents = false);

// The names of the entries of a directory, sorted, without "." and "..".
std::vector<std::string> list(const std::string& path);

// The absolute form of `path`, resolved lexically against the working
// directory: "." and ".." are folded, symbolic links are kept, and there is
// no trailing slash (except for "/").
std::string absolute(const std::string& path);

// The system's directory for temporary files.
std::string tempDir();

// The working directory, and changing it (process-wide).
std::string cwd();
void chdir(const std::string& path);

// ------------------------------------------- positioned and durable operations
//
// The storage ground of the protoCore runtimes' databases (a port of
// ProtoBase shares ProtoBase's on-disk directory with it). Failures throw as
// above, with `sysErrno` set; an interrupted call (EINTR) is retried, and a
// short read or write is continued, and a write that makes no progress
// throws FileSystem (EIO) instead of looping. A negative offset or size
// throws InvalidArgument. On Windows readAt and writeAt move the descriptor's file
// position, so they do not mix with the stream functions on one descriptor.

// The flags of open(2) that Mode cannot express.
struct OpenOptions {
    bool read = false, write = false;        // at least one; both: O_RDWR
    bool create = false, exclusive = false;  // exclusive (O_EXCL, needs create): FileExists if present
    bool truncate = false, append = false;   // truncate needs write; append implies write
    int permissions = 0644;                  // for a created file; ignored on Windows
};

// Opens a file (close-on-exec, binary) and answers its descriptor. An
// impossible combination (neither read nor write, exclusive without create,
// truncate without write) throws InvalidArgument.
int open(const std::string& path, const OpenOptions& options);

// Up to `n` bytes at `offset` (pread); fewer only at the end of the file, and
// none from the end on. For a regular file, `n` is first capped at what the
// file holds from `offset` on, so a huge (corrupt) count allocates no more
// than the file's bytes.
std::string readAt(int fd, std::int64_t offset, std::size_t n);

// Writes all of `data` at `offset` (pwrite), extending the file as needed. A
// descriptor opened with `append` (OpenOptions::append or Mode::Append)
// throws InvalidArgument on every platform: Linux would append and ignore
// the offset. On Windows protoIO recognizes only append descriptors it
// opened itself.
void writeAt(int fd, std::int64_t offset, std::string_view data);

// Writes all of `data` at the end of a writable descriptor and answers the
// offset where it starts. The descriptor may be opened with `append` or not:
// appendData reads the end first (lseek(SEEK_END); GetFileSizeEx) and writes
// there. Concurrent readAt and writeAt calls are positioned and do not use
// the file pointer.
//
// Contract: ONE APPENDER PER DESCRIPTOR at a time, and nothing else may move
// the end of the file during the call (a writeAt past the end, a truncate).
// The offset is the end of the file just before the write, and neither POSIX
// nor Windows reports atomically where an append landed, so concurrent
// appenders on one descriptor, or other processes appending to the same
// file, are not supported. This overload has no guard and does not detect a
// violation; the overload that takes an EndGuard (below) refuses one.
std::int64_t appendData(int fd, std::string_view data);

// The size of the file, in bytes (fstat; GetFileSizeEx).
std::int64_t size(int fd);

// Sets the size of the file (ftruncate), cutting it or extending it with
// zeros. This overload has no guard (see EndGuard below).
void truncate(int fd, std::int64_t size);

// ------------------------------------------------------------ the end guard
//
// appendData answers where its bytes landed only when nothing else moves the
// end of the file during the call. An EndGuard makes a violation of that
// contract fail loudly instead of corrupting offsets: the operations that
// move the end announce themselves on it, and the one that collides throws
// Error with kind Conflict and does nothing. It is one atomic counter, used
// with compare-and-swap only: no lock, no waiting, no timing.
//
//    0      idle: no guarded call is moving the end;
//    n > 0  n writeAt calls that extend the file are in progress. SHARED:
//           each writes its own bytes at its own offset and none relies on
//           where the end is, so they run together;
//   -1      one appendData or truncate is in progress. EXCLUSIVE: they read
//           or move the end and need it alone.
//
// So appendData and truncate are refused while any other guarded call moves
// the end, and an extending writeAt is refused while an appendData or a
// truncate runs. A writeAt that stays inside the file takes no claim and is
// never refused: overwrites stay concurrent. Every claim is released when
// the call ends, by return or by exception.
//
// Scope: an EndGuard guards only the callers that SHARE THIS EndGuard
// OBJECT. Calls through the unguarded overloads, through another EndGuard,
// on another descriptor of the same file, or in another process are not
// seen. A runtime keeps one EndGuard beside each descriptor it exposes as a
// file object (protoST's BinaryFile does) and routes every appendData,
// writeAt and truncate of that object through it. The guard must outlive
// every claim on it.
//
// The size an extending writeAt is judged by is read before its claim: a
// truncate that shrinks the file after that read lets the write extend it
// unclaimed. That is the caller's own race (truncating a region another
// thread is writing into); appendData's answer stays correct regardless.
class EndGuard {
public:
    enum class Mode {
        Shared,     // an extending writeAt: any number at once, never with an Exclusive one
        Exclusive,  // an appendData or a truncate: alone
    };

    // A claim on a guard, released by its destructor. The guarded functions
    // below take one; a runtime may also hold one around operations of its
    // own that move the end of the file (tests use it to hold the guard
    // deterministically).
    class Claim {
    public:
        // Takes `mode` on `guard`, or throws Error (Conflict) when the guard
        // is in a conflicting state, with a message that begins with
        // `operation` (e.g. "appendData").
        Claim(EndGuard& guard, Mode mode, std::string_view operation);
        ~Claim();
        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;
        Claim(Claim&&) = delete;
        Claim& operator=(Claim&&) = delete;

    private:
        EndGuard& guard_;
        const Mode mode_;
    };

    EndGuard() noexcept = default;
    EndGuard(const EndGuard&) = delete;
    EndGuard& operator=(const EndGuard&) = delete;
    EndGuard(EndGuard&&) = delete;
    EndGuard& operator=(EndGuard&&) = delete;

    // The state: 0, n > 0 or -1, as above. A snapshot for diagnostics and
    // tests; another thread may change it at once.
    int state() const noexcept { return state_.load(std::memory_order_acquire); }

private:
    std::atomic<int> state_{0};
};

// appendData under an Exclusive claim on `guard`. Refused (Conflict, nothing
// written) while another guarded appendData, truncate or extending writeAt
// is in progress on the same guard.
std::int64_t appendData(int fd, EndGuard& guard, std::string_view data);

// writeAt; when the write would extend the file (offset + data.size() >
// size(fd)), under a Shared claim on `guard`, refused (Conflict, nothing
// written) while a guarded appendData or truncate is in progress. A write
// inside the file takes no claim. A negative offset throws InvalidArgument
// before any claim.
void writeAt(int fd, EndGuard& guard, std::int64_t offset, std::string_view data);

// truncate under an Exclusive claim on `guard`. Refused (Conflict, the size
// unchanged) while another guarded call moves the end. A negative size
// throws InvalidArgument before any claim.
void truncate(int fd, EndGuard& guard, std::int64_t size);

// Makes the file's data and metadata durable: fsync, FlushFileBuffers on
// Windows. As CPython's os.fsync, on macOS it is plain fsync, not
// F_FULLFSYNC, so the drive's own cache may still hold the data there.
void sync(int fd);

// Makes the file's data durable (and the metadata needed to read it back):
// fdatasync on Linux, fsync on macOS, FlushFileBuffers on Windows. It is
// never a no-op.
void dataSync(int fd);

// Makes the entries of a directory durable (created, renamed or removed
// names): open(O_RDONLY), fsync, close. On Windows, where a directory cannot
// be synced, it is a no-op, as ProtoBase's fsync_dir is; a missing directory
// throws FileNotFound only on POSIX.
void syncDirectory(const std::string& path);

// Renames `from` to `to`, replacing an existing `to` atomically: the
// semantics of CPython's os.replace (rename(2); on Windows a POSIX-semantics
// rename, or MoveFileExW with MOVEFILE_REPLACE_EXISTING). It is `move`,
// which already has them on every platform.
void replace(const std::string& from, const std::string& to);

// An exclusive, advisory, inter-process lock on the file of `fd`. On POSIX it
// is flock(2); on Windows LockFileEx on byte 0, length 1. These are exactly
// the locks ProtoBase's OwnerLock takes (fcntl.flock; msvcrt.locking at
// offset 0, length 1), so each excludes the other. It is never a POSIX
// record lock (fcntl F_SETLK), which on Linux is a separate lock family and
// would not exclude ProtoBase. The lock belongs to the open file, not the
// process: a second descriptor opened on the same file, even in this
// process, does not get it. Take it once per descriptor (Windows does not
// let one descriptor take it twice). On Windows the locked byte cannot be
// read or written through another descriptor, so lock a dedicated file. The
// lock is released by unlock or by closing the descriptor.

// Takes the lock without waiting; false when another descriptor holds it.
bool tryLock(int fd);
// Takes the lock, waiting while another descriptor holds it.
void lock(int fd);
void unlock(int fd);

} // namespace protoio::file
