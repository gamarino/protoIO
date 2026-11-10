# protoIO 0.3.0 Binary Storage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** add durable positioned file operations, a ProtoBase-compatible
inter-process lock, byte encodings, digests and a MessagePack writer and
reader to protoIO, all byte-compatible with CPython.

**Architecture:** the code is split in three.
- File additions go in the existing `file.h`, with POSIX code in `file.cpp`
  and Windows code in `file_win.cpp`.
- Three new headers carry pure, platform-independent code: `encoding.h`,
  `digest.h` and `msgpack.h`.
- Compatibility is proven by JSON vector files that a committed CPython
  script generates.

**Tech Stack:** C++20, CMake, GoogleTest 1.14 (FetchContent), OpenSSL 3.0+
(SHA-256 and HMAC only), CPython 3.12+ with `msgpack` 1.x (vector generation
only).

**Spec:** `docs/specs/2026-11-09-binary-storage-design.md`.

## Global Constraints

- No protoCore dependency.
- Plain C++20 types: bytes are `std::string` or `std::string_view`, and
  descriptors are `int`.
- Every failure throws `protoio::Error`. Bad values give `InvalidArgument`;
  file failures give FileNotFound, FileExists or FileSystem, with `sysErrno`.
- No new third-party dependency: no zlib and no BLAKE2 library. OpenSSL is
  used only for SHA-256 and HMAC.
- BLAKE2b follows RFC 7693, with the digest size in the parameter block. It is
  never a truncated BLAKE2b-512.
- CRC32 uses the reflected zlib polynomial 0xEDB88320.
- The lock is `flock(LOCK_EX[|LOCK_NB])` on POSIX and `LockFileEx` on byte 0,
  length 1, on Windows. Never `fcntl(F_SETLK)`.
- `syncDirectory` is a documented no-op on Windows only.
- One appender per descriptor; this is documented in the header comment.
- MessagePack decoding uses strict map keys (str and bin only) by default.
- Version **0.3.0**; existing API unchanged.
- Linux, macOS and Windows CI must stay green.
- Copyright header: `Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.`

## Review Focus

1. **A short `write`/`pwrite` and `EINTR`.** A partial write must loop to
   completion. Test with a 64 MiB `writeAt`, then verify it with `readAt`.
   (Task 1)
2. **`readAt` past end of file and at exactly the end.** It must answer fewer
   bytes or an empty string, never throw. (Task 1)
3. **MessagePack declared length larger than the input**, for example a
   `bin32` header that claims 4 GiB on 10 bytes. It must throw
   `InvalidArgument` without allocating. (Task 5)
4. **A NaN with a payload and `-0.0` through `putFloat64`/`getFloat64` and the
   MessagePack writer.** They must be bit-identical. (Tasks 2, 5)
5. **A lock taken twice from the same process on two descriptors.** `flock`
   excludes the second descriptor on Linux, and this is the case that matches
   ProtoBase. `tryLock` on the second must answer false. (Task 1)

---

### Task 0: CPython vector generator

**Files:**
- Create: `tests/vectors/generate.py`
- Create (generated, committed): `tests/vectors/crc32.json`, `blake2b.json`,
  `sha256_hmac.json`, `base64.json`, `packing.json`, `msgpack.json`
- Create: `tests/vectors/README.md` (how to regenerate; the CPython and
  msgpack versions used)

**Interfaces:**
- Produces: JSON arrays of objects with hex-encoded binary fields.
  - `crc32.json`: `{data, init, crc}`, plus `combine` cases
    `{a_hex, b_hex, crc_a, crc_b, len_b, combined}`.
  - `blake2b.json`: `{data, key, size, digest}`.
  - `sha256_hmac.json`: `{data, sha256}` and `{key, data, hmac}`.
  - `base64.json`: `{data, text}`.
  - `packing.json`: `{kind: "u"|"i"|"f32"|"f64", width, big_endian, value, bytes}`.
    Floats are given as bit patterns in hex.
  - `msgpack.json`: `{description, bytes, value}`. `value` is a tagged JSON
    tree: `{"t":"int","v":"-33"}`, `{"t":"str","v":"..."}`,
    `{"t":"bin","v":hex}`, `{"t":"f64","bits":hex}`,
    `{"t":"array","v":[...]}`, `{"t":"map","v":[[k,v],...]}`,
    `{"t":"ext","type":n,"v":hex}`, `nil`, `bool`.

