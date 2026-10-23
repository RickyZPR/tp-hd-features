"""bake_los.py - bake the TP HD Cave of Shadows room placement (content/los.bin) into the room models, so the
GC/Dusklight engine (which draws room models with an identity base matrix, MULT being all zero in D_SB11) shows
them where the collision (room.kcl) and the actors (room.dzr) already are.

usage: python -I bake_los.py <los.bin> <in_stage_dir (converted R??_00.arc)> <out_stage_dir> [--rooms 1,2,...]

Per room arc, every *.bmd / *.bdl member is rewritten:
  VTX1 POS  : p' = RotY(angle) * p + T      (T = los x,y,z; angle = los s16, multiples of 0x4000 in D_SB11)
  VTX1 NRM / NBT : rotated (x,z) only
  SHP1 shape bbox / radius, JNT1 joint bbox / radius : recomputed from the transformed vertices actually
       referenced by each shape's display lists (radius = max |v| like retail GC room models).
Rotation matches mDoMtx_stack_c::transS(T); YrotM(angle) (x' = x*c + z*s, z' = -x*s + z*c).
Rooms with no los entry/zero transform, or no model, are copied unchanged. Output archives are raw RARC
(uncompressed), built with ../formats/rarc_write.build_from_rarc.
Only reads the inputs; writes only into out_stage_dir.
"""
import sys, os, glob, math, struct
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, '..', 'formats'))
import yaz0, rarc, rarc_write, j3d, losbin

COMP = {0: ('B', 1), 1: ('b', 1), 2: ('H', 2), 3: ('h', 2), 4: ('f', 4)}
ATTR_ORDER = [9, 10, 25, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]


def rot(x, z, c, s):
    return x * c + z * s, -x * s + z * c


def bake_model(d, T, ang):
    d = bytearray(d)
    a = ang * math.pi / 32768.0
    c, s = round(math.cos(a), 9), round(math.sin(a), 9)
    _, _, secs = j3d.sections(bytes(d))
    S = {t: (o, sz) for t, o, sz in secs}
    vo, vs = S['VTX1']
    fmt_off = struct.unpack_from('>I', d, vo + 8)[0]
    arr_offs = struct.unpack_from('>13I', d, vo + 0xC)
    fmts = {}
    o = vo + fmt_off
    while True:
        at, ct, comp, frac = struct.unpack_from('>IIIB', d, o)
        if at == 0xFF:
            break
        fmts[at] = (ct, comp, frac)
        o += 0x10

    def esize(at):
        ct, comp, frac = fmts[at]
        if at in (11, 12):
            return {0: 2, 1: 3, 2: 4, 3: 2, 4: 3, 5: 4}[comp]
        n = {9: (2 if ct == 0 else 3), 10: (3 if ct == 0 else 9), 25: 9}.get(at, 1 if ct == 0 else 2)
        return n * COMP[comp][1]
    # walk SHP1 display lists: per-shape referenced POS indices
    so, ss = S['SHP1']
    nshp = struct.unpack_from('>H', d, so + 8)[0]
    init_off, idx_off, _, desc_off, mtx_off, dl_off, mtxinit_off, drawinit_off = struct.unpack_from('>8I', d, so + 0xC)
    shape_pos = []
    used = {9: set(), 10: set(), 25: set()}
    mtxtypes = set()
    for i in range(nshp):
        si = so + init_off + i * 0x28
        mtxtype, _, ngrp, desc_idx, mi, di = struct.unpack_from('>BBHHHH', d, si)
        mtxtypes.add(mtxtype)
        desc = []
        q = so + desc_off + desc_idx
        while True:
            at, t = struct.unpack_from('>II', d, q)
            if at == 0xFF:
                break
            desc.append((at, t))
            q += 8
        pos_here = set()
        for g in range(ngrp):
            dsz, doff = struct.unpack_from('>II', d, so + drawinit_off + (di + g) * 8)
            p = so + dl_off + doff
            end = p + dsz
            while p < end:
                op = d[p]
                if op == 0:
                    p += 1
                    continue
                if op & 0xF8 not in (0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8):
                    raise ValueError('bad GX opcode 0x%02x in shape %d' % (op, i))
                n = struct.unpack_from('>H', d, p + 1)[0]
                p += 3
                for _ in range(n):
                    for at, t in desc:
                        if t == 1:
                            if at in (9, 10, 25):
                                raise ValueError('direct POS/NRM not supported')
                            p += 1 if at <= 8 else esize(at)
                        else:
                            if t == 2:
                                v = d[p]
                                p += 1
                            else:
                                v = struct.unpack_from('>H', d, p)[0]
                                p += 2
                            if at in used:
                                used[at].add(v)
                            if at == 9:
                                pos_here.add(v)
        shape_pos.append(pos_here)
    if mtxtypes - {0}:
        raise ValueError('non-normal shape matrix types %s (billboards) - not baked' % sorted(mtxtypes))
    # transform arrays
    newpos = {}
    for at in (9, 10, 25):
        if at not in fmts:
            continue
        ct, comp, frac = fmts[at]
        fc, sz = COMP[comp]
        base = vo + arr_offs[ATTR_ORDER.index(at)]
        ncomp = esize(at) // sz
        q = (1 << frac) if comp != 4 else 1
        for idx in sorted(used[at]):
            off = base + idx * esize(at)
            vals = list(struct.unpack_from('>' + fc * ncomp, d, off))
            vals = [v / q for v in vals] if comp != 4 else vals
            if at == 9:
                x, y, z = vals[0], vals[1], vals[2]
                x, z = rot(x, z, c, s)
                out = [x + T[0], y + T[1], z + T[2]]
                newpos[idx] = out
            else:
                out = list(vals)
                for k in range(0, ncomp, 3):
                    out[k], out[k + 2] = rot(vals[k], vals[k + 2], c, s)
            if comp != 4:
                lim = {'B': (0, 255), 'b': (-128, 127), 'H': (0, 65535), 'h': (-32768, 32767)}[fc]
                iv = [int(round(v * q)) for v in out]
                if any(v < lim[0] or v > lim[1] for v in iv):
                    raise ValueError('quantized attr %d overflows after transform (frac %d)' % (at, frac))
                struct.pack_into('>' + fc * ncomp, d, off, *iv)
            else:
                struct.pack_into('>' + 'f' * ncomp, d, off, *out)

    def bbox(idxs):
        pts = [newpos[i] for i in idxs]
        mn = [min(p[k] for p in pts) for k in range(3)]
        mx = [max(p[k] for p in pts) for k in range(3)]
        r = max(math.sqrt(p[0] ** 2 + p[1] ** 2 + p[2] ** 2) for p in pts)
        return mn, mx, r
    for i in range(nshp):
        if not shape_pos[i]:
            continue
        mn, mx, r = bbox(shape_pos[i])
        si = so + init_off + i * 0x28
        struct.pack_into('>f3f3f', d, si + 0xC, r, *(mn + mx))
    jo, js = S['JNT1']
    nj = struct.unpack_from('>H', d, jo + 8)[0]
    if nj != 1:
        raise ValueError('expected 1 joint, got %d' % nj)
    eo = struct.unpack_from('>I', d, jo + 0xC)[0]
    mn, mx, r = bbox(set().union(*shape_pos))
    struct.pack_into('>f3f3f', d, jo + eo + 0x24, r, *(mn + mx))
    return bytes(d), mn, mx, len(used[9])


