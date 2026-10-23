"""Validation of every generated asset. Each check appends a row (group, check, PASS/FAIL/WARN, detail).

Models (every .bmd/.bdl in every output archive):
  blocks     only J3D blocks Dusklight's J3DModelLoader accepts (no MAT4 left)
  dlcheck    tools/formats/j3d_dlcheck.check: valid GX opcodes, every vertex index inside its VTX1 array
  stride     tools/critic/matstride + j3dcheck: material entries really at 0x14C (content check)
  vtxfmt     tools/critic/j3dcheck: no VTX1 array without a format entry (getFmt OSPanic in v2.0.3)
  pixels     every TEX1 texture / BTI has its pixel data
D_SB11 data (final room.dzr / stage.dzs, checked against unmodified Dusklight v2.0.3 actor code):
  rGate model type <= 1 (l_resNameIdx[2]); Kbota_A model in {0,1,2,15} (M_arcname[3]); swc00 type in
  {0..8,15}; TagEv labels < 822 (saveBitLabels[822]); no FSeirei type >= 4 (sTempBit[4]); Tag_ms flows exist
  in the GC message group; l8warp exits valid; tbox numbers unique; mmvbg / bank archives exist; actor names
  known; the entry room (19, floor 1) has start point 0; the HD section data the mod's section rules need (the
  tier ALLdie records, the rest floors' HD switch chains, the swc00 HD types 10/11 kept in angle.x bits 12-15);
  RCAM types in GC camtype.dat; RTBL / MEC0 consistent
  with the los chains (main and detached); MEM0 fits every room (memfit estimate) AND an independent hard
  floor (archive + PC display-list copies + VcdVat buffers + a minimal per-material size, not memfit's
  constants) leaves >= 5 % of each block; memfit's constants pass the GC calibration.
  Scene particles (fix-cave): the stage's particle file exists (GC disc or the mod's Pscene206.jpc), parses,
  every TDB1 index is valid, every texture has pixels, it contains every particle id the final room actors'
  code references, the ids kytag10 Sparks dereferences without a NULL check, and it is not larger than the
  largest scene particle file on the GC disc (= known to fit dPa_control_c's 0x96000-byte resource heap).
Stamps: Obj_Stamp resource index, 50 PNGs decode and are non-empty, manifest, hd_chests.bin (the 7 HD-only chest
records). Cave: hd_tags.bin (HD's section-end talk tag records). Overlay: every path is new on the GC disc (nothing
replaced)."""
import json
import os
import struct

import common
import memfit
import rarcx
import stagedata
import cavebuild
import rarc
import j3d
import j3d_dlcheck
import j3dcheck
import matstride
import particles

ACCEPTED_BLOCKS = {'INF1', 'VTX1', 'EVP1', 'DRW1', 'JNT1', 'MAT3', 'MAT2', 'SHP1', 'TEX1'}   # J3DModelLoader.cpp
SWC00_TYPES = set(range(0, 9)) | {15}
LABELS = 822
# Constants mirrored from mods/hdfeatures/src/cave (floors.hpp, cave.cpp / hooks.cpp HD sections) and
# research/cos_sections_spec.md §3 (VERIFIED HD data):
ENTRY_ROOM = 19                              # floor 1: every visit (and death-continue) at start point 0
REST_GATES = {25: 0x99, 34: 0xA2}            # floor 6 / 20 rGate switch = 0x80 + room
REST_PORTAL_ANDSW2 = 0xc1c40002              # C1 (ALLdie "locked") & C2 (Link in the centre) -> C4 (l8warp)
REST_GATE_ANDSW2 = {25: 0xc2990002, 34: 0xc2a20002}   # C2 & C3 (ALLdie "unlocked") -> descent gate
TIER_ALLDIE = {10: 0xffc28a01, 25: 0xffc1c301, 48: 0xffc2b002, 34: 0xffc1c302, 49: 0xffffb103}
HD_SWC_TYPES = {19: 10, 10: 11, 48: 11, 49: 11}       # room -> HD swc00 type (cave timer start / stop)
# Particles whose emitters an actor dereferences without a NULL check (d_a_kytag10.cpp:99-114, 198-199).
UNCHECKED_PARTICLES = {'Sparks': (0x852B, 0x852C)}
HARD_MAT = 0x400                             # minimal PC J3DMaterial + blocks per material (hard floor only)


