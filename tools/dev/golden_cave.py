#!/usr/bin/env python3
"""golden_cave.py - golden list for `hdf-convert test cave` (module CV, plan 3.3), made by the Python pipeline (the
executable spec) from YOUR OWN game files and the pinned oracle.

The list holds hashes, sizes, numbers, output file paths and indices only: no game bytes and no names from inside
the files (report and log lines are given as SHA-256 per line). It is still made from your games, so it goes to a
build-*/ folder of the project or a scratch folder, never into the source tree, and is never committed or shared.

USAGE (always with -I -B: no .pyc is written into tools/)
  python -I -B tools/dev/golden_cave.py --hd <TP HD title or content folder> --gc <extracted GZ2E01 files folder>
                                        --oracle <hd_features-oracle-2.0.0.dusk> --out <build dir>/golden/cave.tsv
                                        [--tools <project>/tools]
  python -I -B tools/dev/golden_cave.py --vectors [<project>/tools]
           no game files: prints the generated block of tests/cave_test.cpp (synthetic JPAC2-10 files, Gfx2 files
           and a BMG archive made here, as hex, and the Python spec's results on them).

What it runs: pipeline/cavebuild.build(hd, gc, out, log) without --src (as the oracle was made, plan 7.2 #3) and the
Obj_Stamp part of pipeline/stampbuild.build (its first 8 lines, the same calls). common.write is captured in memory,
so no file is written. It stops unless each of the 56 overlay files equals the oracle.

LINES (tab-separated)
  out      <rel> <size> <sha256>                    the 56 overlay files (= the oracle, checked here)
  msg      <group> <gc flow count> <sha256 of the sorted flow ids, ',' joined> <Tag_ms flows> <HD-only flows>
  conv     <archive file> <models> <mat_fixed> <textures> <no_source> <bti> <mdl3>   (res['conv'], dict order)
  totals   <models> <mat_fixed> <textures> <no_source> <bti> <baked>
  mem      <old MEM0 hex,...> <new MEM0 hex,...>
  budget   <largest GC Pscene file> <its size> <budget>
  pstats   <need> <resources> <from_gc> <from_hd> <textures> <tex_gc> <tex_hd> <halved> <size> <budget>
           <shared_header_differs> <hd_total> <gc_total>
  phalf    <HD texture index>                       (sorted by index)
  pres     <i> <id hex> <GC|HD> <size> <sha256>     resources of the built Pscene206.jpc, in file order
  ptex     <i> <GC|HD> <size> <sha256>              texture blocks, in file order
  objstamp <stamp.bmd entry index HD> <in the output> <size>
  lines    <report|patch|log|stamplog> <count> <sha256 of '\\n'.join(lines)>
  line     <report|patch|log|stamplog> <i> <sha256 of the line>
The 'log' lines are cavebuild.build's log calls up to the object archives (the res/ part, floors.json,
colossal_wallet.png and hd_tags.bin, is not module CV's). 'stamplog' is stampbuild's Obj_Stamp log line.
"""
import hashlib
import os
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
ORACLE_SHA256 = 'a7a48bf643e621b89e3860fe9d7c48b26e76ec9b6cf518af8c5ff2b42bc68f59'
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
    if isinstance(b, str):
        b = b.encode('utf-8')
    return hashlib.sha256(bytes(b)).hexdigest()


def norm(p):
    return os.path.normcase(os.path.realpath(os.path.abspath(p)))


def within(child, parent):
    c, p = norm(child), norm(parent)
    return c == p or c.startswith(p.rstrip('\\/') + os.sep)


def check_out(out, inputs, tools):
    for root in {PROJECT, os.path.dirname(tools)}:
        if within(out, root):
            rel = os.path.relpath(norm(out), norm(root)).replace('\\', '/')
            if not rel.split('/')[0].startswith('build-'):
                raise SystemExit('golden_cave.py: --out must be in a build-*/ folder or outside the project: %s' % out)
    for i in inputs:
        if within(out, i):
            raise SystemExit('golden_cave.py: --out inside an input folder: %s' % out)


def content_dir(hd):
    if os.path.isdir(os.path.join(hd, 'content')):
        return os.path.join(hd, 'content')
    return hd