def main(argv):
    losp, ind, outd = argv[1:4]
    rooms = None
    if '--rooms' in argv:
        rooms = set(int(x) for x in argv[argv.index('--rooms') + 1].split(','))
    L = {e['room']: e for e in losbin.load(losp)['entries']}
    os.makedirs(outd, exist_ok=True)
    for p in sorted(glob.glob(os.path.join(glob.escape(ind), '*.arc'))):
        if not (os.path.basename(p)[0] in 'Rr' and os.path.basename(p)[1:3].isdigit()):
            open(os.path.join(outd, os.path.basename(p)), 'wb').write(open(p, 'rb').read())
            print('%s copied unchanged' % os.path.basename(p))
            continue
        rn = int(os.path.basename(p)[1:3])
        if rooms and rn not in rooms:
            continue
        raw = open(p, 'rb').read()
        arc = rarc.Rarc(raw, os.path.basename(p))
        e = L.get(rn)
        T = (e['x'], e['y'], e['z']) if e else (0.0, 0.0, 0.0)
        ang = e['angleY'] if e else 0
        repl = {}
        notes = []
        if e and e['floor'] > 0:
            for m in arc.files():
                if m.name.lower().endswith(('.bmd', '.bdl')):
                    nb, mn, mx, nv = bake_model(m.data(), T, ang)
                    repl[m.path] = nb
                    notes.append('%s: %d verts -> bbox (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)' % (m.name, nv, *mn, *mx))
        out = rarc_write.build_from_rarc(arc, repl) if repl else yaz0.maybe_decompress(raw)
        open(os.path.join(outd, os.path.basename(p)), 'wb').write(out)
        print('R%02d floor %s T=(%.0f,%.0f,%.0f) ang=%d %s' % (rn, e['floor'] if e else '-', T[0], T[1], T[2], ang, '; '.join(notes) or 'copied unchanged'))


if __name__ == '__main__':
    main(sys.argv)
