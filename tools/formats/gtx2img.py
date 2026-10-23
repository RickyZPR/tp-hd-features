#!/usr/bin/env python3
"""gtx2img.py - de-tile Wii U GX2 surfaces (TP HD .gtx inside .pack.gz) and export DDS / PNG.

CLI USAGE
  python -I gtx2img.py <file.gtx | file.pack(.gz):member.gtx> <out_dir> [--png] [--dds] [--surface N]
        Writes <out_dir>/<stem>_<N>.dds (base mip, linear) and/or .png (decoded RGBA, compSel applied).
        Default: both. Use Windows-style paths (C:/..., F:/...) in the 'pack:member' form.
  python -I gtx2img.py --pack <file.pack.gz> <out_dir> [--png] [--dds] [--filter SUBSTR]
        Every .gtx member of a TMPK pack (optionally only names containing SUBSTR).

LIBRARY
  import gtx2img; lin = gtx2img.deswizzle(surface_dict, image_bytes)   # base level, linear
  rgba = gtx2img.to_rgba(surface_dict, lin)                            # bytes w*h*4
  gtx2img.write_png(path, w, h, rgba);  gtx2img.write_dds(path, surface_dict, lin)

IMPLEMENTATION NOTES
  Address function = R600/R700 addrlib ComputeSurfaceAddrFromCoordMacroTiled / MicroTiled /
  Linear for the Wii U (Latte) config: 2 pipes, 4 banks, 256-byte pipe interleave.
  Only slice 0 / sample 0 / base mip are handled (enough for texture replacement).
  Supported: BC1/BC2/BC3/BC4/BC5 (element = 4x4 block), R8, R4G4, R8G8, R5G6B5, R8G8B8A8.
  VALIDATED visually on TP HD textures (see report) - output PNGs look correct.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdpack  # noqa: E402

NUM_PIPES, NUM_BANKS = 2, 4
PIPE_INTERLEAVE_BITS = 8          # 256 bytes
PIPE_BITS, BANK_BITS = 1, 2

FMT_BPP = {0x01: 8, 0x02: 8, 0x07: 16, 0x08: 16, 0x0A: 16, 0x0B: 16, 0x1A: 32, 0x41A: 32,
           0x31: 64, 0x431: 64, 0x34: 64, 0x234: 64, 0x32: 128, 0x432: 128, 0x33: 128, 0x433: 128,
           0x35: 128, 0x235: 128}
BCN = {0x31, 0x431, 0x32, 0x432, 0x33, 0x433, 0x34, 0x234, 0x35, 0x235}


def thickness(tm):
    return 4 if tm in (3, 7, 11, 13, 15) else (8 if tm in (16, 17) else 1)


def pixel_index(x, y, bpp, tm, z=0):
    if bpp == 8:
        b = (x & 1, (x >> 1) & 1, (x >> 2) & 1, (y >> 1) & 1, y & 1, (y >> 2) & 1)
    elif bpp == 16:
        b = (x & 1, (x >> 1) & 1, (x >> 2) & 1, y & 1, (y >> 1) & 1, (y >> 2) & 1)
    elif bpp in (32, 96):
        b = (x & 1, (x >> 1) & 1, y & 1, (x >> 2) & 1, (y >> 1) & 1, (y >> 2) & 1)
    elif bpp == 64:
        b = (x & 1, y & 1, (x >> 1) & 1, (x >> 2) & 1, (y >> 1) & 1, (y >> 2) & 1)
    elif bpp == 128:
        b = (y & 1, x & 1, (x >> 1) & 1, (x >> 2) & 1, (y >> 1) & 1, (y >> 2) & 1)
    else:
        b = (x & 1, (x >> 1) & 1, y & 1, (x >> 2) & 1, (y >> 1) & 1, (y >> 2) & 1)
    v = b[0] | (b[1] << 1) | (b[2] << 2) | (b[3] << 3) | (b[4] << 4) | (b[5] << 5)
    t = thickness(tm)
    if t > 1:
        v |= (z & 1) << 6 | ((z >> 1) & 1) << 7
    if t == 8:
        v |= ((z >> 2) & 1) << 8
    return v


def pipe_wo_rot(x, y):
    return ((y >> 3) ^ (x >> 3)) & 1


def bank_wo_rot(x, y):
    b0 = ((y // (16 * NUM_PIPES)) ^ (x >> 3)) & 1
    return b0 | 2 * (((y // (8 * NUM_PIPES)) ^ (x >> 4)) & 1)


def addr_linear(x, y, bpp, pitch):
    return (y * pitch + x) * bpp // 8


def addr_micro(x, y, bpp, pitch, tm):
    mtt = thickness(tm)
    mt_bytes = (64 * mtt * bpp + 7) // 8
    mt_off = mt_bytes * ((x >> 3) + (y >> 3) * (pitch >> 3))
    return (bpp * pixel_index(x, y, bpp, tm)) // 8 + mt_off


def addr_macro(x, y, bpp, pitch, height, tm, pipe_swz, bank_swz):
    mtt = thickness(tm)
    elem = (bpp * pixel_index(x, y, bpp, tm)) >> 3
    pipe = pipe_wo_rot(x, y)
    bank = bank_wo_rot(x, y)
    bank_pipe = pipe + NUM_PIPES * bank
    swz = pipe_swz + NUM_PIPES * bank_swz
    bank_pipe ^= swz  # slice 0, sample 0
    bank_pipe %= NUM_PIPES * NUM_BANKS
    pipe = bank_pipe % NUM_PIPES
    bank = bank_pipe // NUM_PIPES
    mtp, mth = 8 * NUM_BANKS, 8 * NUM_PIPES
    if tm in (5, 9):
        mtp >>= 1; mth *= 2
    elif tm in (6, 10):
        mtp >>= 2; mth *= 4
    per_row = pitch // mtp
    mt_bytes = (mtt * bpp * mth * mtp + 7) // 8
    mt_off = ((x // mtp) + per_row * (y // mth)) * mt_bytes
    if tm in (8, 9, 10, 11, 14, 15):
        raise NotImplementedError("bank-swapped tile modes not implemented")
    group_mask = (1 << PIPE_INTERLEAVE_BITS) - 1
    nsw = BANK_BITS + PIPE_BITS
    total = elem + (mt_off >> nsw)
    hi = (total & ~group_mask) << nsw
    lo = total & group_mask
    return (bank << (PIPE_BITS + PIPE_INTERLEAVE_BITS)) | (pipe << PIPE_INTERLEAVE_BITS) | lo | hi


def deswizzle(s, data):
    fmt = s["fmt"] & 0xFFFF
    bpp = FMT_BPP[fmt]
    bc = fmt in BCN
    w = (s["w"] + 3) // 4 if bc else s["w"]
    h = (s["h"] + 3) // 4 if bc else s["h"]
    pitch = s["pitch"]
    tm = s["tileMode"]
    swz = (s["swizzle"] >> 8) & 7
    pipe_swz, bank_swz = swz & 1, (swz >> 1) & 3
    bpe = bpp // 8
    out = bytearray(w * h * bpe)
    for y in range(h):
        for x in range(w):
            if tm in (0, 1):
                a = addr_linear(x, y, bpp, pitch)
            elif tm in (2, 3):
                a = addr_micro(x, y, bpp, pitch, tm)
            else:
                a = addr_macro(x, y, bpp, pitch, s["h"], tm, pipe_swz, bank_swz)
            d = (y * w + x) * bpe
            out[d:d + bpe] = data[a:a + bpe]
    return bytes(out)


def _roundup(v, a):
    return (v + a - 1) // a * a


def level_info(s, L):
    """Geometry of mip level L of a GX2 surface: dict(w, h, pitch, tileMode, data_off, data_size)
    (data_off is absolute in the .gtx buffer). Rules (addrlib R600, Wii U config):
      - levels >= (swizzle >> 16) & 0xFF switch from 2D (macro) tiling to 1D_TILED_THIN1 (2)
      - macro pitch align = max(32, 32 * (256 // bpp // 8)); height align 16 (elements)
      - 1D pitch align = max(8, 256 // bpp); height align 8
      - level 1 is at the start of the mip block, level n>=2 at mipOffsets[n-1]."""
    fmt = s["fmt"] & 0xFFFF
    bpp = FMT_BPP[fmt]
    bc = fmt in BCN
    w, h = max(1, s["w"] >> L), max(1, s["h"] >> L)
    ew, eh = ((w + 3) // 4, (h + 3) // 4) if bc else (w, h)
    tm = s["tileMode"]
    if L == 0:
        return {"w": w, "h": h, "pitch": s["pitch"], "tileMode": tm, "data_off": s["img_off"], "data_size": s["img"]}
    switch = (s["swizzle"] >> 16) & 0xFF
    if tm >= 4 and L >= switch:
        tm = 2
    if tm >= 4:
        pitch = _roundup(ew, max(32, 32 * (256 // bpp // 8)))
        hh = _roundup(eh, 16)
    elif tm in (2, 3):
        pitch = _roundup(ew, max(8, 256 // bpp))
        hh = _roundup(eh, 8)
    else:
        pitch, hh = ew, eh
    off = s["mip_off"] + (0 if L == 1 else s["mipOffsets"][L - 1])
    return {"w": w, "h": h, "pitch": pitch, "tileMode": tm, "data_off": off, "data_size": pitch * hh * bpp // 8}


def deswizzle_level(s, gtx, L):
    """Return (w, h, linear bytes) for mip level L (0 = base)."""
    li = level_info(s, L)
    s2 = dict(s)
    s2.update({"w": li["w"], "h": li["h"], "pitch": li["pitch"], "tileMode": li["tileMode"]})
    data = gtx[li["data_off"]:li["data_off"] + li["data_size"]]
    if li["tileMode"] < 4:
        s2["swizzle"] = 0
    return li["w"], li["h"], deswizzle(s2, data)


def _rgb565(c):
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def decode_bc1(lin, w, h):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    out = bytearray(w * h * 4)
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * 8
            c0, c1, bits = struct.unpack_from("<HHI", lin, o)
            p0, p1 = _rgb565(c0), _rgb565(c1)
            if c0 > c1:
                pal = [p0 + (255,), p1 + (255,),
                       tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            for py in range(4):
                yy = by * 4 + py
                if yy >= h:
                    break
                for px in range(4):
                    xx = bx * 4 + px
                    if xx >= w:
                        continue
                    idx = (bits >> (2 * (py * 4 + px))) & 3
                    d = (yy * w + xx) * 4
                    out[d:d + 4] = bytes(pal[idx])
    return out


def to_rgba(s, lin):
    """Decode to RGBA8 (raw channels), then apply GX2 compSel."""
    fmt = s["fmt"] & 0xFFFF
    w, h = s["w"], s["h"]
    n = w * h
    if fmt in (0x31, 0x431):
        raw = decode_bc1(lin, w, h)
    elif fmt == 0x01:
        raw = bytearray(n * 4)
        for i in range(n):
            v = lin[i]; raw[i * 4:i * 4 + 4] = bytes((v, 0, 0, 255))
    elif fmt == 0x02:  # R4G4: R = low nibble? (GX2: R in bits 0-3, G in 4-7)
        raw = bytearray(n * 4)
        for i in range(n):
            v = lin[i]; r = v & 15; g = v >> 4
            raw[i * 4:i * 4 + 4] = bytes((r * 17, g * 17, 0, 255))
    elif fmt == 0x07:
        raw = bytearray(n * 4)
        for i in range(n):
            raw[i * 4:i * 4 + 4] = bytes((lin[2 * i], lin[2 * i + 1], 0, 255))
    elif fmt == 0x08:
        raw = bytearray(n * 4)
        for i in range(n):
            c = struct.unpack_from("<H", lin, 2 * i)[0]
            raw[i * 4:i * 4 + 4] = bytes(_rgb565(c) + (255,))
    elif fmt in (0x1A, 0x41A):
        raw = bytearray(lin)
    else:
        raise NotImplementedError("decode of GX2 format 0x%x" % fmt)
    comp = bytes.fromhex(s.get("compSel") or "00010203")
    if comp == b"\x00\x01\x02\x03":
        return bytes(raw)
    out = bytearray(n * 4)
    for i in range(n):
        px = raw[i * 4:i * 4 + 4]
        for c in range(4):
            sel = comp[c]
            out[i * 4 + c] = px[sel] if sel < 4 else (0 if sel == 4 else 255)
    return bytes(out)


def write_png(path, w, h, rgba):
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def write_dds(path, s, lin):
    """Base level only. BC1/BC3 -> DXT1/DXT5 FourCC; others -> uncompressed RGBA8 (compSel applied)."""
    fmt = s["fmt"] & 0xFFFF
    w, h = s["w"], s["h"]
    if fmt in (0x31, 0x431, 0x33, 0x433):
        fourcc = b"DXT1" if fmt in (0x31, 0x431) else b"DXT5"
        pf = struct.pack("<II4sIIIII", 32, 0x4, fourcc, 0, 0, 0, 0, 0)
        payload = lin
        flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000
        pitch = len(lin)
    else:
        payload = to_rgba(s, lin)
        pf = struct.pack("<II4sIIIII", 32, 0x41, b"\0\0\0\0", 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
        flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8
        pitch = w * 4
    hdr = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, h, w, pitch, 0, 1) + b"\0" * 44 + pf + \
        struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + payload)


def export(gtx_bytes, stem, out_dir, png=True, dds=True, only=None):
    g = hdpack.gtx_surfaces(gtx_bytes)
    os.makedirs(out_dir, exist_ok=True)
    done = []
    for i, s in enumerate(g["surfaces"]):
        if only is not None and i != only:
            continue
        if s["img_off"] is None:
            continue
        data = gtx_bytes[s["img_off"]:s["img_off"] + s["img"]]
        lin = deswizzle(s, data)
        base = os.path.join(out_dir, "%s_%02d" % (stem, i))
        if dds:
            write_dds(base + ".dds", s, lin)
        if png:
            write_png(base + ".png", s["w"], s["h"], to_rgba(s, lin))
        done.append((i, s["w"], s["h"], s["fmt_name"]))
    return done


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    png = "--png" in argv or "--dds" not in argv
    dds = "--dds" in argv or "--png" not in argv
    only = int(argv[argv.index("--surface") + 1]) if "--surface" in argv else None
    if argv[1] == "--pack":
        t = hdpack.Tmpk(hdpack.read_any(argv[2]))
        flt = argv[argv.index("--filter") + 1] if "--filter" in argv else None
        for e in t.entries:
            if not e["name"].endswith(".gtx") or (flt and flt not in e["name"]):
                continue
            stem = e["name"].replace("/", "_")[:-4]
            for r in export(t.data(e), stem, argv[3], png, dds, only):
                print("%s #%d %dx%d %s" % (e["name"], r[0], r[1], r[2], r[3]))
        return 0
    data = hdpack.load_member(argv[1])
    stem = os.path.basename(argv[1].split(":")[-1])[:-4]
    for r in export(data, stem, argv[2], png, dds, only):
        print("%s #%d %dx%d %s" % (stem, r[0], r[1], r[2], r[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