class Report(object):
    def __init__(self):
        self.rows = []

    def add(self, group, check, ok, detail='', warn=False):
        st = 'PASS' if ok else ('WARN' if warn else 'FAIL')
        self.rows.append((group, check, st, detail))
        return ok

    def failed(self):
        return [r for r in self.rows if r[2] == 'FAIL']

    def table(self):
        w0 = max(len(r[0]) for r in self.rows)
        w1 = max(len(r[1]) for r in self.rows)
        lines = ['%-*s  %-*s  %-4s  %s' % (w0, 'group', w1, 'check', 'res', 'detail'),
                 '%s  %s  %s  %s' % ('-' * w0, '-' * w1, '----', '-' * 40)]
        for g, c, s, d in self.rows:
            lines.append('%-*s  %-*s  %-4s  %s' % (w0, g, w1, c, s, d))
        return '\n'.join(lines)


def gc_has(gc_files, *parts):
    d = os.path.join(gc_files, *parts[:-1])
    return common.exists_ci(d, parts[-1]) is not None


# --------------------------------------------------------------------------------------------- models
def check_model(d, label):
    probs = []
    _, _, secs = j3d.sections(d)
    tags = [t for t, o, s in secs]
    extra = [t for t in tags if t not in ACCEPTED_BLOCKS]
    if extra:
        probs.append('blocks %s' % extra)
    r = j3d_dlcheck.check(d)
    if not r['ok']:
        probs.append('dlcheck: bad_opcodes=%d out_of_range=%s' % (r['bad_opcodes'], r['out_of_range']))
    jc = j3dcheck.check_bytes(d, label)
    if jc.get('bad_stride'):
        probs.append('stride: %s' % jc.get('mat'))
    if jc.get('vtx_missing'):
        probs.append('VTX1 arrays without format: %s' % jc['vtx_missing'])
    for t, o, s in secs:
        if t[:3] == 'MAT':
            errs = matstride.material_errors(d, o, 0x14C)
            if errs:
                probs.append('material entries inconsistent at 0x14C: %s' % (errs[:3],))
        if t == 'TEX1':
            for tx in j3d.tex1(d, o, s):
                if not tx['present'] or tx['expected_bytes'] == 0:
                    probs.append('texture %s without pixels' % tx['name'])
    return probs, r, jc


def check_bti(d):
    h = j3d.bti_header(d, 0)
    exp = j3d.tex_size(h['fmt'], h['w'], h['h'], max(1, h['mips']))
    if not h['img_off'] or len(d) < h['img_off'] + exp:
        return ['BTI without pixels (%d bytes, needs %d at 0x%x)' % (len(d), exp, h['img_off'])]
    return []


def check_archive(name, buf):
    """-> {'name', 'size', 'rarc': [problems], 'models', 'textures', 'model_probs': [...], 'strides': set()}"""
    out = {'name': name, 'size': len(buf), 'rarc': rarcx.check(buf), 'models': 0, 'textures': 0,
           'model_probs': [], 'strides': set(), 'mat_counts': []}
    if out['rarc']:
        return out
    a = rarc.Rarc(buf, name)
    for m in a.files():
        ln = m.name.lower()
        if ln.endswith(('.bmd', '.bdl')):
            d = m.data()
            out['models'] += 1
            try:
                pr, r, jc = check_model(d, m.path)
            except Exception as e:  # noqa: BLE001
                pr, jc = ['exception %r' % e], {}
            if jc.get('stride'):
                out['strides'].add(jc['stride'])
            for t, o, s in j3d.sections(d)[2]:
                if t == 'TEX1':
                    out['textures'] += struct.unpack_from('>H', d, o + 8)[0]
                if t == 'MAT3':
                    out['mat_counts'].append(struct.unpack_from('>H', d, o + 8)[0])
            out['model_probs'] += ['%s:%s: %s' % (name, m.path, p) for p in pr]
        elif ln.endswith('.bti'):
            out['textures'] += 1
            out['model_probs'] += ['%s:%s: %s' % (name, m.path, p) for p in check_bti(m.data())]
    return out


