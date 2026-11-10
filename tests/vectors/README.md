# protoIO test vectors

Byte-compatibility oracle for the C++ tests (CRC32, BLAKE2b, SHA-256/HMAC,
base64, integer/float packing, MessagePack). The values are produced by
CPython's standard library and `msgpack-python`; the C++ tests must match them
exactly. Both the generator and its output are committed, so CI does not need
Python.

## Regenerating

    python3 tests/vectors/generate.py

Generated with CPython 3.14.0 and msgpack 1.1.2. Any CPython 3.12+ and
msgpack 1.x should give identical files. The output is deterministic (no
randomness, fixed key order); `git diff --exit-code tests/vectors` must be
clean after a rerun. Note that `python3 -I` ignores the per-user site
directory, so use it only if `msgpack` is installed system-wide or in a venv.

## Conventions

* Binary data is lowercase hex.
* Integers that may exceed 2^53 are decimal strings (`"value"` in
  `packing.json`, `{"t":"int","v":...}` in `msgpack.json`). Plain JSON numbers
  are used only for `init`, `crc`, `size`, `width`, `len_b` and the CRC
  values, all below 2^32.
* Large inputs are generator descriptors, not hex:
  `{"pattern": "seq", "length": N}` means N bytes where byte `i` is `i % 251`.
  They appear as the `data` of the 1 MiB CRC case in `crc32.json` and as the
  `value` of the 65536-byte `bin` case in `msgpack.json` (as
  `{"t":"bin","pattern":"seq","length":65536}`). A `data` field is therefore
  either a hex string or such a descriptor.

## Files

* `crc32.json`: `{"crc": [{data, init, crc}], "combine": [{a_hex, b_hex,
  crc_a, crc_b, len_b, combined}]}`. `crc = zlib.crc32(data, init)`;
  `combined = zlib.crc32(a + b)`.
* `blake2b.json`: array of `{data, key, size, digest}`; `key` is empty hex for
  unkeyed hashing; `size` is the digest size in bytes (1, 8, 32, 64). Includes
  the RFC 7693 `abc` case (size 64, no key).
* `sha256_hmac.json`: array of `{data, sha256}` and `{key, data, hmac}`
  (HMAC-SHA-256), including RFC 4231 test case 2.
* `base64.json`: array of `{data, text}`; standard alphabet with padding.
* `packing.json`: array of `{kind, width, big_endian, value, bytes}`.
  `kind` is `u`, `i`, `f32` or `f64`; `width` is in bytes. For `u`/`i`,
  `value` is a decimal string. For `f32`/`f64`, `value` is the IEEE-754 bit
  pattern as big-endian hex (8 or 16 digits); `bytes` is the stored
  representation in the stated byte order. Includes NaN (quiet, and with a
  payload), -0.0, +/-inf, subnormals, and min/max of each width.
* `msgpack.json`: array of `{description, bytes, value}`, with `bytes`
  produced by `msgpack.packb(v, use_bin_type=True)`. `value` is a tagged tree:
  `{"t":"nil"}`, `{"t":"bool","v":true}`, `{"t":"int","v":"-33"}`,
  `{"t":"f64","bits":"<16 hex>"}`, `{"t":"str","v":"..."}`,
  `{"t":"bin","v":"<hex>"}`, `{"t":"array","v":[...]}`,
  `{"t":"map","v":[[k,v],...]}` (insertion order), and
  `{"t":"ext","type":n,"v":"<hex>"}`. Floats always encode as float64
  (`0xcb`); NaN payload bits and -0.0 are preserved by `packb`.
  msgpack-python restricts ext type codes to 0..127.
