# protoIO: shared input and output for the protoCore runtimes

Status: **approved** (2026-09-30). Author: Gustavo Marino, with Claude.

## 1. Goal

Give protoScala and protoClojure the same I/O reach that protoST gained in
0.5.0 (files, the operating system, other programs, TCP, UDP, TLS and HTTP).
Each runtime should expose it in its own language's idiom, and all three
should share one implementation of the risky part.

protoST's POSIX layer (`src/primitives/io_prims.cpp`, about 1,400 lines) needed
a review that found 3 critical and 8 important defects. The critical ones were
SIGPIPE, descriptors shared between threads, and a pool mutex held across an
allocation. Copying that layer into two more runtimes would triple the surface
for the same class of bug. Instead, the layer moves into one library that the
three runtimes link, and a defect gets fixed once.

Decided with the maintainer (2026-09-30):

- a common C++ library, separate from protoCore;
- protoClojure gets exceptions (try/catch/finally, throw, ex-info) before I/O;
- order of work: the library with protoST migrated onto it, then protoScala,
  then protoClojure.

## 2. The library

### 2.1 What it is

- **Location and build.** A new sibling project, `protoIO`, MIT licensed. It
  builds a static library, `libprotoio.a`, with public headers under
  `include/protoio/`.
- **Deployment.** It is linked statically into each runtime binary, so the
  runtimes' packages gain no runtime dependency on protoIO; only OpenSSL is
  added. It is found at build time through `find_package(protoIO)` or the
  sibling-directory fallback, the same mechanism protoCore uses.
- **Independence.** It has no dependency on protoCore and knows nothing about
  ProtoObjects, contexts or garbage collection.

### 2.2 Contract with the runtimes

- **Plain C++ types only.** Inputs and outputs are `std::string` (bytes, UTF-8
  where text), integers, `std::vector` and small structs. No function takes or
  returns a runtime value.
