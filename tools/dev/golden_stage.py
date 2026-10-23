#!/usr/bin/env python3
"""golden_stage.py - golden list for `hdf-convert test stage` (module ST, plan 3.3), made by the Python pipeline
(the executable spec) from YOUR OWN game files and the pinned oracle.

The list holds hashes, sizes, counts and numbers only: no game bytes, no names from inside the files. It is
still made from your games, so it goes to a build-*/ folder of the project (or a scratch folder) and is never
committed or shared. --out is refused anywhere else.

USAGE (always with -I -B: no .pyc is written into tools/)
  python -I -B tools/dev/golden_stage.py --hd <TP HD title or content folder> --gc <GC 'files' folder>
                                         --oracle cache/oracle/hd_features-oracle-2.0.0.dusk
                                         --out build-conv/golden/stage.tsv

  python -I -B tools/dev/golden_stage.py vectors > vectors.txt
      Synthetic test vectors (no game data: every input is made here) for tests/stage_test.cpp: prints the
      C++ block that goes between the "generated" markers of that file.

One line per fact, tab-separated:
  los      <count> <unk4> <unk8> <size> <sha256 of the entry lines>
             entry line = 'i room next prev floor x y z unk1c angleY\\n' (floats as repr())
  records  <room> <count> <sha256 of the record lines>         (stagedata.records of the HD room.dzr)
             record line = 'tag layer i off name prm x y z ax ay az setid\\n' (floats as repr())
  flows    <msg group> <Tag_ms flows a,b,..> <HD-only flows a,b,..>          (cavebuild.build, GC bmgres)
  stats    <room> <arc> <models> <mat> <shp> <jnt> <tex> <anm> <files> <dl>  (memfit.arc_stats of the oracle's
             built room archive overlay/res/Stage/D_SB11/Rnn_00.arc; J's numbers until module J lands)
  need     <room> <memfit.estimate>
  mec      <room>:<block>,...                                  (memfit.stage_mem_tables after the rewrite)
  mem0     <old a,b,c> <new a,b,c>                             (hex, memfit.plan)
  room     <room> <size> <sha256 of patched room.dzr> <report lines> <sha256 of '\\n'.join(report)>
  stage    <size> <sha256 of patched stage.dzs> <report lines> <sha256 of '\\n'.join(report)>
  hdtags   <size> <sha256>                                     (cavebuild.hd_tags_bin(hd_tag_records))
  hdchests <size> <sha256>                                     (stampbuild.hd_chests_bin(hd_chest_records))
"""
import hashlib
import io
import os
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.dirname(TOOLS)
sys.path.insert(0, os.path.join(TOOLS, 'pipeline'))
import common  # noqa: E402,F401  (puts formats/critic/cave on sys.path)
import cave_patch  # noqa: E402
import cavebuild  # noqa: E402
import losbin  # noqa: E402
import memfit  # noqa: E402
import rarc  # noqa: E402
import stagedata  # noqa: E402
import stampbuild  # noqa: E402

ORACLE_SHA256 = 'a7a48bf643e621b89e3860fe9d7c48b26e76ec9b6cf518af8c5ff2b42bc68f59'


def sha(b):
    return hashlib.sha256(b).hexdigest()


def norm(p):
    return os.path.normcase(os.path.realpath(os.path.abspath(p)))


def within(child, parent):
    c, p = norm(child), norm(parent)
    return c == p or c.startswith(p.rstrip('\\/') + os.sep)


def check_out(out, inputs):
    if within(out, PROJECT):
        first = os.path.relpath(norm(out), norm(PROJECT)).split(os.sep)[0]
        if not first.startswith('build-') or first == 'build-hdf' or len(first) <= 6:
            raise SystemExit('golden_stage.py: --out must be in a build-*/ folder of the project (not build-hdf) '
                             'or a scratch folder: the list is made from your games')
    for i in inputs:
        if within(out, i) or within(i, out):
            raise SystemExit('golden_stage.py: --out overlaps the input %s' % i)
    appdata = os.environ.get('APPDATA')
    if appdata and within(out, os.path.join(appdata, 'TwilitRealm')):
        raise SystemExit('golden_stage.py: refusing to write into %APPDATA%\\TwilitRealm')


