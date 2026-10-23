#!/usr/bin/env python3
"""j3d.py - inspect J3D model/anim files (bmd/bdl/btk/brk/bck/btp/bva/bpk/bla/blk) and BTI textures.

CLI USAGE
  python -I j3d.py info <file.bmd|file.bdl|file.bti|...> [--tex]
        Header + section table (tag, size). For models: INF1/VTX1/SHP1/MAT3/TEX1 summary
        (vertex attribute formats, #shapes, #materials, #textures). --tex lists every TEX1
        texture: name, GX format, w x h, mips, palette, image bytes present.
  python -I j3d.py cmp <a.bmd> <b.bmd>
        Side-by-side section sizes + texture table diff (use for GC vs HD of the same file).
  Inputs may be raw files or 'archive.arc:member' (member path/basename inside a RARC).

GX texture formats: 0 I4 1 I8 2 IA4 3 IA8 4 RGB565 5 RGB5A3 6 RGBA8 8 C4 9 C8 10 C14X2 14 CMPR
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

GX_FMT = {0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565", 5: "RGB5A3", 6: "RGBA8",
          8: "C4", 9: "C8", 10: "C14X2", 14: "CMPR"}
BPP = {0: 4, 1: 8, 2: 8, 3: 16, 4: 16, 5: 16, 6: 32, 8: 4, 9: 8, 10: 16, 14: 4}
BLOCK = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4), 6: (4, 4),
         8: (8, 8), 9: (8, 4), 10: (4, 4), 14: (8, 8)}
VTX_ATTR = {0: "PNMTXIDX", 9: "POS", 10: "NRM", 11: "CLR0", 12: "CLR1", 13: "TEX0", 14: "TEX1",
            15: "TEX2", 16: "TEX3", 17: "TEX4", 18: "TEX5", 19: "TEX6", 20: "TEX7", 25: "NBT"}
COMP_T = {0: "u8", 1: "s8", 2: "u16", 3: "s16", 4: "f32"}
CLR_T = {0: "RGB565", 1: "RGB8", 2: "RGBX8", 3: "RGBA4", 4: "RGBA6", 5: "RGBA8"}


def load(spec):
    if ":" in spec[2:]:
        idx = spec.rindex(":")
        arcp, member = spec[:idx], spec[idx + 1:]
        if os.path.exists(arcp):
            import rarc
            m = rarc.Rarc.from_file(arcp).get(member)
            if m is None:
                raise SystemExit("member %s not in %s" % (member, arcp))
            return m.data()
    with open(spec, "rb") as f:
        import yaz0
        return yaz0.maybe_decompress(f.read())


def tex_size(fmt, w, h, mips=1):
    bw, bh = BLOCK.get(fmt, (4, 4))
    total = 0
    for i in range(max(mips, 1)):
        ww, hh = max(w >> i, 1), max(h >> i, 1)
        total += ((ww + bw - 1) // bw) * ((hh + bh - 1) // bh) * (bw * bh * BPP.get(fmt, 16) // 8)
    return total


def bti_header(buf, o):
    (fmt, alpha, w, h, ws, wt, pal_en, pal_fmt, pal_n, pal_off, mip_en, edge, bias, aniso,
     minf, magf, minlod, maxlod, nimg, unk, lodbias, img_off) = struct.unpack_from(">BBHHBBBBHIBBBBBBbbBBhI", buf, o)
    return {"fmt": fmt, "fmt_name": GX_FMT.get(fmt, "?%d" % fmt), "alpha": alpha, "w": w, "h": h,
            "wrap": (ws, wt), "pal_fmt": pal_fmt, "pal_n": pal_n, "pal_off": pal_off, "mips": nimg,
            "minf": minf, "magf": magf, "img_off": img_off}


def sections(buf):
    magic = buf[:8].decode("ascii", "replace")
    size, nsec = struct.unpack_from(">II", buf, 8)
    out = []
    o = 0x20
    for _ in range(nsec):
        if o + 8 > len(buf):
            break
        tag = buf[o:o + 4].decode("ascii", "replace")
        ssz = struct.unpack_from(">I", buf, o + 4)[0]
        out.append((tag, o, ssz))
        if ssz == 0:
            break
        o += ssz
    return magic, size, out


def read_strtab(buf, o):
    n = struct.unpack_from(">H", buf, o)[0]
    names = []
    for i in range(n):
        _, off = struct.unpack_from(">HH", buf, o + 4 + i * 4)
        e = buf.index(b"\0", o + off)
        names.append(buf[o + off:e].decode("shift_jis", "replace"))
    return names


def tex1(buf, so, ssz):
    n = struct.unpack_from(">H", buf, so + 8)[0]
    hdr_off, str_off = struct.unpack_from(">II", buf, so + 0xC)
    names = read_strtab(buf, so + str_off) if str_off else []
    texs = []
    for i in range(n):
        ho = so + hdr_off + i * 0x20
        t = bti_header(buf, ho)
        t["name"] = names[i] if i < len(names) else "#%d" % i
        img_abs = ho + t["img_off"]
        exp = tex_size(t["fmt"], t["w"], t["h"], t["mips"])
        t["expected_bytes"] = exp
        t["img_abs"] = img_abs
        t["present"] = (img_abs + exp <= so + ssz) and t["img_off"] != 0
        texs.append(t)
    # actual payload: from first image offset to section end
    return texs


def vtx1(buf, so):
    fmt_off = struct.unpack_from(">I", buf, so + 8)[0]
    o = so + fmt_off
    out = []
    while True:
        attr, ctype, comp, frac = struct.unpack_from(">IIIB", buf, o)
        if attr == 0xFF:
            break
        nm = VTX_ATTR.get(attr, str(attr))
        ty = CLR_T.get(comp) if attr in (11, 12) else COMP_T.get(comp, str(comp))
        out.append("%s:%s%s" % (nm, ty, "" if attr in (11, 12) else "/q%d" % frac))
        o += 0x10
    return out


def info(spec, show_tex=False, quiet=False):
    buf = load(spec)
    lines = []
    if buf[:4] not in (b"J3D1", b"J3D2"):
        if len(buf) >= 0x20:
            t = bti_header(buf, 0)
            exp = tex_size(t["fmt"], t["w"], t["h"], t["mips"])
            lines.append("BTI %s %dx%d mips=%d file=%d bytes, expected image bytes=%d img_off=0x%x -> %s" % (
                t["fmt_name"], t["w"], t["h"], t["mips"], len(buf), exp, t["img_off"],
                "image data present" if len(buf) >= t["img_off"] + exp and t["img_off"] else "HEADER ONLY (no pixels)"))
        if not quiet:
            print("\n".join(lines))
        return {"kind": "bti", "lines": lines}
    magic, size, secs = sections(buf)
    res = {"kind": "j3d", "magic": magic, "size": size, "sections": secs, "tex": [], "vtx": []}
    lines.append("%s size=%d (buf %d) sections=%d" % (magic, size, len(buf), len(secs)))
    for tag, o, ssz in secs:
        extra = ""
        if tag == "TEX1":
            texs = tex1(buf, o, ssz)
            res["tex"] = texs
            pres = sum(1 for t in texs if t["present"])
            extra = "textures=%d with_pixels=%d" % (len(texs), pres)
        elif tag == "VTX1":
            res["vtx"] = vtx1(buf, o)
            extra = " ".join(res["vtx"])
        elif tag in ("MAT3", "MAT2", "MAT4"):
            extra = "materials=%d%s" % (struct.unpack_from(">H", buf, o + 8)[0], "  (HD-only MAT4: GC J3DModelLoader rejects it -> hd2gc.py renames to MAT3)" if tag == "MAT4" else "")
        elif tag == "SHP1":
            extra = "shapes=%d" % struct.unpack_from(">H", buf, o + 8)[0]
        elif tag == "JNT1":
            extra = "joints=%d" % struct.unpack_from(">H", buf, o + 8)[0]
        elif tag == "MDL3":
            extra = "display-list packets=%d" % struct.unpack_from(">H", buf, o + 8)[0]
        lines.append("  %-4s off=0x%06x size=%8d %s" % (tag, o, ssz, extra))
    if show_tex:
        for t in res["tex"]:
            lines.append("    tex %-24s %-6s %4dx%-4d mips=%d pal=%d img_off=0x%x need=%d %s" % (
                t["name"], t["fmt_name"], t["w"], t["h"], t["mips"], t["pal_n"], t["img_off"],
                t["expected_bytes"], "OK" if t["present"] else "NO-PIXELS"))
    if not quiet:
        print("\n".join(lines))
    res["lines"] = lines
    return res


def cmp(a, b):
    ra, rb = info(a, quiet=True), info(b, quiet=True)
    print("A: " + ra["lines"][0])
    print("B: " + rb["lines"][0])
    if ra["kind"] != "j3d" or rb["kind"] != "j3d":
        print("A: " + "\n   ".join(ra["lines"]))
        print("B: " + "\n   ".join(rb["lines"]))
        return
    sa = {t: s for t, _, s in ra["sections"]}
    sb = {t: s for t, _, s in rb["sections"]}
    for t in sorted(set(sa) | set(sb), key=lambda x: [s[0] for s in ra["sections"]].index(x) if x in sa else 99):
        print("  %-4s A=%8s B=%8s %s" % (t, sa.get(t, "-"), sb.get(t, "-"), "" if sa.get(t) == sb.get(t) else "DIFF"))
    print("  VTX A: %s" % " ".join(ra["vtx"]))
    print("  VTX B: %s" % " ".join(rb["vtx"]))
    ta = {t["name"]: t for t in ra["tex"]}
    tb = {t["name"]: t for t in rb["tex"]}
    for n in list(ta) + [x for x in tb if x not in ta]:
        x, y = ta.get(n), tb.get(n)
        fx = "%s %dx%d m%d %s" % (x["fmt_name"], x["w"], x["h"], x["mips"], "px" if x["present"] else "nopx") if x else "-"
        fy = "%s %dx%d m%d %s" % (y["fmt_name"], y["w"], y["h"], y["mips"], "px" if y["present"] else "nopx") if y else "-"
        print("    %-24s A[%s]  B[%s]" % (n, fx, fy))


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    if argv[1] == "info":
        info(argv[2], "--tex" in argv)
    elif argv[1] == "cmp":
        cmp(argv[2], argv[3])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