- [ ] **Step 1:** write `generate.py`, using only `zlib`, `hashlib`, `hmac`,
  `base64`, `struct` and `msgpack`. Cover the cases listed in spec §4.5:
  - CRC sizes 0, 1, 3, 4, 7, 8, 63, 64, 65, 4096 and 1 MiB, with
    `crc32Combine` cases;
  - BLAKE2b sizes 1, 8, 32 and 64, with and without key, plus the RFC 7693
    "abc" vectors;
  - NaN, -0.0, ±inf, and min/max of each integer width;
  - the MessagePack boundaries fixint 127/128, -32/-33, str 31/32, bin
    255/256, array 15/16, map 15/16, int64 and uint64 extremes, a nested
    value, and ext.

  Encode with `msgpack.packb(v, use_bin_type=True)`.
- [ ] **Step 2:** run `python3 -I tests/vectors/generate.py`. Expected: six
  JSON files are written, and a second run produces identical files
  (`git diff --exit-code tests/vectors`).
- [ ] **Step 3:** commit with the message
  `Tests: CPython vectors for encodings, digests and MessagePack`.

### Task 1: Positioned and durable file operations, and the lock

**Files:**
- Modify: `include/protoio/file.h`. Add `OpenOptions`, `open(path, OpenOptions)`,
  `readAt`, `writeAt`, `appendData`, `size`, `truncate`, `sync`, `dataSync`,
  `syncDirectory`, `replace`, `tryLock`, `lock` and `unlock`, as declared in
  spec §4.1, with header comments stating the one-appender and Windows
  directory-sync contracts.
- Modify: `src/file.cpp` (POSIX) and `src/file_win.cpp` (Windows).
- Test: `tests/test_file.cpp` (extend).
- Create: `tests/ownerlock_peer.py`, which takes ProtoBase's lock exactly as
  `ProtoBase/proto_db/storage/ownerlock.py` does (`flock` or
  `msvcrt.locking` on byte 0), prints `LOCKED`, and holds the lock until
  stdin closes.

**Interfaces:**
- Produces, in `namespace protoio::file`:
  - `struct OpenOptions { bool read, write, create, exclusive, truncate, append; int permissions = 0644; };`
  - `int open(const std::string&, const OpenOptions&)`
  - `std::string readAt(int fd, std::int64_t offset, std::size_t n)`
  - `void writeAt(int fd, std::int64_t offset, std::string_view)`
  - `std::int64_t appendData(int fd, std::string_view)`
  - `std::int64_t size(int fd)`
  - `void truncate(int fd, std::int64_t)`
  - `void sync(int fd)`, `void dataSync(int fd)`
  - `void syncDirectory(const std::string&)`
  - `void replace(const std::string& from, const std::string& to)`, which is
    `move` if it already has `os.replace` semantics on all platforms (verified
    in review). Otherwise it gets its own implementation.
  - `bool tryLock(int fd)`, `void lock(int fd)`, `void unlock(int fd)`