- **Every function may block.** The runtime wraps each call in its own
  "leave the collector's quorum" bracket (`ProtoContext::UnmanagedScope`,
  plus protoST's `BlockingIO` pool accounting). The library never needs to
  know about it, because it touches no runtime object.
- **Errors.** Failures throw `protoio::Error { Kind kind; int sysErrno;
  std::string message; }`. `Kind` is one of FileNotFound, FileExists,
  FileSystem, ConnectionRefused, ConnectionTimedOut, NameLookup, Network,
  Process, LineTooLong, BodyTooLarge, InvalidArgument. Each runtime maps a
  Kind to its own error class: protoST's `FileDoesNotExist`, protoScala's
  `FileNotFoundException`, protoClojure's ex-info with `:type`.
- **Threads.** Descriptors, TLS sessions and the buffered state may be used
  from several threads at once, with the guarantees protoST documents:
  - reads are serialised, and so are writes, independently of each other;
  - `close` wakes a thread blocked on a socket;
  - a descriptor number is released only after its last user returns.

### 2.3 Modules

The code is moved out of protoST, not rewritten; the review fixes come with it.

| Header | Contents |
|---|---|
| `protoio/stream.h` | Descriptor registry and `FdState`. `readLine(fd, max)`, `readAll`, `readBytes(n)`, `readChars(n)` (whole UTF-8 characters), `atEnd`, `write`, `flush`, `close`, `setTimeout`. A per-thread SIGPIPE guard: only the process's own stdout keeps the default action. |
| `protoio/file.h` | `open`, whole-file `read`/`write`/`append`, `stat`, `remove` (recursive), `move`, `copy`, `mkdir`, `list`, `absolute` (lexical), `tempDir`, `cwd`, `chdir`. |
| `protoio/process.h` | `run(argv, input)` returning exit code, stdout and stderr, feeding input without SIGPIPE; `spawn(argv)`; `wait(pid)`; `kill(pid, sig)`; `getenv`, `setenv`, `environment`, `pid`, `hostName`, `platform`, `exit`. |
| `protoio/net.h` | `tcpConnect(host, port, timeout)`, `tcpListen`, `tcpAccept(timeout)`, `sockName`/`peerName`, `tlsConnect(fd, host, verify)` (non-blocking, poll-bounded), `udpBind`, `udpSend` (resolves to the socket's address family), `udpReceive(timeout)`. |
| `protoio/http.h` | The HTTP/1.1 message layer, described in §2.4. |

### 2.4 HTTP in the library

protoST's HTTP is protoST code (`lib/http.st`) on top of primitives.
protoClojure has no standard library in its own language to host such code,
and HTTP's safety rules (limits, header validation, redirects) should not be
written three times. The library therefore carries the HTTP message layer:

- **Parsing.** Read a request or response head with limits: an 8 KiB line,
  100 header lines, and a validated Content-Length. Read a body by
  Content-Length or chunked (as bytes), with a maximum size.
- **Validation and serialisation.** Validate a method, target, header name or
  header value (no control characters). Serialise a request or a response.
- **Helpers.** Percent-decode as UTF-8 bytes. Parse a query string, where
  `+` is a space. Resolve a redirect per RFC 3986 and apply the redirect
  policy: a cross-origin redirect drops the caller's headers, and
  https-to-http is refused.
- **Client.** `httpRequest(method, url, headers, body, timeoutMs,
  maxRedirects)` returns status, reason, headers and body. It is one
  blocking call, run by the runtime inside its unmanaged bracket.

The server stays in each runtime, because it dispatches to user code on
actors. The runtime owns the accept loop; for each connection it calls the
library to read the request (limits and 414/431/400/413 included), calls the
user's handler, validates the response through the library, and writes it.

protoST keeps its `lib/http.st` for now; it is tested and documented.
Moving its parsing onto the library's primitives is a later, optional step,
and is not part of this work.

### 2.5 Tests

The library gets its own GoogleTest suite, run under ctest, that exercises
each module against real resources:
- temporary files;
- local TCP and UDP pairs;
- a child process that closes its stdin;
- concurrent readers of one pipe;
- chunked and limit cases on a loopback HTTP server;
- the redirect-resolution table.

Where a test existed in protoST's conformance suite, the library gets the C++
equivalent. It also runs under ThreadSanitizer.

## 3. protoST on the library

`io_prims.cpp` keeps only the bindings: argument conversion, the
`BlockingIO` + `UnmanagedScope` bracket, error-Kind mapping and value
construction. No protoST-visible behaviour changes. The proof is the existing
suite, 1068 cases including `tests/conformance/14-io/` and `cli_io`, run
before and after, plus the talk demos.

## 4. protoScala

Follow Scala by default; wherever the API differs from what a Scala
programmer would write, the difference is recorded as a D-deviation. There is
no `scala.*` or `java.*` namespace (D8), so the names below are prelude
globals.

- **Standard input.** `StdIn.readLine()` (answers null at end of input),
  `StdIn.readInt()`, `StdIn.readDouble()`, and `Source.stdin` with
  `getLines()`.
- **The program.** `sys.env` (a Map), `sys.exit(n)`, `sys.props` only if
  cheap. The script's `args` come from the CLI.
- **Files.** Extend D102's `FileIO` with `readBytes`, `writeBytes`, `list`,
  `mkdirs`, `move`, `copy`, `isDirectory`, `size` and `lastModified`.
  `Source.fromFile(...).getLines()` reads line by line instead of the whole
  file at once.
- **Other programs.** `scala.sys.process`'s shape: `Process(Seq("ls", "-l")).!`
  answers the exit code, `.!!` answers the output (throws on a non-zero exit),
  and `.run()` answers a handle with `exitValue()` and `destroy()`. The
  `"cmd".!` string form is provided if the compiler can express an extension
  on String; otherwise it is a documented deviation.
- **Sockets.** `Socket(host, port)` with `readLine()`, `read(n)`,
  `write(s)`, `writeBytes`, `setSoTimeout`, `startTls(host)` and `close()`.
  `ServerSocket(port)` with `accept()`, `accept(timeoutMs)` and `localPort`.
  `DatagramSocket` for UDP. The names follow java.net, but the streams are
  simplified: no InputStream/Reader stack. That is a deviation.
- **HTTP client.** requests-scala's shape, the most direct Scala idiom:
  `Requests.get(url, headers = Map(...))`, `post(url, data = ...)`, `put`,
  `delete`, `send(method, url, ...)`, answering `statusCode`, `text()`,
  `headers` and `bytes`.
- **HTTP server.** `HttpServer(port) { req => Response(200, "text") }`,
  served on actors, with `start()`, `startInBackground()`, `stop()` and
  `port`.
- **Exceptions.** Add `IOException` subclasses `ConnectException`,
  `SocketTimeoutException` and `UnknownHostException`, and map the other
  Kinds onto them.
- **The existing file primitives.** They get the unmanaged bracket, which
  they lack today.

## 5. protoClojure

### 5.1 Exceptions first

- **Forms.** `try` / `catch` / `finally` and `throw` as special forms.
  `ex-info`, `ex-data`, `ex-message` and `ex-cause` as functions.
- **Catching.** `catch` takes a class symbol: `Exception`, `Throwable`,
  `ExceptionInfo`, `IOException`, `ArithmeticException`, … mapped onto a
  small built-in hierarchy.
- **Primitive errors.** An error raised by a C++ primitive (today a
  `std::runtime_error` whose message starts with the Java class name)
  becomes a catchable exception of that class. Unhandled errors keep
  today's top-level report.
- **Placement in the plan.** This is a language feature with its own
  conformance tests; it lands before any I/O.

### 5.2 I/O

There are no namespaces yet (`ns`/`require`, STATUS), so the functions are
globals named as in Clojure, the same way `clojure.string` is provided today.

- **Core.** `slurp` and `spit` (with `:append`; `slurp` of an http(s) URL
  fetches it, as in Clojure), `read-line`, `line-seq` over a reader,
  `(reader path)` and `(writer path)` with `with-open`, `file-seq`.
- **Shell.** `sh` from `clojure.java.shell`, answering `{:exit :out :err}`,
  with `:in` and `:dir`.
- **Environment.** `(getenv "X")` and `(exit n)`, since `System/getenv`
  needs Java interop syntax.
- **HTTP.**
  - Client: babashka `http-client`'s shape, named `http-get`,
    `http-post` and `http-request` until namespaces exist. It answers
    `{:status :headers :body}` and throws ex-info on a network error.
  - Server: Ring's contract, the Clojure standard.
    `(run-server handler {:port 8080})` calls `(handler request-map)` on
    actors, and the handler answers `{:status :headers :body}`.
- **Sockets.** `tcp-connect`, `tcp-listen`, `tcp-accept`, `udp-socket`, …
  Clojure has no standard-library sockets, only Java interop, so these are
  protoClojure's own, recorded as a deviation.
- **Errors.** ex-info carrying `{:type :file-not-found, :errno …}` and
  friends, catchable as `ExceptionInfo`, or as `IOException` for the
  I/O kinds.

## 6. Delivery

Each step is its own branch, with a test that fails first, the full suite
green, docs, CHANGELOG, and the tutorial updated.

1. **protoIO.** Library, tests and CI; package it as a `-dev` deb (static
   library and headers).
2. **protoST.** Migrate onto protoIO; 1068/1068 before and after.
3. **protoScala.** I/O as in §4, with conformance fixtures under
   `tests/conformance/27-io/…`, both in the VM and transpiled, plus a
   tutorial chapter.
4. **protoClojure (a).** Exceptions.
5. **protoClojure (b).** I/O as in §5.2, with conformance fixtures and a
   tutorial chapter.
6. **Packaging.** Rebuild the installers of the three runtimes.

## 7. Decisions on the open points (2026-09-30)

- The repository is `gamarino/protoIO`, public like the runtimes'. It is pushed at the end of the whole work.
- HTTP clients follow the proposal: requests-scala for protoScala; babashka http-client and Ring for protoClojure.
- POSIX only; Windows through WSL2.
