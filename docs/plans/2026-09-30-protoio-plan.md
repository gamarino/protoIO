# protoIO: implementation plan

Spec: [`docs/specs/2026-09-30-protoio-design.md`](../specs/2026-09-30-protoio-design.md) (approved 2026-09-30).
Mode: unattended. Each phase is committed locally on its own branch and merged
to the default branch after its gate passes. Everything is pushed only at the
end, after phase 6.

## Global constraints

- **Language.** Professional English in code, comments, docs and commit
  messages. Commit messages carry a description of what changed and why.
- **Attribution.** "Gustavo Marino" in LICENSE and copyright lines. Every
  commit ends with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.
- **Machine.** Tests and builds run sequentially on DEV12: no `ctest -j`, and
  `-j4` at most for builds.
- **Test first.** Every behaviour gets a test that fails first. The full suite
  is green before any merge. A test that never failed is reported as such.
- **Runtimes.** No native thread APIs; ProtoThreads only. Every blocking
  library call is bracketed with `ProtoContext::UnmanagedScope`, and touches
  no ProtoObject inside the bracket.
- **Which protoCore.** Verify the linked protoCore with `ldd`. The installed
  `/usr` protoCore 2.6.2 is current.
- **Project rules.** Read each project's own `CLAUDE.md` before working in it.
  protoScala additionally requires a D-deviation for any departure from
  Scala, and a tutorial fixture for every tutorial snippet.

## Phases and gates

| # | Phase | Branch | Gate |
|---|---|---|---|
| 1 | protoIO library: code moved from protoST's `io_prims.cpp` into `include/protoio/*.h` and `src/*.cpp`; HTTP message layer and client (§2.4); CMake with `find_package` config export; CPack `-dev` DEB; GoogleTest suite (§2.5); README, CHANGELOG, LICENSE | `main` (new repo) | ctest green; the TSan build green on the stream, net and process tests |
| 2 | protoST on protoIO: `io_prims.cpp` reduced to bindings; CMake finds protoIO (sibling fallback, like protoCore) | `feature/protoio` | 1068/1068 before and after; talk demos pass; conformance ratchet exit 0 |
| 3 | protoScala I/O (§4) | `feature/io` | full ctest green (VM and transpiled); new fixtures in `tests/conformance/27-io`; tutorial chapter; STATUS deviations |
| 4 | protoClojure exceptions (§5.1) | `feature/exceptions` | full ctest green; conformance fixtures for try/catch/finally/throw/ex-info and catchable primitive errors |
| 5 | protoClojure I/O (§5.2) | `feature/io` | full ctest green; fixtures; tutorial or docs chapter; STATUS deviations |
| 6 | Rebuild the installers of protoIO, protoST, protoScala and protoClojure; create `gamarino/protoIO` on GitHub; push every repository | — | each DEB installs its binary, which links `/usr` protoCore and runs a smoke program |
