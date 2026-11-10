#!/usr/bin/env python3
# Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
"""Generate the CPython byte-compatibility vectors for protoIO.

Run with:  python3 tests/vectors/generate.py

(`python3 -I` also works, but only with a system-wide or venv msgpack: -I
ignores the per-user site directory.)

Uses only zlib, hashlib, hmac, base64, struct and msgpack (1.x). The output is
deterministic: no randomness, fixed key order, no timestamps. Binary data is
hex-encoded; inputs too large to commit are given as generator descriptors
(see README.md).
"""
import base64
import hashlib
import hmac
import json
import math
import os
import struct
import zlib

import msgpack

OUT = os.path.dirname(os.path.abspath(__file__))


def write(name, rows):
    with open(os.path.join(OUT, name), "w", encoding="ascii", newline="\n") as f:
        json.dump(rows, f, indent=1)
        f.write("\n")


def seq(n, seed=0):
    """Deterministic filler: byte i is (i * 7 + seed) mod 256."""
    return bytes((i * 7 + seed) % 256 for i in range(n))


def big_seq(n):
    """Large-input pattern 'seq': byte i is i mod 251."""
    return bytes(i % 251 for i in range(n))


def data_field(b, large=False):
    """Small inputs are hex strings; large ones are generator descriptors."""
    if large:
        return {"pattern": "seq", "length": len(b)}
    return b.hex()


# ---------------------------------------------------------------- CRC32
def gen_crc32():
    rows = []
    for n in (0, 1, 3, 4, 7, 8, 63, 64, 65, 4096, 1 << 20):
        large = n >= 65536
        d = big_seq(n) if large else seq(n, 3)
        rows.append({"data": data_field(d, large), "init": 0, "crc": zlib.crc32(d)})
    # Continuation from a non-zero running CRC.
    for n, init in ((0, 0x12345678), (5, 0xDEADBEEF), (64, 0xFFFFFFFF), (100, 1)):
        d = seq(n, 11)
        rows.append({"data": d.hex(), "init": init, "crc": zlib.crc32(d, init)})
    rows.append({"data": b"123456789".hex(), "init": 0, "crc": zlib.crc32(b"123456789")})
    combine = []
    for la, lb in ((0, 0), (0, 5), (5, 0), (1, 1), (3, 4), (7, 64), (64, 7),
                   (63, 65), (100, 1000), (1000, 100), (4096, 4096)):
        a, b = seq(la, 1), seq(lb, 2)
        combine.append({
            "a_hex": a.hex(), "b_hex": b.hex(),
            "crc_a": zlib.crc32(a), "crc_b": zlib.crc32(b),
            "len_b": lb, "combined": zlib.crc32(a + b),
        })
    write("crc32.json", {"crc": rows, "combine": combine})


# -------------------------------------------------------------- BLAKE2b
def gen_blake2b():
    rows = []
    msgs = [b"", b"abc", seq(1, 5), seq(127, 5), seq(128, 5), seq(129, 5), seq(200, 5)]
    keys = [b"", seq(1, 9), seq(32, 9), seq(64, 9)]
    for size in (1, 8, 32, 64):
        for key in keys:
            for m in msgs:
                h = hashlib.blake2b(m, digest_size=size, key=key)
                rows.append({"data": m.hex(), "key": key.hex(), "size": size,
                             "digest": h.hexdigest()})
    # RFC 7693 Appendix A: BLAKE2b-512("abc"), no key. The whole digest, and
    # it must be one of the rows the C++ tests check.
    rfc = ("ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
           "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923")
    assert hashlib.blake2b(b"abc").hexdigest() == rfc
    assert {"data": b"abc".hex(), "key": "", "size": 64, "digest": rfc} in rows
    write("blake2b.json", rows)