def los_lines(los):
    lines = ''.join('%d %d %d %d %d %r %r %r %d %d\n' % (e['i'], e['room'], e['next'], e['prev'], e['floor'],
                                                        e['x'], e['y'], e['z'], e['unk1c'], e['angleY'])
                    for e in los['entries'])
    return lines.encode('latin-1')


def record_lines(recs):
    out = []
    for r in recs:
        out.append('%s %d %d %d %s %d %r %r %r %d %d %d %d\n' % (
            r['tag'], r['layer'], r['i'], r['off'], r['name'], r['prm'], r['pos'][0], r['pos'][1], r['pos'][2],
            r['ang'][0], r['ang'][1], r['ang'][2], r['setid']))
    return ''.join(out).encode('latin-1')


# ---------------------------------------------------------------------------------------------- vectors
def _dzx(chunks):
    """a dzs/dzr from [(tag, count, payload)]: chunk table, then each payload 4-aligned, in order"""
    n = len(chunks)
    base = 4 + 12 * n
    table, body = b'', b''
    for tag, cnt, payload in chunks:
        table += struct.pack('>4siI', tag, cnt, base + len(body))
        body += payload + b'\0' * ((-len(payload)) % 4)
    return struct.pack('>i', n) + table + body


def _act(name, prm, pos=(0.0, 0.0, 0.0), ang=(0, 0, 0), setid=0xFFFF, scob=False):
    rec = name.ljust(8, b'\0') + struct.pack('>I3f3hH', prm, pos[0], pos[1], pos[2], ang[0], ang[1], ang[2], setid)
    return rec + (bytes((10, 20, 30, 0)) if scob else b'')


def _s16(v):
    return v - 0x10000 if v >= 0x8000 else v


def synth_room():
    actr = [_act(b'rGate', 0x000002C0, (1.5, -2.25, 100.125)),
            _act(b'rGate', 0x000003C1, (0.0, 0.0, -0.0)),
            _act(b'Kbota_A', 0x00030005, (3.0, 4.0, 5.0), (1, 2, 3)),
            _act(b'l8warp', 0x0000AB12),
            _act(b'tboxA0', (63 << 6) | 0x3, (10.0, 20.0, 30.0), (0, 0x4000, 0x0700)),
            _act(b'Obj_x\xe9', 0x12345678, (1e-7, 3.4e38, -1.0))]
    tre0 = [_act(b'tboxB0', (63 << 6) | 0x20000000, ang=(0, 0, _s16(0x9B00))),
            _act(b'tboxB1', (12 << 6), ang=(5, 0, 0x0100))]
    scob = [_act(b'SwAreaC', 0, ang=(0x0A07, 0, 0), scob=True),
            _act(b'SwAreaS', 0, ang=(0x0B01, 0, 0), scob=True),
            _act(b'SwAreaC', 0, ang=(0x0507, 0, 0), scob=True),
            _act(b'TagEv', 0, ang=(900, 7, 1000), scob=True),
            _act(b'TagEv', 0, ang=(_s16(0x8100), 7, -1), scob=True),
            _act(b'TagEv', 0, ang=(_s16(0x8000 | 2000), 7, 821), scob=True),
            _act(b'Tag_ms', 0x11, ang=(0, 0, 1121), scob=True),
            _act(b'Tag_ms', 0x22, ang=(0, 0, 500), scob=True),
            _act(b'FSeirei', 0x04, scob=True),
            _act(b'FSeirei', 0x05, scob=True)]
    sco1 = [_act(b'Tag_ms', 0x33, ang=(0, 0, 1121), scob=True)]
    doo2 = [_act(b'door', 0x44, scob=True)]
    return _dzx([(b'RPPN', 1, bytes(range(16))), (b'ACTR', len(actr), b''.join(actr)),
                 (b'TRE0', len(tre0), b''.join(tre0)), (b'SCOB', len(scob), b''.join(scob)),
                 (b'SCO1', len(sco1), b''.join(sco1)), (b'Doo2', len(doo2), b''.join(doo2)),
                 (b'XYZW', 0, b''), (b'TGOB', -1, b'')])