- [ ] **Step 1: write the failing tests** in `test_file.cpp`.
  - `OpenOptions_ExclusiveRefusesExisting`: the second `open` with
    `{.write=true,.create=true,.exclusive=true}` throws `FileExists`.
  - `ReadAt_WriteAt_RoundTrip`: `writeAt(fd, 10, "abc\0def"s)`, then
    `readAt(fd, 10, 7) == "abc\0def"s`, and `size(fd) == 17`.
  - `ReadAt_PastEnd`: `readAt(fd, size, 10) == ""`, and
    `readAt(fd, size-2, 10).size() == 2`.
  - `WriteAt_Large`: 64 MiB of a pattern written at offset 4096 reads back
    equal.
  - `AppendData_AnswersStartOffset`: on an `append` descriptor, the first call
    answers 0 and the second answers the size of the first.
  - `Truncate_Shrinks`; `Sync_DataSync_SyncDirectory_DoNotThrow`; and
    `SyncDirectory_MissingPath` throws `FileNotFound` on POSIX.
  - `Replace_OverwritesTarget`.
  - `TryLock_SecondDescriptorSameProcess_False`, then true after `unlock`.
  - `TryLock_ExcludesProtoBasePeer`: spawn `python3 tests/ownerlock_peer.py
    <path>` through `protoio::process::spawn`, wait for `LOCKED`, and assert
    `tryLock == false`; close its stdin, wait, and assert `tryLock == true`.
    If `python3` is absent, call `GTEST_SKIP() << "python3 not found:
    ProtoBase lock interop NOT verified"`.
- [ ] **Step 2:** run
  `cmake --build build_release -j4 && ctest --test-dir build_release -R file -V`.
  Expected: the new tests FAIL to compile (undeclared functions).
- [ ] **Step 3:** implement the POSIX side.
  - `pread`/`pwrite` loop on short counts and retry `EINTR`.
  - `appendData` answers `lseek(fd,0,SEEK_END)` before the write loop, which
    is valid under the one-appender contract.
  - `dataSync` is `fdatasync` on Linux and `fsync` on macOS.
  - `syncDirectory` does `open(O_RDONLY|O_CLOEXEC)`, `fsync`, `close`.
  - The lock functions use `flock`.
- [ ] **Step 4:** implement the Windows side.
  - `readAt`/`writeAt` use `ReadFile`/`WriteFile` with `OVERLAPPED`, on
    `_get_osfhandle(fd)`.
  - `size` uses `GetFileSizeEx`; `truncate` uses `SetFileInformationByHandle(FileEndOfFileInfo)`.
  - `sync`/`dataSync` use `FlushFileBuffers`.
  - The lock uses `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK[|LOCKFILE_FAIL_IMMEDIATELY], 0, 1, 0, &ov{offset 0})`
    and `UnlockFileEx`.
  - `syncDirectory` returns.
- [ ] **Step 5:** run the command from Step 2. Expected: all `file` tests
  PASS; the peer test PASSES, or SKIPs with the visible message.
- [ ] **Step 6:** commit with the message
  `Files: positioned and durable operations, and the ProtoBase-compatible lock`.

### Task 2: Integer and float packing, base64

**Files:**
- Create: `include/protoio/encoding.h`, `src/encoding.cpp`;
  add `src/encoding.cpp` to `add_library(protoio ...)`.
- Create: `tests/test_encoding.cpp`, `tests/vectors.h` and `tests/vectors.cpp`.
  These are a minimal JSON reader for the vector files, flat arrays of flat
  objects plus the nested MessagePack value tree. *My call:* a small reader
  instead of a JSON dependency. Add the reader to the
  `protoio_test()` sources.
- Modify: `tests/CMakeLists.txt` with `protoio_test(test_encoding encoding)`,
  and define `PROTOIO_VECTORS_DIR` as the absolute path of `tests/vectors`.

**Interfaces:**
- Consumes: Task 0's `packing.json` and `base64.json`.
- Produces, in `namespace protoio::encoding`, the functions of spec §4.2
  exactly: `putUint`, `putInt`, `putFloat64`, `putFloat32`, `getUint`,
  `getInt`, `getFloat64`, `getFloat32`, `base64Encode` and `base64Decode`.
