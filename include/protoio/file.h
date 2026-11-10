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
// byte"; only stat answers std::nullopt instead. File contents (the `data` of write, append, writeAt, appendData)
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
// A path that holds a NUL byte also answers std::nullopt, without a system
// call: no file can have that name. (Every other function here throws
// InvalidArgument for such a path.) A runtime that wants an error instead,
// as CPython's os.stat raises ValueError, checks the path itself.
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
// appendData reads the end first (fstat; GetFileSizeEx) and then
// writes at that offset (pwrite; WriteFile with an OVERLAPPED offset), never
// at the file position, so stream reads or writes on the same descriptor
// cannot redirect the bytes. On an `append` descriptor Linux's pwrite
// appends and ignores the offset; under the contract below the end is the
// offset it read, so the bytes land there all the same. Concurrent readAt and
// writeAt calls are positioned and do not use the file position.
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
// end of the file during the call, and a write judged to be inside the file
// stays inside only while nothing cuts the file. An EndGuard turns a breach
// of either into an Error (kind Conflict) instead of silent corruption: the
// guarded calls below claim a use of the guard for their duration, and the
// call whose use conflicts with one in progress throws and does nothing.
//
// The four uses, and what each conflicts with:
//
//   Append    (appendData)                  Append, Truncate, Extend
//   Truncate  (truncate)                    every use (it needs the file alone)
//   Extend    (a writeAt past the end)      Append, Truncate
//   Inside    (every guarded writeAt)       Truncate
//
// Extend and Inside are shared (any number at once); Append and Truncate are
// exclusive in the sense above. Appends and in-file writes run together: an
// in-file write does not move the end.
//
// A guarded writeAt claims Inside first, then reads the size: with a
// truncate excluded the size can only grow, so a write judged inside stays
// inside. A write that extends the file then also claims Extend, which an
// append in progress refuses.
//
// The state is one 64-bit atomic word (append flag, truncate flag, extending
// writes count, inside writes count), changed by compare-and-swap only: no
// lock, no waiting, no timing. Every claim is released when its call ends, by
// return or by exception.
//
// Guaranteed, for the callers that SHARE ONE EndGuard and reach the file
// only through it (the guarded calls, and readAt, size, sync, which never
// move the end):
//   - appendData's bytes land at the offset it answers, and no other guarded
//     call moves the end or extends the file while it writes them;
//   - no guarded write lands past the end of a file that a truncate cut
//     meanwhile: truncate runs alone among guarded calls;
//   - so the end of the file moves only under a claim, and an appended
//     record is overwritten only by a writeAt whose range its caller chose
//     to overlap it. Writes to overlapping byte ranges are never ordered:
//     that is the callers' own concern, guarded or not.
// Not seen, hence not guaranteed against: the unguarded overloads; another
// EndGuard; any other write to, or sequential read from, the descriptor
// (protoio::write, the stream reads); another descriptor of the same file,
// including a dup; another process. A runtime keeps one EndGuard beside
// each descriptor it exposes as a file object (protoST's BinaryFile does)
// and routes every appendData, writeAt and truncate of that object through
// it. The guard must outlive every claim on it.
//
// Cost: a guarded writeAt adds one atomic read-modify-write pair and one
// size query (fstat; GetFileSizeEx) to the write, two pairs when it extends.
class EndGuard {
public:
    enum class Use {
        Append,    // appendData
        Truncate,  // truncate
        Extend,    // a writeAt that extends the file (held together with Inside)
        Inside,    // any guarded writeAt
    };

    // A claim of one use, released by its destructor. The guarded functions
    // below take them; a runtime may also hold one around operations of its
    // own (tests use it to hold the guard deterministically).
    class Claim {
    public:
        // Claims `use` on `guard`, or throws Error (Conflict) when a
        // conflicting use is in progress, with a message that begins with
        // `operation` (e.g. "appendData"). Nothing changes on a refusal.
        Claim(EndGuard& guard, Use use, std::string_view operation);
        ~Claim();
        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;
        Claim(Claim&&) = delete;
        Claim& operator=(Claim&&) = delete;

    private:
        EndGuard& guard_;
        const std::uint64_t unit_;
    };

    // A snapshot of the uses in progress, for diagnostics and tests; another
    // thread may change the guard at once.
    struct State {
        bool append = false;
        bool truncate = false;
        std::uint32_t extending = 0;  // writeAt calls extending the file
        std::uint32_t inside = 0;     // guarded writeAt calls (extending ones included)
        bool idle() const noexcept { return !append && !truncate && extending == 0 && inside == 0; }
    };

    EndGuard() noexcept = default;
    EndGuard(const EndGuard&) = delete;
    EndGuard& operator=(const EndGuard&) = delete;
    EndGuard(EndGuard&&) = delete;
    EndGuard& operator=(EndGuard&&) = delete;

    State state() const noexcept;

private:
    std::atomic<std::uint64_t> word_{0};
};

// appendData under an Append claim on `guard`: refused (Conflict, nothing
// written) while a guarded appendData, truncate or extending writeAt is in
// progress. In-file writes do not refuse it.
std::int64_t appendData(int fd, EndGuard& guard, std::string_view data);

// writeAt under an Inside claim on `guard`, plus an Extend claim when the
// write extends the file (offset + data.size() > size(fd), judged under the
// Inside claim). Refused (Conflict, nothing written) while a guarded truncate
// is in progress, and, when it extends the file, while a guarded appendData
// is. A negative offset throws InvalidArgument before any claim; empty data
// writes nothing and claims nothing.
void writeAt(int fd, EndGuard& guard, std::int64_t offset, std::string_view data);

// truncate under a Truncate claim on `guard`: refused (Conflict, the size
// unchanged) while any other guarded call is in progress, in-file writes
// included. A negative size throws InvalidArgument before any claim.
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