# ---------------------------------------------------------------------------------------------- real
def real(args):
    tools = os.path.abspath(args.get('tools') or os.path.dirname(HERE))
    setup(tools)
    hd = content_dir(os.path.abspath(args['hd']))
    gc = os.path.abspath(args['gc'])
    oracle = os.path.abspath(args['oracle'])
    out = os.path.abspath(args['out'])
    check_out(out, [hd, gc, os.path.dirname(oracle)], tools)
    with open(oracle, 'rb') as f:
        if hashlib.sha256(f.read()).hexdigest() != ORACLE_SHA256:
            raise SystemExit('golden_cave.py: the oracle is not the pinned file')
    z = zipfile.ZipFile(oracle)

    import common
    import cavebuild
    import convert
    import rarc
    import rarcx
    import particles
    import stampbuild

    captured = {}

    def cap_write(path, data):
        captured[os.path.normpath(path)] = bytes(data)
    common.write = cap_write

    logs = []

    def log(msg, *a):
        logs.append(msg % a if a else msg)

    root = os.path.join('X:' + os.sep, 'cave_golden')
    ov = os.path.join(root, 'overlay')
    cave_out = {'stage_dir': os.path.join(ov, 'res', 'Stage', 'D_SB11'), 'obj_dir': os.path.join(ov, 'res', 'Object'),
                'res_dir': os.path.join(root, 'res', 'cave'), 'particle_dir': os.path.join(ov, 'res', 'Particle')}
    res = cavebuild.build(hd, gc, cave_out, log, hd_gates=False, src_root=None)
    ncave_log = len(logs)
    # the res/ part (floors.json, colossal_wallet.png, hd_tags.bin) is not CV's: keep the log lines before it
    cut = [i for i, l in enumerate(logs) if l.startswith('cave: hd_tags.bin')]
    cave_log = logs[:cut[0]] if cut else logs[:]

    # stampbuild.build, part 1 (Obj_Stamp.arc), the same calls
    stamp_log = []
    p = common.exists_ci(os.path.join(hd, 'res', 'Object'), 'Obj_Stamp.arc')
    a, repl, st, rep = convert.convert_arc(p)
    b = rarcx.rebuild_same_layout(a, repl)
    common.write(os.path.join(cave_out['obj_dir'], 'Obj_Stamp.arc'), b)
    idx_hd = rarcx.entry_index(a, 'stamp.bmd')
    idx_out = rarcx.entry_index(rarc.Rarc(b, 'out'), 'stamp.bmd')
    stamp_log.append('stamps: Obj_Stamp.arc %d bytes, stamp.bmd entry index %s (HD %s), %d model(s), MAT repack %d' % (
        len(b), idx_out, idx_hd, st['models'], st['mat_fixed']))

    rows = []
    # the 56 overlay files
    outs = {}
    for path, data in captured.items():
        rel = os.path.relpath(path, root).replace('\\', '/')
        if rel.startswith('overlay/'):
            outs[rel] = data
    bad = 0
    for rel in sorted(outs):
        data = outs[rel]
        try:
            od = z.read(rel)
        except KeyError:
            od = None
        if od != data:
            print('golden_cave.py: %s differs from the oracle' % rel, file=sys.stderr)
            bad += 1
        rows.append(['out', rel, len(data), sha(data)])
    oracle_ov = sorted(n for n in z.namelist() if n.startswith('overlay/') and not n.endswith('/'))
    if sorted(outs) != oracle_ov or len(outs) != 56:
        print('golden_cave.py: overlay set differs from the oracle (%d vs %d)' % (len(outs), len(oracle_ov)),
              file=sys.stderr)
        bad += 1
    if bad:
        raise SystemExit('golden_cave.py: the Python spec does not give the oracle (%d problems)' % bad)

    flows = cavebuild.gc_flow_ids(gc, res['msg_group'])
    rows.append(['msg', res['msg_group'], len(flows), sha(','.join(str(x) for x in sorted(flows))),
                 ','.join(str(x) for x in res['tagms_flows']) or '-', ','.join(str(x) for x in res['drop_tagms']) or '-'])
    for n, s in res['conv'].items():
        rows.append(['conv', n, s['models'], s['mat_fixed'], s['textures'], s['no_source'], s['bti'], s['mdl3']])
    t = res['totals']
    rows.append(['totals', t['models'], t['mat_fixed'], t['textures'], t['no_source'], t['bti'], t['baked']])
    rows.append(['mem', ','.join('0x%x' % x for x in res['mem']['old']), ','.join('0x%x' % x for x in res['mem']['new'])])
    big_name, big_size = res['particles']['budget_ref']
    budget, _, _ = particles.gc_budget(gc)
    rows.append(['budget', big_name, big_size, budget])
    ps = res['particles']['stats']
    rows.append(['pstats', len(res['particles']['need']), ps['resources'], ps['from_gc'], ps['from_hd'], ps['textures'],
                 ps['tex_gc'], ps['tex_hd'], len(ps['halved']), ps['size'], ps['budget'], ps['shared_header_differs'],
                 ps['hd_total'], ps['gc_total']])
    # halved textures by HD index; resources and texture blocks of the built file
    hd206 = common.read(os.path.join(hd, 'res', 'Particle', particles.STAGE_FILE))
    gc200 = common.read(os.path.join(gc, 'res', 'Particle', 'Pscene200.jpc'))
    H = particles.parse(hd206)
    G = particles.parse(gc200)
    hd_index = {tt['name']: i for i, tt in enumerate(H['tex'])}
    for i in sorted(hd_index[n] for n in ps['halved']):
        rows.append(['phalf', i])
    built = particles.parse(outs['overlay/res/Particle/' + particles.STAGE_FILE])
    gids = {r['id'] for r in G['res']}
    gnames = {tt['name'] for tt in G['tex']}
    for i, r in enumerate(built['res']):
        rows.append(['pres', i, '%04x' % r['id'], 'GC' if r['id'] in gids else 'HD', len(r['raw']), sha(r['raw'])])
    for i, tt in enumerate(built['tex']):
        rows.append(['ptex', i, 'GC' if tt['name'] in gnames else 'HD', len(tt['raw']), sha(tt['raw'])])
    rows.append(['objstamp', idx_hd, idx_out, len(b)])
    for kind, lines in (('report', res['report']), ('patch', res['patch_log']), ('log', cave_log),
                        ('stamplog', stamp_log)):
        rows.append(['lines', kind, len(lines), sha('\n'.join(lines))])
        for i, l in enumerate(lines):
            rows.append(['line', kind, i, sha(l)])

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', newline='\n') as f:
        f.write('# golden_cave.py: hashes, sizes, numbers and indices only (never commit). %d rows\n' % len(rows))
        f.write('# counts out=%d report=%d patch=%d log=%d(of %d) particles=%d/%d\n' % (
            len(outs), len(res['report']), len(res['patch_log']), len(cave_log), ncave_log, len(built['res']),
            len(built['tex'])))
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print('golden_cave.py: %d rows -> %s (56/56 overlay files = oracle)' % (len(rows), out))
    return 0