- Produces for later tests: `protoio_test::loadVectors(const std::string& file)`
  returns `std::vector<protoio_test::JsonValue>`. `JsonValue` has `operator[](key)`,
  `str()`, `num()` and `items()`, and `fromHex(std::string)` is included.

- [ ] **Step 1: write the failing tests.**
  - `Packing_MatchesCPython` iterates `packing.json`. For each entry it checks
    that the put function produces `bytes`, and that the get function at
    offset 0 returns the value. For floats it compares bit patterns via
    `std::bit_cast<uint64_t>`.
  - `Packing_OutOfRange` covers two errors that throw `InvalidArgument`:
    `putUint(out, 256, 1, false)`, and `getUint("ab", 1, 2, false)` (out of
    bounds).
  - `Base64_MatchesCPython`; `Base64Decode_RejectsBadInput`, for `"abc"`
    (bad length), `"ab!="` and `"=abc"`.
- [ ] **Step 2:** build and run `ctest -R encoding`. Expected: FAIL (missing
  header).
- [ ] **Step 3:** implement, copying through `std::bit_cast` and never through
  arithmetic.
- [ ] **Step 4:** run `ctest -R encoding`. Expected: PASS.
- [ ] **Step 5:** commit with the message
  `Encoding: integer and IEEE 754 packing, base64`.

### Task 3: CRC32 and crc32Combine

**Files:**
- Create: `include/protoio/digest.h` (whole header, spec §4.3) and `src/crc32.cpp`.
- Create: `tests/test_digest.cpp`.
- Modify: `tests/CMakeLists.txt` with `protoio_test(test_digest digest)`.

**Interfaces:**
- Produces: `uint32_t crc32(std::string_view, uint32_t crc = 0)` and
  `uint32_t crc32Combine(uint32_t, uint32_t, uint64_t lenB)`.

- [ ] **Step 1: failing tests.**
  - `Crc32_MatchesCPython`, over every `crc32.json` entry, including `init`.
  - `Crc32Combine_MatchesCPython`.
  - `Crc32Combine_EqualsConcatenation`:
    `crc32Combine(crc32(a), crc32(b), b.size()) == crc32(a + b)` for 100 random
    splits of a 1 MiB buffer.
- [ ] **Step 2:** run `ctest -R digest`. Expected: FAIL.
- [ ] **Step 3:** implement a table-driven CRC with slicing-by-8. For
  `crc32Combine`, use zlib's GF(2) matrix method (the algorithm of
  `crc32_combine` in zlib 1.2.x, rewritten, not copied).
- [ ] **Step 4:** run `ctest -R digest`. Expected: PASS.
- [ ] **Step 5:** commit with the message `Digest: CRC32 and crc32Combine (zlib-compatible)`.

### Task 4: BLAKE2b, SHA-256 and HMAC-SHA256

**Files:**
- Create: `src/blake2b.cpp` and `src/sha.cpp`. The latter uses OpenSSL
  `EVP_Digest` and `HMAC`, or `EVP_MAC` on OpenSSL ≥ 3.
- Test: `tests/test_digest.cpp` (extend).

**Interfaces:**
- Produces: `std::string blake2b(std::string_view data, int digestSize = 64, std::string_view key = {})`,
  `class Blake2b { Blake2b(int digestSize = 64, std::string_view key = {}); void update(std::string_view); std::string finish(); }`,
  `std::string sha256(std::string_view)`, `class Sha256 { void update(std::string_view); std::string finish(); }`
  and `std::string hmacSha256(std::string_view key, std::string_view data)`.

- [ ] **Step 1: failing tests.**
  - `Blake2b_MatchesCPython`, over all of `blake2b.json`.
  - `Blake2b_Size8_IsNotTruncated512`:
    `blake2b("abc", 8) != blake2b("abc", 64).substr(0, 8)`.
  - `Blake2b_Incremental_EqualsOneShot`, with 1 MiB fed in uneven chunks.
  - `Blake2b_BadParams`: size 0, size 65 and key length 65 each throw
    `InvalidArgument`.
  - `Sha256_MatchesCPython`; `Hmac_MatchesCPython`.
