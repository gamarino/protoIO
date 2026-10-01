# Changelog

All notable changes to protoIO are recorded here. The project follows
[Semantic Versioning](https://semver.org/).

## Unreleased

### Added

- Native Windows support (MSVC 2022, Winsock, Win32): `src/platform_win.cpp`
  (socket descriptors, error mapping, UTF-8 conversion), `src/file_win.cpp`
  and `src/process_win.cpp` (`CreateProcessW`), and `#ifdef _WIN32` blocks in
  the stream and network code. The POSIX build is unchanged. See README.md,
  "Windows", for what differs there.
- `net::nativeSocket(fd)` (Windows only): the `SOCKET` behind a descriptor.
- Tests: non-ASCII file names, a read timeout on a pipe; on Windows, argument
  quoting, PATH search and unsupported signals, with a small child program
  (`tests/testchild.cpp`) standing in for `sh`, `cat`, `true` and `sleep`.

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
