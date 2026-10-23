#!/usr/bin/env python3
"""hdpack.py - Wii U Twilight Princess HD '.pack(.gz)' containers (TMPK) + GMX2 / GTX(Gfx2) probes.

In TP HD every R??_00.arc / STG_00.arc / res/Object/<name>.arc that has HD assets is paired
with <same>.pack.gz = gzip( TMPK ). The TMPK holds, per J3D model in the .arc:
   geo/<arc path>/<member>.bmd.gmx   GMX2 = HD GPU mesh (VERT float vertex buffers, INDX, VMAP, MESH)
   tex/<arc path>/<member>.bmd.gtx   Gfx2 = GX2 texture file, one surface per TEX1 entry
plus standalone textures for .bti members (tex/.../foo.bti.gtx).

CLI USAGE
  python -I hdpack.py list    <file.pack.gz|file.pack>            TMPK table of contents
  python -I hdpack.py extract <file.pack.gz|file.pack> <out_dir>  write every member (tree kept)
  python -I hdpack.py gtx     <file.gtx | pack:member>            list GX2 surfaces (dim, fmt, mips, tile)
  python -I hdpack.py gmx     <file.gmx | pack:member>            list GMX2 sections (MESH/VERT/INDX/VMAP)
  python -I hdpack.py summary <file.pack.gz>                      one line per member + gtx/gmx digest

TMPK FORMAT (VERIFIED on D_MN05/D_SB11 packs)
  0x00 'TMPK'  u32 num_files  u32 alignment(0x1000)  u32 0
  0x10 num_files x { u32 name_off, u32 data_off, u32 size, u32 0 }   (offsets from file start)
  names: NUL-terminated ASCII paths; data blocks aligned to 'alignment' (first may be 0xC0)
GTX: 'Gfx2' hdr(0x20, v7.1, gpu 2) then 'BLK{' blocks (0x20 hdr: magic, hdrsize, 1, 0, type,
  data_size, id, idx). v7 types: 0x0B surface info (GX2Surface, 0x9C), 0x0C image, 0x0D mips, 0x01 end.
GMX2: 'GMX2' u32 8, then sections {tag[4], u32 size incl. header}: PADX, MESH, VERT, INDX, VMAP, ENDX.
"""
import gzip
import os
import struct
import sys

GX2_FMT = {0x01: "R8_UNORM", 0x07: "R8G8_UNORM", 0x08: "R5G6B5", 0x0A: "R5G5B5A1", 0x0B: "R4G4B4A4",
           0x1A: "R8G8B8A8_UNORM", 0x41A: "R8G8B8A8_SRGB", 0x31: "BC1_UNORM", 0x431: "BC1_SRGB",
           0x32: "BC2_UNORM", 0x432: "BC2_SRGB", 0x33: "BC3_UNORM", 0x433: "BC3_SRGB",
           0x34: "BC4_UNORM", 0x234: "BC4_SNORM", 0x35: "BC5_UNORM", 0x235: "BC5_SNORM",
           0x02: "R4G4_UNORM", 0x05: "R16_UNORM", 0x19: "R10G10B10A2", 0x80E: "R32_FLOAT"}


def read_any(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] == b"\x1f\x8b":
        d = gzip.decompress(d)
    return d


class Tmpk(object):
    def __init__(self, buf):
        if buf[:4] != b"TMPK":
            raise ValueError("not TMPK (magic %r)" % buf[:4])
        self.buf = buf
        self.n, self.align, self.unk = struct.unpack_from(">III", buf, 4)
        self.entries = []
        for i in range(self.n):
            no, do, sz, z = struct.unpack_from(">IIII", buf, 0x10 + i * 16)
            e = buf.index(b"\0", no)
            self.entries.append({"name": buf[no:e].decode("ascii", "replace"), "off": do, "size": sz})

    def data(self, ent):
        return self.buf[ent["off"]:ent["off"] + ent["size"]]

    def get(self, name):
        for e in self.entries:
            if e["name"] == name or e["name"].endswith("/" + name) or os.path.basename(e["name"]) == name:
                return self.data(e)
        return None