# ---------------------------------------------------------------------------------------------- unit vectors
class Rng(object):
    def __init__(self, seed):
        self.x = seed & 0x7fffffff

    def byte(self):
        self.x = (self.x * 1103515245 + 12345) & 0x7fffffff
        return (self.x >> 16) & 255

    def bytes(self, n):
        return bytes(self.byte() for _ in range(n))


def make_gtx(rng, surfaces, gtx2img):
    """Gfx2 with several surfaces (fmt, w, h, mips), 1D tiled (mode 2), identity compSel (as golden_j3d.py)."""
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


def make_res(rng, rid, tdb, extra=b''):
    """A JPA resource: u16 id, u16 nblk, u8 nfld, u8 nkey, u8 ntdb, u8 pad; blocks BEM1 (random), TDB1, extra."""
    blocks = []
    body = rng.bytes(24)
    blocks.append(b'BEM1' + struct.pack('>I', 8 + len(body)) + body)
    if tdb is not None:
        t = struct.pack('>%dH' % len(tdb), *tdb)
        t += b'\0' * ((-len(t)) % 4)
        blocks.append(b'TDB1' + struct.pack('>I', 8 + len(t)) + t)
    if extra:
        blocks.append(b'ESP1' + struct.pack('>I', 8 + len(extra)) + extra)
    head = struct.pack('>HHBBBB', rid, len(blocks), 1, 0, len(tdb) if tdb else 0, 0)
    return head + b''.join(blocks)


