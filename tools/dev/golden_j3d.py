#!/usr/bin/env python3
"""golden_j3d.py - golden list for `hdf-convert test j3d` (module J, plan 3.3), made by the Python pipeline (the
executable spec) from YOUR OWN game files and the pinned oracle.

The list holds hashes, sizes, numbers, input/output file paths and indices only: no game bytes and no names from
inside the files (archive members are given by their walk index in rarc.Rarc.files()). It is still made from your
games, so it goes to a build-*/ folder of the project or a scratch folder, never into the source tree, and is never
committed or shared.

USAGE (always with -I -B: no .pyc is written into tools/)
  python -I -B tools/dev/golden_j3d.py --hd <TP HD title or content folder> --oracle <hd_features-oracle-2.0.0.dusk>
                                       --out <build dir>/golden/j3d.tsv [--tools <project>/tools]
  python -I -B tools/dev/golden_j3d.py --vectors [<project>/tools]
           no game files: prints the generated block of tests/j3d_test.cpp (synthetic models and Gfx2 files made
           here, as hex, and the Python spec's results on them).

The 55 archives are the oracle's overlay/res/Stage/** and overlay/res/Object/** .arc files. For each, this script
runs pipeline/convert.convert_arc, then (D_SB11 rooms with a los floor > 0) cave/bake_los.bake_model on every
model, as pipeline/cavebuild.build does, and stops unless every converted member and every rebuilt archive equals
the oracle. Room archives take their room.dzr (and STG_00 its stage.dzs) from the oracle: those are module ST's.

LINES (tab-separated; a = archive index in this list, m = member walk index)
  angle  <ang> <repr cos> <repr sin> <cos bits> <sin bits>    (round(math.cos(a), 9), round(math.sin(a), 9))
  arc    <a> <oracle file> <hd arc> <hd pack|-> <layout gc|same> <bake angle|-> <bake x y z repr|->
  st     <a> <models> <mat_fixed> <textures> <no_source> <bti> <mdl3> <report lines> <sha256 of '\\n'.join(report)>
  model  <a> <m> <in size> <in sha256> <gtx tmpk idx|-1> <convert_j3d size> <sha256> <MAT strides> <fixed>
         <out size> <out sha256>                              (hd2gc.convert_j3d, then mat4fix.fix_j3d)
  bti    <a> <m> <in size> <in sha256> <gtx tmpk idx|-1> <out size> <out sha256>     (hd2gc.convert_bti)
  bake   <a> <m> <vertices> <min x y z repr> <max x y z repr> <size> <sha256>       (bake_los.bake_model)
  final  <a> <m> <oracle member idx> <size> <sha256>          (= the oracle member, checked here)
  dl     <a> <m> <hd|conv|final> <shapes> <packets> <raw> <optimized> <copy>   (j3d_dlcheck.pc_dl_bytes; 'err')
  mstats <a> <m> <mat> <shp> <jnt> <tex> <dl>                 (memfit.model_stats of the final model)
  idem   <a> <m> <fixed> <unchanged 0|1>                      (mat4fix.fix_j3d of the final = oracle model)
  built  <a> <size> <sha256>                                  (the rebuilt archive = the oracle archive)
  arcst  <a> <arc> <models> <mat> <shp> <jnt> <tex> <anm> <files> <dl>   (memfit.arc_stats of the built archive)
MAT strides: matstride.effective_stride of every MAT block of the convert_j3d output, comma-separated, '-' = None.
"""
import hashlib
import math
import os
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
ORACLE_SHA256 = 'a7a48bf643e621b89e3860fe9d7c48b26e76ec9b6cf518af8c5ff2b42bc68f59'
ANGLES = (-0x8000, -0x4000, 0, 0x4000)
OBJ_SAME_LAYOUT = ('O_gD_puL4.arc', 'Obj_Stamp.arc')    # cavebuild OBJ_ITEM, stampbuild Obj_Stamp
_T = {}


def setup(tools):
    """sys.path like tools/pipeline/common.py (formats, critic, cave first; pipeline for its modules)."""
    if _T.get('tools') == tools:
        return
    _T['tools'] = tools
    for sub in ('pipeline', 'cave', 'critic', 'formats'):
        p = os.path.join(tools, sub)
        if p in sys.path:
            sys.path.remove(p)
        sys.path.insert(0, p)


def sha(b):
    return hashlib.sha256(bytes(b)).hexdigest()


def norm(p):
    return os.path.normcase(os.path.realpath(os.path.abspath(p)))


def within(child, parent):
    c, p = norm(child), norm(parent)
    return c == p or c.startswith(p.rstrip('\\/') + os.sep)


def check_out(out, inputs, tools):
    for root in {PROJECT, os.path.dirname(tools)}:
        if within(out, root):
            first = os.path.relpath(norm(out), norm(root)).split(os.sep)[0]
            if not first.startswith('build-') or first == 'build-hdf' or len(first) <= 6:
                raise SystemExit('golden_j3d.py: --out must be in a build-*/ folder of the project (not build-hdf) '
                                 'or a scratch folder: the list is made from your games')
    for i in inputs:
        if within(out, i) or within(i, out):
            raise SystemExit('golden_j3d.py: --out overlaps the input %s' % i)
    appdata = os.environ.get('APPDATA')
    if appdata and within(out, os.path.join(appdata, 'TwilitRealm')):
        raise SystemExit('golden_j3d.py: refusing to write into %APPDATA%\\TwilitRealm')


