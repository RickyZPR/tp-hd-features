#!/usr/bin/env python3
"""golden_ui.py - the golden list of module U (ui) for `hdf-convert test ui`, made by the Python pipeline (the
executable spec: tools/pipeline/stampbuild.py, tools/gen_stamp_table.py, tools/pipeline/stampbook.py,
tools/pipeline/cosconfirm.py, tools/pipeline/hdtext.py) from YOUR OWN game files.

The list holds sizes and hashes only. It is still made from your games, so it goes to a build-*/ folder of the
project (or a scratch folder) and is never committed or shared. The Python outputs are written to a work folder
next to --out (<out folder>/ui-work), hashed, and deleted again.

USAGE (always with -I -B: no .pyc is written into tools/)
  python -I -B tools/dev/golden_ui.py --hd <TP HD title or content folder> --out build-conv/golden/ui.tsv
                                      [--tools <tools folder>]
  python -I -B tools/dev/golden_ui.py vectors [--tools <tools folder>]
        prints the unit-test vectors of tests/ui_test.cpp (the block between the "vectors" markers): the Python
        spec run on SYNTHETIC inputs only (a made-up BLO and TGA; no game file is read).

One line per made file, tab-separated (135 lines):
  png   <rel> <size> <sha256>   the 53 stamp-folder PNGs (50 stamps, frame, empty, background) and the 25 amiibo
                                PNGs, as common.png_bytes writes them (58 kept + 20 on the drop list)
  file  <rel> <size> <sha256>   the 50 icons (build_icons), stampResR_gc.arc + the 4 loose BTIs, cosconfirm_gc.arc,
                                layout.json
"""
import hashlib
import json
import os
import shutil
import struct
import sys

sys.dont_write_bytecode = True
DEV = os.path.dirname(os.path.abspath(__file__))


TOOLS = None


def setup(tools):
    """as build_assets.py: tools/pipeline first; `import common` then puts formats/critic/cave in front (tools/rarc.py
    at the top level is an older RARC module and must not shadow tools/formats/rarc.py)"""
    global TOOLS
    TOOLS = tools
    sys.path.insert(0, DEV)
    sys.path.insert(0, os.path.join(tools, 'pipeline'))
    import common  # noqa: F401


