# Changelog

All notable changes to protoIO are recorded here. The project follows
[Semantic Versioning](https://semver.org/).

## Unreleased

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