def add_archive_rows(rep, group, results):
    rp = ['%s: %s' % (r['name'], '; '.join(r['rarc'][:2])) for r in results if r['rarc']]
    rep.add(group, 'RARC structure', not rp, '; '.join(rp[:3]) if rp else '%d archive(s), %d bytes' % (
        len(results), sum(r['size'] for r in results)))
    mp = [p for r in results for p in r['model_probs']]
    nm = sum(r['models'] for r in results)
    nt = sum(r['textures'] for r in results)
    multi = sum(1 for r in results for c in r['mat_counts'] if c > 1)
    strides = set().union(*[r['strides'] for r in results]) if results else set()
    if nm or nt:
        rep.add(group, 'models: blocks/dlcheck/stride/vtxfmt/pixels', not mp, '; '.join(mp[:3]) if mp else
                '%d model(s) (%d with >1 material), %d texture(s); MAT entry stride %s' % (
                    nm, multi, nt, '/'.join('0x%X' % x for x in sorted(strides)) or '-'))


# --------------------------------------------------------------------------------------------- cave data
def gc_start_point(gc_files, st, room, point):
    """GC stage st has a PLYR record with start point `point` in room `room` (dStage_playerInit matches
    (u8)angle.z against the start point)"""
    d = os.path.join(gc_files, 'res', 'Stage', st)
    p = common.exists_ci(d, 'R%02d_00.arc' % room) if os.path.isdir(d) else None
    if p is None:
        return False
    m = rarc.Rarc.from_file(p).get('room.dzr')
    return m is not None and any(r['tag'] == 'PLYR' and (r['ang'][2] & 0xFF) == point
                                 for r in stagedata.records(m.data()))


def gc_save_tables(gc_files):
    """{GC stage: STAG save table} (dStage_stagInfo_GetSaveTbl = (f09 >> 1) & 0x1F)"""
    out = {}
    sd = os.path.join(gc_files, 'res', 'Stage')
    for st in sorted(os.listdir(sd)):
        p = common.exists_ci(os.path.join(sd, st), 'STG_00.arc')
        if p is None:
            continue
        m = rarc.Rarc.from_file(p).get('stage.dzs')
        if m is None:
            continue
        sg = stagedata.stag(m.data())
        if sg:
            out[st] = (sg['f09'] >> 1) & 0x1F
    return out


def camtype_names(gc_files):
    a = rarc.Rarc.from_file(os.path.join(gc_files, 'res', 'Object', 'CamParam.arc'))
    d = a.get('camtype.dat').data()
    n = struct.unpack_from('>I', d, 4)[0]
    es = (len(d) - 8) // n
    return {d[8 + i * es:8 + i * es + 24].split(b'\0')[0].decode('latin-1') for i in range(n)}