- [ ] **Step 2:** run `ctest -R digest`. Expected: the new tests FAIL.
- [ ] **Step 3:** implement BLAKE2b from RFC 7693 §3 (12 rounds, parameter
  block with digest length, key length, fanout 1 and depth 1). Wrap OpenSSL
  for the other two.
- [ ] **Step 4:** run `ctest -R digest`. Expected: PASS.
- [ ] **Step 5:** commit with the message `Digest: BLAKE2b (RFC 7693), SHA-256 and HMAC-SHA256`.

### Task 5: MessagePack writer and event reader

**Files:**
- Create: `include/protoio/msgpack.h` and `src/msgpack.cpp`.
- Create: `tests/test_msgpack.cpp`.
- Modify: `tests/CMakeLists.txt` with `protoio_test(test_msgpack msgpack)`.

**Interfaces:**
- Produces `Writer`, `Handler` and `read` exactly as in spec §4.4, plus:
  - `struct Limits { uint32_t maxDepth = 512; uint64_t maxElements = 1u << 26; bool strictMapKeys = true; };`
    These are *my call*, generous next to the HTTP layer.
  - `size_t read(std::string_view, Handler&, const Limits& = {})`.
  - `Handler::unsignedInteger` is called only for values > INT64_MAX.

- [ ] **Step 1: failing tests.**
  - `Writer_MatchesCPython`. For each `msgpack.json` entry, walk `value` into
    `Writer` calls, and compare `take()` with `bytes`.
  - `Reader_MatchesCPython`. A `TreeHandler` in the test rebuilds the tagged
    tree from events; it must equal `value`, and the bytes consumed must equal
    `bytes.size()`.
  - `Reader_Truncated`: every proper prefix of every vector throws
    `InvalidArgument`.
  - `Reader_HugeDeclaredLength`: `"\xc6\xff\xff\xff\xff" + 10 bytes` throws
    `InvalidArgument`, and allocation stays below 1 MiB (assert with a
    counting `Handler` that receives no `bin` call).
  - `Reader_DepthLimit`: 600 nested `0x91` bytes throw.
  - `Reader_StrictMapKeys`: map `{1: 2}` throws by default and decodes with
    `strictMapKeys=false`.
  - `Writer_InvalidUtf8` throws.
  - `Float_BitExact`: a NaN with payload `0x7ff8000000000123` and -0.0
    round-trip bit-exactly.
- [ ] **Step 2:** run `ctest -R msgpack`. Expected: FAIL.
- [ ] **Step 3:** implement. The writer uses the smallest encoding, as
  msgpack-python does. Integers ≥ 0 use positive fixint, uint8, uint16,
  uint32 and uint64, and negative ones use negative fixint, int8 and so on;
  str uses fixstr, str8, str16 and str32. The reader validates every declared
  length against the remaining input before reporting an event.
- [ ] **Step 4:** run `ctest -R msgpack`. Expected: PASS.
- [ ] **Step 5:** commit with the message `MessagePack: writer and event-driven reader, msgpack-python compatible`.

### Task 6: Release 0.3.0

**Files:**
- Modify: `CMakeLists.txt` (`project(... VERSION 0.3.0)`), `README.md` (API
  overview table: new headers and file functions), `CHANGELOG.md`.

- [ ] **Step 1:** run the full suite twice: `ctest --test-dir build_release`,
  then the TSan build `ctest --test-dir build_tsan`, inside `setarch -R`.
  Expected: everything PASSes, and any SKIP prints its reason.
- [ ] **Step 2:** update the docs and the version, then commit with the
  message `protoIO 0.3.0: binary storage, encodings, digests and MessagePack`.
- [ ] **Step 3:** push the branch and open a PR so that the Linux, macOS and
  Windows CI run. Expected: green on all three. Do not tag or release without
  the maintainer.
