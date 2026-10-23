#!/usr/bin/env python3
"""j3d_dlcheck.py - sanity-check a J3D model's SHP1 GX display lists against its VTX1 arrays.

USAGE: python -I j3d_dlcheck.py <file.bmd | archive.arc:member.bmd>
Walks every SHP1 packet display list (GX draw opcodes 0x80..0xB8), using each shape's
vertex descriptor (index8/index16/direct), counts primitives/vertices and the max index used
per attribute, and compares with the number of entries in the matching VTX1 array.
Prints OK when every index is in range (= the GX geometry is self-contained & loadable).

LIBRARY (added by the assets pipeline, 2026-10-18; CLI output unchanged):
  j3d_dlcheck.check(model_bytes) -> {'ok', 'shapes', 'prims', 'verts', 'bad_opcodes', 'ranges': {attr: (max, count)},
                                     'out_of_range': [attr names]}
  j3d_dlcheck.pc_dl_bytes(model_bytes) -> {'shapes', 'packets', 'raw', 'optimized', 'copy'}   (fix-cave, 2026-10-18)
        Bytes the PC build copies per model: on TARGET_PC every J3DShapeDraw copies its packet display list
        into the current heap (J3DShapeDraw.cpp set_display_list_copy, JKR_NEW_ARRAY aligned/padded to 0x20)
        after aurora::gx::dl::optimize (extern/aurora/lib/gx/dl.cpp optimize/flush_batch, dl.hpp
        expand_triangles): NOPs dropped; consecutive expandable draws with the same VAT merge into one batch
        (vertex count <= 0xFFFF); a batch of only GX_TRIANGLES is re-emitted as one 3-byte TRIANGLES header +
        vertices, any other batch as a 10-byte GX_AURORA DRAW_INDEXED header + 2 bytes per index (3 per
        triangle) + vertices; non-expandable draws (lines/points/degenerate counts) are copied verbatim.
        'copy' = sum over packets of align32(max(raw, optimized)) (max: the raw list is copied if the
        optimizer fails). The room-block estimate adds J3DShape::kVcdVatDLSize (0x180 on PC) per shape.
  j3d_dlcheck.pos_indices(model_bytes) -> set of POS array indices referenced by the SHP1 display lists
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import j3d

COMP_SZ = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4}
GX_DRAW_OPS = (0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8)
GX_QUADS, GX_TRIANGLES, GX_TRIANGLESTRIP, GX_TRIANGLEFAN = 0x80, 0x90, 0x98, 0xA0


def _vtx_formats(d, vo):
    fmt_off = struct.unpack_from(">I", d, vo + 8)[0]
    fmts = {}
    o = vo + fmt_off
    while True:
        a, ct, comp, frac = struct.unpack_from(">IIIB", d, o)
        if a == 0xFF:
            break
        fmts[a] = (ct, comp, frac)
        o += 0x10
    return fmts


def _elem_size(fmts, a):
    ct, comp = fmts[a][0], fmts[a][1]
    if a in (11, 12):
        return {0: 2, 1: 3, 2: 4, 3: 2, 4: 3, 5: 4}[comp]
    n = {9: (2 if ct == 0 else 3), 10: 3, 25: 9}.get(a, 1 if ct == 0 else 2)
    return n * COMP_SZ[comp]


def _packets(d):
    """yield (shape_index, desc [(attr, type, direct_size)], stride, dl_offset, dl_size) for every SHP1 packet"""
    magic, size, secs = j3d.sections(d)
    S = {t: (o, s) for t, o, s in secs}
    fmts = _vtx_formats(d, S["VTX1"][0])
    so, ss = S["SHP1"]
    nshp = struct.unpack_from(">H", d, so + 8)[0]
    (init_off, idx_off, _, desc_off, mtx_off, dl_off, mtxinit_off, drawinit_off) = struct.unpack_from(">8I", d, so + 0xC)
    for i in range(nshp):
        si = so + init_off + i * 0x28
        mtxtype, _, ngrp, desc_idx, mtxinit_i, drawinit_i = struct.unpack_from(">BBHHHH", d, si)
        desc = []
        q = so + desc_off + desc_idx
        while True:
            a, t = struct.unpack_from(">II", d, q)
            if a == 0xFF:
                break
            desc.append((a, t, (1 if a <= 8 else _elem_size(fmts, a)) if t == 1 else (1 if t == 2 else 2)))
            q += 8
        stride = sum(sz for a, t, sz in desc if t in (1, 2, 3))
        for g in range(ngrp):
            dsz, doff = struct.unpack_from(">II", d, so + drawinit_off + (drawinit_i + g) * 8)
            yield i, desc, stride, so + dl_off + doff, dsz


def _tri_indices(prim, n):
    """index count after aurora expand_triangles, or None when the draw is not expandable"""
    if prim == GX_TRIANGLES:
        return n if n >= 3 and n % 3 == 0 else None
    if prim in (GX_TRIANGLESTRIP, GX_TRIANGLEFAN):
        return (n - 2) * 3 if n >= 3 else None
    if prim == GX_QUADS:
        return (n // 4) * 6 if n >= 4 and n % 4 == 0 else None
    return None


def _optimized_size(d, p, end, stride):
    out = 0
    bv = bi = 0          # batch vertices / indices
    ball = True          # batch only GX_TRIANGLES
    bfmt = None

    def flush():
        nonlocal out, bv, bi, ball, bfmt
        if bv:
            out += (3 if ball else 10 + 2 * bi) + bv * stride
        bv = bi = 0
        ball = True
        bfmt = None

    while p < end:
        op = d[p]
        if op == 0:                       # GX_NOP: dropped, does not split a batch
            p += 1
            continue
        if (op & 0xF8) not in GX_DRAW_OPS:
            return None                   # unknown command: Reader fails, the raw list is copied
        n = struct.unpack_from(">H", d, p + 1)[0]
        prim, fmt = op & 0xF8, op & 7
        size = 3 + n * stride
        if p + size > end:
            return None
        tri = _tri_indices(prim, n)
        if tri is None:
            flush()
            out += size
        else:
            if bv and (bfmt != fmt or bv + n > 0xFFFF):
                flush()
            if not bv:
                bfmt = fmt
            if prim != GX_TRIANGLES:
                ball = False
            bv += n
            bi += tri
        p += size
    flush()
    return out


def pc_dl_bytes(d):
    shapes = set()
    packets = raw = opt = copy = 0
    for shp, desc, stride, p, dsz in _packets(d):
        shapes.add(shp)
        packets += 1
        o = _optimized_size(d, p, p + dsz, stride)
        raw += dsz
        opt += o if o is not None else dsz
        copy += (max(dsz, o if o is not None else 0) + 0x1F) & ~0x1F
    return {"shapes": len(shapes), "packets": packets, "raw": raw, "optimized": opt, "copy": copy}


def pos_indices(d):
    out = set()
    for shp, desc, stride, p, dsz in _packets(d):
        end = p + dsz
        while p < end:
            op = d[p]
            if op == 0:
                p += 1
                continue
            if (op & 0xF8) not in GX_DRAW_OPS:
                break
            n = struct.unpack_from(">H", d, p + 1)[0]
            p += 3
            if p + n * stride > end:
                break
            for _ in range(n):
                vp = p
                for a, t, sz in desc:
                    if a == 9 and t == 2:
                        out.add(d[vp])
                    elif a == 9 and t == 3:
                        out.add(struct.unpack_from(">H", d, vp)[0])
                    if t in (1, 2, 3):
                        vp += sz
                p += stride
    return out

def check(d):
    magic, size, secs = j3d.sections(d)
    S = {t: (o, s) for t, o, s in secs}
    vo, vs = S["VTX1"]
    fmt_off = struct.unpack_from(">I", d, vo + 8)[0]
    arr_offs = struct.unpack_from(">13I", d, vo + 0xC)
    # attribute -> (array offset, element size)
    attr_order = [9, 10, 25, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]  # POS NRM NBT CLR0 CLR1 TEX0-7
    fmts = {}
    o = vo + fmt_off
    while True:
        a, ct, comp, frac = struct.unpack_from(">IIIB", d, o)
        if a == 0xFF: break
        fmts[a] = (ct, comp); o += 0x10
    def elem_size(a):
        ct, comp = fmts[a]
        if a in (11, 12):
            return {0: 2, 1: 3, 2: 4, 3: 2, 4: 3, 5: 4}[comp]
        n = {9: (2 if ct == 0 else 3), 10: 3, 25: 9}.get(a, 1 if ct == 0 else 2)
        return n * COMP_SZ[comp]
    present = [a for a in attr_order if a in fmts]
    offs = sorted(set([x for x in arr_offs if x] + [vs]))
    counts = {}
    for a in present:
        idx = attr_order.index(a)
        ao = arr_offs[idx]
        nxt = min(x for x in offs if x > ao)
        counts[a] = (nxt - ao) // elem_size(a)
    so, ss = S["SHP1"]
    nshp = struct.unpack_from(">H", d, so + 8)[0]
    (init_off, idx_off, _, desc_off, mtx_off, dl_off, mtxinit_off, drawinit_off) = struct.unpack_from(">8I", d, so + 0xC)
    maxidx = {}
    prims = verts = 0
    bad = 0
    for i in range(nshp):
        si = so + init_off + i * 0x28
        mtxtype, _, mtxgrp_num, desc_idx, mtxinit_i, drawinit_i = struct.unpack_from(">BBHHHH", d, si)
        # vertex descriptor list
        desc = []
        q = so + desc_off + desc_idx
        while True:
            a, t = struct.unpack_from(">II", d, q)
            if a == 0xFF: break
            desc.append((a, t)); q += 8
        for g in range(mtxgrp_num):
            dsz, doff = struct.unpack_from(">II", d, so + drawinit_off + (drawinit_i + g) * 8)
            p = so + dl_off + doff; end = p + dsz
            while p < end:
                op = d[p]
                if op == 0:
                    p += 1; continue
                if op & 0xF8 not in (0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8):
                    bad += 1; break
                n = struct.unpack_from(">H", d, p + 1)[0]; p += 3; prims += 1; verts += n
                for _ in range(n):
                    for a, t in desc:
                        if t == 1:  # direct
                            p += 1 if a in (0, 1, 2, 3, 4, 5, 6, 7, 8) else elem_size(a)
                        elif t == 2:
                            v = d[p]; p += 1
                            maxidx[a] = max(maxidx.get(a, 0), v)
                        elif t == 3:
                            v = struct.unpack_from(">H", d, p)[0]; p += 2
                            maxidx[a] = max(maxidx.get(a, 0), v)
    names = {9: "POS", 10: "NRM", 25: "NBT", 11: "CLR0", 12: "CLR1"}
    ok = bad == 0
    ranges = {}
    oor = []
    for a, m in sorted(maxidx.items()):
        nm = names.get(a, "TEX%d" % (a - 13))
        if a in counts:
            inr = m < counts[a]
            ok = ok and inr
            ranges[nm] = (m, counts[a])
            if not inr:
                oor.append(nm)
        elif a in (9, 10, 25, 11, 12) or 13 <= a <= 20:
            # indexed attribute with no VTX1 array/format at all
            ok = False
            ranges[nm] = (m, 0)
            oor.append(nm)
    return {"ok": ok, "shapes": nshp, "prims": prims, "verts": verts, "bad_opcodes": bad, "ranges": ranges,
            "out_of_range": oor}


def main(spec):
    r = check(j3d.load(spec))
    for nm, (m, c) in r["ranges"].items():
        print("  %-5s max index %6d / array entries %6d %s" % (nm, m, c, "ok" if m < c else "OUT OF RANGE"))
    print("%s: shapes=%d primitives=%d vertices=%d bad_opcodes=%d -> %s" % (spec.split('/')[-1], r["shapes"], r["prims"], r["verts"], r["bad_opcodes"], "OK" if r["ok"] else "PROBLEM"))

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    main(sys.argv[1])