def check_cave(rep, gc_files, stage_dir, obj_dir, res_dir, cres, particle_dir=None):
    G = 'cave'
    names = sorted(os.listdir(stage_dir))
    arcs = {n: common.read(os.path.join(stage_dir, n)) for n in names}
    los = cres['los']['entries']
    chain = cavebuild.floor_chain(los)
    chain_rooms = [e['room'] for e in chain]
    have_rooms = {int(n[1:3]) for n in names if n[0] in 'Rr'}
    rep.add(G, 'archive set', 'STG_00.arc' in arcs and all(r in have_rooms for r in chain_rooms) and len(arcs) == 51,
            '%d archives (STG + %d rooms); all %d floor rooms present' % (len(arcs), len(have_rooms), len(chain)))
    objnames = stagedata.objnames()
    overlay_objs = {os.path.splitext(n)[0].lower() for n in os.listdir(obj_dir)}

    def arc_exists(name):
        return name.lower() in overlay_objs or gc_has(gc_files, 'res', 'Object', name + '.arc')

    cams = camtype_names(gc_files)
    gc_flows = cavebuild.gc_flow_ids(gc_files, cres['msg_group'])
    bad = {k: [] for k in ('unknown', 'rgate', 'kbota', 'swc', 'tagev', 'fseirei', 'tagms', 'l8warp', 'mmvbg', 'rcam')}
    tbox = {}
    warps = set()
    need_arcs = set()
    starts = {ENTRY_ROOM: []}
    gates = {}
    andsw2 = {}
    alldie = {}
    hd_swc = {}
    bad_swc_mark = []
    portal_sw = {}
    actor_names = set()
    room_scls = {}
    for n, b in arcs.items():
        if n[0] not in 'Rr':
            continue
        rn = int(n[1:3])
        a = rarc.Rarc(b, n)
        m = a.get('room.dzr')
        if m is None:
            continue
        dzr = m.data()
        sc = stagedata.scls(dzr)
        room_scls[rn] = sc
        for cname in stagedata.rcam(dzr):
            if cname not in cams:
                bad['rcam'].append('R%02d %s' % (rn, cname))
        for r in stagedata.records(dzr):
            nm, p, (ax, ay, az) = r['name'], r['prm'], r['ang']
            actor_names.add(nm)
            where = 'R%02d %s %s prm=%08x' % (rn, r['tag'], nm, p)
            if rn in REST_GATES and nm == 'rGate':
                gates.setdefault(rn, []).append(p & 0xFF)
            if rn in REST_GATES and nm == 'AND_SW2':
                andsw2.setdefault(rn, []).append(p)
            if rn in REST_GATES and nm == 'l8warp':
                portal_sw.setdefault(rn, []).append(p & 0xFF)
            if nm == 'ALLdie':
                alldie.setdefault(rn, []).append(p)
            if objnames.get(nm, ('',))[0] == 'fpcNm_SWC00_e' and (ax & 0xF000) != 0:
                hd_type = (ax & 0xF000) >> 12
                if hd_type in (10, 11) and (ax & 0xF00) >> 8 == 15:
                    hd_swc.setdefault(rn, []).append(hd_type)
                else:
                    bad_swc_mark.append(where + ' angle.x 0x%04x' % (ax & 0xFFFF))
            if nm not in objnames:
                bad['unknown'].append(where)
            if nm == 'rGate':
                t = (p >> 8) & 0xF
                if t > 1:
                    bad['rgate'].append(where)
                else:
                    need_arcs.add(('P_Rgate', 'SDGate')[t])          # d_a_obj_sWallShutter.cpp l_resNameIdx[2]
            if nm == 'Kbota_A':
                t = (p >> 16) & 0xF
                if t not in (0, 1, 2, 15):
                    bad['kbota'].append(where)
                else:
                    need_arcs.add(('Kbota_00', 'S_lv3bota', 'S_lv6bota')[0 if t == 15 else t])   # M_arcname[3]
            if objnames.get(nm, ('',))[0] == 'fpcNm_SWC00_e' and ((ax & 0xF00) >> 8) not in SWC00_TYPES:
                bad['swc'].append(where + ' type %d' % ((ax & 0xF00) >> 8))
            if nm == 'TagEv':
                inv, val = ax & 0x7FFF, az & 0xFFFF
                if (inv not in (0, 0x7FFF) and inv >= LABELS) or (val not in (0, 0xFFFF) and val >= LABELS):
                    bad['tagev'].append(where + ' labels %d/%d' % (inv, val))
            if nm == 'FSeirei' and (p & 0xFF) >= 4:
                bad['fseirei'].append(where)
            if nm == 'Tag_ms' and (az & 0xFFFF) not in gc_flows:
                bad['tagms'].append(where + ' flow %d' % (az & 0xFFFF))
            if nm == 'l8warp':
                si = (p >> 8) & 0xFF
                if si != 0xFF:
                    if si >= len(sc):
                        bad['l8warp'].append(where + ' scls %d of %d' % (si, len(sc)))
                    elif not gc_start_point(gc_files, sc[si]['stage'], sc[si]['room'], sc[si]['start']):
                        bad['l8warp'].append(where + ' -> %s room %d point %d not on GC disc' % (
                            sc[si]['stage'], sc[si]['room'], sc[si]['start']))
                    else:
                        warps.add('%s r%d p%d' % (sc[si]['stage'], sc[si]['room'], sc[si]['start']))
            if objnames.get(nm, ('',))[0] == 'fpcNm_SET_BG_OBJ_e' or nm == 'mmvbg':
                an = '@bg%04x' % (p & 0xFFFF)
                if not arc_exists(an):
                    bad['mmvbg'].append(where + ' ' + an)
            if nm.startswith('tbox'):
                no = (p >> 6) & 0x3F
                tbox.setdefault(no, []).append('R%02d' % rn)
            if rn in starts and r['tag'] == 'PLYR':
                starts[rn].append(az & 0xFF)
    labels = {'unknown': 'actor names known to Dusklight (l_objectName)',
              'rgate': 'rGate model type <= 1', 'kbota': 'Kbota_A model in {0,1,2,15}',
              'swc': 'swc00 types in {0..8,15}', 'tagev': 'TagEv labels < 822',
              'fseirei': 'no FSeirei type >= 4', 'tagms': 'Tag_ms flows exist in GC group %d' % cres['msg_group'],
              'l8warp': 'l8warp exits -> room SCLS -> GC start point', 'mmvbg': 'mmvbg @bg archives exist',
              'rcam': 'room RCAM types in GC camtype.dat'}
    for k, lab in labels.items():
        rep.add(G, lab, not bad[k], '; '.join(bad[k][:3]) if bad[k] else ('OK: ' + ', '.join(sorted(warps)) if k == 'l8warp' else 'OK'))
    miss = sorted(x for x in need_arcs if not arc_exists(x))
    rep.add(G, 'gate/switch model archives exist', not miss, ('MISSING %s' % miss) if miss else
            ', '.join(sorted(need_arcs)) + ' (GC disc)')
    dup = {no: rr for no, rr in tbox.items() if len(rr) > 1}
    rep.add(G, 'tbox numbers unique', not dup, ('dup %s' % dup) if dup else
            ', '.join('%d:%s' % (no, rr[0]) for no, rr in sorted(tbox.items())))
    rep.add(G, 'entry room (floor 1) has start point 0', chain_rooms[0] == ENTRY_ROOM and 0 in starts[ENTRY_ROOM],
            'R%02d PLYR points %s' % (ENTRY_ROOM, starts[ENTRY_ROOM]))
    okg = all(gates.get(r) == [sw] and REST_PORTAL_ANDSW2 in andsw2.get(r, []) and
              REST_GATE_ANDSW2[r] in andsw2.get(r, []) and portal_sw.get(r) == [0xC4]
              for r, sw in REST_GATES.items())
    rep.add(G, 'rest floors keep HD switch chains (portal C1&C2->C4, gate C2&C3->0x80+room)', okg,
            '; '.join('R%02d rGate sw %s, l8warp sw %s, AND_SW2 %s' % (
                r, ['0x%X' % x for x in gates.get(r, [])], ['0x%X' % x for x in portal_sw.get(r, [])],
                ['%08x' % x for x in andsw2.get(r, [])]) for r in sorted(REST_GATES)))
    okt = all(prm in alldie.get(r, []) for r, prm in TIER_ALLDIE.items())
    rep.add(G, 'tier ALLdie records (HD section rule)', okt,
            '; '.join('R%02d %s' % (r, ['%08x' % x for x in alldie.get(r, [])]) for r in sorted(TIER_ALLDIE)))
    oks = not bad_swc_mark and all(hd_swc.get(r) == [t] for r, t in HD_SWC_TYPES.items()) and         set(hd_swc) == set(HD_SWC_TYPES)
    rep.add(G, 'swc00 HD types 10/11 kept for the cave timer (type 15 + bits 12-15)', oks,
            ('BAD %s; ' % bad_swc_mark[:3] if bad_swc_mark else '') +
            '; '.join('R%02d %s' % (r, hd_swc.get(r, [])) for r in sorted(set(HD_SWC_TYPES) | set(hd_swc))))

    # stage
    a = rarc.Rarc(arcs['STG_00.arc'], 'STG_00.arc')
    dzs = a.get('stage.dzs').data()
    st = stagedata.stag(dzs)
    pno = (st['f0a'] >> 3) & 0xFF
    layer_p = [x for x in st['particleNo'] if x != 0xFF]
    check_particles(rep, G, gc_files, particle_dir, sorted(set([pno] + layer_p)), actor_names, cres)
    scams = stagedata.rcam(dzs)
    rep.add(G, 'stage RCAM types in GC camtype.dat', all(c in cams for c in scams), ', '.join(scams))
    tbl = (st['f09'] >> 1) & 0x1F
    gc_tbls = gc_save_tables(gc_files)
    users = sorted(k for k, v in gc_tbls.items() if v == tbl)
    rep.add(G, 'STAG save table unused by GC stages', not users,
            'table 0x%x; GC stages use %d other tables (%d stages checked)%s' % (
                tbl, len(set(gc_tbls.values())), len(gc_tbls), (' USED BY %s' % users) if users else ''))
    bank = a.get('name.bin')
    bnames = []
    if bank is not None:
        bd = bank.data()           # u8 count, then count x char[10] (dStage_roomControl_c::dStage_bankName)
        bnames = [bd[1 + 10 * i:11 + 10 * i].split(b'\0')[0].decode('latin-1') for i in range(bd[0])]
    missing = [x for x in bnames if not arc_exists(x)]
    rep.add(G, 'bank name.bin archives exist', not missing, ', '.join(bnames) + (' MISSING %s' % missing if missing else ''))
    # RTBL / MEC0 / MEM0
    rt = stagedata.rtbl(dzs)
    mem, mec = memfit.stage_mem_tables(dzs)
    by_room = {e['room']: e for e in los}
    probs = []
    every = chain + [e for e in los if e['floor'] > 0 and e['room'] not in set(chain_rooms)]
    for e in every:
        r = e['room']
        want = {r}
        if e['next'] >= 0:
            want.add(e['next'])
        if e['prev'] >= 0:
            want.add(e['prev'])
            pp = by_room.get(e['prev'])
            if e['next'] < 0 and pp and pp['prev'] >= 0:
                want.add(pp['prev'])
        got = {x & 0x3F for x in rt.get(r, [])}
        if got != want:
            probs.append('R%02d loads %s want %s' % (r, sorted(got), sorted(want)))
        blocks = [mec.get(x) for x in got]
        if None in blocks or len(set(blocks)) != len(blocks) or any(bk >= len(mem) for bk in blocks):
            probs.append('R%02d blocks %s' % (r, blocks))
        if any(x not in have_rooms for x in got):
            probs.append('R%02d loads a room without archive' % r)
    rep.add(G, 'RTBL = los chains, distinct MEC0 blocks', not probs,
            '; '.join(probs[:3]) if probs else '%d floors (%d main chain + %d detached), each loads self/next/prev '
            'in different blocks' % (len(every), len(chain), len(every) - len(chain)))
    # MEM0 fit (recomputed from the final archives, independent of the build's own numbers)
    stats = {int(n[1:3]): memfit.arc_stats(b) for n, b in arcs.items() if n[0] in 'Rr'}
    worst = None
    over = []
    for r, s in sorted(stats.items()):
        need = memfit.estimate(s)
        b_ = mec.get(r)
        if b_ is None or b_ >= len(mem):
            over.append('R%02d no block' % r)
            continue
        cap = memfit.block_bytes_pc(mem[b_])
        if need > cap:
            over.append('R%02d need %d > %d' % (r, need, cap))
        hr = (cap - need) / float(need)
        if worst is None or hr < worst[0]:
            worst = (hr, r, need, cap, s['arc'])
    total = sum(memfit.block_bytes_pc(x) for x in mem)
    # independent hard floor: what the PC game certainly allocates in the block, without memfit's constants
    hard = []
    hworst = None
    for n, b in sorted(arcs.items()):
        if n[0] not in 'Rr':
            continue
        r = int(n[1:3])
        b_ = mec.get(r)
        if b_ is None or b_ >= len(mem):
            continue
        a_ = rarc.Rarc(b, n)
        need = common.align(len(b), 32)
        for m in a_.files():
            if m.name.lower().endswith(('.bmd', '.bdl')):
                d = m.data()
                dl = j3d_dlcheck.pc_dl_bytes(d)
                nmat = sum(struct.unpack_from('>H', d, o + 8)[0] for t, o, z in j3d.sections(d)[2] if t[:3] == 'MAT')
                need += dl['copy'] + 0x180 * dl['shapes'] + HARD_MAT * nmat
        cap = memfit.block_bytes_pc(mem[b_])
        left = (cap - need) / float(cap)
        if left < 0.05:
            hard.append('R%02d floor %d cap %d (%.1f%% left)' % (r, need, cap, left * 100))
        if hworst is None or left < hworst[0]:
            hworst = (left, r, need, cap)
    rep.add(G, 'MEM0 hard floor (arc + PC DL copies + VcdVat + 1 KB/material) leaves >= 5%', not hard,
            '; '.join(hard[:3]) if hard else 'tightest R%02d floor %d of %d (%.1f%% left)' % (
                hworst[1], hworst[2], hworst[3], hworst[0] * 100))
    rep.add(G, 'MEM0 fits every room', not over,
            '; '.join(over[:3]) if over else 'MEM0 %s -> PC blocks %.2f MiB total; tightest R%02d arc %d need %d cap %d (+%.1f%%)'
            % (['0x%x' % x for x in mem], total / 1048576.0, worst[1], worst[4], worst[2], worst[3], worst[0] * 100))
    rows, tight, fails = memfit.calibrate(gc_files)
    rep.add(G, 'memfit constants vs GC disc', not fails,
            ('%d GC rooms with models and tight blocks: estimate >= 2x GC slack for all; tightest %s R%02d slack %d est %d'
             % (len(tight), tight[0]['stage'], tight[0]['room'], tight[0]['slack'], tight[0]['est_ovh'])) if (tight and not fails)
            else ('; '.join('%s R%02d slack %d est %d' % (f['stage'], f['room'], f['slack'], f['est_ovh']) for f in fails[:3]) or 'no tight GC rooms'))
    # floors.json
    fl = json.loads(common.read(os.path.join(res_dir, 'floors.json')).decode('utf-8'))
    okf = (len(fl) == len(chain) and [f['floor'] for f in fl] == list(range(1, len(fl) + 1)) and
           fl[-1]['kind'] == 'reward' and fl[-2]['kind'] == 'final' and
           all(f['room'] in have_rooms for f in fl) and
           all(set(f) == {'floor', 'room', 'next_room', 'prev_room', 'kind'} for f in fl))
    kinds = {}
    for f in fl:
        kinds.setdefault(f['kind'], []).append(f['floor'])
    rep.add(G, 'floors.json', okf, '%d floors, room %d first; rest %s, final %s, reward %s' % (
        len(fl), fl[0]['room'], kinds.get('rest'), kinds.get('final'), kinds.get('reward')))
    check_hd_tags(rep, G, os.path.join(res_dir, 'hd_tags.bin'))
    return arcs