def find_ci(root, rel):
    cur = root
    for part in rel.split('/'):
        if not os.path.isdir(cur):
            return None
        low = part.lower()
        hit = None
        for n in sorted(os.listdir(cur)):
            if n.lower() == low:
                hit = n
                break
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def bits(x):
    return '%08x' % struct.unpack('>I', struct.pack('>f', x))[0]


def dbits(x):
    return '%016x' % struct.unpack('>Q', struct.pack('>d', x))[0]


def dl_row(j3d_dlcheck, d):
    try:
        r = j3d_dlcheck.pc_dl_bytes(d)
        return (r['shapes'], r['packets'], r['raw'], r['optimized'], r['copy'])
    except Exception:  # noqa: BLE001
        return ('err',)


def strides(j3d, matstride, d):
    out = []
    _, _, secs = j3d.sections(d)
    for tag, o, s in secs:
        if tag in ('MAT3', 'MAT4'):
            try:
                es = matstride.effective_stride(bytes(d), o)
                out.append('-' if es is None else '%x' % es)
            except Exception as ex:  # noqa: BLE001
                out.append('err:' + type(ex).__name__)
    return ','.join(out) or '-'


def convert_members(a, tm, arc_name, rows, ai):
    """pipeline/convert.convert_arc's member loop, with the intermediate results (checked against it below)."""
    import hd2gc
    import mat4fix
    import j3d
    import matstride
    import j3d_dlcheck
    report = []
    repl = {}
    st = {'models': 0, 'mat_fixed': 0, 'textures': 0, 'no_source': 0, 'bti': 0, 'mdl3': 0}
    for mi, m in enumerate(a.files()):
        ln = m.name.lower()
        if not ln.endswith(('.bmd', '.bdl', '.bti')):
            continue
        gtx, gi = None, -1
        if tm:
            for ei, e in enumerate(tm.entries):
                if e['name'].lower().endswith('/' + m.path.lower() + '.gtx'):
                    gtx, gi = tm.data(e), ei
                    break
        d = m.data()
        lab = '%s:%s' % (arc_name, m.path)
        n0 = len(report)
        if ln.endswith('.bti'):
            nd = hd2gc.convert_bti(d, gtx, 1, report, lab)
            st['bti'] += 1
            rows.append(('bti', ai, mi, len(d), sha(d), gi, len(nd), sha(nd)))
        else:
            cj = hd2gc.convert_j3d(d, gtx, 1, {}, None, report, lab)
            sd = strides(j3d, matstride, cj)
            nd, fixed = mat4fix.fix_j3d(cj)
            st['models'] += 1
            st['mat_fixed'] += fixed
            if fixed:
                report.append('%s: MAT 0x14E -> 0x14C repack (%d block)' % (lab, fixed))
            rows.append(('model', ai, mi, len(d), sha(d), gi, len(cj), sha(cj), sd, fixed, len(nd), sha(nd)))
            rows.append(('dl', ai, mi, 'hd') + dl_row(j3d_dlcheck, d))
            rows.append(('dl', ai, mi, 'conv') + dl_row(j3d_dlcheck, nd))
        for line in report[n0:]:
            if 'NO SOURCE' in line or 'left as is' in line:
                st['no_source'] += 1
            elif 'MDL3' in line:
                st['mdl3'] += 1
            elif '<- GTX' in line:
                st['textures'] += 1
        repl[m.path] = nd
    return repl, st, report


