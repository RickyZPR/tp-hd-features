#!/usr/bin/env python3
"""golden_tex.py - golden list for `hdf-convert test tex` (module T, plan 3.3), made by the Python pipeline (the
executable spec) from YOUR OWN game files and the pinned oracle.

The list holds hashes, sizes, numbers, input/output file paths and indices only: no game bytes and no names from
inside the files (archive members and pack members are given by their index). It is still made from your games, so
it goes to a build-*/ folder of the project or a scratch folder, never into the source tree, and is never committed
or shared.

USAGE (always with -I -B: no .pyc is written into tools/)
  python -I -B tools/dev/golden_tex.py --hd <TP HD title or content folder> --gc <GC 'files' folder>
                                       --oracle <hd_features-oracle-2.0.0.dusk> --out <build dir>/golden/tex.tsv
                                       [--tools <project>/tools] [--jobs N]

  --tools  the folder holding formats/, pipeline/, gen_stamp_table.py (default: the tools/ folder this script is in)

  python -I -B tools/dev/golden_tex.py --vectors [<project>/tools]
           no game files: prints the unit-test table of tests/tex_test.cpp (SHA-256 of the Python spec's output on
           synthetic inputs made by a small LCG that the C++ test repeats).

LINES (tab-separated; idx = walk index in rarc.Rarc.files() or TMPK entry index; -1 = a bare Gfx2 file)
  surf   <path> <tmpk idx> <surface> <fmt> <w> <h> <mips> <tileMode> <swizzle> <pitch> <compSel hex|-> <img> <mip>
  lvl    <path> <tmpk idx> <surface> <level> <w> <h> <pitch> <tileMode> <linear size> <sha256 deswizzle_level>
         <rgba size> <sha256 to_rgba>
  lvlerr <path> <tmpk idx> <surface> <level> <exception name>
  model  <hd arc> <member idx> <tex idx> <hd pack|-> <tmpk idx> <oracle file> <oracle member idx> <fmt> <w> <h>
         <levels> <pixel size> <sha256 pixels>                      (hd2gc.convert_texture, TEX1 entry tex idx)
  bti    <hd arc> <member idx> <hd pack|-> <tmpk idx> <oracle file> <oracle member idx> <size> <sha256>
                                                                    (hd2gc.convert_bti, whole member)
  ptex   <hd tex idx> <half 0|1> <oracle block idx> <size> <sha256> (particles._convert_hd_texture, TEX1 block)
  ui     <hd arc> <member idx> <hd pack> <tmpk idx> <oracle file> <oracle member idx|-1> <fmt> <rgba sha256>
         <size> <sha256>                                            (stampbook.Source.texture + make_bti)
  icon   <i> <icon rgba sha256> <bc1 sha256> <size> <sha256>        (gen_stamp_table: oracle stampNN.png ->
                                                                     make_icon_rgba -> bti_cmpr = stampNN.bti)
Every converted job is also checked against the oracle here: the script stops if Python != oracle.
"""
import gzip
import hashlib
import importlib.util
import os
import shutil
import struct
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
ICON_ONLY = {'res/object/logous.pack.gz', 'res/object/title.pack.gz'}
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
                raise SystemExit('golden_tex.py: --out must be in a build-*/ folder of the project (not build-hdf) '
                                 'or a scratch folder: the list is made from your games')
    for i in inputs:
        if within(out, i) or within(i, out):
            raise SystemExit('golden_tex.py: --out overlaps the input %s' % i)
    appdata = os.environ.get('APPDATA')
    if appdata and within(out, os.path.join(appdata, 'TwilitRealm')):
        raise SystemExit('golden_tex.py: refusing to write into %APPDATA%\\TwilitRealm')


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