def check_hd_tags(rep, G, path):
    """res/cave/hd_tags.bin: one HD Tag_ms record (flow facts.CAVE_TAG_FLOW) per facts.CAVE_TAG_ROOMS room."""
    import facts
    probs = []
    rooms = []
    d = common.read(path) if os.path.exists(path) else b''
    if len(d) < 8 or d[:4] != b'HDFT' or d[4] != 1 or len(d) != 8 + d[5] * 0x28:
        probs.append('missing or bad header/size (%d bytes)' % len(d))
    else:
        for i in range(d[5]):
            e = d[8 + i * 0x28:8 + (i + 1) * 0x28]
            rec = e[4:]
            rooms.append(e[0])
            az = struct.unpack_from('>H', rec, 0x1C)[0]
            if rec[:8].split(b'\0')[0] != b'Tag_ms' or az != facts.CAVE_TAG_FLOW:
                probs.append('room %d: not a Tag_ms record with flow %d' % (e[0], facts.CAVE_TAG_FLOW))
        if tuple(rooms) != tuple(facts.CAVE_TAG_ROOMS):
            probs.append('rooms %s, expected %s' % (rooms, list(facts.CAVE_TAG_ROOMS)))
    rep.add(G, 'hd_tags.bin (HD section-end talk tag places)', not probs, '; '.join(probs[:3]) if probs else
            '%d Tag_ms record(s), rooms %s' % (len(rooms), rooms))


