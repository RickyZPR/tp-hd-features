#!/usr/bin/env python3
"""hd2gc.py - PROTOTYPE converter: TP HD (Wii U) .arc + .pack.gz  ->  GC-engine-style .arc.

What it does per archive member
  *.bmd / *.bdl : 'MAT4' section renamed to 'MAT3' (HD MAT4 = MAT3 header + 1 extra trailing
                  offset table; the 30 MAT3 offsets and 0x14C-byte material entries are unchanged).
                  TEX1 rebuilt WITH pixel data taken from the paired GTX surfaces in the .pack.gz
                  (TEX1 entry i <-> GTX surface i, verified 1:1):
                    - GX2 BC1 -> GX CMPR: lossless block transcode of the GTX mip level whose size
                      equals the chosen target size (no re-encoding).
                    - R8/R8G8/R4G4/RGBA8/RGB565 -> decoded (compSel applied) -> re-encoded to the
                      TEX1 GX format (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8; C4/C8 -> RGB5A3).
                    - optional --donor-index gc_tex_index.tsv: use the GC disc's own pixels when a
                      GC texture with identical name+format+size exists.
                  Target size: the TEX1 header's own (GC-scale) w/h by default; --scale N keeps N x
                  that (capped at 1024 and at the HD size). Mip count kept when GTX has the levels.
  *.bti         : HD header-only BTI (0x20 bytes) -> full BTI with pixels from <name>.bti.gtx.
  everything else (dzr/dzs/kcl/plc/dzb/btk/brk/bck/stb/dat/bin...) copied unchanged.
  Output archive written with rarc_write (retail GC layout, uncompressed - no Yaz0).

CLI USAGE
  python -I hd2gc.py <hd.arc> <out.arc> [--pack hd.pack.gz] [--scale N] [--donor-index tsv]
                     [--gc-res <GC files/res dir>] [--report out.txt]
      --pack defaults to <hd.arc minus .arc>.pack.gz next to the input.
      --donor-index (from tex_index.py build on the GC root) + --gc-res: copy GC pixels for
        textures whose name+format+size match exactly (only when --scale 1).
  Exit code 0 on success; the report lists every texture and its source.

WARNING (critic, 2026-10-18): HD MAT4 material init entries are 0x14E bytes, not 0x14C. This tool only
renames the tag, so its output still needs tools/critic/mat4fix.py (0x14E -> 0x14C repack) before the GC
engine can read any model with more than one material. tools/build_assets.py runs both.

STATUS: structural output verified (j3d.py / j3d_dlcheck.py parse, TEX1 pixels present,
CMPR preview decodes correctly). NOT verified in-game: MAT4->MAT3 material semantics and
HD-specific shading are untested in Dusklight. MDL3 (bdl) display lists are left untouched
(they may hard-code the old texture format/size -> prefer 1:1 size/format for bdl).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rarc, rarc_write, hdpack, gtx2img, gxtex, j3d  # noqa: E402

GX_FROM_NAME = {"I4": 0, "I8": 1, "IA4": 2, "IA8": 3, "RGB565": 4, "RGB5A3": 5, "RGBA8": 6, "C4": 8, "C8": 9,
                "C14X2": 10, "CMPR": 14}


def load_donors(path):
    don = {}
    if not path:
        return don
    for l in open(path, encoding="utf-8"):
        f = l.rstrip("\n").split("\t")
        if len(f) >= 8 and f[5] == "1":
            don.setdefault((f[0].lower(), f[1], int(f[2]), int(f[3])), f[7])
    return don


_donor_cache = {}


def donor_pixels(src, root):
    """src like 'Stage/D_SB01/R00_00.arc:r00_00/bmdr/model.bmd#3' (relative to GC res root)."""
    arcp, rest = src.split(":", 1)
    member, idx = (rest.split("#") + [None])[:2]
    key = (arcp, member)
    if key not in _donor_cache:
        a = rarc.Rarc.from_file(os.path.join(root, arcp))
        _donor_cache[key] = a.get(member).data()
    d = _donor_cache[key]
    if idx is None:
        t = j3d.bti_header(d, 0)
        return t, d[t["img_off"]:t["img_off"] + j3d.tex_size(t["fmt"], t["w"], t["h"], t["mips"])]
    magic, size, secs = j3d.sections(d)
    for tag, o, s in secs:
        if tag == "TEX1":
            t = j3d.tex1(d, o, s)[int(idx)]
            return t, d[t["img_abs"]:t["img_abs"] + t["expected_bytes"]]
    return None, None


def pick_level(s, tw, th):
    """Smallest GTX level whose size is >= target (prefer exact). Returns level index."""
    best = 0
    for L in range(max(1, s["mips"])):
        w, h = max(1, s["w"] >> L), max(1, s["h"] >> L)
        if w >= tw and h >= th:
            best = L
    return best


def convert_texture(hdr, gtx_buf, surf, scale, report, label):
    """hdr: dict from j3d.bti_header (GC-scale header). Returns (new 0x20 header bytes w/o img_off fixup, pixels)."""
    fmt = hdr["fmt"]
    tw, th = hdr["w"] * scale, hdr["h"] * scale
    tw, th = min(tw, 1024, surf["w"]), min(th, 1024, surf["h"])
    L0 = pick_level(surf, tw, th)
    w0, h0 = max(1, surf["w"] >> L0), max(1, surf["h"] >> L0)
    want_mips = max(1, hdr["mips"])
    out_fmt = fmt if fmt in (0, 1, 2, 3, 4, 5, 6, 14) else 5  # palette formats -> RGB5A3
    gx2 = surf["fmt"] & 0xFFFF
    levels = []
    for k in range(want_mips):
        L = L0 + k
        if L >= max(1, surf["mips"]):
            break
        w, h, lin = gtx2img.deswizzle_level(surf, gtx_buf, L)
        if out_fmt == 14 and gx2 in (0x31, 0x431):
            levels.append(gxtex.bc1_to_cmpr(lin, w, h))
        else:
            s2 = dict(surf); s2["w"], s2["h"] = w, h
            rgba = gtx2img.to_rgba(s2, lin)
            if out_fmt == 14:      # non-BC1 source for a CMPR slot: store as RGB5A3 instead
                out_fmt = 5
            levels.append(gxtex.encode(out_fmt, rgba, w, h))
        if w <= 1 and h <= 1:
            break
    pix = b"".join(levels)
    report.append("%s: %s %dx%d mips=%d <- GTX %s %dx%d level %d (%d levels)" % (
        label, j3d.GX_FMT.get(out_fmt, out_fmt), w0, h0, len(levels), surf["fmt_name"], surf["w"], surf["h"], L0, len(levels)))
    return out_fmt, w0, h0, len(levels), pix


def patch_header(raw_hdr, fmt, w, h, nmips, img_off):
    b = bytearray(raw_hdr)
    b[0] = fmt
    struct.pack_into(">HH", b, 2, w, h)
    if fmt not in (8, 9, 10):
        b[8] = 0; b[9] = 0; struct.pack_into(">HI", b, 0xA, 0, 0)  # no palette
    b[0x18] = nmips
    if nmips <= 1:
        b[0x10] = 0          # mipmap disabled
        if b[0x14] > 1:      # min filter used mips -> GX_LINEAR
            b[0x14] = 1
        b[0x17] = 0          # max LOD
    else:
        b[0x10] = 1
        b[0x17] = (nmips - 1) * 8   # maxLOD in 1/8 units
    struct.pack_into(">I", b, 0x1C, img_off)
    return bytes(b)


def convert_j3d(d, gtx_buf, scale, donors, gc_root, report, label):
    magic, size, secs = j3d.sections(d)
    out = bytearray(d[:0x20])
    g = hdpack.gtx_surfaces(gtx_buf) if gtx_buf else {"surfaces": []}
    for tag, o, s in secs:
        sec = bytearray(d[o:o + s])
        if tag == "MAT4":
            sec[0:4] = b"MAT3"
            report.append("%s: MAT4 -> MAT3 (header offsets kept; 31st offset ignored by loader)" % label)
        elif tag == "MDL3":
            report.append("%s: WARNING MDL3 present (precompiled material DLs untouched)" % label)
        elif tag == "TEX1":
            n = struct.unpack_from(">H", d, o + 8)[0]
            hdr_off, str_off = struct.unpack_from(">II", d, o + 0xC)
            names = j3d.read_strtab(d, o + str_off) if str_off else ["#%d" % i for i in range(n)]
            strtab = d[o + str_off:o + s] if str_off else b""
            # string table length: up to section end (keep as is)
            hdrs = []
            pixels = []
            for i in range(n):
                ho = o + hdr_off + i * 0x20
                h = j3d.bti_header(d, ho)
                lab = "%s TEX1[%d] %s" % (label, i, names[i])
                key = (names[i].lower(), h["fmt_name"], h["w"], h["h"])
                if donors and key in donors and scale == 1:
                    t, px = donor_pixels(donors[key], gc_root)
                    if px:
                        hdrs.append((d[ho:ho + 0x20], h["fmt"], h["w"], h["h"], max(1, t["mips"]), px))
                        report.append("%s: GC donor %s" % (lab, donors[key]))
                        continue
                if i < len(g["surfaces"]):
                    f2, w2, h2, nm, px = convert_texture(h, gtx_buf, g["surfaces"][i], scale, report, lab)
                    hdrs.append((d[ho:ho + 0x20], f2, w2, h2, nm, px))
                else:
                    report.append("%s: NO SOURCE (left without pixels)" % lab)
                    hdrs.append((d[ho:ho + 0x20], h["fmt"], h["w"], h["h"], 1, b""))
            # layout: header 0x20, entries, pixel blobs (32-aligned), string table
            body = bytearray(0x20 + n * 0x20)
            blob_offs = []
            for (_, _, _, _, _, px) in hdrs:
                while len(body) % 32:
                    body.append(0)
                blob_offs.append(len(body))
                body.extend(px)
            while len(body) % 4:
                body.append(0)
            new_str_off = len(body)
            body.extend(strtab)
            while len(body) % 32:
                body.append(0)
            body[0:4] = b"TEX1"
            struct.pack_into(">IHHII", body, 4, len(body), n, 0xFFFF, 0x20, new_str_off)
            for i, (raw, f2, w2, h2, nm, px) in enumerate(hdrs):
                eo = 0x20 + i * 0x20
                rel = (blob_offs[i] - eo) if px else 0
                body[eo:eo + 0x20] = patch_header(raw, f2, w2, h2, nm, rel)
            sec = body
        out.extend(sec)
    struct.pack_into(">I", out, 8, len(out))
    return bytes(out)


def convert_bti(d, gtx_buf, scale, report, label):
    h = j3d.bti_header(d, 0)
    exp = j3d.tex_size(h["fmt"], h["w"], h["h"], h["mips"])
    if h["img_off"] and len(d) >= h["img_off"] + exp:
        return d  # already has pixels
    if not gtx_buf:
        report.append("%s: header-only BTI and no GTX -> left as is" % label)
        return d
    surf = hdpack.gtx_surfaces(gtx_buf)["surfaces"][0]
    # HD BTI headers already carry HD dims; scale relative to GC = user choice -> use header/scale
    hdr = dict(h)
    if scale == 1:
        hdr["w"], hdr["h"] = h["w"], h["h"]
    f2, w2, h2, nm, px = convert_texture(hdr, gtx_buf, surf, 1, report, label)
    return patch_header(d[:0x20], f2, w2, h2, nm, 0x20) + px


def convert_archive(arc_path, out_path, pack_path=None, scale=1, donor_index=None, gc_root=None, report=None):
    report = report if report is not None else []
    a = rarc.Rarc.from_file(arc_path)
    pack_path = pack_path or (arc_path[:-4] + ".pack.gz")
    tm = hdpack.Tmpk(hdpack.read_any(pack_path)) if os.path.exists(pack_path) else None
    donors = load_donors(donor_index)
    repl = {}
    for m in a.files():
        ln = m.name.lower()
        if not ln.endswith((".bmd", ".bdl", ".bti")):
            continue
        gtx = None
        if tm:
            for e in tm.entries:
                if e["name"].lower().endswith("/" + m.path.lower() + ".gtx"):
                    gtx = tm.data(e)
                    break
        d = m.data()
        lab = "%s:%s" % (os.path.basename(arc_path), m.path)
        if ln.endswith(".bti"):
            repl[m.path] = convert_bti(d, gtx, scale, report, lab)
        else:
            repl[m.path] = convert_j3d(d, gtx, scale, donors, gc_root, report, lab)
    data = rarc_write.build_from_rarc(a, repl)
    with open(out_path, "wb") as f:
        f.write(data)
    report.append("wrote %s (%d bytes, %d converted members)" % (out_path, len(data), len(repl)))
    return report


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    opt = lambda k, dflt=None: argv[argv.index(k) + 1] if k in argv else dflt
    rep = convert_archive(argv[1], argv[2], opt("--pack"), int(opt("--scale", "1")), opt("--donor-index"),
                          opt("--gc-res"), [])
    if opt("--report"):
        open(opt("--report"), "w").write("\n".join(rep) + "\n")
    print("\n".join(rep[-40:]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