def hd_inputs(tools):
    out = []
    with open(os.path.join(tools, 'dist', 'hd_files.txt'), encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#') and line not in ICON_ONLY:
                out.append(line)
    return out


# ---------------------------------------------------------------------------------------------- detile
def detile_one(job):
    tools, root, path = job
    setup(tools)
    import hdpack
    import gtx2img
    with open(find_ci(root, path), 'rb') as f:
        raw = f.read()
    d = gzip.decompress(raw) if raw[:2] == b'\x1f\x8b' else raw
    members = []
    if d[:4] == b'TMPK':
        t = hdpack.Tmpk(d)
        for i, e in enumerate(t.entries):
            g = t.data(e)
            if g[:4] == b'Gfx2':
                members.append((i, g))
    elif d[:4] == b'Gfx2':
        members.append((-1, d))
    rows = []
    for idx, g in members:
        G = hdpack.gtx_surfaces(g)
        for si, s in enumerate(G['surfaces']):
            rows.append(('surf', path, idx, si, s['fmt'], s['w'], s['h'], s['mips'], s['tileMode'], s['swizzle'],
                         s['pitch'], s['compSel'] or '-', s['img'], s['mip']))
            for L in range(max(1, s['mips'])):
                try:
                    li = gtx2img.level_info(s, L)
                    w, h, lin = gtx2img.deswizzle_level(s, g, L)
                    s2 = dict(s)
                    s2['w'], s2['h'] = w, h
                    try:
                        rgba = gtx2img.to_rgba(s2, lin)
                        rs, rh = len(rgba), sha(rgba)
                    except Exception as ex:  # noqa: BLE001
                        rs, rh = -1, type(ex).__name__
                    rows.append(('lvl', path, idx, si, L, w, h, li['pitch'], li['tileMode'], len(lin), sha(lin), rs, rh))
                except Exception as ex:  # noqa: BLE001
                    rows.append(('lvlerr', path, idx, si, L, type(ex).__name__))
    return rows


# ---------------------------------------------------------------------------------------------- models + BTIs
def files_index(a, path):
    low = path.lower()
    for i, m in enumerate(a.files()):
        if m.path.lower() == low:
            return i, m
    return None, None


def model_rows(hd, oz, rows, counts):
    import rarc
    import hdpack
    import hd2gc
    import j3d
    names = sorted(n for n in oz.namelist() if n.lower().endswith('.arc') and
                   (n.startswith('overlay/res/Stage/') or n.startswith('overlay/res/Object/')))
    for on in names:
        hd_rel = on[len('overlay/'):]
        p = find_ci(hd, hd_rel)
        if p is None:
            raise SystemExit('golden_tex.py: no HD source for %s' % on)
        hd_rel = os.path.relpath(p, hd).replace(os.sep, '/')
        pack = p[:-4] + '.pack.gz'
        tm = hdpack.Tmpk(hdpack.read_any(pack)) if os.path.exists(pack) else None
        pack_rel = os.path.relpath(find_ci(hd, hd_rel[:-4] + '.pack.gz'), hd).replace(os.sep, '/') if tm else '-'
        a = rarc.Rarc.from_file(p)
        oa = rarc.Rarc(oz.read(on), on)
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
            omi, om = files_index(oa, m.path)
            if om is None:
                raise SystemExit('golden_tex.py: %s: member %d not in the oracle' % (on, mi))
            od = om.data()
            if ln.endswith('.bti'):
                out = hd2gc.convert_bti(d, gtx, 1, [], 'x')
                if out != od:
                    raise SystemExit('golden_tex.py: %s member %d: convert_bti != oracle' % (on, mi))
                rows.append(('bti', hd_rel, mi, pack_rel, gi, on, omi, len(out), sha(out)))
                counts['bti'] += 1
                continue
            g = hdpack.gtx_surfaces(gtx)['surfaces'] if gtx else []
            _, _, secs = j3d.sections(d)
            _, _, osecs = j3d.sections(od)
            t1 = [(o, s) for tag, o, s in secs if tag == 'TEX1']
            ot1 = [(o, s) for tag, o, s in osecs if tag == 'TEX1']
            if not t1:
                continue
            o, _ = t1[0]
            oo, _ = ot1[0]
            n = struct.unpack_from('>H', d, o + 8)[0]
            hdr_off = struct.unpack_from('>I', d, o + 0xC)[0]
            ohdr_off = struct.unpack_from('>I', od, oo + 0xC)[0]
            counts['models'] += 1
            for i in range(n):
                if i >= len(g):
                    counts['no_source'] += 1
                    continue
                ho = o + hdr_off + i * 0x20
                h = j3d.bti_header(d, ho)
                f2, w2, h2, nm, px = hd2gc.convert_texture(h, gtx, g[i], 1, [], 'x')
                eo = oo + ohdr_off + i * 0x20
                rel = struct.unpack_from('>I', od, eo + 0x1C)[0]
                want = hd2gc.patch_header(d[ho:ho + 0x20], f2, w2, h2, nm, rel if px else 0)
                if od[eo:eo + 0x20] != want or od[eo + rel:eo + rel + len(px)] != px:
                    raise SystemExit('golden_tex.py: %s member %d TEX1[%d]: convert_texture != oracle' % (on, mi, i))
                rows.append(('model', hd_rel, mi, i, pack_rel, gi, on, omi, f2, w2, h2, nm, len(px), sha(px)))
                counts['model'] += 1


# ---------------------------------------------------------------------------------------------- particles
def particle_rows(hd, gc, oz, rows, counts):
    import hdpack
    import particles
    import facts
    hd206 = open(find_ci(hd, 'res/Particle/Pscene206.jpc'), 'rb').read()
    gc200 = open(find_ci(gc, 'res/Particle/Pscene200.jpc'), 'rb').read()
    gtx = hdpack.read_any(find_ci(hd, 'tex/Particle/Pscene206.jpc.gtx.gz'))
    universe = {r['id'] for r in particles.parse(hd206)['res']}
    need = set(facts.CAVE_PARTICLE_IDS) & universe
    budget = particles.gc_budget(gc)[0]
    data, _, stats = particles.build_scene(gc200, hd206, gtx, need, budget)
    od = oz.read('overlay/res/Particle/Pscene206.jpc')
    if data != od:
        raise SystemExit('golden_tex.py: particles.build_scene != oracle Pscene206.jpc')
    O, H, G = particles.parse(od), particles.parse(hd206), particles.parse(gc200)
    gnames = {t['name'] for t in G['tex']}
    hd_index = {t['name']: i for i, t in enumerate(H['tex'])}
    surfaces = hdpack.gtx_surfaces(gtx)['surfaces']
    for j, t in enumerate(O['tex']):
        if t['name'] in gnames:
            continue
        i = hd_index[t['name']]
        half = t['name'] in stats['halved']
        blk = particles._convert_hd_texture(H['tex'][i], gtx, surfaces[i], half, [])
        if blk != t['raw']:
            raise SystemExit('golden_tex.py: particle texture %d != oracle block %d' % (i, j))
        rows.append(('ptex', i, int(half), j, len(blk), sha(blk)))
        counts['ptex'] += 1
        counts['ptex_half'] += int(half)


# ---------------------------------------------------------------------------------------------- UI BTIs
def ui_rows(hd, oz, rows, counts, work):
    import rarc
    import stampbook
    import cosconfirm
    calls = []
    orig_texture = stampbook.Source.texture
    orig_make = stampbook.make_bti

    def texture(self, member_path):
        t = orig_texture(self, member_path)
        mi, _ = files_index(self.arc, self.member(member_path).path)
        suffix = '/' + member_path.lower() + '.gtx'
        gi = -1
        for k, e in self._gtx.items():
            if k.endswith(suffix):
                gi = self.pack.entries.index(e)
                break
        t['_src'] = (os.path.relpath(self.arc_path, hd).replace(os.sep, '/'), mi,
                     os.path.relpath(self.pack_path, hd).replace(os.sep, '/'), gi)
        return t

    def make_bti(t):
        r = orig_make(t)
        calls.append((phase[0], t['_src'], r[0], r[1]['fmt'], sha(t['rgba'])))
        return r

    phase = ['book']
    stampbook.Source.texture = texture
    stampbook.make_bti = make_bti
    try:
        log = lambda *a: None  # noqa: E731
        stampbook.build(hd, os.path.join(work, 'book'), log)
        phase[0] = 'confirm'
        cosconfirm.build(hd, os.path.join(work, 'confirm'), log)
    finally:
        stampbook.Source.texture = orig_texture
        stampbook.make_bti = orig_make
    places = {'book': [], 'confirm': []}
    for on, ph in (('res/stamps/stampbook/stampResR_gc.arc', 'book'), ('res/cave/confirm/cosconfirm_gc.arc', 'confirm')):
        oa = rarc.Rarc(oz.read(on), on)
        for mi, m in enumerate(oa.files()):
            places[ph].append((on, mi, m.data()))
    for fn in ('stampicon.bti', 'stampbutton.bti', 'stampbuticon.bti', 'filesel_stampicon.bti'):
        on = 'res/stamps/stampbook/' + fn
        places['book'].append((on, -1, oz.read(on)))
    used = set()
    for ph, src, data, fmt, rsha in calls:
        hit = None
        for k, (on, mi, d) in enumerate(places[ph]):
            if (ph, k) not in used and d == data:
                hit = (on, mi)
                used.add((ph, k))
                break
        if hit is None:
            raise SystemExit('golden_tex.py: a %s make_bti output is not in the oracle' % ph)
        arc_rel, mi, pack_rel, gi = src
        rows.append(('ui', arc_rel, mi, pack_rel, gi, hit[0], hit[1], fmt, rsha, len(data), sha(data)))
        counts['ui_loose' if hit[1] < 0 else 'ui_' + ph] += 1


# ---------------------------------------------------------------------------------------------- icons
def icon_rows(tools, oz, rows, counts):
    import gxtex  # noqa: F401  (gen_stamp_table.bti_cmpr imports it)
    spec = importlib.util.spec_from_file_location('gen_stamp_table', os.path.join(tools, 'gen_stamp_table.py'))
    gst = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gst)
    for i in range(50):
        png = oz.read('res/stamps/stamps/stamp%02d.png' % i)
        want = oz.read('res/stamps/stamp_icons/stamp%02d.bti' % i)
        w, h, rgba = gst.png_decode(png)
        icon = gst.make_icon_rgba(w, h, rgba)
        bc1 = gst.bc1_encode(icon, 48, 48)
        out = gst.bti_cmpr(icon, 48)
        if out != want:
            raise SystemExit('golden_tex.py: icon %d != oracle' % i)
        rows.append(('icon', i, sha(icon), sha(bc1), len(out), sha(out)))
        counts['icon'] += 1