def check_hd_chests(rep, G, path):
    """res/stamps/stamps/hd_chests.bin: the facts.HD_CHESTS chests' TRES records, each decoding to its chest
    number and stamp item."""
    import facts
    probs = []
    d = common.read(path) if os.path.exists(path) else b''
    n = d[5] if len(d) >= 8 else 0
    if len(d) < 8 or d[:4] != b'HDFC' or d[4] != 1 or len(d) != 8 + n * 0x24 or n != len(facts.HD_CHESTS):
        probs.append('missing or bad header/size (%d bytes)' % len(d))
    else:
        for i, (idx, stage, room, tbox, item) in enumerate(facts.HD_CHESTS):
            e = d[8 + i * 0x24:8 + (i + 1) * 0x24]
            rec = e[4:]
            prm = struct.unpack_from('>I', rec, 8)[0]
            if (e[0] != idx or e[2] != room or not rec.startswith(b'tbox') or ((prm >> 6) & 0x3F) != tbox or
                    rec[0x1C] != item):
                probs.append('stamp %d: record does not match (chest %d, item 0x%02X)' % (idx, tbox, item))
    rep.add(G, 'hd_chests.bin (7 HD-only chest records)', not probs, '; '.join(probs[:3]) if probs else
            '%d records, stamps %s' % (n, ', '.join(str(c[0]) for c in facts.HD_CHESTS)))


