# protoIO: binary storage, encodings and MessagePack

Status: **approved** (2026-11-09). Author: Gustavo Marino, with Claude.
Companion: protoST's language surface for these functions is specified in
`protoST/docs/superpowers/specs/2026-11-09-bytes-files-timers-design.md`.

## 1. Intent

ProtoBase (the Python `proto_db`) is a transactional, perennial object database.
A Smalltalk version, protoBaseST, is planned for protoST. It must read and write
the **same on-disk format** as ProtoBase; the acceptance test is a round trip
in both directions between the two implementations. The protoBaseST design
itself waits until ProtoBase's documentation is complete. The ground it will
stand on can be built now, because the format has been stable for some time.

A gap audit (2026-11-09) found that no protoCore runtime can do what
ProtoBase's storage layer does:
- positioned reads and writes;
- fsync and directory fsync;
- the inter-process lock;
- CRC32 with combination;
- BLAKE2b with an 8-byte digest;
- MessagePack.

The maintainer decided these belong in protoIO, so every runtime (protoST,
protoScala, protoClojure) gets them from one reviewed implementation.

This is **groundwork, not a database**. Nothing here knows about ProtoBase's
records, atoms or tags. Those belong to protoBaseST.

## 2. Goals, non-goals, anti-goals

**Goals**
- Byte-for-byte compatibility with what CPython produces for the same inputs:
  - `zlib.crc32`;
  - `hashlib.blake2b(digest_size=8)` and other sizes;
  - `hashlib.sha256` and `hmac`;
  - `base64.b64encode`;
  - `msgpack.packb(..., use_bin_type=True)`, and decoding what it produces;
  - `struct` packing.

  Proven by test vectors generated from CPython and checked into the repo.
- Durable, positioned file operations that behave exactly as ProtoBase's
  `os.*` calls, including the **same inter-process lock**. A Python process
  and a protoST process opening the same database directory must exclude each
  other.
- Linux, macOS and Windows, like the rest of protoIO.

**Non-goals**
- ProtoBase's `order_key`/`hash_key`, record framing, atom codec and tags
  (`@int`, `@dt`, `@sym`, ...): those are protoBaseST.
- Language bindings for protoScala and protoClojure. They gain the C++
  functions; their bindings are separate follow-ups. protoST's bindings are the
  companion spec.
- mmap, non-blocking file I/O, compression, cloud SDKs.

**Anti-goals** (failures even if everything "works")
- A durability call that silently does less than it says, for example a
  `dataSync` that is a no-op, or a directory sync that quietly does nothing
  where it could work. Where the OS cannot do something (directory fsync on
  Windows), the function documents it and the behaviour matches ProtoBase's.
- A lock that does not exclude a Python ProtoBase process.
- An encoding that differs from CPython's for any input in its domain, or
  that accepts out-of-domain input instead of throwing.

## 3. Constraints

- protoIO's existing rules hold: no protoCore dependency; plain C++20 types;
  bytes are `std::string`; descriptors are `int` (on Windows, CRT descriptors
  over `HANDLE`s, as `file_win.cpp` already does); every failure is a
  `protoio::Error`.
- No new third-party dependency. OpenSSL is already linked and may be used
  for SHA-256 and HMAC.
- Scope note: `proto_db`'s format needs CRC32, BLAKE2b, packing, MessagePack,
  positioned I/O, fsync, directory sync and the lock. SHA-256, HMAC, base64
  and `dataSync` are not used by the format. They are included now, at low
  cost, for the later S3 storage and cluster work.
  - **BLAKE2b must not use OpenSSL.** OpenSSL 3.0 (Ubuntu 24.04) has no
    variable digest size for BLAKE2b, and BLAKE2b with an 8-byte digest is
    *not* a truncated BLAKE2b-512: the digest length is part of the
    parameter block.
  - Implement BLAKE2b from RFC 7693 (key, digest size 1–64).
- CRC32 is the zlib / IEEE 802.3 polynomial (reflected 0xEDB88320). Implement
  it in protoIO (table-driven), without linking zlib.