# ---------------------------------------------------------------------------------------------- unit vectors
# `golden_tex.py --vectors` (no game files): SHA-256 of the Python spec's output on synthetic, LCG-made inputs. The
# printed C++ table is pasted into mods/hdfeatures/convert/tests/tex_test.cpp, which rebuilds the same inputs.
class Rng(object):
    def __init__(self, seed):
        self.x = seed & 0x7fffffff

    def byte(self):
        self.x = (self.x * 1103515245 + 12345) & 0x7fffffff
        return (self.x >> 16) & 255

    def bytes(self, n):
        return bytes(self.byte() for _ in range(n))


def _ru(v, a):
    return (v + a - 1) // a * a


def _greyish(rgba):
    """every even pixel grey (r = g = b), so the encoders' intensity branches run too"""
    b = bytearray(rgba)
    for i in range(0, len(b) // 4, 2):
        b[4 * i + 1] = b[4 * i + 2] = b[4 * i]
    return bytes(b)


def make_gtx(rng, fmt, w, h, mips, tm, swizzle, comp, gtx2img):
    """A synthetic Gfx2 file: one surface (0x9C block with compSel at +0x84), its image and mip blocks."""
    bpp = gtx2img.FMT_BPP[fmt]
    bc = fmt in gtx2img.BCN
    ew, eh = ((w + 3) // 4, (h + 3) // 4) if bc else (w, h)
    if tm >= 4:
        pitch = _ru(ew, max(32, 32 * (256 // bpp // 8)))
    else:
        pitch = _ru(ew, max(8, 256 // bpp))
    img_size = pitch * _ru(eh, 32) * bpp // 8
    s = {'fmt': fmt, 'w': w, 'h': h, 'mips': mips, 'tileMode': tm, 'swizzle': swizzle, 'pitch': pitch,
         'img_off': 0, 'img': img_size, 'mip_off': 0, 'mipOffsets': [0] * 13}
    offs = [0] * 13
    total = 0
    for L in range(1, mips):
        if L >= 2:
            offs[L - 1] = total
        s['mipOffsets'] = offs
        total += gtx2img.level_info(s, L)['data_size']
    surf = bytearray(0x9C)
    struct.pack_into('>16I', surf, 0, 1, w, h, 1, mips, fmt, 0, 1, img_size, 0, total, 0, tm, swizzle, 0x200, pitch)
    struct.pack_into('>13I', surf, 0x40, *offs)
    surf[0x84:0x88] = bytes.fromhex(comp)

    def blk(t, data):
        return b'BLK{' + struct.pack('>7I', 0x20, 1, 0, t, len(data), 0, 0) + data
    out = b'Gfx2' + struct.pack('>5I', 0x20, 7, 1, 2, 0) + b'\0' * 8
    out += blk(0x0B, bytes(surf)) + blk(0x0C, rng.bytes(img_size))
    if mips > 1:
        out += blk(0x0D, rng.bytes(total))
    return out + blk(0x01, b'')


def vectors(tools):
    setup(tools)
    import hdpack
    import gtx2img
    import gxtex
    import hd2gc
    import stampbook as sb
    spec = importlib.util.spec_from_file_location('gen_stamp_table', os.path.join(tools, 'gen_stamp_table.py'))
    gst = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gst)
    out = []

    def put(name, value):
        out.append('    {"%s", "%s"},' % (name, value))
    fmts = [0x01, 0x02, 0x07, 0x08, 0x1A, 0x31]
    # A. deswizzle: per (format, tile mode), all sizes and swizzles concatenated
    for fmt in fmts:
        bpp = gtx2img.FMT_BPP[fmt]
        for tm in (1, 2, 4, 5, 6):
            acc = b''
            for (w, h) in ((37, 21), (64, 64), (130, 70)):
                for swz in ((0, 0x100, 0x600, 0x700) if tm >= 4 else (0,)):
                    bc = fmt in gtx2img.BCN
                    ew, eh = ((w + 3) // 4, (h + 3) // 4) if bc else (w, h)
                    pitch = (_ru(ew, max(32, 32 * (256 // bpp // 8))) if tm >= 4 else
                             _ru(ew, max(8, 256 // bpp)) if tm == 2 else _ru(ew, 16))
                    rng = Rng(fmt * 1000 + w * 7 + h * 13 + tm * 31 + swz)
                    data = rng.bytes(pitch * _ru(eh, 64) * (bpp // 8) + 4096)
                    s = {'fmt': fmt, 'w': w, 'h': h, 'pitch': pitch, 'tileMode': tm, 'swizzle': swz}
                    lin = gtx2img.deswizzle(s, data)
                    assert len(lin) == ew * eh * bpp // 8
                    acc += lin
            put('deswizzle/%x/%d' % (fmt, tm), sha(acc))
    # B. to_rgba: per format, the 5 compSel values concatenated (13 x 9)
    for fmt in fmts:
        acc = b''
        for comp in ('00010203', '00000001', '00000000', '00010205', '05040302'):
            bpp = gtx2img.FMT_BPP[fmt]
            n = (4 * 3 * 8) if fmt == 0x31 else 13 * 9 * bpp // 8
            lin = Rng(fmt + int(comp, 16)).bytes(n)
            acc += gtx2img.to_rgba({'fmt': fmt, 'w': 13, 'h': 9, 'compSel': comp}, lin)
        put('to_rgba/%x' % fmt, sha(acc))
    # C. gxtex.encode / decode (13 x 9 and 16 x 8, half the pixels grey)
    imgs = [(13, 9, _greyish(Rng(11).bytes(13 * 9 * 4))), (16, 8, _greyish(Rng(12).bytes(16 * 8 * 4)))]
    for f in (0, 1, 2, 3, 4, 5, 6):
        enc = [gxtex.encode(f, rgba, w, h) for w, h, rgba in imgs]
        put('gx_encode/%d' % f, sha(b''.join(enc)))
        put('gx_decode/%d' % f, sha(b''.join(gxtex.decode(f, e, w, h) for e, (w, h, _) in zip(enc, imgs))))
    put('gx_decode/14', sha(gxtex.decode(14, Rng(13).bytes(128), 16, 16) + gxtex.decode(14, Rng(14).bytes(128), 12, 10)))
    put('bc1_to_cmpr', sha(b''.join(gxtex.bc1_to_cmpr(Rng(15 + w).bytes(((w + 3) // 4) * ((h + 3) // 4) * 8), w, h)
                                    for w, h in ((13, 9), (16, 16), (20, 4)))))
    put('box_downscale', sha(gxtex.box_downscale(Rng(16).bytes(16 * 12 * 4), 16, 12, 8, 6) +
                             gxtex.box_downscale(Rng(17).bytes(10 * 10 * 4), 10, 10, 3, 3)))
    # D. stampbook encode / decode
    for f in (0, 1, 2, 3, 4, 5, 6):
        enc = [sb.encode(f, rgba, w, h) for w, h, rgba in imgs]
        put('ui_encode/%d' % f, sha(b''.join(enc)))
        put('ui_decode/%d' % f, sha(b''.join(sb.decode(f, e, w, h) for e, (w, h, _) in zip(enc, imgs))))
    # E. choose_format + make_bti on constructed images (12 x 10)
    n = 120
    rnd = Rng(21).bytes(n * 4)
    cases = []
    cases.append(('i4', bytes(v for i in range(n) for v in ((i % 16) * 17,) * 4)))
    cases.append(('i8', bytes(v for i in range(n) for v in (rnd[4 * i],) * 4)))
    cases.append(('ia4', bytes(v for i in range(n) for v in ((i % 16) * 17,) * 3 + (((i * 7) % 16) * 17,))))
    cases.append(('ia8', bytes(v for i in range(n) for v in (rnd[4 * i],) * 3 + (rnd[4 * i + 1],))))
    cases.append(('ink', bytes(v for i in range(n) for v in ((0x30, 0x80, 0xC8, 255) if i % 3 else (rnd[4 * i], 1, 2, 0)))))
    cases.append(('ink_ok', bytes(v for i in range(n) for v in ((0x31, 0x84, 0xC6, 255) if i % 3 else (rnd[4 * i], rnd[4 * i + 1], 2, 0)))))
    cases.append(('ink_far', bytes(v for i in range(n) for v in ((4, 8, 16, 255) if i % 2 else (0, 0, 0, 0)))))
    cases.append(('565', bytes(v for i in range(n) for v in (sb.E5[i % 32], sb.E6[(i * 3) % 64], sb.E5[(i * 5) % 32], 255))))
    cases.append(('rgba8', rnd))
    for k, (name, rgba) in enumerate(cases):
        for hd_fmt in (5, 9):
            stub = bytearray(Rng(30 + k).bytes(0x20))
            stub[0] = hd_fmt
            stub[0x14] = 5
            t = {'hdr': sb.bti_fields(bytes(stub)), 'rgba': rgba, 'w': 12, 'h': 10, 'stub': bytes(stub), 'gtx_fmt': 'x'}
            data, info, back = sb.make_bti(t)
            put('make_bti/%s/%d' % (name, hd_fmt), '%s|%d|%d|%s|%s' % (info['fmt'], info['max_err_visible'],
                                                                    info['max_err_transparent'], sha(data), sha(back)))
            if hd_fmt == 5:
                put('choose/%s' % name, '%d|%s' % sb.choose_format(rgba, hd_fmt))
    # F. icons
    for k, (w, h) in enumerate(((100, 100), (37, 50), (200, 120))):
        rgba = bytearray(Rng(40 + k).bytes(w * h * 4))
        for i in range(w * h):  # alpha: 0, 255 or random
            rgba[4 * i + 3] = (0, 255, rgba[4 * i + 3])[i % 3]
        icon = gst.make_icon_rgba(w, h, bytes(rgba))
        put('icon/%d' % k, '%s|%s|%s' % (sha(icon), sha(gst.bc1_encode(icon, 48, 48)), sha(gst.bti_cmpr(icon, 48))))
    # G. patch_header
    for k, (fmt, nm) in enumerate(((5, 1), (9, 1), (14, 3), (0, 4))):
        raw = bytearray(Rng(50 + k).bytes(0x20))
        raw[0x14] = 4
        put('patch_header/%d' % k, hd2gc.patch_header(bytes(raw), fmt, 64 >> k, 32 >> k, nm, 0x60 + k).hex())
    # H. convert_texture on synthetic GTX files
    gcases = [(0x31, 256, 128, 5, 4, 0x00030000, '00010203', (14, 128, 64, 3)),
              (0x1A, 64, 64, 4, 4, 0x00020000, '00010203', (6, 32, 32, 2)),
              (0x01, 100, 60, 3, 4, 0x00010000, '00000000', (1, 100, 60, 3)),
              (0x01, 100, 60, 3, 4, 0x00010000, '00000000', (14, 50, 30, 2)),
              (0x02, 50, 30, 2, 2, 0x00000000, '00000001', (2, 50, 30, 2)),
              (0x07, 40, 40, 1, 4, 0x00000000, '00000001', (9, 40, 40, 1)),
              (0x08, 32, 16, 2, 4, 0x00010000, '00010205', (5, 32, 16, 4))]
    for k, (fmt, w, h, mips, tm, swz, comp, (hf, hw, hh, hm)) in enumerate(gcases):
        g = make_gtx(Rng(60 + k), fmt, w, h, mips, tm, swz, comp, gtx2img)
        surf = hdpack.gtx_surfaces(g)['surfaces'][0]
        f2, w2, h2, nm, px = hd2gc.convert_texture({'fmt': hf, 'w': hw, 'h': hh, 'mips': hm}, g, surf, 1, [], 'x')
        put('convert/%d' % k, '%d|%d|%d|%d|%s|%s' % (f2, w2, h2, nm, sha(g), sha(px)))
        bti = bytearray(Rng(70 + k).bytes(0x20))
        bti[0], bti[0x18] = hf, hm
        struct.pack_into('>HH', bti, 2, hw, hh)
        struct.pack_into('>I', bti, 0x1C, 0)
        put('convert_bti/%d' % k, sha(hd2gc.convert_bti(bytes(bti), g, 1, [], 'x')))
        # stampbook.Source.texture after its lookup: surface 0, level 0
        w0, h0, lin = gtx2img.deswizzle_level(surf, g, 0)
        s2 = dict(surf)
        s2['w'], s2['h'] = w0, h0
        t = {'stub': bytes(bti), 'hdr': sb.bti_fields(bytes(bti)), 'w': w0, 'h': h0,
             'rgba': gtx2img.to_rgba(s2, lin), 'gtx_fmt': 'x'}
        data, info, back = sb.make_bti(t)
        put('bti_job/%d' % k, '%s|%s' % (sha(t['rgba']), sha(data)))
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
            raise SystemExit('golden_tex.py: unexpected argument %r' % argv[i])
    for k in ('hd', 'gc', 'oracle', 'out'):
        if k not in args:
            raise SystemExit('golden_tex.py: --%s is required' % k)
    tools = os.path.abspath(args.get('tools') or os.path.dirname(HERE))
    hd, gc, out = args['hd'], args['gc'], args['out']
    if os.path.isdir(os.path.join(hd, 'content')):
        hd = os.path.join(hd, 'content')
    check_out(out, [args['hd'], gc, args['oracle']], tools)
    # wave2_T_verify L4: make the --out folder first; the UI part's work folder (mkdtemp) is made inside it
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    setup(tools)
    import common  # noqa: F401  (pipeline path setup, as build_assets does)
    rows, counts = [], {}
    for k in ('surf', 'lvl', 'lvlerr', 'model', 'models', 'no_source', 'bti', 'ptex', 'ptex_half', 'ui_book',
              'ui_confirm', 'ui_loose', 'icon'):
        counts[k] = 0
    paths = [p for p in hd_inputs(tools) if p.endswith('.gz')]
    jobs = [(tools, hd, p) for p in paths]
    n = int(args.get('jobs', '8'))
    if n > 1:
        import multiprocessing
        with multiprocessing.Pool(n, initializer=setup, initargs=(tools,)) as pool:
            results = pool.map(detile_one, jobs, chunksize=1)
    else:
        results = [detile_one(j) for j in jobs]
    for r in results:
        for row in r:
            rows.append(row)
            counts[row[0]] += 1
    oz = zipfile.ZipFile(args['oracle'])
    model_rows(hd, oz, rows, counts)
    particle_rows(hd, gc, oz, rows, counts)
    work = tempfile.mkdtemp(prefix='golden_tex_', dir=os.path.dirname(os.path.abspath(out)))
    try:
        ui_rows(hd, oz, rows, counts, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    icon_rows(tools, oz, rows, counts)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# hdf golden tex 1: made by tools/dev/golden_tex.py from your own game files (hashes, numbers and '
                'indices only; never commit or share)\n')
        f.write('# counts %s\n' % ' '.join('%s=%d' % kv for kv in counts.items()))
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print('golden tex: %d lines -> %s (%s)' % (len(rows), out, ', '.join('%s %d' % kv for kv in counts.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
