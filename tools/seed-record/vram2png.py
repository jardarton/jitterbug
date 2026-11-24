#!/usr/bin/env python3
"""Convert raw VRAM dumps (u16 little-endian, A1B5G5R5) into golden PNGs.

Usage: vram2png.py OUT_DIR RAW...

RAW is named NN-scene.Sx.raw (S = 1 for 1024x512, N for 1024N x 512N); the
PNG is OUT_DIR/NN-scene.Sx.png, 8-bit RGBA: each 5-bit channel shifted left
by 3 (low three bits zero), alpha 255 where the mask bit (15) is clear and
128 where it is set. tests/seed/FORMAT.md, "Golden images", is the reference.
Output is deterministic: per-row filter chosen by the smallest sum of
absolute values, zlib level 9.
"""
import os
import struct
import sys
import zlib


def rgba(pix):
    """8-bit RGBA bytes for a sequence of u16 pixels."""
    out = bytearray(len(pix) * 4)
    for i, p in enumerate(pix):
        out[i * 4] = (p & 31) << 3
        out[i * 4 + 1] = ((p >> 5) & 31) << 3
        out[i * 4 + 2] = ((p >> 10) & 31) << 3
        out[i * 4 + 3] = 128 if p & 0x8000 else 255
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filtered(row, prev):
    n = len(row)
    sub = bytearray(n)
    up = bytearray(n)
    pae = bytearray(n)
    for i in range(n):
        a = row[i - 4] if i >= 4 else 0
        b = prev[i]
        c = prev[i - 4] if i >= 4 else 0
        sub[i] = (row[i] - a) & 255
        up[i] = (row[i] - b) & 255
        pae[i] = (row[i] - paeth(a, b, c)) & 255
    cands = [(0, row), (1, bytes(sub)), (2, bytes(up)), (4, bytes(pae))]

    def cost(c):
        return sum(v if v < 128 else 256 - v for v in c[1])

    kind, data = min(cands, key=cost)
    return bytes([kind]) + data


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)


def write_png(path, raw, w, h):
    pix = struct.unpack("<%dH" % (w * h), raw)
    prev_pix = None
    prev = bytes(w * 4)
    out = bytearray()
    for y in range(h):
        row_pix = pix[y * w:(y + 1) * w]
        if row_pix == prev_pix:
            out += bytes([2]) + bytes(w * 4)  # Up filter: the row repeats
            continue
        row = rgba(row_pix)
        if len(set(row_pix)) == 1:
            out += bytes([0]) + row  # one colour: compresses to nothing anyway
        else:
            out += filtered(row, prev)
        prev, prev_pix = row, row_pix
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(out), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def read_png_as_vram(path):
    """Decodes a PNG written above back into u16 pixels (a self-check)."""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4
    prev = bytearray(stride)
    pix = []
    for y in range(h):
        kind = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            row[i] = (row[i] + (0, a, b, None, paeth(a, b, c))[kind]) & 255 if kind else row[i]
        for x in range(w):
            r, g, b, al = row[x * 4:x * 4 + 4]
            pix.append((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | (0x8000 if al == 128 else 0))
        prev = row
    return w, h, struct.pack("<%dH" % len(pix), *pix)


def main():
    args = sys.argv[1:]
    check = args and args[0] == "--check"
    if check:
        args = args[1:]
    out_dir = args[0]
    for src in args[1:]:
        base = os.path.basename(src)[:-len(".raw")]
        scale = int(base.rsplit(".", 1)[1].rstrip("x"))
        w, h = 1024 * scale, 512 * scale
        with open(src, "rb") as f:
            raw = f.read()
        if len(raw) != w * h * 2:
            sys.exit("%s: %d bytes, want %d" % (src, len(raw), w * h * 2))
        dst = os.path.join(out_dir, base + ".png")
        write_png(dst, raw, w, h)
        if check and read_png_as_vram(dst) != (w, h, raw):
            sys.exit("%s does not decode back to %s" % (dst, src))


if __name__ == "__main__":
    main()