def gtx_surfaces(buf):
    if buf[:4] != b"Gfx2":
        raise ValueError("not Gfx2")
    hsz, major, minor, gpu, align = struct.unpack_from(">IIIII", buf, 4)
    o = hsz
    surfs = []
    blocks = []
    t_surf = 0x0B if major >= 7 else 0x0A
    t_img = t_surf + 1
    t_mip = t_surf + 2
    cur = None
    while o + 0x20 <= len(buf):
        if buf[o:o + 4] != b"BLK{":
            break
        bh, bmaj, bmin, btype, bsize, bid, bidx = struct.unpack_from(">IIIIIII", buf, o + 4)
        d0 = o + bh
        blocks.append((btype, bsize))
        if btype == t_surf:
            (dim, w, h, depth, mips, fmt, aa, use, isz, iptr, msz, mptr, tile, swz, aln, pitch
             ) = struct.unpack_from(">16I", buf, d0)
            comp = buf[d0 + 0x84:d0 + 0x88] if bsize >= 0x88 else b""  # GX2Texture.compSel (R,G,B,A sources: 0-3=RGBA,4=0,5=1)
            mipoffs = struct.unpack_from(">13I", buf, d0 + 0x40)
            cur = {"dim": dim, "w": w, "h": h, "depth": depth, "mips": mips, "fmt": fmt,
                   "fmt_name": GX2_FMT.get(fmt, "0x%x" % fmt), "imageSize": isz, "mipSize": msz,
                   "tileMode": tile, "swizzle": swz, "pitch": pitch, "compSel": comp.hex(), "img": 0, "mip": 0,
                   "alignment": aln, "mipOffsets": list(mipoffs), "use": use, "aa": aa, "img_off": None, "mip_off": None}
            surfs.append(cur)
        elif btype == t_img and cur is not None:
            cur["img"] = bsize
            cur["img_off"] = d0
        elif btype == t_mip and cur is not None:
            cur["mip"] = bsize
            cur["mip_off"] = d0
        elif btype == 1:
            break
        o = d0 + bsize
    return {"version": "%d.%d" % (major, minor), "gpu": gpu, "surfaces": surfs, "blocks": len(blocks)}


def gmx_sections(buf):
    if buf[:4] != b"GMX2":
        raise ValueError("not GMX2")
    o = 8
    secs = []
    while o + 8 <= len(buf):
        tag = buf[o:o + 4].decode("ascii", "replace")
        sz = struct.unpack_from(">I", buf, o + 4)[0]
        info = ""
        if tag == "MESH" and sz >= 0x28:
            vals = struct.unpack_from(">8I", buf, o + 8)
            info = " ".join("%x" % v for v in vals)
        secs.append((tag, o, sz, info))
        if tag == "ENDX" or sz == 0:
            break
        o += sz
    return secs


def load_member(spec):
    if ":" in spec[2:]:
        i = spec.rindex(":")
        p, m = spec[:i], spec[i + 1:]
        if os.path.exists(p):
            d = Tmpk(read_any(p)).get(m)
            if d is None:
                raise SystemExit("member %s not found" % m)
            return d
    return read_any(spec)


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd in ("list", "summary", "extract"):
        t = Tmpk(read_any(argv[2]))
        print("# %s: TMPK files=%d align=0x%x" % (argv[2], t.n, t.align))
        for e in t.entries:
            line = "%10d  @0x%08x  %s" % (e["size"], e["off"], e["name"])
            if cmd == "summary":
                d = t.data(e)
                try:
                    if d[:4] == b"Gfx2":
                        g = gtx_surfaces(d)
                        fm = {}
                        for s in g["surfaces"]:
                            k = "%s" % s["fmt_name"]
                            fm[k] = fm.get(k, 0) + 1
                        dims = ",".join("%dx%d" % (s["w"], s["h"]) for s in g["surfaces"][:6])
                        line += "   [Gfx2 v%s surfaces=%d fmts=%s dims=%s%s]" % (
                            g["version"], len(g["surfaces"]), fm, dims, "..." if len(g["surfaces"]) > 6 else "")
                    elif d[:4] == b"GMX2":
                        s = gmx_sections(d)
                        cnt = {}
                        for tag, _, _, _ in s:
                            cnt[tag] = cnt.get(tag, 0) + 1
                        cnt.pop("PADX", None)
                        line += "   [GMX2 %s]" % cnt
                    else:
                        line += "   [magic %r]" % d[:8]
                except Exception as ex:
                    line += "   [parse error %s]" % ex
            print(line)
        if cmd == "extract":
            out = argv[3]
            for e in t.entries:
                parts = [p for p in e["name"].replace("\\", "/").split("/") if p not in ("", ".", "..")]
                dest = os.path.join(out, *parts)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as f:
                    f.write(t.data(e))
            print("extracted %d -> %s" % (len(t.entries), out))
    elif cmd == "gtx":
        g = gtx_surfaces(load_member(argv[2]))
        print("Gfx2 v%s gpu=%d blocks=%d surfaces=%d" % (g["version"], g["gpu"], g["blocks"], len(g["surfaces"])))
        for i, s in enumerate(g["surfaces"]):
            print("  [%2d] dim=%d %4dx%-4d mips=%-2d fmt=%-14s tile=%d swz=0x%x pitch=%d img=%d mip=%d comp=%s" % (
                i, s["dim"], s["w"], s["h"], s["mips"], s["fmt_name"], s["tileMode"], s["swizzle"], s["pitch"],
                s["img"], s["mip"], s["compSel"]))
    elif cmd == "gmx":
        for tag, o, sz, info in gmx_sections(load_member(argv[2])):
            if tag != "PADX":
                print("  %-4s @0x%06x size=%8d %s" % (tag, o, sz, info))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