def synth_stage():
    stag = struct.pack('>ffB', 1.0, 50000.0, 3) + bytes((9,)) + struct.pack('>H', (206 << 3) | 5)
    stag += struct.pack(">II", 0x01020304, 0x05060708) + bytes(6) + struct.pack('>hhh', -1, 2, -3)
    stag += bytes(0x28 - len(stag)) + bytes((5, 0)) + struct.pack('>H', 0x1234) + bytes(range(16))
    assert len(stag) == 0x3C
    nroom = 5
    # RTBL: offset table at the chunk, then 8-byte entries, then the room lists (4 bytes apart)
    base = 4 + 12 * 5
    tab_off = base + len(stag)
    ent_off = tab_off + 4 * nroom
    list_off = ent_off + 8 * nroom
    tab = b''.join(struct.pack('>I', ent_off + 8 * r) for r in range(nroom))
    ents = b''.join(struct.pack('>BBBxI', 1, 0, 0, list_off + 4 * r) for r in range(nroom))
    lists = b''.join(bytes(((r | 0xC0) if r == 2 else r, 0, 0, 0)) for r in range(nroom))
    rtbl = tab + ents + lists
    mec0 = bytes((0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 2, 1))   # room 2 twice: dict keeps the last block
    rcam = b'BonusDungeon'.ljust(16, b'\0') + bytes(8) + b'Other'.ljust(16, b'\0') + bytes(8)
    mem0 = struct.pack('>III', 0x15a180, 0x15a180, 0x15a180)
    d = _dzx([(b'STAG', 1, stag), (b'RTBL', nroom, rtbl), (b'MEC0', 6, mec0), (b'RCAM', 2, rcam),
              (b'MEM0', 3, mem0)])
    assert struct.unpack_from('>I', d, 4 + 12 + 8)[0] == tab_off
    return d


def synth_los():
    es = [(0, -1, -1, 0, 0.5, 1.0, -2.0, 7, -100),
          (1, 2, -1, 1, 10.0, 20.0, 30.0, 0, 0x4000),
          (2, 3, 1, 2, -1.25, 0.0, 1e10, -5, -0x8000),
          (3, 4, 2, 3, 0.1, 0.2, 0.3, 0, 0),
          (4, -1, 3, 4, 1.0, 1.0, 1.0, 1, 1)]
    b = struct.pack('>III', len(es), 43, 41)
    for r, n, pv, fl, x, y, z, u, a in es:
        b += struct.pack('>Iiiifffhh', r, n, pv, fl, x, y, z, u, a)
    return b + b'\x01\x02'


def _stats(room, arc, models, mat, shp, jnt, tex, anm, files, dl):
    return room, {'arc': arc, 'models': models, 'mat': mat, 'shp': shp, 'jnt': jnt, 'tex': tex, 'anm': anm,
                  'files': files, 'dl': dl}


def cpp_str(s):
    out = '"'
    for ch in s:
        o = ord(ch)
        if ch == '\\' or ch == '"':
            out += '\\' + ch
        elif ch == '\n':
            out += '\\n"\n        "'
        elif 32 <= o < 127:
            out += ch
        else:
            out += '\\x%02x""' % o
    return out + '"'


def cpp_hex(b):
    h = b.hex()
    return '\n        '.join('"%s"' % h[i:i + 96] for i in range(0, len(h), 96)) if h else '""'


