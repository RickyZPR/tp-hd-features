#!/usr/bin/env python3
"""gxtex.py - GameCube GX texture encode/decode helpers (pure Python, no deps).

LIBRARY
  bc1_to_cmpr(bc1_linear, w, h) -> bytes      lossless DXT1/BC1 (PC block order, LE) -> GX CMPR
  cmpr_to_rgba(data, w, h) -> bytes           decode GX CMPR (for previews / checks)
  encode(fmt, rgba, w, h) -> bytes            RGBA8 -> GX I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8
  decode(fmt, data, w, h) -> rgba bytes       GX I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR -> RGBA8
  box_downscale(rgba, w, h, nw, nh) -> bytes  integer-factor box filter
  GX format ids: I4=0 I8=1 IA4=2 IA8=3 RGB565=4 RGB5A3=5 RGBA8=6 CMPR=14

CLI
  python -I gxtex.py preview <file.bti | arc:member.bti> <out.png>   decode a BTI to PNG

GX LAYOUTS (standard; tile = block of pixels stored contiguously, tiles row-major)
  I4 8x8 (4bpp, high nibble first)   I8 8x4   IA4 8x4 (A<<4|I)   IA8 4x4 (A,I)
  RGB565 4x4 (BE)   RGB5A3 4x4 (BE: 1RRRRRGGGGGBBBBB or 0AAARRRRGGGGBBBB)
  RGBA8 4x4 tile = 32 bytes AR pairs then 32 bytes GB pairs
  CMPR 8x8 tile = 4 DXT1 sub-blocks (TL,TR,BL,BR); colors BE; index bytes per row with
       pixel 0 in the top 2 bits (BC1: colors LE, pixel 0 in the low 2 bits).
"""
import os
import struct
import sys

I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR = 0, 1, 2, 3, 4, 5, 6, 14
TILE = {I4: (8, 8), I8: (8, 4), IA4: (8, 4), IA8: (4, 4), RGB565: (4, 4), RGB5A3: (4, 4), RGBA8: (4, 4), CMPR: (8, 8)}

_REV2 = bytes(((b & 3) << 6) | (((b >> 2) & 3) << 4) | (((b >> 4) & 3) << 2) | ((b >> 6) & 3) for b in range(256))


def bc1_to_cmpr(lin, w, h):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    tw, th = (bw + 1) // 2, (bh + 1) // 2
    out = bytearray(tw * th * 32)
    o = 0
    for ty in range(th):
        for tx in range(tw):
            for sy in range(2):
                for sx in range(2):
                    bx, by = tx * 2 + sx, ty * 2 + sy
                    if bx < bw and by < bh:
                        s = (by * bw + bx) * 8
                        c0, c1 = struct.unpack_from("<HH", lin, s)
                        out[o:o + 4] = struct.pack(">HH", c0, c1)
                        out[o + 4:o + 8] = bytes(_REV2[lin[s + 4 + r]] for r in range(4))
                    o += 8
    return bytes(out)


def _565(c):
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def cmpr_to_rgba(data, w, h):
    out = bytearray(w * h * 4)
    tw, th = (w + 7) // 8, (h + 7) // 8
    o = 0
    for ty in range(th):
        for tx in range(tw):
            for sy in range(2):
                for sx in range(2):
                    c0, c1 = struct.unpack_from(">HH", data, o)
                    p0, p1 = _565(c0), _565(c1)
                    if c0 > c1:
                        pal = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
                    else:
                        pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
                    for py in range(4):
                        row = data[o + 4 + py]
                        for px in range(4):
                            x = tx * 8 + sx * 4 + px; y = ty * 8 + sy * 4 + py
                            if x < w and y < h:
                                idx = (row >> (6 - 2 * px)) & 3
                                d = (y * w + x) * 4
                                out[d:d + 4] = bytes(pal[idx])
                    o += 8
    return bytes(out)