def real(args):
    tools = os.path.abspath(args.get('tools') or os.path.dirname(HERE))
    hd, out = args['hd'], args['out']
    if os.path.isdir(os.path.join(hd, 'content')):
        hd = os.path.join(hd, 'content')
    check_out(out, [args['hd'], args['oracle']], tools)
    with open(args['oracle'], 'rb') as f:
        if hashlib.sha256(f.read()).hexdigest() != ORACLE_SHA256:
            raise SystemExit('golden_j3d.py: the oracle is not the pinned file')
    setup(tools)
    import common  # noqa: F401  (pipeline path setup, as build_assets does)
    import rarc
    import rarc_write
    import rarcx
    import hdpack
    import convert
    import bake_los
    import losbin
    import memfit
    import mat4fix
    import j3d_dlcheck
    rows = []
    for ang in ANGLES:
        a = ang * math.pi / 32768.0
        c, s = round(math.cos(a), 9), round(math.sin(a), 9)
        rows.append(('angle', ang, repr(c), repr(s), dbits(c), dbits(s)))
    by_room = {e['room']: e for e in losbin.load(os.path.join(hd, 'los.bin'))['entries']}
    oz = zipfile.ZipFile(args['oracle'])
    names = sorted(n for n in oz.namelist() if n.lower().endswith('.arc') and
                   (n.startswith('overlay/res/Stage/') or n.startswith('overlay/res/Object/')))
    counts = {'archives': 0, 'models': 0, 'bti': 0, 'baked': 0, 'identical': 0, 'mat_fixed': 0}
    for ai, on in enumerate(names):
        hd_rel = on[len('overlay/'):]
        p = find_ci(hd, hd_rel)
        if p is None:
            raise SystemExit('golden_j3d.py: no HD source for %s' % on)
        hd_rel = os.path.relpath(p, hd).replace(os.sep, '/')
        pack = p[:-4] + '.pack.gz'
        tm = hdpack.Tmpk(hdpack.read_any(pack)) if os.path.exists(pack) else None
        pack_rel = os.path.relpath(find_ci(hd, hd_rel[:-4] + '.pack.gz'), hd).replace(os.sep, '/') if tm else '-'
        fn = os.path.basename(p)
        a = rarc.Rarc.from_file(p)
        repl, st, report = convert_members(a, tm, fn, rows, ai)
        # the replication above must equal pipeline/convert.convert_arc
        a2, repl2, st2, rep2 = convert.convert_arc(p)
        if repl2 != repl or st2 != st or rep2 != report:
            raise SystemExit('golden_j3d.py: %s: replication != convert.convert_arc' % on)
        counts['archives'] += 1
        counts['models'] += st['models']
        counts['bti'] += st['bti']
        counts['mat_fixed'] += st['mat_fixed']
        is_room = '/Stage/' in on and fn[0] in 'Rr' and fn[1:3].isdigit()
        is_stg = '/Stage/' in on and fn.upper().startswith('STG')
        e = by_room.get(int(fn[1:3])) if is_room else None
        bake = e if (e and e['floor'] > 0) else None
        layout = 'same' if fn in OBJ_SAME_LAYOUT else 'gc'
        rows.append(('arc', ai, on, hd_rel, pack_rel, layout, bake['angleY'] if bake else '-',
                     ' '.join(repr(bake[k]) for k in ('x', 'y', 'z')) if bake else '-'))
        rows.append(('st', ai, st['models'], st['mat_fixed'], st['textures'], st['no_source'], st['bti'], st['mdl3'],
                     len(report), sha('\n'.join(report).encode('utf-8'))))
        if bake:
            for mi, m in enumerate(a.files()):
                if m.path in repl and m.path.lower().endswith(('.bmd', '.bdl')):
                    nb, mn, mx, nv = bake_los.bake_model(repl[m.path], (bake['x'], bake['y'], bake['z']),
                                                         bake['angleY'])
                    repl[m.path] = nb
                    counts['baked'] += 1
                    rows.append(('bake', ai, mi, nv, ' '.join(repr(v) for v in mn), ' '.join(repr(v) for v in mx),
                                 len(nb), sha(nb)))
        oa = rarc.Rarc(oz.read(on), on)
        opaths = [om.path.lower() for om in oa.files()]
        for mi, m in enumerate(a.files()):
            if m.path not in repl:
                continue
            nd = repl[m.path]
            omi = opaths.index(m.path.lower()) if m.path.lower() in opaths else -1
            if omi < 0 or oa.files()[omi].data() != nd:
                raise SystemExit('golden_j3d.py: %s member %d != oracle' % (on, mi))
            counts['identical'] += 1
            rows.append(('final', ai, mi, omi, len(nd), sha(nd)))
            if m.name.lower().endswith(('.bmd', '.bdl')):
                rows.append(('dl', ai, mi, 'final') + dl_row(j3d_dlcheck, nd))
                ms = memfit.model_stats(nd)
                rows.append(('mstats', ai, mi, ms['mat'], ms['shp'], ms['jnt'], ms['tex'], ms['dl']))
                fd, fixed = mat4fix.fix_j3d(nd)
                rows.append(('idem', ai, mi, fixed, 1 if fd == nd else 0))
        # the archive as the pipeline writes it (room.dzr / stage.dzs: the oracle's, module ST makes them)
        full = dict(repl)
        for key in (('room.dzr',) if is_room else ('stage.dzs',) if is_stg else ()):
            hm, om = a.get(key), oa.get(key)
            if hm is not None and om is not None and om.data() != hm.data():
                full[hm.path] = om.data()
        b = rarcx.rebuild_same_layout(a, full) if layout == 'same' else rarc_write.build_from_rarc(a, full)
        if b != oz.read(on):
            raise SystemExit('golden_j3d.py: %s: rebuilt archive != oracle' % on)
        rows.append(('built', ai, len(b), sha(b)))
        s = memfit.arc_stats(b)
        rows.append(('arcst', ai, s['arc'], s['models'], s['mat'], s['shp'], s['jnt'], s['tex'], s['anm'],
                     s['files'], s['dl']))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# hdf golden j3d 1: made by tools/dev/golden_j3d.py from your own game files (hashes, numbers and '
                'indices only; never commit or share)\n')
        f.write('# counts %s\n' % ' '.join('%s=%d' % kv for kv in counts.items()))
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print('golden j3d: %d lines -> %s (%s)' % (len(rows), out, ', '.join('%s %d' % kv for kv in counts.items())))
    return 0


# ---------------------------------------------------------------------------------------------- unit vectors
# Synthetic J3D models and Gfx2 files (no game data), written as hex into tests/j3d_test.cpp with the spec's results.
class Rng(object):
    def __init__(self, seed):
        self.x = seed & 0x7fffffff

    def byte(self):
        self.x = (self.x * 1103515245 + 12345) & 0x7fffffff
        return (self.x >> 16) & 255

    def bytes(self, n):
        return bytes(self.byte() for _ in range(n))


def _pad(b, a):
    while len(b) % a:
        b.append(0)


