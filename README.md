# protoIO

Shared input and output for the protoCore runtimes: files, the operating
system, other programs, TCP, UDP, TLS and HTTP/1.1, in one small C++20 static
library.

protoST, protoScala and protoClojure each expose I/O in their own language's
idiom, but they share the risky part: the POSIX layer. A code review of
protoST's first implementation found SIGPIPE deaths, descriptors shared
unsafely between threads and unbounded HTTP input. protoIO holds that layer
once, with those fixes, so a defect is fixed once for every runtime.

- **Who uses it:** the protoCore runtimes (protoST, protoScala, protoClojure),
  linked statically. Their packages gain no runtime dependency on protoIO,
  only on OpenSSL.
- **Independent:** no dependency on protoCore; plain C++ types only
  (`std::string` for bytes and UTF-8 text, integers, `std::vector`,
  `std::optional`, small structs).
- **Platform:** POSIX (Linux first) and Windows (MSVC, native Winsock and
  Win32; see [Windows](#windows)).
- **License:** MIT, Copyright (c) 2026 Gustavo Marino.

Design: [`docs/specs/2026-09-30-protoio-design.md`](docs/specs/2026-09-30-protoio-design.md).

## The protoCore ecosystem

| Project | Role | Repository |
|---|---|---|
| protoCore | C++20 object model and runtime kernel: immutable structures, concurrent GC, GIL-free threads | https://github.com/numaes/protoCore |
| protoJS | JavaScript runtime on protoCore | https://github.com/gamarino/protoJS |
| protoPython | Python 3 runtime (protopy) and ahead-of-time compiler (protopyc) on protoCore | https://github.com/gamarino/protoPython |
| protoScala | Scala 3 dialect on protoCore: object model, pattern matching, GIL-free actors | https://github.com/gamarino/protoScala |
| protoST | Smalltalk-inspired actor language on protoCore | https://github.com/gamarino/protoST |
| protoClojure | Clojure dialect on protoCore (early stage) | https://github.com/gamarino/protoClojure |
| protoCpp | Examples and benchmarks using protoCore directly from C++ | https://github.com/gamarino/protoCpp |
| protoIO | Shared input and output for the runtimes: files, processes, TCP, UDP, TLS and HTTP/1.1 (used by protoST, protoScala and protoClojure) | https://github.com/gamarino/protoIO |

Each runtime documents the I/O it exposes in its own language: protoST in
[tutorial chapter 15](https://github.com/gamarino/protoST/blob/main/docs/tutorial/15-input-and-output.md),
protoScala in [tutorial chapter 18](https://github.com/gamarino/protoScala/blob/main/docs/tutorial/18-input-and-output.md), protoClojure in [tutorial chapter 14](https://github.com/gamarino/protoClojure/blob/main/docs/tutorial/14-input-and-output.md) and
[`docs/LANGUAGE.md` §18](https://github.com/gamarino/protoClojure/blob/main/docs/LANGUAGE.md#18-input-and-output).

## API overview

Everything is in namespace `protoio`. Descriptors are plain `int`s.

| Header | Contents |
|---|---|
| `protoio/error.h` | `struct Error : std::runtime_error { Kind kind; int sysErrno; }` and `kindName(kind)` |
| `protoio/stream.h` | `readLine(fd, max = 0)` (`std::nullopt` at end; LF or CRLF stripped; over `max` throws LineTooLong), `readAll`, `readBytes(fd, n)`, `readChars(fd, n)` (whole UTF-8 characters), `atEnd`, `write`, `flush`, `close`, `setTimeout(fd, ms)`, `forget(fd)`, and `SigpipeGuard` |
| `protoio/file.h` | `file::open(path, Mode)`, `read`, `write`, `append`, `stat` (`std::optional<Stat>`), `remove(path, recursive)`, `move`, `copy` (trees too), `mkdir(path, parents)`, `list` (sorted names), `absolute` (lexical: keeps symbolic links), `tempDir`, `cwd`, `chdir` |
| `protoio/process.h` | `process::run(argv, input)` returns `{exitCode, out, err}` (128 + signal when a signal ended the child); `run(argv, RunOptions{input, directory, environment})` also sets the child's working directory and replaces its environment, without a shell; `shell(command, input)` does the same for a command line handed verbatim to the system shell (`/bin/sh -c`, or `cmd.exe /d /s /c` on Windows); `spawn(argv)`, `wait(pid)`, `kill(pid, sig)`, `getenv`, `setenv`, `environment`, `pid`, `hostName`, `platform`, `exit` |
| `protoio/net.h` | `net::tcpConnect(host, port, timeoutMs)`, `tcpListen(host, port, backlog)` (an empty host: one dual-stack socket, IPv4 and IPv6), `tcpAccept(fd, timeoutMs)` (`std::nullopt` on timeout or close), `sockName`/`peerName`, `tlsConnect(fd, host, verify)`, `trustCertificates(pem)` (extra trusted CAs), `udpBind`, `udpSend` (resolved in the socket's own address family), `udpReceive(fd, timeoutMs)` (`std::optional<Datagram{data, host, port}>`) |
| `protoio/http.h` | The HTTP/1.1 message layer and client, below |

### HTTP (`protoio::http`)

- **Types:** `Headers` (an ordered vector of `{name, value}`; parsed names are
  lower case), `RequestHead {method, target, version, headers}`,
  `ResponseHead {status, reason, headers}`, `Limits {maxLine = 8192,
  maxHeaderLines = 100, maxBody = 64 MiB}`, `Url {scheme, host, port,
  target}`, `Request`, `Response`.
- **Parsing:** `readRequestHead(fd, limits)` answers `std::nullopt` on a clean
  end of stream and throws `HttpRefusal` (an `Error` with `status`) for 400
  (malformed request line or Content-Length), 414 (request line too long),
  431 (a header line too long, or more than 100) and 413 (declared body over
  `maxBody`). `readResponseHead(fd)`, `readHeaders(fd)`, `contentLength`,
  `readBody(fd, headers, maxBytes)` (chunked, decoded as bytes, or by
  Content-Length; over `maxBytes` throws BodyTooLarge).
- **Validation and output:** `validateToken`, `validateValue`,
  `validateTarget`, `validateHeaders` (InvalidArgument, the message contains
  "invalid"), `serializeRequest`/`writeRequest` (a content-length only when
  there is a body or the method is POST, PUT or PATCH),
  `serializeResponse`/`writeResponse`, `reasonPhrase`.
- **Helpers:** `percentDecode` (bytes, so `%C3%A9` is one UTF-8 character),
  `parseQuery` (`+` is a space, then decode), `parseUrl`, `authority`,
  `origin`, `resolveRedirect(location, base)` (RFC 3986; https to http is
  refused with Network).
- **Client:** `Response httpRequest(const Request&)` with `Request {method,
  url, headers, body, timeoutMs = 30000, maxRedirects = 5, userAgent}`.
  It verifies TLS certificates, follows 301/302/303/307/308 (303 turns into a
  GET without a body), drops the caller's headers on a redirect to another
  origin, refuses https to http, and validates everything before anything is
  sent.

A server stays in each runtime, because it dispatches to user code: the
runtime owns the accept loop and, per connection, calls `readRequestHead`,
`readBody`, its handler and `writeResponse`, answering an `HttpRefusal`'s
status (or 413 for BodyTooLarge and 400 for Network from `readBody`).

## Threading and blocking contract

- **Every call may block.** A runtime wraps each call in its own "leave the
  collector's quorum" bracket (`ProtoContext::UnmanagedScope`, plus protoST's
  `BlockingIO` accounting). The library touches no runtime object, so it
  needs no knowledge of the bracket.
- **Descriptors may be shared between threads:**
  - reads on one descriptor are serialised, and so are writes, independently
    of each other: each line goes to exactly one reader, and a thread waiting
    for input never blocks a writer;
  - `close` shuts a socket down, which wakes a thread blocked reading it,
    accepting on it or receiving from it at once (on macOS and Windows,
    where shutting a socket down does not wake every waiter, each waiting
    thread also waits on a wake-up channel of its own, which `close`
    signals);
  - a descriptor number is released only after its last user returns, so a
    concurrent `open` never reuses it under a waiting thread.
- **SIGPIPE:** writes never raise it (they fail with an Error carrying EPIPE),
  except to the process's own standard output, which keeps the default action
  so a filter piped into `head` stops as Unix filters do.
- **TLS:** a TLS socket is non-blocking; the handshake, reads and writes wait
  in `poll` bounded by `setTimeout`.
- **Descriptors the runtime opens itself** (with `pipe(2)`, `dup(2)`, ...)
  should be announced with `protoio::forget(fd)` in case the library holds a
  stale state for a reused number. Descriptors created by the library are
  handled automatically.
- Standard output and error (descriptors 1 and 2) are written through C stdio,
  keeping their order with the runtime's own printing.

## Errors

Every failure throws `protoio::Error`. `sysErrno` is the failing call's errno,
or 0 when there was none.

| Kind | When |
|---|---|
| `FileNotFound` | ENOENT on a file operation |
| `FileExists` | EEXIST on a file operation |
| `FileSystem` | any other file or pipe failure (including EPIPE on a pipe) |
| `ConnectionRefused` | ECONNREFUSED |
| `ConnectionTimedOut` | a connect, read, write or TLS handshake exceeded its timeout |
| `NameLookup` | a host did not resolve (in the socket's family, for UDP) |
| `Network` | other socket, TLS or HTTP protocol failures |
| `Process` | a child could not be run, waited for or signalled |
| `LineTooLong` | a line over the given limit |
| `BodyTooLarge` | an HTTP body over the given limit |
| `InvalidArgument` | a refused value: a header with a line break, a bad URL, an empty argv |

## Build, test, install

Requirements: CMake 3.21+, a C++20 compiler, OpenSSL 3 (`libssl-dev`).
GoogleTest is fetched at configure time.

```sh
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release -j4
ctest --test-dir build_release --output-on-failure      # sequential

# ThreadSanitizer (stream, net and process suites)
cmake -S . -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DPROTOIO_TSAN=ON
cmake --build build_tsan -j4
setarch "$(uname -m)" -R ctest --test-dir build_tsan -L 'stream|net|process' --output-on-failure

# Debian package: protoio-dev_0.2.2_<arch>.deb (static library, headers, CMake package)
(cd build_release && cpack -G DEB)
```

Options: `PROTOIO_BUILD_TESTS` and `PROTOIO_INSTALL` (both ON only for a
top-level build), `PROTOIO_TSAN`, `PROTOIO_WARNINGS_AS_ERRORS` (OFF; CI
turns it on: the library builds without warnings at `/W4` with MSVC and
`-Wall -Wextra -Wpedantic` with GCC and Clang).

A test that cannot run on a host is skipped with the reason
(`GTEST_SKIP`), and ctest lists it as skipped, not passed: the IPv6 loopback
tests where `::1` is unusable, the https test against a public host
(`www.github.com`) without network access, and the Windows console test
when no console can be created.

## Windows

protoIO builds natively with MSVC (Visual Studio 2022, C++20) and Ninja, from
a "x64 Native Tools Command Prompt for VS 2022" (or after `vcvars64.bat`).
Any OpenSSL 3 with headers and import libraries will do; PostgreSQL 17 ships
one:

```bat
cmake -S . -B C:\build\protoio -G Ninja -DCMAKE_BUILD_TYPE=Release "-DOPENSSL_ROOT_DIR=C:/Program Files/PostgreSQL/17"
cmake --build C:\build\protoio
ctest --test-dir C:\build\protoio --output-on-failure
cmake --install C:\build\protoio --prefix C:\protoio
```

The install leaves `lib\protoio.lib`, the headers and
`lib\cmake\protoIO\protoIOConfig.cmake`: a consumer configures with
`-DCMAKE_PREFIX_PATH=C:/protoio` and the same `OPENSSL_ROOT_DIR`, and uses the
`find_package` snippet below (`cpack` zips the same tree). protoIO links
`ws2_32` for its consumers. The library is built for one configuration (`/MD`
for Release, `/MDd` for Debug): install the one the consumer builds. The test
programs get the OpenSSL DLLs copied next to them; any other program that
links protoIO needs them on its `PATH` or beside it.

The API is the same; what differs:

- **Descriptors.** Files, pipes and the standard streams are C runtime
  descriptors (`_wopen`, `_pipe`), opened in binary mode. A `SOCKET` is not
  one, so each socket the library creates gets a number of its own (65536 and
  up); `net::nativeSocket(fd)` answers the `SOCKET` behind it, for calls the
  library does not make. A socket a runtime creates itself cannot be used
  with the stream functions.
- **Text.** Paths, arguments and environment strings are UTF-8, converted to
  and from the UTF-16 the system uses. `absolute`, `cwd` and `tempDir` answer
  native separators (`C:\dir\file`); `absolute("/")` is the drive root.
- **Reading is binary.** The library reads every descriptor as bytes with
  `ReadFile`, whatever the C runtime's mode for it. Standard input starts in
  text mode, where the C runtime would turn CR LF into LF and stop at a
  Ctrl+Z (0x1A) byte; through the library a 0x1A is data and CR LF arrives as
  it is (`readLine` strips the CR; `readAll` and `readBytes` keep it). A
  console is read with `ReadConsoleW` and answered as UTF-8, whatever its
  code page; there a line that starts with Ctrl+Z is the end of the input,
  as Ctrl+D is on a POSIX terminal, and lines end in CR LF.
- **Writing.** Files and pipes are written as bytes. Descriptors 1 and 2 go
  through the C stdio streams and stay as the C runtime opened them (text
  mode: a `"\n"` becomes CR LF); a runtime that wants raw bytes there calls
  `_setmode(fd, _O_BINARY)`.
- **SIGPIPE** does not exist: `SigpipeGuard` does nothing, and a write to a
  pipe whose reader is gone fails with EPIPE by itself.
- **Waits.** Closing a socket does not wake a thread waiting on it in
  `WSAPoll`, so each waiting thread also waits on a wake-up socket of its
  own (a UDP socket bound to 127.0.0.1, created on the thread's first wait
  and closed when the thread ends), and `close` sends it a byte: the waiter
  returns at once, with no polling interval. Should that socket be
  impossible to create, the thread falls back to waiting in 50 ms slices.
  Anonymous pipes cannot be polled: a read from one blocks in `ReadFile`,
  and a read timeout cancels it at the deadline (`CancelSynchronousIo`, from
  a thread-pool timer), so an idle pipe costs nothing while it waits. Read
  timeouts apply to console input too, but a console read cannot be
  cancelled cleanly (the cancelled read lives on in the console and swallows
  the next line), so a timed console read first waits for a complete line
  (any character, outside line mode): blocked on the console handle while
  nothing is typed, re-checking every 10 ms while a line is partly typed.
  Typed characters are echoed once the read starts, that is when the line
  is complete. A write to a pipe ignores the timeout. `close` does not wake a
  thread blocked reading a pipe (nor does it on POSIX; the contract is for
  sockets).
- **Errors.** Winsock and Win32 codes are mapped to the errno values of the
  POSIX build (`WSAECONNREFUSED` is `ECONNREFUSED`, a broken pipe is `EPIPE`,
  ...). A write to a TCP connection the peer closed reports `ECONNRESET` or
  `ECONNABORTED` where Linux reports `EPIPE`.
- **TCP.** `tcpListen` does not set `SO_REUSEADDR` (on Windows it would let
  another socket take the port) and sets `SO_EXCLUSIVEADDRUSE`, so no other
  socket can bind the port while it listens; the port can be listened on
  again at once after the listener closes, even while its connections linger
  in TIME_WAIT. Windows refuses a connect (with a reset) both when nobody
  listens and while the listener's backlog is full, and then sends the SYN
  again for about two seconds. On a loopback address those kernel retries
  are switched off and `tcpConnect` retries a refused connect itself, after
  10, 20, 40, 80 and 160 ms (within the timeout): a port nobody listens on
  is refused after about 0.3 s instead of 2 s (POSIX: at once), and a
  listener that drains a full backlog within that time gets the connection.
- **TLS.** Verification uses the Windows certificate stores: when the
  verifying context is created, every certificate of the current user's and
  the machine's `ROOT` and `CA` stores is added to OpenSSL's trust store
  (as are `SSL_CERT_FILE` and `SSL_CERT_DIR`, when set).
  `net::trustCertificates(pem)` adds more. This works with any OpenSSL 3;
  OpenSSL 3.2's `org.openssl.winstore:` store was not used because it ties
  verification to how the linked OpenSSL was built. One limit holds for
  both: Windows downloads some trusted roots from Windows Update only the
  first time its own (CryptoAPI) code needs them, so a root no program on
  the machine has used yet may be missing, and a site that chains to it
  fails with "unable to get local issuer certificate".
- **Files.** Files are opened with `CreateFileW`, sharing read, write and
  delete access, so a file a stream has open can still be deleted, renamed
  or replaced, as on POSIX. `move` renames with POSIX semantics
  (`FileRenameInfoEx`: an existing target is replaced even while open; on a
  file system without it, `MoveFileExW` replaces a target nobody has open).
- **Processes.** `run` and `spawn` use `CreateProcessW`, and the arguments
  are quoted so that the child's C runtime (`CommandLineToArgvW`) gets
  `argv` back unchanged.
  - *Program search.* A name with a directory (`.\tool.exe`, `C:\bin\x`) is
    used as it is. A bare name is searched in the application's directory,
    the system directory, the Windows directory and `PATH`, and **not** in
    the working directory (where `CreateProcess` would look before the system
    directories), so a planted `git.exe` or `cmd.exe` there never runs.
    `.exe` is appended when the name has no extension. With
    `RunOptions::directory`, a relative name with a directory (`bin\tool`)
    is taken from that directory, as on POSIX.
  - *Directory and environment.* `RunOptions::directory` and
    `RunOptions::environment` become `CreateProcessW`'s working directory and
    Unicode environment block (sorted by name, as `CreateProcess` documents).
    A bare program name is still searched in the caller's `PATH`. When the
    given environment has no `SystemRoot`, the caller's is added: some system
    DLLs (Winsock's among them) fail to load without it, which is also why
    the JVM adds it.
  - *Batch files are refused.* A target that is a `.bat` or `.cmd` file
    throws `InvalidArgument`: `CreateProcess` runs it through `cmd.exe`,
    which parses the command line again by its own rules, so an argument such
    as `a&calc` would run a second command whatever the quoting (BatBadBut,
    CVE-2024-24576). Running `cmd.exe /c ...` explicitly is the caller's
    choice of a shell, exactly like `sh -c` on POSIX: everything after `/c`
    is cmd syntax, and protoIO does not (cannot reliably) escape it. Never
    put untrusted text there.
  - *Shell commands.* `run({"cmd.exe", "/c", command})` quotes `command` by
    the C runtime's rules, which cmd.exe does not undo: `echo "a b"` reaches
    it as `"echo \"a b\""`. `shell(command, input)` hands a command line to
    cmd.exe verbatim instead: it runs `cmd.exe` from the system directory
    (never one found through `PATH`, `COMSPEC` or the working directory) with
    the command line `cmd.exe /d /s /c "<command>"`, as CPython's
    `subprocess` does for `shell=True`. `/s` makes cmd strip exactly the
    outer quotes, so `command` arrives as written; `/d` skips the AutoRun
    commands. The command is cmd syntax: the caller quotes or escapes any
    metacharacters (`& | < > ^ %`) in the data it puts there.
  - *Handles.* A child inherits only its three standard handles. They are
    created non-inheritable; inheritable duplicates exist only for the length
    of the `CreateProcessW` call and reach the child through
    `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. The residual window: a process that
    another thread starts during that call with handle inheritance on and no
    handle list (`system()`, `_popen`) can inherit those duplicates too, and
    then holds the pipe open until it exits.
  - *Exit codes.* A child that ends with an exception gets 128 + the POSIX
    signal (Linux numbers) for the same fault, as the shell answers for a
    child that signal ended: an access violation or a stack overflow is 139
    (SIGSEGV), Ctrl+C (`STATUS_CONTROL_C_EXIT`) 130 (SIGINT), a division by
    zero or another arithmetic fault 136 (SIGFPE), an illegal instruction 132
    (SIGILL), a breakpoint 133 (SIGTRAP), an in-page error or a misaligned
    access 135 (SIGBUS), and any other NTSTATUS error code (0xC0000000 and up:
    fail-fast, which `abort()` and buffer-overrun checks raise, heap
    corruption, ...) 134 (SIGABRT). Every other exit code is answered as it is
    (a code above 255 too, unlike POSIX).
  - *Signals.* `kill` supports 0, `SIGTERM` (15) and `SIGKILL` (9); the last
    two end the child with `TerminateProcess` and exit code 128 + signal, what
    `wait` and `run` answer on POSIX for a child that signal ended. Any other
    signal throws `Process` with `ENOSYS`. `wait` works for children started
    by `spawn` (others throw `Process`, `ECHILD`). `platform()` is
    `"windows"`.

## Consuming protoIO from CMake

protoIO exports the target `protoIO::protoio`. The recommended snippet for a
runtime prefers an installed package and falls back to a sibling checkout,
the same resolution order the runtimes use for protoCore:

```cmake
find_package(protoIO 0.2 CONFIG QUIET)
if(NOT protoIO_FOUND)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../protoIO/CMakeLists.txt")
        # Developer fallback: build the sibling source tree as part of this one.
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../protoIO"
                         "${CMAKE_CURRENT_BINARY_DIR}/_protoIO" EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "protoIO not found: install protoio-dev or check out ../protoIO")
    endif()
endif()
target_link_libraries(my_runtime PRIVATE protoIO::protoio)
```

`find_package` finds:

- an installed package (`/usr/lib/<arch>/cmake/protoIO/` from the deb, or any
  prefix given with `-DCMAKE_PREFIX_PATH=<prefix>`);
- a build tree, without installing: `-DprotoIO_DIR=<path>/protoIO/build_release`.

The package pulls in `OpenSSL::SSL`, `OpenSSL::Crypto` and `Threads::Threads`
as link dependencies, since the library is static.