# ------------------------------------------------------- SHA-256 / HMAC
def gen_sha256_hmac():
    rows = []
    for n in (0, 1, 3, 55, 56, 63, 64, 65, 119, 120, 128, 1000):
        d = seq(n, 17)
        rows.append({"data": d.hex(), "sha256": hashlib.sha256(d).hexdigest()})
    d = b"abc"
    rows.append({"data": d.hex(), "sha256": hashlib.sha256(d).hexdigest()})
    for kl in (0, 1, 5, 32, 64, 65, 131):
        for dl in (0, 8, 64, 200):
            k, d = seq(kl, 23), seq(dl, 29)
            rows.append({"key": k.hex(), "data": d.hex(),
                         "hmac": hmac.new(k, d, hashlib.sha256).hexdigest()})
    # RFC 4231 test case 2.
    k, d = b"Jefe", b"what do ya want for nothing?"
    rows.append({"key": k.hex(), "data": d.hex(),
                 "hmac": hmac.new(k, d, hashlib.sha256).hexdigest()})
    write("sha256_hmac.json", rows)


# --------------------------------------------------------------- base64
def gen_base64():
    rows = []
    for n in list(range(0, 13)) + [31, 32, 33, 255, 256, 257]:
        d = seq(n, 31)
        rows.append({"data": d.hex(), "text": base64.b64encode(d).decode("ascii")})
    d = bytes(range(256))
    rows.append({"data": d.hex(), "text": base64.b64encode(d).decode("ascii")})
    d = b"\xfb\xff\xbe\xff\xfe"  # exercises '+' and '/'
    rows.append({"data": d.hex(), "text": base64.b64encode(d).decode("ascii")})
    write("base64.json", rows)


# -------------------------------------------------------------- packing
def gen_packing():
    rows = []
    for width in (1, 2, 4, 8):
        bits = width * 8
        umax, imin, imax = (1 << bits) - 1, -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        uvals = sorted({0, 1, 0x7F if width > 1 else 0x55, umax, imax, imax + 1,
                        0xA5 if width == 1 else 0x1234 if width == 2
                        else 0x12345678 if width == 4 else 0x0123456789ABCDEF})
        ivals = sorted({imin, imin + 1, -2, -1, 0, 1, imax - 1, imax})
        for be in (False, True):
            order = ">" if be else "<"
            code = {1: "B", 2: "H", 4: "I", 8: "Q"}[width]
            for v in uvals:
                rows.append({"kind": "u", "width": width, "big_endian": be,
                             "value": str(v), "bytes": struct.pack(order + code, v).hex()})
            for v in ivals:
                rows.append({"kind": "i", "width": width, "big_endian": be,
                             "value": str(v),
                             "bytes": struct.pack(order + code.lower(), v).hex()})
    f64 = [0.0, -0.0, 1.0, -1.5, math.pi, math.inf, -math.inf,
           5e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
           struct.unpack(">d", bytes.fromhex("7ff8000000000000"))[0],
           struct.unpack(">d", bytes.fromhex("7ff8000000000123"))[0]]
    f32 = [0.0, -0.0, 1.0, -1.5, 0.1, math.inf, -math.inf,
           1.401298464324817e-45, 1.1754943508222875e-38, 3.4028234663852886e38,
           struct.unpack(">f", bytes.fromhex("7fc00000"))[0],
           struct.unpack(">f", bytes.fromhex("7fc00123"))[0]]
    for be in (False, True):
        order = ">" if be else "<"
        for v in f32:
            raw = struct.pack(order + "f", v)
            bits = raw if be else raw[::-1]  # value bit pattern is big-endian hex
            rows.append({"kind": "f32", "width": 4, "big_endian": be,
                         "value": bits.hex(), "bytes": raw.hex()})
        for v in f64:
            raw = struct.pack(order + "d", v)
            bits = raw if be else raw[::-1]
            rows.append({"kind": "f64", "width": 8, "big_endian": be,
                         "value": bits.hex(), "bytes": raw.hex()})
    write("packing.json", rows)


# ------------------------------------------------------------- MessagePack
class V:
    """A Python value paired with its tagged-tree description."""

    def __init__(self, py, tree):
        self.py, self.tree = py, tree


def nil():
    return V(None, {"t": "nil"})


def boolean(b):
    return V(b, {"t": "bool", "v": b})


def integer(n):
    return V(n, {"t": "int", "v": str(n)})


def string(s):
    return V(s, {"t": "str", "v": s})


def binary(b):
    return V(b, {"t": "bin", "v": b.hex()})


def f64(bits_hex):
    x = struct.unpack(">d", bytes.fromhex(bits_hex))[0]
    return V(x, {"t": "f64", "bits": bits_hex})