def _tiles(w, h, fmt):
    bw, bh = TILE[fmt]
    for ty in range((h + bh - 1) // bh):
        for tx in range((w + bw - 1) // bw):
            yield tx * bw, ty * bh, bw, bh


def _px(rgba, w, h, x, y):
    if x >= w or y >= h:
        return (0, 0, 0, 0)
    o = (y * w + x) * 4
    return rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]


def _lum(p):
    return (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8


def encode(fmt, rgba, w, h):
    out = bytearray()
    for x0, y0, bw, bh in _tiles(w, h, fmt):
        if fmt == RGBA8:
            ar = bytearray(); gb = bytearray()
            for y in range(bh):
                for x in range(bw):
                    r, g, b, a = _px(rgba, w, h, x0 + x, y0 + y)
                    ar += bytes((a, r)); gb += bytes((g, b))
            out += ar + gb
            continue
        nib = []
        for y in range(bh):
            for x in range(bw):
                p = _px(rgba, w, h, x0 + x, y0 + y)
                if fmt == I8:
                    out.append(p[0] if p[0] == p[1] == p[2] else _lum(p))
                elif fmt == I4:
                    nib.append((p[0] if p[0] == p[1] == p[2] else _lum(p)) >> 4)
                elif fmt == IA4:
                    out.append(((p[3] >> 4) << 4) | ((p[0] if p[0] == p[1] == p[2] else _lum(p)) >> 4))
                elif fmt == IA8:
                    out += bytes((p[3], p[0] if p[0] == p[1] == p[2] else _lum(p)))
                elif fmt == RGB565:
                    out += struct.pack(">H", ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3))
                elif fmt == RGB5A3:
                    if p[3] >= 0xE0:
                        v = 0x8000 | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3)
                    else:
                        v = ((p[3] >> 5) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4)
                    out += struct.pack(">H", v)
                else:
                    raise NotImplementedError("encode GX fmt %d (CMPR: use bc1_to_cmpr)" % fmt)
        if fmt == I4:
            for i in range(0, len(nib), 2):
                out.append((nib[i] << 4) | nib[i + 1])
    return bytes(out)


def decode(fmt, data, w, h):
    if fmt == CMPR:
        return cmpr_to_rgba(data, w, h)
    out = bytearray(w * h * 4)
    o = 0
    for x0, y0, bw, bh in _tiles(w, h, fmt):
        if fmt == RGBA8:
            for i in range(16):
                x, y = x0 + i % 4, y0 + i // 4
                a, r = data[o + 2 * i], data[o + 2 * i + 1]
                g, b = data[o + 32 + 2 * i], data[o + 33 + 2 * i]
                if x < w and y < h:
                    out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, a))
            o += 64
            continue
        for i in range(bw * bh):
            x, y = x0 + i % bw, y0 + i // bw
            if fmt == I4:
                v = data[o + i // 2]; v = (v >> 4) if i % 2 == 0 else (v & 15); v *= 17; p = (v, v, v, 255)
            elif fmt == I8:
                v = data[o + i]; p = (v, v, v, 255)
            elif fmt == IA4:
                v = data[o + i]; p = ((v & 15) * 17,) * 3 + ((v >> 4) * 17,)
            elif fmt == IA8:
                a, v = data[o + 2 * i], data[o + 2 * i + 1]; p = (v, v, v, a)
            elif fmt == RGB565:
                p = _565(struct.unpack_from(">H", data, o + 2 * i)[0]) + (255,)
            elif fmt == RGB5A3:
                v = struct.unpack_from(">H", data, o + 2 * i)[0]
                if v & 0x8000:
                    p = (((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
                else:
                    p = (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 // 7)
            else:
                raise NotImplementedError("decode GX fmt %d" % fmt)
            if x < w and y < h:
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(p)
        o += {I4: bw * bh // 2, I8: bw * bh, IA4: bw * bh}.get(fmt, bw * bh * 2)
    return bytes(out)


def box_downscale(rgba, w, h, nw, nh):
    fx, fy = max(1, w // nw), max(1, h // nh)
    out = bytearray(nw * nh * 4)
    n = fx * fy
    for y in range(nh):
        for x in range(nw):
            acc = [0, 0, 0, 0]
            for yy in range(fy):
                row = ((y * fy + yy) * w + x * fx) * 4
                for xx in range(fx):
                    o = row + xx * 4
                    acc[0] += rgba[o]; acc[1] += rgba[o + 1]; acc[2] += rgba[o + 2]; acc[3] += rgba[o + 3]
            d = (y * nw + x) * 4
            out[d:d + 4] = bytes(v // n for v in acc)
    return bytes(out)


def main(argv):
    if len(argv) < 4 or argv[1] != "preview":
        print(__doc__)
        return 1
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import j3d, gtx2img
    d = j3d.load(argv[2])
    t = j3d.bti_header(d, 0)
    px = d[t["img_off"]:]
    gtx2img.write_png(argv[3], t["w"], t["h"], decode(t["fmt"], px, t["w"], t["h"]))
    print("%s %dx%d -> %s" % (t["fmt_name"], t["w"], t["h"], argv[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