def check_particles(rep, G, gc_files, particle_dir, numbers, actor_names, cres):
    budget, big_name, big_size = particles.gc_budget(gc_files)
    for pno in numbers:
        fn = 'Pscene%03d.jpc' % pno
        on_gc = gc_has(gc_files, 'res', 'Particle', fn)
        ours = os.path.join(particle_dir, fn) if particle_dir else None
        if ours and os.path.exists(ours):
            if on_gc:
                rep.add(G, 'scene particle %s' % fn, False, 'the overlay would REPLACE the GC disc file')
                continue
            d = common.read(ours)
            hd = cres.get('hd')
            hdp = os.path.join(hd, 'res', 'Particle', fn) if hd else None
            universe = {r['id'] for r in particles.parse(common.read(hdp))['res']} if hdp and os.path.exists(hdp) else set()
            src = cres.get('src_root')              # None: facts.CAVE_PARTICLE_IDS
            need, _, _ = particles.needed_ids(src, actor_names, stagedata.objnames(), universe)
            must = set()
            for nm, ids in UNCHECKED_PARTICLES.items():
                if nm in actor_names:
                    must.update(ids)
            probs = particles.verify(d, need | must)
            if not universe:
                probs.append('HD %s not readable for the reference scan' % fn)
            if len(d) > big_size:
                probs.append('%d B > largest GC scene file %s %d B (resource heap)' % (len(d), big_name, big_size))
            rep.add(G, 'scene particle %s (mod overlay)' % fn, not probs, '; '.join(probs[:3]) if probs else
                    '%d B <= GC %s %d B; all %d ids the final room actors reference present (incl. %s)' % (
                        len(d), big_name, big_size, len(need | must), ', '.join('%04X' % x for x in sorted(must)) or '-'))
        else:
            detail = 'GC disc file' if on_gc else 'MISSING (not on the GC disc and not generated)'
            if pno == 200:
                detail += '; WARNING: GC 200 lacks particles the cave actors dereference (see particles.py)'
            rep.add(G, 'scene particle %s' % fn, on_gc and pno != 200, detail, warn=on_gc)