def make_gtx(rng, surfaces, gtx2img):
    """Gfx2 with several surfaces (fmt, w, h, mips), 1D tiled (mode 2, as TP HD's small levels), identity compSel."""
    out = bytearray(b'Gfx2' + struct.pack('>5I', 0x20, 7, 1, 2, 0) + b'\0' * 8)

    def blk(t, data):
        out.extend(b'BLK{' + struct.pack('>7I', 0x20, 1, 0, t, len(data), 0, 0) + data)

    def ru(v, a):
        return (v + a - 1) // a * a
    for fmt, w, h, mips in surfaces:
        bpp = gtx2img.FMT_BPP[fmt]
        bc = fmt in gtx2img.BCN
        tm = 2
        ew, eh = ((w + 3) // 4, (h + 3) // 4) if bc else (w, h)
        pitch = ru(ew, max(8, 256 // bpp))
        img_size = pitch * ru(eh, 8) * bpp // 8
        s = {'fmt': fmt, 'w': w, 'h': h, 'mips': mips, 'tileMode': tm, 'swizzle': 0, 'pitch': pitch,
             'img_off': 0, 'img': img_size, 'mip_off': 0, 'mipOffsets': [0] * 13}
        offs = [0] * 13
        total = 0
        for L in range(1, mips):
            if L >= 2:
                offs[L - 1] = total
            s['mipOffsets'] = offs
            total += gtx2img.level_info(s, L)['data_size']
        surf = bytearray(0x9C)
        struct.pack_into('>16I', surf, 0, 1, w, h, 1, mips, fmt, 0, 1, img_size, 0, total, 0, tm, 0, 0x200, pitch)
        struct.pack_into('>13I', surf, 0x40, *offs)
        surf[0x84:0x88] = bytes((0, 1, 2, 3))
        blk(0x0B, bytes(surf))
        blk(0x0C, rng.bytes(img_size))
        if mips > 1:
            blk(0x0D, rng.bytes(total))
    blk(0x01, b'')
    return bytes(out)


def make_tmpk(entries):
    """hdpack.Tmpk layout: 'TMPK', count, align 0x20, 0; (name off, data off, size, 0) per entry; names; data
    32-aligned. tests/j3d_test.cpp builds the same bytes."""
    n = len(entries)
    names = bytearray()
    noffs = []
    for name, _ in entries:
        noffs.append(0x10 + 16 * n + len(names))
        names += name.encode('ascii') + b'\0'
    out = bytearray(b'TMPK' + struct.pack('>III', n, 0x20, 0))
    out += b'\0' * (16 * n)
    out += names
    for i, (name, data) in enumerate(entries):
        _pad(out, 32)
        struct.pack_into('>IIII', out, 0x10 + 16 * i, noffs[i], len(out), len(data), 0)
        out += data
    return bytes(out)


def make_model(rng, opt):
    """A small J3D2 model: VTX1 (POS f32 or s16/frac 1, NRM s16/frac 14, TEX0 s16/frac 8), SHP1 (shapes with
    TRIANGLES / STRIP / FAN / QUADS / LINES / NOPs, index8 and index16), JNT1 (1 joint), MAT4 (0x14E entries,
    HD layout) or MAT3, TEX1 (header-only entries + string table), optional MDL3."""
    nv = opt.get('nv', 24)
    pos_q = opt.get('pos_q', False)
    secs = []
    # ---- VTX1
    v = bytearray(0x40)
    v[0:4] = b'VTX1'
    nrm_f = opt.get('nrm_f32', False)
    fmts = [(9, 1, 3 if pos_q else 4, 1 if pos_q else 0), (10, 0, 4 if nrm_f else 3, 0 if nrm_f else 14),
            (13, 1, 3, 8)]
    struct.pack_into('>I', v, 8, 0x40)
    for at, ct, comp, frac in fmts:
        v += struct.pack('>IIIB', at, ct, comp, frac) + b'\xff\xff\xff'
    v += struct.pack('>IIIB', 0xFF, 1, 0, 0) + b'\xff\xff\xff'
    _pad(v, 32)
    arr = [0] * 13
    arr[0] = len(v)
    for i in range(nv):
        x, y, z = [((rng.byte() << 8 | rng.byte()) - 32768) / 16.0 for _ in range(3)]
        if i == 0:
            x, y, z = 0.0, -0.0, 0.0
        if pos_q:
            v += struct.pack('>hhh', *[int(t / 8) for t in (x, y, z)])
        else:
            v += struct.pack('>fff', x, y, z)
    _pad(v, 32)
    arr[1] = len(v)
    for i in range(nv):
        a = (rng.byte() * 2 * math.pi) / 256.0
        if nrm_f:   # zeros in x / z: a rotation by -0x8000 (sin -0.0) gives -0.0 there
            n3 = ((0.0, 1.0, 0.0), (0.0, 0.0, 0.5), (0.25, 0.0, 0.0), (math.cos(a), 0.125, math.sin(a)))[i % 4]
            v += struct.pack('>fff', *n3)
        else:
            v += struct.pack('>hhh', int(math.cos(a) * 16000), rng.byte() * 8 - 1024, int(math.sin(a) * 16000))
    _pad(v, 32)
    arr[5] = len(v)
    for i in range(nv):
        v += struct.pack('>hh', rng.byte() * 4, rng.byte() * 4)
    _pad(v, 32)
    struct.pack_into('>13I', v, 0xC, *arr)
    struct.pack_into('>I', v, 4, len(v))
    secs.append(bytes(v))
    # ---- SHP1
    shapes = opt.get('shapes')
    nshp = len(shapes)
    hdr = 0x2C
    init_off = 0x40
    desc_off = init_off + nshp * 0x28
    descs = [struct.pack('>II', 9, 3) + struct.pack('>II', 10, 3) + struct.pack('>II', 13, 3) + struct.pack('>II', 0xFF, 0),
             struct.pack('>II', 0, 1) + struct.pack('>II', 9, 2) + struct.pack('>II', 10, 2) + struct.pack('>II', 0xFF, 0)]
    desc_blob = b''.join(descs)
    dls = []
    for sh in shapes:
        kind = sh['desc']
        stride = 6 if kind == 0 else 3
        blob = bytearray()
        for prim, idxs, nops in sh['prims']:
            blob += b'\0' * nops
            blob += bytes((prim,)) + struct.pack('>H', len(idxs))
            for ix in idxs:
                if kind == 0:
                    blob += struct.pack('>HHH', ix, ix, ix)
                else:
                    blob += bytes((rng.byte() % 10, ix, ix))
        if sh.get('bad_op'):
            blob += b'\x13\x00\x01'
        _pad(blob, 32)
        dls.append(bytes(blob))
    dl_off = desc_off + len(desc_blob)
    dl_off = (dl_off + 31) // 32 * 32
    s = bytearray(dl_off)
    s[0:4] = b'SHP1'
    struct.pack_into('>H', s, 8, nshp)
    doffs, cur = [], 0
    for b in dls:
        doffs.append(cur)
        cur += len(b)
    drawinit_off = dl_off + cur
    for i, sh in enumerate(shapes):
        si = init_off + i * 0x28
        struct.pack_into('>BBHHHH', s, si, sh.get('mtx', 0), 0xFF, 1, 0 if sh['desc'] == 0 else 0x20, 0, i)
    s[desc_off:desc_off + len(desc_blob)] = desc_blob
    for b in dls:
        s += b
    for i, b in enumerate(dls):
        s += struct.pack('>II', len(b), doffs[i])
    _pad(s, 32)
    struct.pack_into('>8I', s, 0xC, init_off, 0, 0, desc_off, 0, dl_off, 0, drawinit_off)
    struct.pack_into('>I', s, 4, len(s))
    secs.append(bytes(s))
    # ---- JNT1
    j = bytearray(0x20 + 0x40)
    j[0:4] = b'JNT1'
    struct.pack_into('>H', j, 8, opt.get('joints', 1))
    struct.pack_into('>I', j, 0xC, 0x20)
    struct.pack_into('>I', j, 4, len(j))
    secs.append(bytes(j))
    # ---- MAT4 / MAT3
    nmat = opt.get('nmat', 3)
    stride = opt.get('mat_stride', 0x14E)
    m = bytearray(0x88)
    m[0:4] = opt.get('mat_tag', b'MAT4')
    struct.pack_into('>H', m, 8, nmat)
    ent_off = 0x88
    tables = bytearray()
    ents = bytearray()
    for i in range(nmat):
        e = bytearray(b'\xff' * 0x14C)
        e[0] = (1, 2, 4)[i % 3]
        e[1] = 0   # cull mode index 0 (table 4)
        ents += e
        if stride == 0x14E:
            ents += struct.pack('>H', opt.get('poly', 1 + i))
    _pad(ents, 4)
    ids_off = ent_off + len(ents)
    tables += struct.pack('>%dH' % nmat, *range(nmat))
    _pad(tables, 4)
    cull_off = ids_off + len(tables)
    tables += struct.pack('>I', 0)      # one cull mode
    names_off = ids_off + len(tables)
    tables += b'\0' * 8
    m += ents + tables
    _pad(m, 32)
    offs = [0] * 30
    offs[0], offs[1], offs[2], offs[4] = ent_off, ids_off, names_off, cull_off
    struct.pack_into('>30I', m, 0xC, *offs)
    struct.pack_into('>I', m, 4, len(m))
    secs.append(bytes(m))
    # ---- MDL3
    if opt.get('mdl3'):
        secs.append(b'MDL3' + struct.pack('>IH', 0x20, 0) + b'\0' * 22)
    # ---- TEX1
    texs = opt.get('tex', [])
    t = bytearray(0x20)
    t[0:4] = b'TEX1'
    struct.pack_into('>HH', t, 8, len(texs), 0xFFFF)
    struct.pack_into('>I', t, 0xC, 0x20)
    for fmt, w, h, mips in texs:
        b = bytearray(0x20)
        struct.pack_into('>BBHH', b, 0, fmt, 1, w, h)
        b[6] = b[7] = 1
        b[8], b[9] = (1, 2) if fmt in (8, 9) else (0, 0)
        b[0x14], b[0x15] = (5, 1) if mips > 1 else (1, 1)
        b[0x18] = mips
        b[0x17] = (mips - 1) * 8
        t += b
    str_off = len(t)
    st = bytearray(struct.pack('>HH', len(texs), 0xFFFF))
    names = [('tex%02d' % i).encode() for i in range(len(texs))]
    base = 4 + 4 * len(texs)
    o = base
    for nm in names:
        st += struct.pack('>HH', 0, o)
        o += len(nm) + 1
    for nm in names:
        st += nm + b'\0'
    t += st
    _pad(t, 32)
    struct.pack_into('>I', t, 0x10, str_off if opt.get('strtab', True) else 0)
    struct.pack_into('>I', t, 4, len(t))
    secs.append(bytes(t))
    body = b''.join(secs)
    head = bytearray(b'J3D2bmd3' + struct.pack('>II', 0x20 + len(body), len(secs)) + b'SVR3' + b'\xff' * 12)
    return bytes(head) + body


def cxx_bytes(name, b):
    lines = ['const char* const %s =' % name]
    h = b.hex()
    for i in range(0, len(h), 112):
        lines.append('    "%s"' % h[i:i + 112])
    lines[-1] += ';'
    return lines


def vectors(tools):
    setup(tools)
    import hd2gc
    import mat4fix
    import bake_los
    import j3d_dlcheck
    import matstride
    import memfit
    import gtx2img
    import rarc_write
    rng = Rng(0x4A3D)
    tris = list(range(0, 9))
    strip = [9, 10, 11, 12, 13]
    fan = [14, 15, 16, 17]
    quads = [18, 19, 20, 21, 22, 23, 0, 1]
    shapes_a = [{'desc': 0, 'prims': [(0x90, tris, 2), (0x98, strip, 0), (0x90, [1, 2, 3], 1), (0xA0, fan, 0)]},
                {'desc': 1, 'prims': [(0x80, quads, 0), (0xA8, [3, 4], 0), (0x91, [5, 6, 7], 0), (0x90, [7, 8], 0)]},
                {'desc': 0, 'prims': [], 'bad_op': False},
                {'desc': 1, 'prims': [(0xB8, [2], 3), (0x98, [4, 5], 0)], 'bad_op': True}]
    cases = []
    # model A: MAT4 with 3 materials (0x14E), 2 textures with GTX (BC1 -> CMPR, RGBA8 -> RGB5A3), MDL3
    gtx_a = make_gtx(rng, [(0x31, 16, 16, 3), (0x1A, 8, 8, 1)], gtx2img)
    cases.append(('a', make_model(rng, {'shapes': shapes_a, 'mdl3': True, 'tex': [(14, 16, 16, 3), (5, 8, 8, 1)]}),
                  gtx_a))
    # model B: quantized POS, 1 material with polygon offset (0x14E, uniq 1), 3 textures but only 1 surface
    gtx_b = make_gtx(rng, [(0x1A, 4, 4, 1)], gtx2img)
    cases.append(('b', make_model(rng, {'shapes': shapes_a[:2], 'pos_q': True, 'nmat': 1, 'poly': 7,
                                        'tex': [(6, 4, 4, 1), (9, 4, 4, 1), (0, 8, 8, 1)]}), gtx_b))
    # model C: MAT3 already at 0x14C (GC layout), no GTX, no string table
    cases.append(('c', make_model(rng, {'shapes': shapes_a[:1], 'mat_tag': b'MAT3', 'mat_stride': 0x14C,
                                        'tex': [(14, 8, 8, 1)], 'strtab': False}), None))
    # model D: 1 material, polygon offset 0 (uniq 1, trailing 0): stays 0x14C; 2 joints (bake refuses)
    cases.append(('d', make_model(rng, {'shapes': shapes_a[:1], 'nmat': 1, 'poly': 0, 'joints': 2, 'tex': []}), None))
    # model E: a billboard shape (mtx type 1): bake refuses
    cases.append(('e', make_model(rng, {'shapes': [dict(shapes_a[0], mtx=1)], 'tex': []}), None))
    # model F: float normals with zero components (the signed zero of the -0x8000 bake shows in them)
    cases.append(('f', make_model(rng, {'shapes': shapes_a[:2], 'nrm_f32': True, 'tex': []}), None))
    out = []
    out.append('// ---- generated by tools/dev/golden_j3d.py --vectors (synthetic data only) ----')
    for name, d, gtx in cases:
        out += cxx_bytes('kModel_%s' % name, d)
        if gtx:
            out += cxx_bytes('kGtx_%s' % name, gtx)
    out.append('struct ModelVec {')
    out.append('    const char* name; const char* model; const char* gtx;')
    out.append('    const char* conv;       // sha256 of hd2gc.convert_j3d')
    out.append('    const char* report;     // sha256 of "\\n".join(report) (label "")')
    out.append('    int nreport;')
    out.append('    const char* strides;    // effective_stride per MAT block of the convert_j3d output')
    out.append('    int fixed; const char* fixd;  // mat4fix.fix_j3d')
    out.append('    const char* dl;         // pc_dl_bytes of the fixed model: shapes packets raw optimized copy | err')
    out.append('    const char* mstats;     // memfit.model_stats: mat shp jnt tex dl')
    out.append('    int idem_fixed; int idem_same;')
    out.append('};')
    out.append('const ModelVec kModelVecs[] = {')
    fixed_models = {}
    for name, d, gtx in cases:
        rep = []
        cj = hd2gc.convert_j3d(d, gtx, 1, {}, None, rep, '')
        sd = []
        import j3d
        _, _, secs = j3d.sections(cj)
        for tag, o, s in secs:
            if tag in ('MAT3', 'MAT4'):
                try:
                    es = matstride.effective_stride(bytes(cj), o)
                    sd.append('-' if es is None else '%x' % es)
                except Exception as ex:  # noqa: BLE001
                    sd.append('err')
        fd, fixed = mat4fix.fix_j3d(cj)
        fixed_models[name] = fd
        try:
            r = j3d_dlcheck.pc_dl_bytes(fd)
            dl = '%d %d %d %d %d' % (r['shapes'], r['packets'], r['raw'], r['optimized'], r['copy'])
        except Exception:  # noqa: BLE001
            dl = 'err'
        ms = memfit.model_stats(fd)
        f2, n2 = mat4fix.fix_j3d(fd)
        out.append('    {"%s", kModel_%s, %s, "%s", "%s", %d, "%s", %d, "%s", "%s", "%d %d %d %d %d", %d, %d},' % (
            name, name, ('kGtx_%s' % name) if gtx else 'nullptr', sha(cj), sha('\n'.join(rep).encode()), len(rep),
            ','.join(sd), fixed, sha(fd), dl, ms['mat'], ms['shp'], ms['jnt'], ms['tex'], ms['dl'], n2,
            1 if f2 == fd else 0))
    out.append('};')
    # bakes: every angle on model A and B (fixed), plus refusals
    out.append('struct BakeVec { const char* model; int angle; float t[3]; const char* result; };')
    out.append('// result: "<sha256> <vertices> <min x y z repr> <max x y z repr>" or "err"')
    out.append('const BakeVec kBakeVecs[] = {')
    ts = [(0.0, 0.0, 0.0), (-4100.0, 1250.0, 0.0), (0.0, -30000.0, -4100.0), (123.5, -0.25, 77.75)]
    for name in ('a', 'b', 'd', 'e', 'f'):
        for ang in (-0x8000, -0x4000, 0, 0x4000):
            for t in ts[:2] if name in ('d', 'e') else ts:
                try:
                    nb, mn, mx, nv = bake_los.bake_model(fixed_models[name], t, ang)
                    res = '%s %d %s %s' % (sha(nb), nv, ' '.join(repr(x) for x in mn), ' '.join(repr(x) for x in mx))
                except Exception:  # noqa: BLE001
                    res = 'err'
                out.append('    {"%s", %d, {%sf, %sf, %sf}, "%s"},' % (name, ang, repr(t[0]), repr(t[1]), repr(t[2]),
                                                                        res))
    out.append('};')
    # BTIs: header-only + GTX, header-only without GTX, with pixels already
    out.append('struct BtiVec { const char* bti; const char* gtx; const char* out; const char* report; int nreport; };')
    btis = []
    g1 = make_gtx(rng, [(0x31, 8, 8, 2)], gtx2img)
    h = bytearray(0x20)
    struct.pack_into('>BBHH', h, 0, 14, 0, 8, 8)
    h[0x14] = 5
    h[0x18] = 2
    btis.append((bytes(h), g1))
    btis.append((bytes(h), None))
    full = bytearray(h)
    struct.pack_into('>I', full, 0x1C, 0x20)
    full += rng.bytes(64)  # tex_size(CMPR, 8, 8, 2) = 64: "already has pixels"
    btis.append((bytes(full), g1))
    for i, (b, g) in enumerate(btis):
        out += cxx_bytes('kBti_%d' % i, b)
    out += cxx_bytes('kBtiGtx', g1)
    out.append('const BtiVec kBtiVecs[] = {')
    for i, (b, g) in enumerate(btis):
        rep = []
        nb = hd2gc.convert_bti(b, g, 1, rep, '')
        out.append('    {kBti_%d, %s, "%s", "%s", %d},' % (i, 'kBtiGtx' if g else 'nullptr', sha(nb),
                                                         sha('\n'.join(rep).encode()), len(rep)))
    out.append('};')
    # an archive with models A and C (+ an animation and a BTI) for arc_stats
    arc_entries = [('bmdr/a.bmd', fixed_models['a']), ('bmdr/c.bdl', fixed_models['c']),
                   ('btk/x.btk', b'J3D1btk1' + b'\0' * 24), ('timg/t.bti', btis[2][0]), ('dzb/x.dzb', b'\0' * 16)]
    arcb = rarc_write.build('arc', arc_entries)
    s = memfit.arc_stats(arcb)
    out.append('// arc_stats of rarc_write.build("arc", [bmdr/a.bmd = fixed a, bmdr/c.bdl = fixed c, btk/x.btk = '
               '"J3D1btk1" + 24 zeros, timg/t.bti = kBti_2, dzb/x.dzb = 16 zeros])')
    out.append('const char* const kArcSha = "%s";' % sha(arcb))
    out.append('const char* const kArcStats = "%d %d %d %d %d %d %d %d %d";' % (
        s['arc'], s['models'], s['mat'], s['shp'], s['jnt'], s['tex'], s['anm'], s['files'], s['dl']))
    # convert_members / convert_arc on a synthetic archive "hd" (+ a TMPK whose first match for a.bmd is a decoy)
    import convert  # noqa: F401  (pipeline/convert.py; its member loop is convert_members above)
    hd_entries = [('bmdr/a.bmd', cases[0][1]), ('bmdr/b.bdl', cases[1][1]), ('bmdr/c.bmd', cases[2][1]),
                  ('timg/t.bti', btis[0][0]), ('timg/u.bti', btis[2][0]), ('timg/v.bti', btis[1][0]),
                  ('dzr/x.dzr', b'junk' * 4), ('bck/w.bck', b'J3D1bck1' + b'\0' * 24)]
    hd_arc = rarc_write.build('hd', hd_entries)
    pack = make_tmpk([('zz/HD/BMDR/A.BMD.gtx', cases[1][2]), ('content/hd/bmdr/a.bmd.gtx', cases[0][2]),
                      ('x/hd/bmdr/b.bdl.gtx', cases[1][2]), ('x/hd/timg/t.bti.gtx', g1),
                      ('x/hd/timg/u.bti.gtx', g1), ('x/hd/bmdr/c.bmd.gtx.bak', cases[0][2])])
    import rarc
    import rarcx
    a = rarc.Rarc(hd_arc, 'x')
    import hdpack
    tm = hdpack.Tmpk(pack)
    repl, st, rep = convert_members(a, tm, 'x.arc', [], 0)
    out.append('// rarc_write.build("hd", [bmdr/a.bmd = model a, bmdr/b.bdl = model b, bmdr/c.bmd = model c, '
               'timg/t.bti = kBti_0, timg/u.bti = kBti_2, timg/v.bti = kBti_1, dzr/x.dzr = "junk" x 4, '
               'bck/w.bck = "J3D1bck1" + 24 zeros])')
    out.append('const char* const kHdArcSha = "%s";' % sha(hd_arc))
    out.append('// make_tmpk([zz/HD/BMDR/A.BMD.gtx = gtx b, content/hd/bmdr/a.bmd.gtx = gtx a, x/hd/bmdr/b.bdl.gtx = '
               'gtx b, x/hd/timg/t.bti.gtx = kBtiGtx, x/hd/timg/u.bti.gtx = kBtiGtx, x/hd/bmdr/c.bmd.gtx.bak = gtx a])')
    out.append('const char* const kPackSha = "%s";' % sha(pack))
    out.append('const char* const kMembersStats = "%d %d %d %d %d %d";' % (
        st['models'], st['mat_fixed'], st['textures'], st['no_source'], st['bti'], st['mdl3']))
    out.append('const char* const kMembersReport = "%s";  // sha256 of the %d lines' % (
        sha('\n'.join(rep).encode()), len(rep)))
    out.append('const int kMembersReportLines = %d;' % len(rep))
    out.append('const struct { const char* path; const char* sha; } kMembersOut[] = {')
    for k, v in repl.items():
        out.append('    {"%s", "%s"},' % (k, sha(v)))
    out.append('};')
    gc_layout = rarc_write.build_from_rarc(a, repl)
    same = rarcx.rebuild_same_layout(a, repl)
    s1, s2 = memfit.arc_stats(gc_layout), memfit.arc_stats(same)
    fmt9 = '%d %d %d %d %d %d %d %d %d'
    out.append('const char* const kConvArcGc = "%s %s";' % (sha(gc_layout), fmt9 % tuple(
        s1[k] for k in ('arc', 'models', 'mat', 'shp', 'jnt', 'tex', 'anm', 'files', 'dl'))))
    out.append('const char* const kConvArcSame = "%s %s";' % (sha(same), fmt9 % tuple(
        s2[k] for k in ('arc', 'models', 'mat', 'shp', 'jnt', 'tex', 'anm', 'files', 'dl'))))
    # convert_arc with a bake: archive "rm" with model f only (no pack), angle -0x8000, T = (123.5, -0.25, 77.75)
    rm_arc = rarc_write.build('rm', [('bmdr/model.bmd', cases[5][1]), ('dzr/room.dzr', b'room' * 2)])
    a2 = rarc.Rarc(rm_arc, 'x')
    repl2, st2, rep2 = convert_members(a2, None, 'R07_00.arc', [], 0)
    for k in list(repl2):
        if k.lower().endswith(('.bmd', '.bdl')):
            repl2[k] = bake_los.bake_model(repl2[k], (123.5, -0.25, 77.75), -0x8000)[0]
    b2 = rarc_write.build_from_rarc(a2, repl2)
    s3 = memfit.arc_stats(b2)
    out.append('// rarc_write.build("rm", [bmdr/model.bmd = model f, dzr/room.dzr = "room" x 2]), converted, baked '
               '(-0x8000, T = 123.5 -0.25 77.75), GC layout')
    out.append('const char* const kRmArcSha = "%s";' % sha(rm_arc))
    out.append('const char* const kConvArcBaked = "%s %s";' % (sha(b2), fmt9 % tuple(
        s3[k] for k in ('arc', 'models', 'mat', 'shp', 'jnt', 'tex', 'anm', 'files', 'dl'))))
    out.append('// angle table: round(cos(a), 9), round(sin(a), 9) as IEEE bits')
    out.append('const struct { int angle; unsigned long long c, s; } kAngleBits[] = {')
    for ang in ANGLES:
        a = ang * math.pi / 32768.0
        c, s_ = round(math.cos(a), 9), round(math.sin(a), 9)
        out.append('    {%d, 0x%sull, 0x%sull},' % (ang, dbits(c), dbits(s_)))
    out.append('};')
    out.append('// ---- end generated ----')
    print('\n'.join(out))
    return 0


def main(argv):
    args = {}
    i = 1
    while i < len(argv):
        if argv[i] in ('-h', '--help'):
            print(__doc__)
            return 0
        if argv[i] == '--vectors':
            return vectors(os.path.abspath(argv[i + 1]) if i + 1 < len(argv) else os.path.dirname(HERE))
        if argv[i].startswith('--') and i + 1 < len(argv):
            args[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            raise SystemExit('golden_j3d.py: unexpected argument %r' % argv[i])
    for k in ('hd', 'oracle', 'out'):
        if k not in args:
            raise SystemExit('golden_j3d.py: --%s is required' % k)
    return real(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