def array(items):
    return V([i.py for i in items], {"t": "array", "v": [i.tree for i in items]})


def mapping(pairs):
    return V({k.py: v.py for k, v in pairs},
             {"t": "map", "v": [[k.tree, v.tree] for k, v in pairs]})


def ext(code, data):
    return V(msgpack.ExtType(code, data), {"t": "ext", "type": code, "v": data.hex()})


def f64_bits(tree):
    """The bit patterns (hex) of every float64 in a value tree."""
    if tree.get("t") == "f64":
        yield tree["bits"]
    elif tree.get("t") == "array":
        for item in tree["v"]:
            yield from f64_bits(item)
    elif tree.get("t") == "map":
        for key, value in tree["v"]:
            yield from f64_bits(key)
            yield from f64_bits(value)


def gen_msgpack():
    rows = []

    def add(desc, v):
        packed = msgpack.packb(v.py, use_bin_type=True)
        # Self-check: the tree's Python value must decode back from the bytes.
        back = msgpack.unpackb(packed, raw=False, strict_map_key=False)
        if not (isinstance(back, float) and math.isnan(back)):
            assert back == v.py or isinstance(v.py, msgpack.ExtType), desc
        # Every float64 is packed with its exact bit pattern (NaN payloads
        # included), which == above cannot check.
        for bits in f64_bits(v.tree):
            assert bytes.fromhex(bits) in packed, (desc, bits)
        rows.append({"description": desc, "bytes": packed.hex(), "value": v.tree})

    add("nil", nil())
    add("false", boolean(False))
    add("true", boolean(True))
    for n in (0, 1, 127, 128, 255, 256, 65535, 65536, 4294967295, 4294967296,
              18446744073709551615, -1, -32, -33, -128, -129, -32768, -32769,
              -2147483648, -2147483649, -9223372036854775808, 9223372036854775807,
              9223372036854775808):
        add("int %d" % n, integer(n))
    for bits, d in (("0000000000000000", "+0.0"), ("8000000000000000", "-0.0"),
                    ("3ff0000000000000", "1.0"), ("400921fb54442d18", "pi"),
                    ("7ff0000000000000", "+inf"), ("fff0000000000000", "-inf"),
                    ("7ff8000000000000", "quiet NaN"),
                    ("7ff8000000000123", "NaN with payload 0x123"),
                    ("0000000000000001", "smallest subnormal"),
                    ("7fefffffffffffff", "largest finite")):
        add("float64 " + d, f64(bits))
    for n in (0, 1, 31, 32, 255, 256):
        add("str len %d" % n, string("s" * n))
    add("str non-ASCII", string("héllo 世界 \U0001f600"))
    for n in (0, 1, 255, 256):
        add("bin len %d" % n, binary(seq(n, 41)))
    # bin32 boundary: the value is a generator descriptor (pattern "seq").
    big = V(big_seq(65536), {"t": "bin", "pattern": "seq", "length": 65536})
    add("bin len 65536", big)
    for n in (0, 1, 15, 16):
        add("array len %d" % n, array([integer(i % 200) for i in range(n)]))
    for n in (0, 1, 15, 16):
        add("map len %d" % n,
            mapping([(string("k%d" % i), integer(i)) for i in range(n)]))
    add("map with int keys", mapping([(integer(1), string("a")), (integer(-1), string("b"))]))
    add("nested", mapping([
        (string("name"), string("proto")),
        (string("list"), array([integer(1), integer(-33), nil(), boolean(True),
                                f64("3ff8000000000000"), binary(b"\x00\x01"),
                                array([array([])])])),
        (string("obj"), mapping([(string("x"), integer(2 ** 63)),
                                 (string("y"), mapping([]))])),
    ]))
    for size in (1, 2, 4, 8, 16, 3, 5, 17, 255, 256):
        add("ext type 5 size %d" % size, ext(5, seq(size, 43)))
    add("ext type 0 empty", ext(0, b""))
    add("ext type 127", ext(127, b"\x01\x02"))
    write("msgpack.json", rows)


if __name__ == "__main__":
    gen_crc32()
    gen_blake2b()
    gen_sha256_hmac()
    gen_base64()
    gen_packing()
    gen_msgpack()