def gen_stamp_table():
    """tools/gen_stamp_table.py as a module (build_assets._gen_stamp_table)"""
    import importlib.util
    spec = importlib.util.spec_from_file_location('gen_stamp_table', os.path.join(TOOLS, 'gen_stamp_table.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def sha(b):
    return hashlib.sha256(b).hexdigest()


def read(p):
    with open(p, 'rb') as f:
        return f.read()


def stamp_pngs(hd):
    """stampbuild.build part 2 (the PNGs) + gen_stamp_table.build_icons, in memory: {rel: bytes}"""
    import common
    import hdpack
    import rarc
    import stampbuild
    gst = gen_stamp_table()
    lr = os.path.join(hd, 'res', 'LayoutRevo')
    tm = hdpack.Tmpk(hdpack.read_any(common.exists_ci(lr, 'stampResR.pack.gz')))
    arc = rarc.Rarc.from_file(common.exists_ci(lr, 'stampResR.arc'))
    out = {}
    for i in range(50):
        r = stampbuild._gtx_png(tm, 'stamp%d' % i)
        if r is None:
            r = stampbuild._tga_rgba(arc.get('timg/stamp%d.tga' % i).data())
        w, h, rgba = r
        png = common.png_bytes(w, h, rgba)
        out['res/stamps/stamps/stamp%02d.png' % i] = ('png', png)
        # build_icons: png_decode(the PNG just written) -> make_icon_rgba -> bti_cmpr(icon, 48)
        dw, dh, drgba = gst.png_decode(png)
        bti = gst.bti_cmpr(gst.make_icon_rgba(dw, dh, drgba), 48)
        assert len(bti) <= 0xC00, len(bti)
        out['res/stamps/stamp_icons/stamp%02d.bti' % i] = ('file', bti)
    for fn, srcname in stampbuild.FRAMES.items():
        r = stampbuild._gtx_png(tm, srcname)
        if r is None:
            continue
        w, h, rgba = r
        out['res/stamps/stamps/' + fn] = ('png', common.png_bytes(w, h, rgba))
    return out


def cmd_list(args):
    setup(args['tools'])
    import golden
    hd = args['hd']
    if os.path.isdir(os.path.join(hd, 'content')):
        hd = os.path.join(hd, 'content')
    out = args['out']
    golden.check_out(out, [args['hd']])
    hd = os.path.abspath(hd)
    work = os.path.join(os.path.dirname(os.path.abspath(out)), 'ui-work')
    golden.check_out(work, [args['hd']])
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    log = lambda *a: None  # noqa: E731
    files = stamp_pngs(hd)
    try:
        import stampbook
        import cosconfirm
        import hdtext
        d = os.path.join(work, 'stampbook')
        stampbook.build(hd, d, log)
        for fn in ['stampResR_gc.arc'] + [x[0] for x in stampbook.LOOSE]:
            files['res/stamps/stampbook/' + fn] = ('file', read(os.path.join(d, fn)))
        d = os.path.join(work, 'confirm')
        cosconfirm.build(hd, d, log)
        files['res/cave/confirm/cosconfirm_gc.arc'] = ('file', read(os.path.join(d, 'cosconfirm_gc.arc')))
        d = os.path.join(work, 'amiibo')
        os.makedirs(d)
        res = hdtext.build_amiibo(hd, d, log)
        for fn in res['pngs']:
            files['res/cave/amiibo/' + fn] = ('png', read(os.path.join(d, fn)))
        files['res/cave/amiibo/layout.json'] = ('file', read(os.path.join(d, 'layout.json')))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    rows = sorted('%s\t%s\t%d\t%s' % (k, rel, len(b), sha(b)) for rel, (k, b) in files.items())
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# golden_ui.py: module U outputs as the Python spec makes them (sizes + sha256 only)\n')
        for r in rows:
            f.write(r + '\n')
    print('golden_ui.py: %d rows -> %s' % (len(rows), out))
    return 0


# ---------------------------------------------------------------------------------------------- vectors
def blk(tag, body):
    """one SCRNblo2 block: tag, size (8 + body, padded to 4), body"""
    b = bytes(body)
    while (len(b) + 8) % 4:
        b += b'\0'
    return tag.encode('latin1') + struct.pack('>I', 8 + len(b)) + b


def resref_block(tag, refs):
    """TEX1/FNT1: u16 count at +8, u16 0, u32 list offset (0x10) at +12; list: u16 n, u16 entry offsets, entries
    (u8 type, u8 len, name)"""
    n = len(refs)
    head = 2 + 2 * n
    ents = b''
    offs = []
    for typ, name in refs:
        offs.append(head + len(ents))
        ents += bytes((typ, len(name))) + name
    lst = struct.pack('>H', n) + b''.join(struct.pack('>H', o) for o in offs) + ents
    body = struct.pack('>HHI', n, 0, 0x10) + lst
    return blk(tag, body)


def mat1_block():
    """MAT1 with 2 materials (init data 0xE8 each), a name table, 2 colours, a texture-number table, texgen and
    tev-stage number tables and 2 GXColorS10 colours."""
    names = [b'mat_a', b'mat_b']
    init = bytearray(0xE8 * 2)
    # material 0: texgen idx 0 (-> 1), tev stage idx 0 (-> 3), matcolor 0, textures k0 -> texno 0, k1 -> texno 1,
    #             tev colours 0, 1
    init[3], init[4] = 0, 0
    struct.pack_into('>H', init, 8, 0)
    for k in range(8):
        struct.pack_into('>H', init, 0x38 + 2 * k, 0xFFFF)
        struct.pack_into('>H', init, 0xE8 + 0x38 + 2 * k, 0xFFFF)
    struct.pack_into('>HH', init, 0x38, 0, 1)
    struct.pack_into('>HHHH', init, 0x92, 0, 1, 0xFFFF, 0xFFFF)
    # material 1: no texgen (0xFF), tev stage idx 1 (-> 1), matcolor 1, texture k0 -> texno 2 (-> 'c.bfn\n')
    init[0xE8 + 3], init[0xE8 + 4] = 0xFF, 1
    struct.pack_into('>H', init, 0xE8 + 8, 1)
    struct.pack_into('>H', init, 0xE8 + 0x38, 2)
    struct.pack_into('>HHHH', init, 0xE8 + 0x92, 0xFFFF, 1, 0xFFFF, 0xFFFF)
    ids = struct.pack('>HH', 0, 1)
    ntab = struct.pack('>HH', 2, 0)
    noff = 4 + 4 * 2
    ent = b''
    for i, n in enumerate(names):
        ntab += struct.pack('>HH', 0, noff + len(ent))
        ent += n + b'\0'
    ntab += ent
    while len(ntab) % 4:
        ntab += b'\0'
    matcol = bytes.fromhex('11223344a0b0c0d0')
    texno = struct.pack('>HHH', 0, 1, 2) + b'\0\0'
    texgen = bytes((1, 0, 0, 0))
    tevst = bytes((3, 1, 0, 0))
    tevcol = struct.pack('>4h4h', -1, 256, 300, 255, 16, 32, -48, 64)
    parts = [init, ids, ntab, matcol, texno, texgen, tevst, tevcol]
    base = 0xC + 4 * 24
    offs = []
    o = base
    for p in parts:
        offs.append(o)
        o += len(p)
    hdr = [0] * 24
    hdr[0], hdr[1], hdr[2], hdr[5], hdr[11], hdr[8], hdr[16], hdr[14] = offs
    body = struct.pack('>HH', 2, 0) + struct.pack('>24I', *hdr) + b''.join(bytes(p) for p in parts)
    return blk('MAT1', body[0:])


def pane(tag, name, basep, f9, vis=1, aid=0xFFFF, extra=b''):
    """PAN2 (0x48 bytes from the block start) or a PIC2/TBX2/WIN2 wrapper (+8, then the PAN2 part, then extra)"""
    p = bytearray(0x48)
    p[0:4] = b'PAN2'
    struct.pack_into('>I', p, 4, 0x48)
    struct.pack_into('>H', p, 0xA, aid)
    p[0xC], p[0xD] = vis, basep
    p[0x10:0x10 + len(name)] = name
    p[0x18:0x1C] = b'usr\0'
    struct.pack_into('>9f', p, 0x20, *f9)
    if tag == 'PAN2':
        return bytes(p)
    return blk(tag, bytes(p) + bytes(extra))


def synthetic_blo():
    tex = resref_block('TEX1', [(2, b'timg\\Tex_A.BTI'), (1, b'tex_b.bti'), (2, b'c.bfn\n')])
    mat = mat1_block()
    pic_extra = bytearray(0x30)
    struct.pack_into('>H', pic_extra, 4, 0)
    for k in range(4):
        struct.pack_into('>HH', pic_extra, 0x10 + 4 * k, 256 * k, 128 + k)
        pic_extra[0x20 + 4 * k:0x24 + 4 * k] = bytes((k, 0x10 + k, 0x20 + k, 0xF0 + k))
    text = 'はい'.encode('shift_jis')
    tbx_extra = bytearray(0x20)
    struct.pack_into('>HHHhhHHBB', tbx_extra, 0, 0, 0, 1, -2, 3, 24, 22, 1, 2)
    tbx_extra[0x10:0x14] = bytes.fromhex('ffeeddcc')
    tbx_extra[0x14:0x18] = bytes.fromhex('01020304')
    struct.pack_into('>H', tbx_extra, 0x1E, len(text) + 3)
    tbx_extra += text + b'\0zz' + b'\0'
    tbx2_extra = bytearray(tbx_extra)
    struct.pack_into('>H', tbx2_extra, 4, 7)    # material 7: out of range, no material keys
    raw2 = b'x\x80\xd7y\x81'                     # not Shift-JIS: cp1252 with 'replace'
    tbx2_extra = tbx2_extra[:0x1E] + struct.pack('>H', len(raw2)) + raw2 + b'\0'
    win_extra = bytearray(0x28)
    struct.pack_into('>H', win_extra, 0x24, 1)
    f = struct.unpack('>9f', struct.pack('>9f', 100.25, 50.5, 1.0, 0.75, 0.0, 0.0, 12.5, 10.1, -3.3))
    g = struct.unpack('>9f', struct.pack('>9f', 33.3, 20.0, 2.0, 1.0, 0.0, 0.0, -90.0, 0.7, 1.9))
    h = struct.unpack('>9f', struct.pack('>9f', 7.0, 9.0, 1.0, 1.0, 0.0, 0.0, 0.0, -5.55, 2.25))
    b = bytearray(b'SCRNblo2' + b'\0' * 0x18)
    b += blk('INF1', struct.pack('>HHI', 608, 448, 0))
    b += tex
    b += mat
    b += pane('PAN2', b'root', 4, f)
    b += blk('BGN1', b'')
    b += pane('PIC2', b'pic_a', 0, g, aid=3, extra=pic_extra)
    b += pane('TBX2', b'tbx_a', 8, h, vis=0, extra=tbx_extra)
    b += blk('BGN1', b'')
    b += pane('TBX2', b'tbx_b', 2, f, extra=tbx2_extra)
    b += pane('WIN2', b'win_a', 9, g, extra=win_extra)
    b += blk('END1', b'')
    b += blk('END1', b'')
    b += pane('PIC2', b'pic_b', 5, h, extra=bytes(pic_extra[:4]) + struct.pack('>H', 1) + bytes(pic_extra[6:]))
    b += blk('EXT1', b'')
    return bytes(b)


def synthetic_tga(desc):
    w, h = 3, 2
    px = bytes(range(w * h * 4))
    return bytes((2, 0, 2)) + b'\0' * 9 + struct.pack('<HH', w, h) + bytes((32, desc)) + b'ID' + px


def cxx_bytes(name, b):
    lines = ['const unsigned char %s[] = {' % name]
    for i in range(0, len(b), 24):
        lines.append('    ' + ', '.join('0x%02x' % x for x in b[i:i + 24]) + ',')
    lines.append('};')
    return '\n'.join(lines)


def cxx_str(name, s):
    out = ['const char %s[] =' % name]
    for line in s.split('\n'):
        out.append('    "%s\\n"' % line.replace('\\', '\\\\').replace('"', '\\"'))
    out[-1] = out[-1][:-3] + '"'  # the last line has no newline
    return '\n'.join(out) + ';'


def cmd_vectors(tools):
    setup(tools)
    import hdtext
    import stampbook
    import stampbuild
    b = synthetic_blo()
    parsed = hdtext.parse_blo(b)
    js = json.dumps(parsed, indent=1)
    blo = stampbook.Blo(b)
    print('// ---- vectors: made by `python -I -B tools/dev/golden_ui.py vectors` (Python %d.%d.%d; synthetic data '
          'only)' % sys.version_info[:3])
    print(cxx_bytes('kBlo', b))
    print('// hdtext.parse_blo(kBlo) as json.dumps(indent=1)')
    print(cxx_str('kBloJson', js))
    print('// stampbook.Blo(kBlo): TEX1 refs (type, name), FNT1 count, blocks, counts, pane names in walk order')
    print('const std::vector<std::pair<int, std::string>> kBloTex = {%s};' %
          ', '.join('{%d, "%s"}' % (t, n.replace('\\', '\\\\').replace('\n', '\\n')) for t, n in blo.tex))
    print('const std::vector<std::string> kBloBlocks = {%s};' % ', '.join('"%s"' % x for x in blo.blocks))
    print('const std::vector<std::pair<std::string, int>> kBloCounts = {%s};' %
          ', '.join('{"%s", %d}' % kv for kv in blo.counts.items()))
    walk = [p for p in blo.walk()]
    print('const std::vector<std::string> kBloWalk = {%s};' % ', '.join('"%s"' % p['name'] for p in walk))
    print('// _place matrices (repr) of the walk, m[0][0..2], m[1][0..2]')
    print('const std::vector<std::array<double, 6>> kBloPlace = {')
    for p in walk:
        m = p['m']
        print('    {%s},' % ', '.join(repr(float(v)) for v in m[0] + m[1]))
    print('};')
    # Python's shift_jis on every pair (lead 0x81-0xFF, trail 0x40-0xFF): 'LLTT=CCCC\n' lines (code point, 0 when
    # bytes.decode raises or gives more than one character), hashed
    lines = []
    ok = 0
    for lead in range(0x81, 0x100):
        for trail in range(0x40, 0x100):
            try:
                s = bytes((lead, trail)).decode('shift_jis')
                cp = ord(s) if len(s) == 1 else 0
            except UnicodeDecodeError:
                cp = 0
            ok += cp != 0
            lines.append('%02x%02x=%04x\n' % (lead, trail, cp))
    print('// shift_jis pairs: %d decode to one character' % ok)
    print('const char kSjisPairsSha256[] = "%s";' % sha(''.join(lines).encode('ascii')))
    print('constexpr int kSjisPairsOk = %d;' % ok)
    for desc in (0, 0x20):
        t = synthetic_tga(desc)
        w, h, rgba = stampbuild._tga_rgba(t)
        print(cxx_bytes('kTga%02x' % desc, t))
        print(cxx_bytes('kTga%02xRgba' % desc, rgba))
    print('// ---- end of vectors')
    return 0


def main(argv):
    tools = os.path.dirname(DEV)
    args = {}
    rest = argv[1:]
    cmd = 'list'
    if rest and rest[0] == 'vectors':
        cmd = 'vectors'
        rest = rest[1:]
    i = 0
    while i < len(rest):
        if rest[i].startswith('--') and i + 1 < len(rest):
            args[rest[i][2:]] = rest[i + 1]
            i += 2
        else:
            raise SystemExit('golden_ui.py: unexpected argument %r' % rest[i])
    args.setdefault('tools', tools)
    if cmd == 'vectors':
        return cmd_vectors(args['tools'])
    for k in ('hd', 'out'):
        if k not in args:
            raise SystemExit('golden_ui.py: --%s is required' % k)
    return cmd_list(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