- Blocking calls stay blocking. Runtimes already wrap them in their own
  blocking-I/O scopes (protoST's `BlockingIO` + `UnmanagedScope`).

## 4. The design

### 4.1 Files: `protoio/file.h` additions

The existing `Mode` enum cannot express ProtoBase's flag combinations
(`O_EXCL`, `O_APPEND | O_TRUNC`, read-write without truncation). Add:

```cpp
struct OpenOptions {
    bool read = false, write = false;
    bool create = false, exclusive = false;   // exclusive => O_EXCL (FileExists if present)
    bool truncate = false, append = false;
    int  permissions = 0644;                  // ignored on Windows
};
int open(const std::string& path, const OpenOptions& options);   // close-on-exec, binary
```

Positioned and durable operations on a descriptor:

| Function | POSIX | Windows | Notes |
|---|---|---|---|
| `std::string readAt(int fd, int64 offset, size_t n)` | `pread` loop | `ReadFile` with `OVERLAPPED` offset | Answers fewer bytes only at end of file. |
| `void writeAt(int fd, int64 offset, string_view data)` | `pwrite` loop | `WriteFile` with `OVERLAPPED` offset | Writes everything or throws. An `append` descriptor throws `InvalidArgument` (Linux's `pwrite` would append and ignore the offset). |
| `int64 appendData(int fd, string_view data)` | `write` loop on an `append` descriptor | `GetFileSizeEx`, then `WriteFile` with `OVERLAPPED` at that end (never the file pointer, which a concurrent `readAt` moves) | Answers the offset where the data started. **Contract: one appender per descriptor at a time.** POSIX gives no atomic "where did my append land", and neither does Windows, so concurrent appenders on one descriptor are not supported. This is documented, and concurrent `readAt` is fine. ProtoBase has a single writer per segment. *My call.* |
| `int64 size(int fd)` | `fstat` | `GetFileSizeEx` | |
| `void truncate(int fd, int64 size)` | `ftruncate` | `SetFileInformationByHandle` / `SetEndOfFile` | |
| `void sync(int fd)` | `fsync` | `FlushFileBuffers` | Same as CPython's `os.fsync`. On macOS that is plain `fsync`, not `F_FULLFSYNC`; documented. |
| `void dataSync(int fd)` | `fdatasync` (Linux), `fsync` (macOS) | `FlushFileBuffers` | Never a no-op. |
| `void syncDirectory(const string& path)` | `open(O_RDONLY)` + `fsync` | documented no-op | Matches ProtoBase's `fsync_dir`, which skips on Windows. |
| `void replace(const string& from, const string& to)` | `rename` | `MoveFileExW(REPLACE_EXISTING)` | `os.replace` semantics; existing `move` may already be this. Builder to reuse it. |
| `bool tryLock(int fd)` / `void lock(int fd)` / `void unlock(int fd)` | `flock(LOCK_EX[\|LOCK_NB])` | `LockFileEx` on byte 0, length 1 (`FAIL_IMMEDIATELY` for `tryLock`) | **Must interoperate with ProtoBase's `ownerlock.py`**: `fcntl.flock` on POSIX, `msvcrt.locking(fd, LK_NBLCK, 1)` at offset 0 on Windows. Never POSIX record locks (`fcntl(F_SETLK)`): on Linux they are a different lock family and would not exclude ProtoBase. |

Errors follow `file.h`'s existing mapping (FileNotFound, FileExists,
FileSystem), with `sysErrno` set. Interrupted calls (`EINTR`) are retried.
Short reads and writes are looped.

### 4.2 Encodings: new header `protoio/encoding.h`

```cpp
namespace protoio::encoding {
// Fixed-width integers and IEEE 754, at a byte offset into `data`.
// Throws InvalidArgument when out of range or out of bounds.
void     putUint(std::string& out, uint64_t v, int width, bool bigEndian);  // width 1,2,4,8
void     putInt (std::string& out, int64_t  v, int width, bool bigEndian);
void     putFloat64(std::string& out, double v, bool bigEndian);
void     putFloat32(std::string& out, float  v, bool bigEndian);
uint64_t getUint(std::string_view data, size_t offset, int width, bool bigEndian);
int64_t  getInt (std::string_view data, size_t offset, int width, bool bigEndian);
double   getFloat64(std::string_view data, size_t offset, bool bigEndian);
float    getFloat32(std::string_view data, size_t offset, bool bigEndian);

std::string base64Encode(std::string_view data);       // RFC 4648, padded, no line breaks
std::string base64Decode(std::string_view text);       // strict: InvalidArgument on bad input
}
```

NaN payloads and signed zero round-trip bit-exactly (copy through
`std::bit_cast`, never arithmetic).

### 4.3 Digests: new header `protoio/digest.h`

```cpp
namespace protoio::digest {
uint32_t crc32(std::string_view data, uint32_t crc = 0);            // zlib.crc32(data, crc)
uint32_t crc32Combine(uint32_t crcA, uint32_t crcB, uint64_t lenB); // zlib crc32_combine
std::string blake2b(std::string_view data, int digestSize = 64, std::string_view key = {});
std::string sha256(std::string_view data);
std::string hmacSha256(std::string_view key, std::string_view data);
// Incremental forms for large inputs:
class Blake2b { public: Blake2b(int digestSize = 64, std::string_view key = {}); void update(std::string_view); std::string finish(); };
class Sha256  { /* update / finish, same shape */ };
}
```

`crc32Combine` is required: ProtoBase's `storage/crc.py` uses the combine
arithmetic for prefix CRCs over large segments.

### 4.4 MessagePack: new header `protoio/msgpack.h`

protoIO cannot build runtime objects, so it does not produce a value tree. It
offers **a writer and an event-driven reader**. Each runtime walks its own
objects and emits writer calls, and builds its own objects from reader events.
There is no intermediate tree and no double copy. *My call:* this is the shape
that keeps protoIO free of protoCore and costs one pass.

```cpp
namespace protoio::msgpack {
class Writer {                       // appends to an internal std::string
public:
    void nil(); void boolean(bool);
    void integer(int64_t); void unsignedInteger(uint64_t);   // smallest encoding, as msgpack-python
    void float64(double);                                     // always float64 (msgpack-python default)
    void str(std::string_view utf8);                          // InvalidArgument if not valid UTF-8
    void bin(std::string_view bytes);                         // bin 8/16/32 (use_bin_type=True)
    void beginArray(uint32_t n); void beginMap(uint32_t n);   // caller then writes n / 2n items
    void ext(int8_t type, std::string_view data);
    const std::string& bytes() const; std::string take();
};
struct Handler {                     // reader callbacks; the runtime implements it
    virtual void nil() = 0; virtual void boolean(bool) = 0;
    virtual void integer(int64_t) = 0; virtual void unsignedInteger(uint64_t) = 0;  // the latter only above INT64_MAX
    virtual void float64(double) = 0;                          // float32 on the wire is widened
    virtual void str(std::string_view) = 0; virtual void bin(std::string_view) = 0;
    virtual void beginArray(uint32_t n) = 0; virtual void beginMap(uint32_t n) = 0;
    virtual void ext(int8_t type, std::string_view data) = 0;
    virtual ~Handler() = default;
};
// Decodes exactly one top-level object; answers the bytes consumed.
size_t read(std::string_view data, Handler& h, const Limits& limits = {});
}
```

- `Limits` bounds the nesting depth and the element count. It also rejects a
  declared length longer than the remaining input, so hostile input cannot
  allocate unbounded memory, following the HTTP layer's practice.
- Truncated or invalid input throws `InvalidArgument`.
- Map keys: `Limits::strictMapKeys` (default **true**) accepts only str and
  bin keys and throws `InvalidArgument` otherwise. This mirrors msgpack-python
  1.x, whose `unpackb` defaults to `strict_map_key=True`; every ProtoBase call
  site relies on that default. A binding may turn it off explicitly.

The encoder chooses the smallest representation exactly as msgpack-python 1.x
does: positive and negative fixint, then the uint/int widths, fixstr, str8,
and so on. The test vectors pin this down byte for byte. Re-encoding a decoded
multi-key map is only required to be *value*-equal to CPython's output, not
byte-identical, because map order is the caller's. ProtoBase never relies on
byte-identical re-encoding (D51 rejected content addressing).

### 4.5 Test vectors from CPython

- **Generator script:** `tests/vectors/generate.py`, run with CPython 3.12+ and
  `msgpack` 1.x. It writes `tests/vectors/*.json`, with hex strings for the
  binary data.
- **Coverage:**
  - CRC32 over edge sizes, plus `crc32Combine` cases;
  - BLAKE2b at digest sizes 1, 8, 32 and 64, with and without key, including
    the RFC 7693 vectors;
  - SHA-256, HMAC and base64;
  - int and float packing, including NaN, -0.0, ±inf, and min/max of each width;
  - MessagePack for every type boundary (fixint 127/128, -32/-33, str 31/32,
    bin 255/256, array 15/16, map 15/16, int64/uint64 extremes, nested values,
    and ext).
- **Commitment:** both the generated files and the script are committed. CI
  does not need Python, and the vectors can be regenerated and diffed.

### 4.6 Version

These are additions only, released as protoIO **0.3.0**. Existing API is
unchanged.

## 5. Left to the builder

- Table layout or slicing for CRC32, as long as it is correct and portable.
  Performance is secondary but should be within reason (≥ 1 GB/s on DEV12 is
  plenty).
- Whether `replace` is a new function or a documented alias of `move`, after
  checking that `move` already gives `os.replace` semantics on all three
  platforms.
- Internal organisation of the source files, following protoIO's existing
  split (`file.cpp` / `file_win.cpp`).
- Exact `Limits` defaults: generous, documented, comparable to the HTTP layer.

## 6. Open questions for the maintainer

None at the time of writing; see the companion spec.