def cmd_vectors():
    import tempfile
    import facts
    import rarc_write
    prod = {'no_rtbl': False, 'no_meco': False, 'cam': 'Hall', 'particle': None, 'mem': None,
            'gates': True, 'kbota': True, 'swc': True, 'tagev': True, 'fseirei': True,
            'drop_tagms': [1121], 'exit': cavebuild.EXIT, 'tbox_unique': True, 'fairy_bypass': True,
            'reward_item': None, 'banks_gc': False}
    off = dict(prod, cam=None, gates=False, kbota=False, swc=False, tagev=False, fseirei=False, drop_tagms=[],
               exit=None, tbox_unique=False, fairy_bypass=False, no_rtbl=True, no_meco=True)
    variants = {'prod': prod, 'reward': dict(prod, reward_item=0x36), 'off': off,
                'flows': dict(off, drop_tagms=[500, 1121]), 'exit_only': dict(off, exit=('F_SP1245678', 200, 3))}
    room = synth_room()
    stage = synth_stage()
    losb = synth_los()
    o = []
    o.append('// BEGIN generated by tools/dev/golden_stage.py vectors (synthetic data only; the Python spec)')
    o.append('constexpr const char* kRoomHex =\n        %s;' % cpp_hex(room))
    recs = stagedata.records(room)
    o.append('constexpr const char* kRoomRecordsSha = "%s";' % sha(record_lines(recs)))
    o.append('constexpr std::size_t kRoomRecordsCount = %d;' % len(recs))
    rows = []
    for rn, name in [(49, 'prod'), (14, 'prod'), (25, 'prod'), (34, 'prod'), (3, 'prod'), (14, 'reward'),
                     (49, 'off'), (25, 'flows'), (14, 'exit_only')]:
        rep = []
        out = cave_patch.patch_room(rn, room, variants[name], rep)
        rows.append('    {%d, "%s", %d, "%s",\n        %s},' % (rn, name, len(out), sha(out), cpp_str('\n'.join(rep))))
    o.append('constexpr RoomVec kRoomVecs[] = {\n%s\n};' % '\n'.join(rows))
    # add_scls: no-op on an output that already has one; added on the input
    out25 = cave_patch.patch_room(25, room, prod, [])
    again, added = cave_patch.add_scls(out25, b'X' * 13)
    assert not added and again == out25
    scls2, added2 = cave_patch.add_scls(room, b'ABCDEFGH' + bytes((1, 2, 3, 4, 5, 6, 7)))
    assert added2
    o.append('constexpr const char* kAddSclsSha = "%s";  // add_scls(room, "ABCDEFGH" 01..07): added' % sha(scls2))
    o.append('constexpr std::size_t kAddSclsSize = %d;' % len(scls2))
    # stage + los
    o.append('constexpr const char* kStageHex =\n        %s;' % cpp_hex(stage))
    o.append('constexpr const char* kLosHex =\n        %s;' % cpp_hex(losb))
    with tempfile.TemporaryDirectory() as td:
        lp = os.path.join(td, 'los.bin')
        with open(lp, 'wb') as f:
            f.write(losb)
        L = losbin.load(lp)
    o.append('constexpr const char* kLosLinesSha = "%s";' % sha(los_lines(L)))
    chain = [e['room'] for e in cavebuild.floor_chain(L['entries'])]
    o.append('constexpr std::array<u32, %d> kFloorChain = {%s};' % (len(chain), ', '.join(map(str, chain))))
    stats = dict([_stats(0, 1000, 0, 0, 0, 0, 0, 0, 1, 0), _stats(1, 0x40001, 2, 5, 9, 3, 4, 2, 8, 12345),
                  _stats(2, 300000, 1, 50, 80, 20, 30, 0, 4, 999999), _stats(3, 31, 1, 1, 1, 1, 1, 1, 3, 1),
                  _stats(4, 2000000, 3, 100, 200, 60, 70, 9, 20, 4000000)])
    srows = ['    {%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d},' % (
        r, s['arc'], s['models'], s['mat'], s['shp'], s['jnt'], s['tex'], s['anm'], s['files'], s['dl'],
        memfit.estimate(s)) for r, s in stats.items()]
    o.append('constexpr StatsVec kStats[] = {\n%s\n};' % '\n'.join(srows))
    needs = [0, 1, 0x7FF, 0x8000, 123456, 2000000, 3000001, 999999999, 2 ** 31 + 12345]
    o.append('constexpr std::pair<u64, u32> kMem0For[] = {%s};' % ', '.join(
        '{%d, 0x%x}' % (n, memfit.mem0_for(n)) for n in needs))
    E = L['entries']
    nomem = cave_patch.patch_stage(stage, E, dict(prod, mem=None), [])
    mem_old, _ = memfit.stage_mem_tables(stage)
    _, mec_new = memfit.stage_mem_tables(nomem)
    mem0, _ = memfit.plan(stats, mec_new, len(mem_old))
    o.append('constexpr std::array<u32, 3> kPlan = {%s};' % ', '.join('0x%x' % x for x in mem0))
    o.append('constexpr const char* kMecNew = "%s";' % ','.join('%d:%d' % kv for kv in mec_new.items()))
    srows = []
    for name, opts in [('prod', dict(prod, mem=mem0)),
                       ('particle', dict(prod, particle=200, cam='LongerCamName1234', mem=[0x20, 0x40, 0x60])),
                       ('off', dict(off, mem=None))]:
        rep = []
        out = cave_patch.patch_stage(stage, E, opts, rep)
        srows.append('    {"%s", %d, "%s",\n        %s},' % (name, len(out), sha(out), cpp_str('\n'.join(rep))))
    o.append('constexpr StageVec kStageVecs[] = {\n%s\n};' % '\n'.join(srows))
    # res/ binaries from synthetic archives (written to a temporary folder for stampbuild.hd_chest_records)
    tag_rooms = {}
    for room_no in facts.CAVE_TAG_ROOMS:
        trecs = [_act(b'Tag_ms', 0x100 + room_no, (float(room_no), 2.0, 3.0), (0, 0, 1121), scob=True),
                 _act(b'Tag_ms', 0x200 + room_no, ang=(0, 0, 1122), scob=True)]
        tag_rooms[room_no] = _dzx([(b'ACTR', 1, _act(b'Tag_ms', 1, ang=(0, 0, 1121))),
                                   (b'SCO%d' % (room_no % 10), 2, b''.join(trecs))])
    hd_records = {r: stagedata.records(d) for r, d in tag_rooms.items()}
    tags = cavebuild.hd_tags_bin(cavebuild.hd_tag_records(tag_rooms, hd_records))
    with tempfile.TemporaryDirectory() as td:
        chest_dzr = {}
        for idx, st, rn, tb, item in facts.HD_CHESTS:
            layer_tag = b'TRES' if idx % 2 else b'TRE%d' % (idx % 10)
            crecs = [_act(b'tboxA0', (tb << 6), ang=(0, 0, _s16(((item + 1) & 0xFF) << 8))),   # wrong item
                     _act(b'Obj_box', (tb << 6), ang=(0, 0, _s16(item << 8))),                # not a chest
                     _act(b'tboxB0', (tb << 6) | (idx << 12), (float(idx), 1.0, 2.0), (0, 0, _s16(item << 8)))]
            d = _dzx([(b'ACTR', 1, _act(b'tboxA0', tb << 6, ang=(0, 0, _s16(item << 8)))),
                      (layer_tag, 3, b''.join(crecs))])
            chest_dzr[(st, rn)] = d
            arc = rarc_write.build('r%02d_00' % rn, [('dzr/room.dzr', d)])
            os.makedirs(os.path.join(td, 'res', 'Stage', st), exist_ok=True)
            with open(os.path.join(td, 'res', 'Stage', st, 'R%02d_00.arc' % rn), 'wb') as f:
                f.write(arc)
        chests = stampbuild.hd_chests_bin(stampbuild.hd_chest_records(td))
    o.append('constexpr const char* kTagRoomHex[2] = {\n        %s,\n        %s};' % (
        cpp_hex(tag_rooms[facts.CAVE_TAG_ROOMS[0]]), cpp_hex(tag_rooms[facts.CAVE_TAG_ROOMS[1]])))
    o.append('constexpr const char* kHdTagsHex =\n        %s;' % cpp_hex(tags))
    o.append('constexpr const char* kChestDzrHex[7] = {\n%s};' % ',\n'.join(
        '        ' + cpp_hex(chest_dzr[(st, rn)]) for _, st, rn, _, _ in facts.HD_CHESTS))
    o.append('constexpr const char* kHdChestsHex =\n        %s;' % cpp_hex(chests))
    o.append('// END generated')
    print('\n'.join(o))
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == 'vectors':
        return cmd_vectors()
    args = {}
    i = 1
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            args[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            print(__doc__)
            raise SystemExit('golden_stage.py: unexpected argument %r' % argv[i])
    for k in ('hd', 'gc', 'oracle', 'out'):
        if k not in args:
            raise SystemExit('golden_stage.py: --%s is required' % k)
    hd, gc, oracle, out = args['hd'], args['gc'], args['oracle'], args['out']
    if os.path.isdir(os.path.join(hd, 'content')):
        hd = os.path.join(hd, 'content')
    check_out(out, [args['hd'], gc, oracle])
    with open(oracle, 'rb') as f:
        ob = f.read()
    if sha(ob) != ORACLE_SHA256:
        raise SystemExit('golden_stage.py: the oracle is not the pinned file (sha256 %s)' % sha(ob))
    z = zipfile.ZipFile(io.BytesIO(ob))
    rows = []

    # --- HD inputs, as cavebuild.build reads them
    sdir, names = cavebuild._list_stage(hd)
    los = losbin.load(os.path.join(hd, 'los.bin'))
    L = los['entries']
    rows.append(('los', los['count'], los['unk4'], los['unk8'], los['size'], sha(los_lines(los))))
    hd_records, hd_dzr, hd_stage_dzs = {}, {}, None
    for n in names:
        a = rarc.Rarc.from_file(os.path.join(sdir, n))
        if n.upper().startswith('STG'):
            hd_stage_dzs = a.get('stage.dzs').data()
        else:
            m = a.get('room.dzr')
            if m is not None:
                hd_dzr[int(n[1:3])] = m.data()
                hd_records[int(n[1:3])] = stagedata.records(m.data())
    for room in sorted(hd_records):
        rows.append(('records', room, len(hd_records[room]), sha(record_lines(hd_records[room]))))
    msg_group = stagedata.stag(hd_stage_dzs)['msgGroup']
    gc_flows = cavebuild.gc_flow_ids(gc, msg_group)
    tagms_flows = sorted({r['ang'][2] & 0xFFFF for rr in hd_records.values() for r in rr if r['name'] == 'Tag_ms'})
    drop_flows = [f for f in tagms_flows if f not in gc_flows]
    rows.append(('flows', msg_group, ','.join(str(x) for x in tagms_flows) or '-',
                 ','.join(str(x) for x in drop_flows) or '-'))

    opts = {'no_rtbl': False, 'no_meco': False, 'cam': 'Hall', 'particle': None, 'mem': None,
            'gates': True, 'kbota': True, 'swc': True, 'tagev': True, 'fseirei': True,
            'drop_tagms': drop_flows, 'exit': cavebuild.EXIT, 'tbox_unique': True, 'fairy_bypass': True,
            'reward_item': None, 'banks_gc': False}

    # --- rooms (cave_patch.patch_room on the HD room.dzr, as cavebuild.build)
    for room in sorted(hd_dzr):
        plog = []
        nd = cave_patch.patch_room(room, hd_dzr[room], opts, plog)
        rows.append(('room', room, len(nd), sha(nd), len(plog), sha('\n'.join(plog).encode('latin-1'))))

    # --- model stats of the built room archives (the oracle's), then MEM0 as cavebuild.build
    room_stats = {}
    for n in names:
        if n.upper().startswith('STG'):
            continue
        b = z.read('overlay/res/Stage/D_SB11/' + n)
        room_stats[int(n[1:3])] = memfit.arc_stats(b)
    for room, s in room_stats.items():
        rows.append(('stats', room, s['arc'], s['models'], s['mat'], s['shp'], s['jnt'], s['tex'], s['anm'],
                     s['files'], s['dl']))
        rows.append(('need', room, memfit.estimate(s)))
    tmp_log = []
    dzs_nomem = cave_patch.patch_stage(hd_stage_dzs, L, dict(opts, mem=None), tmp_log)
    mem_old, _ = memfit.stage_mem_tables(hd_stage_dzs)
    _, mec_new = memfit.stage_mem_tables(dzs_nomem)
    mem0, _ = memfit.plan(room_stats, mec_new, len(mem_old))
    rows.append(('mec', ','.join('%d:%d' % kv for kv in mec_new.items())))
    rows.append(('mem0', ','.join('0x%x' % x for x in mem_old), ','.join('0x%x' % x for x in mem0)))
    plog = []
    dzs = cave_patch.patch_stage(hd_stage_dzs, L, dict(opts, mem=mem0), plog)
    rows.append(('stage', len(dzs), sha(dzs), len(plog), sha('\n'.join(plog).encode('latin-1'))))

    # --- res/ binaries
    tags = cavebuild.hd_tags_bin(cavebuild.hd_tag_records(hd_dzr, hd_records))
    rows.append(('hdtags', len(tags), sha(tags)))
    chests = stampbuild.hd_chests_bin(stampbuild.hd_chest_records(hd))
    rows.append(('hdchests', len(chests), sha(chests)))

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# hdf golden stage 1: made by tools/dev/golden_stage.py from your own game files and the pinned '
                'oracle (hashes and numbers only; never commit or share)\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print('golden stage: %d lines -> %s (MEM0 %s -> %s)' % (len(rows), out, ['0x%x' % x for x in mem_old],
                                                           ['0x%x' % x for x in mem0]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