def make_tex(rng, name, fmt, w, h, mips, pixels):
    """TEX1 block: 'TEX1', size, 0, name[20], ResTIMG (img_off 0x20), pixels (as the stored GC blocks)."""
    nm = name.encode('latin-1')[:20]
    hdr = bytearray(0x20)
    struct.pack_into('>BBHHBBBBHI', hdr, 0, fmt, 1, w, h, 1, 1, 0, 0, 0, 0)
    hdr[0x10] = 1 if mips > 1 else 0
    hdr[0x14] = 5 if mips > 1 else 1
    hdr[0x15] = 1
    hdr[0x17] = (mips - 1) * 8
    hdr[0x18] = mips
    struct.pack_into('>I', hdr, 0x1C, 0x20)
    blk = bytearray(b'TEX1' + b'\0' * 8 + nm + b'\0' * (20 - len(nm))) + hdr + pixels
    struct.pack_into('>I', blk, 4, len(blk))
    return bytes(blk)


def make_jpc(resources, textures, texoff_pad=0):
    body = b''.join(resources)
    texoff = (0x10 + len(body) + 0x1F) // 0x20 * 0x20 + texoff_pad
    out = bytearray(b'JPAC2-10' + struct.pack('>HHI', len(resources), len(textures), texoff)) + body
    out += b'\0' * (texoff - len(out))
    for t in textures:
        out += t
    return bytes(out)


def make_bmg_arc(flows_by_section, rarc_write):
    """A BMG ('MESGbmg1') with INF1 and one or more FLI1 sections, inside a RARC (bmgres-style)."""
    secs = [b'INF1' + struct.pack('>I', 0x10) + b'\0' * 8]
    for flows in flows_by_section:
        body = struct.pack('>HH', len(flows), 0) + b'\0' * 4
        for i, f in enumerate(flows):
            body += struct.pack('>IHH', (f << 16) | (i & 0xFFFF), 0, 0)
        body += b'\0' * ((-len(body)) % 16)
        secs.append(b'FLI1' + struct.pack('>I', 8 + len(body)) + body)
    bmg = b'MESGbmg1' + struct.pack('>II', 0x20 + sum(len(s) for s in secs), len(secs)) + b'\x02' + b'\0' * 15
    bmg += b''.join(secs)
    return rarc_write.build('arc', [('zel_00.bmg', bmg), ('other.dat', b'x' * 8)])


def cxx_bytes(name, b):
    lines = ['constexpr const char* %s =' % name]
    h = b.hex()
    if not h:
        lines.append('    ""')
    for i in range(0, len(h), 112):
        lines.append('    "%s"' % h[i:i + 112])
    lines[-1] += ';'
    return lines