# --------------------------------------------------------------------------------------------- stamps
def check_stamps(rep, stamps_dir, sres):
    G = 'stamps'
    check_hd_chests(rep, G, os.path.join(stamps_dir, 'hd_chests.bin'))
    o = sres['obj_stamp']
    rep.add(G, 'Obj_Stamp stamp.bmd resource index', o['bmd_index'] == o['bmd_index_hd'] == 4,
            'entry %s (HD %s; HD item_resource uses 4)' % (o['bmd_index'], o['bmd_index_hd']))
    probs = []
    sizes = {}
    for i in range(50):
        fn = 'stamp%02d.png' % i
        p = os.path.join(stamps_dir, fn)
        if not os.path.exists(p):
            probs.append(fn + ' missing')
            continue
        try:
            w, h, px = common.png_read(common.read(p))
        except Exception as e:  # noqa: BLE001
            probs.append('%s: %r' % (fn, e))
            continue
        nz = sum(1 for k in range(3, len(px), 4) if px[k])
        if nz < 50:
            probs.append('%s nearly empty (%d px)' % (fn, nz))
        sizes['%dx%d' % (w, h)] = sizes.get('%dx%d' % (w, h), 0) + 1
    rep.add(G, '50 stamp PNGs decode, non-empty', not probs, '; '.join(probs[:3]) if probs else
            ', '.join('%d x %s' % (v, k) for k, v in sorted(sizes.items())))
    fr = [f for f in ('stamp_frame.png', 'stamp_empty.png', 'stamp_background.png')
          if os.path.exists(os.path.join(stamps_dir, f))]
    ok = True
    for f in fr:
        try:
            common.png_read(common.read(os.path.join(stamps_dir, f)))
        except Exception:  # noqa: BLE001
            ok = False
    rep.add(G, 'frame PNGs', ok and len(fr) == 3, ', '.join(fr))
    try:
        man = json.loads(common.read(os.path.join(stamps_dir, 'stamps.json')).decode('utf-8'))
        okm = len(man['stamps']) == 50 and [s['index'] for s in man['stamps']] == list(range(50))
    except Exception:  # noqa: BLE001
        okm = False
    rep.add(G, 'stamps.json', okm, '50 entries, index 0..49' if okm else 'bad manifest')


# --------------------------------------------------------------------------------------------- overlay
def check_overlay_paths(rep, gc_files, overlay_root, group):
    new, repl = [], []
    for dp, dn, fn in os.walk(overlay_root):
        for f in fn:
            rel = os.path.relpath(os.path.join(dp, f), overlay_root).replace('\\', '/')
            parts = rel.split('/')
            (repl if gc_has(gc_files, *parts) else new).append(rel)
    rep.add(group, 'overlay paths are new on the GC disc', not repl,
            ('replaces %s' % repl[:3]) if repl else '%d new file(s), nothing replaced' % len(new), warn=bool(repl))
    return new, repl
