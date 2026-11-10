# Changelog

All notable changes to protoIO are recorded here. The project follows
[Semantic Versioning](https://semver.org/).

## Unreleased

## 0.4.0 - 2026-11-10

A minor release: new API, a new `Error::Kind` value and a changed answer of
`file::stat`.

### Added

- `file::EndGuard`, grown from protoST's `BinaryFile` end guard so every
  runtime gets it. One 64-bit atomic word per open file object (append flag,
  truncate flag, extending-writes count, in-file-writes count), changed by
  compare-and-swap only: no lock, no waiting. Four uses, claimed through the
  public RAII `EndGuard::Claim` and named by `EndGuard::Use`:
  `Append` (conflicts with Append, Truncate, Extend), `Truncate` (with
  everything), `Extend` (with Append, Truncate) and `Inside` (with Truncate
  only). Guarded overloads `appendData(fd, guard, data)`,
  `writeAt(fd, guard, offset, data)` (claims Inside, then reads the size, and
  also claims Extend when the write extends the file) and
  `truncate(fd, guard, size)` refuse a conflicting call with `Conflict` and
  do nothing; every claim is released on every path. For callers sharing one
  guard, an appended record's bytes land at the answered offset and no
  guarded write becomes an extending one under a truncate; other descriptors,
  other processes, other guards, sequential I/O on the descriptor and the
  unguarded overloads are not seen (see `file.h`). The unguarded overloads
  remain.
- `Error::Kind::Conflict` (`kindName`: "Conflict"), appended after `Internal`
  (no existing value changes): an operation conflicts with one already in
  progress on the same object. The guarded file operations throw it.

### Changed

- A string handed to the operating system as a C string no longer
  truncates silently at an embedded NUL byte (`"a\0b"` used to open `"a"`).
  File paths (every path argument of `protoio/file.h` except `stat`'s),
  process arguments, working directories, environment names and values,
  shell commands (`protoio/process.h`), host names (`protoio/net.h`) and
  HTTP URLs (`protoio/http.h`) are refused with `InvalidArgument` and a
  message naming the function and the argument, before any system call. A
  redirect location holding a NUL byte is a malformed response (`Network`).
  Data arguments (file contents, process input, datagram and HTTP bodies)
  are unchanged.
- `file::stat` answers `std::nullopt` for a path holding a NUL byte, without
  a system call (no file can have that name); it used to answer about the
  prefix. Existence predicates built on it now answer false, as the JVM's
  `File.exists()` and Python's `os.path.exists` do.
- POSIX `appendData` reads the end with `fstat` and writes with `pwrite` at
  that offset, as the Windows version does: a stream read or write on the
  same descriptor can no longer redirect its bytes, and on POSIX it no
  longer moves the file position (on Windows the positioned `WriteFile`
  still does). On an `append` descriptor `pwrite` may append regardless of
  the offset (Linux does; other systems may), which under the one-appender
  contract is the same place.
- `appendData` (guarded and unguarded, POSIX and Windows) refuses a
  descriptor that is not a regular file with `FileSystem` (`sysErrno`
  ESPIPE) before writing anything. Pipes, sockets and terminals already
  failed; a block device, whose `fstat` size is 0, would otherwise have been
  written over from offset 0.

### Upgrading

- Runtimes that map `Error::Kind` with an exhaustive `switch` (protoST,
  protoScala, protoClojure) must add a case for `Conflict`; under `-Werror`
  the missing case is a build error (`-Wswitch`).
- The CMake package uses `SameMinorVersion` compatibility: a consumer of the
  new API must ask for `find_package(protoIO 0.4 ...)`.

## 0.3.0 - 2026-11-09

### Added

- Positioned and durable file operations (`protoio/file.h`):
  `open(path, OpenOptions)`, `readAt`, `writeAt`, `appendData` (one appender
  per descriptor), `size`, `truncate`, `sync`, `dataSync`, `syncDirectory`
  (a no-op on Windows) and `replace`.
- A ProtoBase-compatible inter-process lock: `tryLock`, `lock`, `unlock`
  (`flock` on POSIX, `LockFileEx` on byte 0 on Windows).
- `protoio/encoding.h`: fixed-width integers and IEEE 754 floats in either
  byte order (as CPython's `struct`), and strict `base64Encode` /
  `base64Decode`.
- `protoio/digest.h`: `crc32`, `crc32Combine`, `blake2b` (RFC 7693), `sha256`,
  `hmacSha256`, and incremental `Blake2b` and `Sha256`.
- `protoio/msgpack.h`: a `Writer`, an event-based `read` with a `Handler` and
  `Limits` (depth, element count, strict map keys), byte-compatible with
  msgpack-python 1.x.
- Test vectors generated with CPython under `tests/vectors`.
- `Error::Kind::Internal` (`kindName`: "Internal"): a failure inside a
  library protoIO relies on, caused neither by the caller nor by the
  environment. The OpenSSL-backed digests (`sha256`, `hmacSha256`, `Sha256`)
  report OpenSSL internal failures with it. A runtime that maps kinds with an
  exhaustive `switch` (protoST, protoScala, protoClojure) must add a case.

### Changed

- `writeAt` on a descriptor opened with `append` throws `InvalidArgument` on
  every platform (Linux's `pwrite` used to append and ignore the offset).
- On Windows, `appendData` writes at the end it read (`GetFileSizeEx`, then
  positioned `WriteFile`) instead of through the C runtime's append, which a
  concurrent `readAt` could redirect into the middle of the file.
- A positioned or whole-file write that makes no progress throws
  `FileSystem` (EIO) instead of looping.
- `readAt` caps its allocation at what a regular file holds from the offset.

### Upgrading

- The CMake package uses `SameMinorVersion` compatibility, so
  `find_package(protoIO 0.2 ...)` does not accept 0.3.0: a consumer that
  wants these APIs must raise its minimum to 0.3 (protoClojure's
  `CMakeLists.txt` still asks for 0.2.3).

## 0.2.3 - not yet tagged

### Added

- `readBufferedLine(fd, max)`: the next line when the read-ahead buffer
  already holds all of it, else `std::nullopt`; it never reads and never
  waits (it answers `std::nullopt` when another thread holds the read lock).
  A runtime can take most lines without leaving the collector's quorum and
  call `readLine` inside that bracket only when a read is needed. Lines come
  off the same buffer as `readLine`'s, so the two mix freely.

### Changed

- Reading a line no longer moves the rest of the 64 KiB read-ahead buffer to
  its front (`erase(0, n)` per line, about 32 KiB moved per line on
  average). The buffer keeps an offset to its first unread byte; the
  consumed front is dropped only when the next read appends to it. On a
  1.4 million-line file this was about 0.9 s of a protoClojure `line-seq`
  pass (benchmarks/reports/2026-10-25-grep-gap in protoClojure). Every
  reader (`readLine`, `readBytes`, `readChars`, `readAll`, `atEnd` and the
  HTTP layer) shares the offset.

## 0.2.2 - 2026-10-02

### Added

- `process::run(argv, RunOptions)`: a working directory and a replacement
  environment for the child, set by the spawn call itself
  (`posix_spawn_file_actions_addchdir_np` and the `envp` of `posix_spawnp`;
  `CreateProcessW`'s directory and environment block), so a runtime no longer
  needs `sh -c 'cd ...'` or `env -i` (which Windows lacks) for them. A bare
  program name is searched in the caller's `PATH`, as the JVM does; a
  relative name with a directory is taken from the child's directory; on
  Windows `SystemRoot` is added when the environment lacks it.

### Fixed

- `tcpListen("", port)` listens on every interface, IPv6 included, as
  documented: one dual-stack socket (`IPV6_V6ONLY` off) where the host has
  IPv6, else 0.0.0.0. It bound 0.0.0.0 only, so a client that tried `::1`
  first ("localhost" on Windows and macOS) was refused there before it
  reached 127.0.0.1, which cost each connection about 0.3 s on Windows.
  `sockName` and `peerName` answer an IPv4 peer of a dual-stack socket as
  its IPv4 address (127.0.0.1, not ::ffff:127.0.0.1).

## 0.2.1 - 2026-10-02

### Added

- `process::shell(command, input)`: runs a command line through the system
  shell, feeding input and collecting outputs as `run` does. On POSIX it is
  `run({"/bin/sh", "-c", command}, input)`. On Windows it runs the system
  directory's `cmd.exe` with the prebuilt command line
  `cmd.exe /d /s /c "<command>"`, so the command reaches cmd verbatim; `run`
  with `cmd.exe /c` applies the C runtime's argument quoting, which cmd does
  not undo (`echo "a b"` arrived as `"echo \"a b\""`). An empty command
  throws `InvalidArgument`.

## 0.2.0 - 2026-10-02

Native Windows and macOS support, and the fixes of the Windows-port review.
Behaviour changes on Windows (see "Changed"), hence the minor version:
consumers ask for `find_package(protoIO 0.2 CONFIG)`.

### Added

- Native Windows support (MSVC 2022, Winsock, Win32): `src/platform_win.cpp`
  (socket descriptors, error mapping, UTF-8 conversion, descriptor reads),
  `src/file_win.cpp` and `src/process_win.cpp` (`CreateProcessW`), and
  `#ifdef _WIN32` blocks in the stream and network code. The POSIX build is
  unchanged. See README.md, "Windows", for what differs there.
- macOS support (Apple clang): descriptors made close-on-exec and free of
  SIGPIPE without SOCK_CLOEXEC, accept4, pipe2 or MSG_NOSIGNAL.
- `net::nativeSocket(fd)` (Windows only): the `SOCKET` behind a descriptor.
- `net::trustCertificates(pem)`: extra trusted certificates (a private CA,
  a test's own) for verified TLS and https.
- `PROTOIO_WARNINGS_AS_ERRORS` CMake option (off by default; CI turns it
  on: no warnings at `/W4` or `-Wall -Wextra -Wpedantic`).
- CI on Linux, macOS and Windows.
- Tests: non-ASCII file names, a read timeout on a pipe; on Windows, argument
  quoting, PATH search and unsupported signals, with a small child program
  (`tests/testchild.cpp`) standing in for `sh`, `cat`, `true` and `sleep`.
  For the review: a successful verified TLS handshake against a generated
  CA, https to a public host through the system trust store, deleting and
  renaming open files, batch-file refusal, no working-directory search,
  crash exit codes, Unicode argv and environment, console input, text-mode
  descriptors, close wake-up latency, a briefly full backlog, re-listening
  while connections linger, port take-over.

### Changed (Windows)

- `run` and `spawn` resolve `argv[0]` themselves and never search the
  working directory; `.bat` and `.cmd` targets throw `InvalidArgument`
  (BatBadBut, CVE-2024-24576). The README no longer suggests `cmd /c` as
  the way to run a batch file with arguments: it is a shell, like `sh -c`.
- A child that ends with an exception (an NTSTATUS code) answers 128 + the
  matching POSIX signal (139 for an access violation, 130 for Ctrl+C, 134
  for any other NTSTATUS error), not the raw negative code.
- The library reads every descriptor as bytes (`ReadFile`), bypassing the C
  runtime's text mode: a Ctrl+Z byte on standard input is data, CR LF
  arrives unchanged. The console is read as UTF-8 (`ReadConsoleW`); a line
  that starts with Ctrl+Z ends it.
- Files are opened with delete sharing, so an open file can be deleted,
  renamed and replaced; `move` replaces its target with POSIX semantics.
- Listeners set `SO_EXCLUSIVEADDRUSE`.
- A refused loopback connect is retried for about 0.3 s, so a briefly full
  backlog (which Windows answers with a reset) no longer fails a connect at
  once; a port nobody listens on is refused after about 0.3 s.

### Fixed

- Windows: verified TLS (and every https request) failed with "unable to
  get local issuer certificate": the verifying context now loads the
  Windows ROOT and CA certificate stores.
- Windows: socket waits polled in 50 ms slices and pipe reads with a timeout
  every 5 ms. A socket wait now also waits on the thread's wake-up socket,
  which `close` signals; a pipe read blocks in the read and a thread-pool
  timer cancels it at the deadline.
- Windows: read timeouts were ignored on console input. A timed console read
  now waits for a complete line on the console handle first (a cancelled
  console read would swallow the next line).
- Windows: `udpSend` ignored the descriptor's timeout and could retry
  forever on a full send buffer.
- Windows: if `run` could not start its standard-error reader thread while
  the input feeder ran, the process ended in `std::terminate`; now the child
  is ended, started threads joined and the error rethrown.
- Windows: the child's pipe ends were inheritable for their whole life, so a
  concurrent `system()` could inherit them; now only duplicates scoped to
  the `CreateProcessW` call are (the residual window is documented).
- macOS: close now wakes a thread waiting on a listening or UDP socket at
  once, through the same per-thread wake-up channel (a pipe), instead of
  within a 50 ms slice.
- The IPv6 loopback test is split from the by-name test and reports a host
  without a usable `::1` as skipped, with the reason.

## 0.1.0 - 2026-09-30

First release: protoST 0.5.0's POSIX I/O layer moved into a library shared by
the protoCore runtimes, with the HTTP/1.1 message layer ported from protoST's
`lib/http.st`.

### Added

- `protoio/stream.h`: buffered descriptor streams (`readLine` with a length
  limit, `readAll`, `readBytes`, `readChars` over whole UTF-8 characters,
  `atEnd`, `write`, `flush`, `close`, `setTimeout`, `forget`) and
  `SigpipeGuard`.
- `protoio/file.h`: whole-file read, write and append, `stat`, recursive
  `remove`, `move`, tree `copy`, `mkdir`, sorted `list`, lexical `absolute`,
  `tempDir`, `cwd`, `chdir`.
- `protoio/process.h`: `run` with input and both outputs, `spawn`, `wait`,
  `kill`, the environment, `pid`, `hostName`, `platform`, `exit`.
- `protoio/net.h`: TCP connect/listen/accept with timeouts, socket names, TLS
  client upgrade with verification, UDP bind/send/receive.
- `protoio/http.h`: request and response heads with limits (400, 414, 431,
  413), bodies by length or chunked, validation against header injection,
  serialisation, percent-decoding, query parsing, URL parsing, RFC 3986
  redirect resolution with the redirect policy, and a blocking `httpRequest`
  client.
- Typed errors (`protoio::Error` with eleven kinds and the system errno).
- CMake package (`find_package(protoIO 0.1 CONFIG)`, installed or from a build
  tree), `add_subdirectory` support, a `protoio-dev` Debian package, a
  ThreadSanitizer build option and a GoogleTest suite.

### Kept from protoST's review hardening (2026-09-29)

- One read lock, one write lock and one TLS lock per descriptor; the state
  owns the descriptor, `close` shuts a socket down to wake blocked threads,
  and the number is released only after its last user returns.
- No SIGPIPE on writes other than the process's standard output, including
  the input of a child process that exits without reading it.
- Non-blocking TLS with poll-bounded handshake, reads and writes.
- HTTP: control characters refused in methods, targets and headers; chunked
  bodies decoded as bytes; UTF-8 percent-decoding with `+` as a space only in
  queries; RFC 3986 redirects that drop the caller's headers across origins
  and refuse https to http; bounded lines, header counts and bodies.

### Changed relative to protoST 0.5.0

- `readLine` with a limit no longer consumes a partial line when the limit
  falls exactly on a pending CR: a too-long line leaves the stream untouched.
- A Content-Length body that ends early is a Network error instead of a short
  body; several differing Content-Length values are refused (400 on a server).
- `readRequestHead` refuses a request line with more than three words, a
  method that is not a token or a target with control characters (400), and
  checks the declared body size against `Limits::maxBody` (413) before the
  body is read.
- Responses to 1xx, 204 and 304 carry neither a Content-Length nor a body.
- Caller-supplied Content-Length, Connection and Transfer-Encoding headers are
  replaced by the library's own framing.
- Header names must be ASCII tokens.
- Two threads accepting on one listener, or receiving on one UDP socket, each
  honour their own timeout: the listener is non-blocking, and a waiter that
  lost the race waits again instead of blocking.