def vectors(tools):
    setup(tools)
    import particles
    import gtx2img
    import rarc_write
    import cavebuild
    rng = Rng(0x0CA7E)
    out = ['// BEGIN generated by tools/dev/golden_cave.py --vectors (synthetic data only; the Python spec)']

    # GC "200": 5 textures (CMPR/RGB5A3/I8, random texels), 6 resources (ids 0x8001..), 1 without TDB1
    gtex = [make_tex(rng, 'g_tex%d' % i, f, w, h, m, rng.bytes(sz)) for i, (f, w, h, m, sz) in enumerate(
        [(14, 16, 16, 1, 128), (5, 8, 8, 1, 128), (1, 16, 8, 1, 128), (14, 32, 32, 2, 640), (5, 4, 4, 1, 32)])]
    # shared name with HD: 'shared'
    gtex.append(make_tex(rng, 'shared', 5, 8, 8, 1, rng.bytes(128)))
    # a repeated texture name: {t['name']: t for t in G['tex']} keeps the last one
    gtex.append(make_tex(rng, 'g_tex1', 5, 8, 8, 1, rng.bytes(128)))
    gres = [make_res(rng, 0x8001, [0, 1]), make_res(rng, 0x8002, [2]), make_res(rng, 0x8003, None),
            make_res(rng, 0x8005, [3, 5, 0]), make_res(rng, 0x8007, [4], extra=rng.bytes(12)),
            make_res(rng, 0x8009, []),
            make_res(rng, 0x8005, [6, 4])]           # a repeated id: {r['id']: r for r in G['res']} keeps the last
    gc200 = make_jpc(gres, gtex)
    # HD "206": 7 header-only-ish textures (HD blocks carry GC-size headers + junk pixels) and a Gfx2 with one surface
    # per texture. Formats: CMPR from BC1, CMPR slot from RGBA8 (-> RGB5A3), RGBA8, IA8, palette C8 (-> RGB5A3), I4.
    hd_specs = [('h_big', 14, 64, 64, 1, 0x31, 128, 128, 3), ('h_rgba', 6, 32, 32, 1, 0x1A, 32, 32, 2),
                ('h_cmpr2', 14, 32, 16, 1, 0x1A, 32, 16, 1), ('shared', 5, 8, 8, 1, 0x1A, 8, 8, 1),
                ('h_ia8', 3, 16, 16, 2, 0x1A, 32, 32, 3), ('h_c8', 9, 16, 16, 1, 0x1A, 16, 16, 1),
                ('h_i4', 0, 64, 32, 1, 0x1A, 64, 32, 2),
                ('h_thin', 6, 64, 8, 1, 0x1A, 64, 8, 2),     # same block size as h_big (tie on size); 8 high
                ('h_tall', 6, 8, 64, 1, 0x1A, 8, 64, 2),     # the same, 8 wide
                ('h_c8', 9, 16, 16, 1, 0x1A, 16, 16, 1)]     # a repeated name: hd_index keeps the last (surface 9)
    htex = [make_tex(rng, n, f, w, h, m, rng.bytes(16)) for n, f, w, h, m, _, _, _, _ in hd_specs]
    hgtx = make_gtx(rng, [(gf, gw, gh, gm) for _, _, _, _, _, gf, gw, gh, gm in hd_specs], gtx2img)
    hres = [make_res(rng, 0x8001, [3]),              # also in GC: GC raw wins
            make_res(rng, 0x8002, [1, 2]),           # in GC, header differs from HD's (counts shared_header_differs)
            make_res(rng, 0x8004, [0, 3, 7]),        # HD-only: big BC1, the 'shared' name (taken from GC), h_thin
            make_res(rng, 0x8005, [6]),              # in GC
            make_res(rng, 0x8006, [4, 5, 6, 1, 8]),  # HD-only
            make_res(rng, 0x8008, [2]),              # HD-only, not needed
            make_res(rng, 0x8003, None),             # in GC, no TDB1
            make_res(rng, 0x8009, []),               # in GC, empty TDB
            make_res(rng, 0x8006, [9, 2])]           # a repeated HD-only id: both are kept, in H order
    hd206 = make_jpc(hres, htex, texoff_pad=0x20)
    need = {0x8001, 0x8002, 0x8003, 0x8004, 0x8005, 0x8006, 0x8009, 0x7777}
    out += cxx_bytes('kGc200', gc200)
    out += cxx_bytes('kHd206', hd206)
    out += cxx_bytes('kHdGtx', hgtx)
    out.append('constexpr unsigned short kNeed[] = {%s};' % ', '.join('0x%04X' % x for x in sorted(need)))
    # parse results
    for nm, d in (('Gc', gc200), ('Hd', hd206)):
        p = particles.parse(d)
        out.append('// particles.parse(k%s): resources (id, raw size, sha, tdb_off or -1, tdb) / textures (raw size, '
                   'sha, fmt w h mips img_off) / end' % ('Gc200' if nm == 'Gc' else 'Hd206'))
        out.append('constexpr const char* kParse%s =' % nm)
        parts = []
        for r in p['res']:
            parts.append('r %04x %d %s %d %s' % (r['id'], len(r['raw']), sha(r['raw']),
                                                 -1 if r['tdb_off'] is None else r['tdb_off'],
                                                 ','.join(str(x) for x in r['tdb']) or '-'))
        for t in p['tex']:
            h = t['h']
            parts.append('t %d %s %d %d %d %d %d' % (len(t['raw']), sha(t['raw']), h['fmt'], h['w'], h['h'], h['mips'],
                                                     h['img_off']))
        parts.append('end %d' % p['end'])
        for s in parts:
            out.append('    "%s\\n"' % s)
        out[-1] += ';'
    # build_scene at several budgets: no halving, some halving, impossible (stops when nothing is left to halve)
    full = particles.build_scene(gc200, hd206, hgtx, need, 1 << 30)
    sizes = [1 << 30, full[2]['size'], full[2]['size'] - 1, full[2]['size'] - 2000, full[2]['size'] - 3000,
             full[2]['size'] - 3073, full[2]['size'] - 4000, full[2]['size'] - 5000, 64]
    out.append('struct SceneVec { unsigned long long budget; const char* sha; unsigned size; const char* stats; '
               'const char* report; int nreport; const char* last; };')
    out.append('// stats: resources from_gc from_hd textures tex_gc tex_hd shared_header_differs hd_total gc_total '
               'halved(names, comma) | report: sha256 of "\\n".join(report) | last: sha256 of report[-1]')
    out.append('constexpr SceneVec kSceneVecs[] = {')
    for b in sizes:
        data, rep, st = particles.build_scene(gc200, hd206, hgtx, need, b)
        stats = '%d %d %d %d %d %d %d %d %d %s' % (st['resources'], st['from_gc'], st['from_hd'], st['textures'],
                                                 st['tex_gc'], st['tex_hd'], st['shared_header_differs'],
                                                 st['hd_total'], st['gc_total'], ','.join(st['halved']) or '-')
        out.append('    {%dull, "%s", %d, "%s", "%s", %d, "%s"},' % (b, sha(data), len(data), stats,
                                                                  sha('\n'.join(rep)), len(rep), sha(rep[-1])))
        probs = particles.verify(data, need & {r['id'] for r in particles.parse(hd206)['res']})
        assert not probs, probs
    out.append('};')
    # build() and tex_block() directly
    out += cxx_bytes('kBuildRes1', b'\x80\x01' + b'\0' * 6)
    rb = rng.bytes(13)
    tb = rng.bytes(0x40)
    b1 = particles.build([b'\x80\x01' + b'\0' * 6, rb], [tb])
    out += cxx_bytes('kBuildRes2', rb)
    out += cxx_bytes('kBuildTex', tb)
    out.append('constexpr const char* kBuildSha = "%s";  // particles.build([kBuildRes1, kBuildRes2], [kBuildTex])' %
               sha(b1))
    nh = rng.bytes(0x24)
    ti = rng.bytes(0x20)
    px = rng.bytes(37)
    blk = particles.tex_block(nh, ti, px)
    out += cxx_bytes('kTbName', nh)
    out += cxx_bytes('kTbTimg', ti)
    out += cxx_bytes('kTbPix', px)
    out.append('constexpr const char* kTexBlockSha = "%s";  // particles.tex_block(kTbName, kTbTimg, kTbPix)' % sha(blk))
    # gc_flow_ids on a synthetic bmgres archive (two FLI1 sections, a repeated flow)
    arc = make_bmg_arc([[3, 1121, 7, 3], [500, 0]], rarc_write)
    import tempfile
    tmp = tempfile.mkdtemp(prefix='golden_cave_')
    try:
        d = os.path.join(tmp, 'res', 'Msgus')
        os.makedirs(d)
        with open(os.path.join(d, 'bmgres4.arc'), 'wb') as f:
            f.write(arc)
        flows = cavebuild.gc_flow_ids(tmp, 4)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    out += cxx_bytes('kBmgArc', arc)
    out.append('constexpr const char* kBmgFlows = "%s";  // sorted(cavebuild.gc_flow_ids(<kBmgArc as bmgres4.arc>, 4))' %
               ','.join(str(x) for x in sorted(flows)))
    out.append('// END generated')
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
            raise SystemExit('golden_cave.py: unexpected argument %r' % argv[i])
    for k in ('hd', 'gc', 'oracle', 'out'):
        if k not in args:
            raise SystemExit('golden_cave.py: --%s is required' % k)
    return real(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
