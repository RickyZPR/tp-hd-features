"""Cave of Shadows (HD stage D_SB11) -> Dusklight overlay.

Per archive (STG_00 + R00..R49):
  1. hd2gc         GTX -> GX texture pixels (TEX1 / BTI), MAT4 tag -> MAT3          (tools/formats/hd2gc.py)
  2. mat4fix       material init data 0x14E -> 0x14C repack (57 of 71 HD models)    (tools/critic/mat4fix.py)
  3. bake_los      room model placement from content/los.bin baked into the vertices (tools/cave/bake_los.py)
  4. cave_patch    data fixes for unmodified GC/Dusklight actor code                (tools/cave/cave_patch.py):
                   rGate model types >=2 -> 1, Kbota_A model 3 -> 1, swc00 types 10/11 -> 15 with the HD type
                   kept in angle.x bits 12-15 (the mod's cave timer), TagEv labels >= 822 -> none, FSeirei type 4
                   deleted, Tag_ms with HD-only flows deleted, RCAM BonusDungeon -> Hall (the camera every GC Cave
                   of Ordeals floor uses: 49 D_SB01 room RCAMs are Hall), RTBL/MEC0 from the los floor chain,
                   rest/final l8warp exits (room SCLS -> F_SP124 start 6; the rest floors' AND_SW2 chains and the
                   ALLdie tier bytes stay HD's, the mod runs HD's tier rule), unique tbox numbers 60/61/62,
                   floor-40 gate opens on the floor's ALLdie switch (0xB1), MEM0 sized by memfit (now including
                   the PC display-list copies).
                   The STAG scene particle number stays HD's 206 (fix-cave: it used to be swapped for GC 200,
                   which lacks particles the cave actors dereference, see particles.py).
Particles: overlay/res/Particle/Pscene206.jpc, a GC-format scene particle file with every particle the cave's
actors reference, sized to fit Dusklight's particle resource heap (tools/pipeline/particles.py).
Objects: @bg0084/@bg0085 (stage bank name.bin + R24 mmvbg, looked up by name; GC-retail RARC layout), O_gD_puL4
(Colossal Wallet model, HD entry layout kept so HD's item_resource index 1 stays valid); optional HD gate/lift/block
models (--hd-gates, HD entry layout kept: their actors address resources by index).
res/: floors.json (los floor chain), colossal_wallet.png (HD item icon, for UI/toasts), hd_tags.bin (HD's
section-end talk tag records: the place and size of the mod's own talk tags on floors 5 and 19; hd_tag_records).
"""
import json
import os
import struct

import common
import convert
import memfit
import rarcx
import stagedata
import rarc
import rarc_write
import losbin
import bake_los
import cave_patch
import hdpack
import gtx2img
import particles

STAGE = 'D_SB11'
OBJ_STAGE = ['@bg0084', '@bg0085']                      # needed by name.bin (bank) and R24 mmvbg prm 0x84
OBJ_ITEM = ['O_gD_puL4']                                # Colossal Wallet item model (HD item 0xDA)
OBJ_HD_GATES = ['LOSGate1', 'LOSGate2', 'LOSGate3', 'LOSGate4', 'LOSGate5', 'los_Maze', 'S_losbota',
                'LOSL6Lbl', 'L8LiftSt']                 # only useful with code that extends the GC tables
EXIT = ('F_SP124', 6, 0)                                # GC D_SB01's own stage exit (Cave of Ordeals door)


def _list_stage(hd):
    d = os.path.join(hd, 'res', 'Stage', STAGE)
    names = sorted(n for n in os.listdir(d) if n.lower().endswith('.arc'))
    return d, [n for n in names if n.upper().startswith('STG')] + [n for n in names if n[0] in 'Rr']


def gc_flow_ids(gc_files, group):
    """flow ids present in GC res/Msgus/bmgres<group>.arc (FLI1)"""
    name = 'bmgres.arc' if group == 0 else 'bmgres%d.arc' % group
    p = os.path.join(gc_files, 'res', 'Msgus', name)
    a = rarc.Rarc.from_file(p)
    b = [m for m in a.files() if m.name.lower().endswith('.bmg')][0].data()
    n = struct.unpack_from('>I', b, 0xC)[0]
    o = 0x20
    flows = set()
    for _ in range(n):
        tag = b[o:o + 4]
        sz = struct.unpack_from('>I', b, o + 4)[0]
        if tag == b'FLI1':
            cnt = struct.unpack_from('>H', b, o + 8)[0]
            for i in range(cnt):
                flows.add(struct.unpack_from('>I', b, o + 0x10 + i * 8)[0] >> 16)
        o += sz
    return flows


def floor_chain(L):
    """main chain from the floor-1 room following next links; returns ordered list of los entries"""
    by_room = {e['room']: e for e in L}
    start = [e for e in L if e['floor'] == 1]
    if len(start) != 1:
        raise ValueError('los.bin: %d rooms with floor 1' % len(start))
    chain = []
    e = start[0]
    seen = set()
    while e is not None and e['room'] not in seen:
        seen.add(e['room'])
        chain.append(e)
        e = by_room.get(e['next']) if e['next'] >= 0 else None
    return chain


def floors_table(L, hd_room_records):
    chain = floor_chain(L)
    reward = chain[-1]['room']
    out = []
    for i, e in enumerate(chain):
        names = {r['name'] for r in hd_room_records.get(e['room'], [])}
        if e['next'] < 0:
            kind = 'reward'
        elif e['next'] == reward:
            kind = 'final'
        elif 'l8warp' in names:
            kind = 'rest'
        else:
            kind = 'fight'
        out.append({'floor': e['floor'], 'room': e['room'], 'next_room': e['next'] if e['next'] >= 0 else -1,
                    'prev_room': e['prev'] if e['prev'] >= 0 else -1, 'kind': kind})
    return out


def hd_tag_records(hd_dzr, hd_records):
    """[(room, the 0x24-byte HD SCOB record)] of HD's section-end talk tags (Tag_ms with facts.CAVE_TAG_FLOW in
    facts.CAVE_TAG_ROOMS). The asset patch removes these actors (their flow is HD-only); the mod re-adds them at
    the same place and size with its own flow (res/cave/hd_tags.bin)."""
    import facts
    out = []
    for room in facts.CAVE_TAG_ROOMS:
        hits = [r for r in hd_records.get(room, []) if r['name'] == 'Tag_ms' and r['tag'][:3] == 'SCO' and
                (r['ang'][2] & 0xFFFF) == facts.CAVE_TAG_FLOW]
        if len(hits) != 1:
            raise ValueError('D_SB11 room %d: %d Tag_ms records with flow %d (expected 1)' % (
                room, len(hits), facts.CAVE_TAG_FLOW))
        o = hits[0]['off']
        out.append((room, bytes(hd_dzr[room][o:o + 0x24])))
    return out


def hd_tags_bin(tags):
    """'HDFT' u8 version=1 u8 count u16 0, then per tag u8 room, u8[3] 0, u8 record[0x24]."""
    b = bytearray(b'HDFT' + bytes((1, len(tags), 0, 0)))
    for room, rec in tags:
        b += bytes((room, 0, 0, 0)) + rec
    return bytes(b)


def _icon_png(hd, name):
    p = os.path.join(hd, 'res', 'Layout', 'itemicon.pack.gz')
    tm = hdpack.Tmpk(hdpack.read_any(p))
    g = tm.get(name + '.bti.gtx')
    if g is None:
        return None
    s = hdpack.gtx_surfaces(g)['surfaces'][0]
    w, h, lin = gtx2img.deswizzle_level(s, g, 0)
    s2 = dict(s)
    s2['w'], s2['h'] = w, h
    return common.png_bytes(w, h, gtx2img.to_rgba(s2, lin))


def build(hd, gc_files, out, log, hd_gates=False, src_root=None):
    """out: dict with 'stage_dir', 'obj_dir', 'res_dir', 'particle_dir' (staging). src_root: the Dusklight
    v2.0.3 source checkout (for the particle reference scan). Returns a result dict."""
    src_root = src_root or None                     # None: particle ids from facts.py (no source scan)
    res = {'arcs': {}, 'objects': {}, 'report': [], 'conv': {}, 'patch_log': [], 'hd': hd, 'src_root': src_root}
    sdir, names = _list_stage(hd)
    los = losbin.load(os.path.join(hd, 'los.bin'))
    L = los['entries']
    res['los'] = los
    by_room = {e['room']: e for e in L}

    # HD (pre-patch) data: actor records per room, STAG message group, Tag_ms flows
    hd_records = {}
    hd_dzr = {}
    hd_stage_dzs = None
    for n in names:
        a = rarc.Rarc.from_file(os.path.join(sdir, n))
        if n.upper().startswith('STG'):
            hd_stage_dzs = a.get('stage.dzs').data()
        else:
            m = a.get('room.dzr')
            if m is not None:
                hd_dzr[int(n[1:3])] = m.data()
                hd_records[int(n[1:3])] = stagedata.records(m.data())
    msg_group = stagedata.stag(hd_stage_dzs)['msgGroup']
    gc_flows = gc_flow_ids(gc_files, msg_group)
    tagms_flows = sorted({r['ang'][2] & 0xFFFF for rr in hd_records.values() for r in rr if r['name'] == 'Tag_ms'})
    drop_flows = [f for f in tagms_flows if f not in gc_flows]
    res['msg_group'] = msg_group
    res['tagms_flows'] = tagms_flows
    res['drop_tagms'] = drop_flows
    log('cave: %d archives, los.bin %d entries, msg group %d, Tag_ms flows %s (HD-only: %s)',
        len(names), len(L), msg_group, tagms_flows, drop_flows)

    opts = {'no_rtbl': False, 'no_meco': False, 'cam': 'Hall', 'particle': None, 'mem': None,
            'gates': True, 'kbota': True, 'swc': True, 'tagev': True, 'fseirei': True,
            'drop_tagms': drop_flows, 'exit': EXIT, 'tbox_unique': True, 'fairy_bypass': True,
            'reward_item': None, 'banks_gc': False}
    res['opts'] = opts

    built = {}
    actor_names = set()
    stg = None
    tot = {'models': 0, 'mat_fixed': 0, 'textures': 0, 'no_source': 0, 'bti': 0, 'baked': 0}
    for n in names:
        p = os.path.join(sdir, n)
        a, repl, st, rep = convert.convert_arc(p)
        res['report'].extend(rep)
        res['conv'][n] = st
        for k in st:
            if k in tot:
                tot[k] += st[k]
        if n.upper().startswith('STG'):
            stg = (a, repl)
            m = a.get('stage.dzs')
            if m is not None:
                actor_names.update(r['name'] for r in stagedata.records(m.data()))
            continue
        rn = int(n[1:3])
        e = by_room.get(rn)
        if e and e['floor'] > 0:
            for path in list(repl):
                if path.lower().endswith(('.bmd', '.bdl')):
                    nb, mn, mx, nv = bake_los.bake_model(repl[path], (e['x'], e['y'], e['z']), e['angleY'])
                    repl[path] = nb
                    tot['baked'] += 1
                    res['report'].append('%s:%s baked los T=(%.0f,%.0f,%.0f) ang=%d -> bbox (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)'
                                         % (n, path, e['x'], e['y'], e['z'], e['angleY'], *mn, *mx))
        m = a.get('room.dzr')
        if m is not None:
            plog = []
            nd = cave_patch.patch_room(rn, m.data(), opts, plog)
            res['patch_log'].extend(plog)
            if nd != m.data():
                repl[m.path] = nd
            actor_names.update(r['name'] for r in stagedata.records(nd))
        built[n] = rarc_write.build_from_rarc(a, repl)
    res['totals'] = tot
    log('cave: converted %d models (%d MAT blocks repacked 0x14E->0x14C, %d baked), %d textures, %d BTI, '
        '%d without source', tot['models'], tot['mat_fixed'], tot['baked'], tot['textures'], tot['bti'], tot['no_source'])

    # MEM0: block of every room after the MEC0 rewrite, then size each block for its largest room
    a_stg, repl_stg = stg
    dzs_m = a_stg.get('stage.dzs')
    tmp_log = []
    dzs_nomem = cave_patch.patch_stage(dzs_m.data(), L, dict(opts, mem=None), tmp_log)
    mem_old, mec = memfit.stage_mem_tables(dzs_m.data())
    _, mec_new = memfit.stage_mem_tables(dzs_nomem)
    room_stats = {int(n[1:3]): memfit.arc_stats(b) for n, b in built.items()}
    mem0, rows = memfit.plan(room_stats, mec_new, len(mem_old))
    res['mem'] = {'old': mem_old, 'new': mem0, 'rows': rows, 'mec': mec_new}
    log('cave: MEM0 %s -> %s (PC blocks %s bytes, total %.2f MiB; was %.2f MiB)',
        ['0x%x' % x for x in mem_old], ['0x%x' % x for x in mem0],
        [memfit.block_bytes_pc(x) for x in mem0], sum(memfit.block_bytes_pc(x) for x in mem0) / 1048576.0,
        sum(memfit.block_bytes_pc(x) for x in mem_old) / 1048576.0)
    plog = []
    repl_stg[dzs_m.path] = cave_patch.patch_stage(dzs_m.data(), L, dict(opts, mem=mem0), plog)
    res['patch_log'].extend(plog)
    built[[n for n in names if n.upper().startswith('STG')][0]] = rarc_write.build_from_rarc(a_stg, repl_stg)

    for n, b in built.items():
        common.write(os.path.join(out['stage_dir'], n), b)
        res['arcs'][n] = len(b)

    # scene particles (Pscene206.jpc, GC format, see particles.py)
    hd206 = common.read(os.path.join(hd, 'res', 'Particle', particles.STAGE_FILE))
    gc200 = common.read(os.path.join(gc_files, 'res', 'Particle', 'Pscene200.jpc'))
    gtx = hdpack.read_any(os.path.join(hd, 'tex', 'Particle', particles.STAGE_FILE + '.gtx.gz'))
    universe = {r['id'] for r in particles.parse(hd206)['res']}
    need, prep, need_src = particles.needed_ids(src_root, actor_names, stagedata.objnames(), universe)
    if particles.is_checkout(src_root):
        import facts
        table = set(facts.CAVE_PARTICLE_IDS) & universe
        if need != table:
            prep.append('particles: WARNING: the source scan differs from facts.CAVE_PARTICLE_IDS (+%s -%s); '
                        'update tools/pipeline/facts.py' % (
                            ' '.join('%04X' % x for x in sorted(need - table)) or '-',
                            ' '.join('%04X' % x for x in sorted(table - need)) or '-'))
            log('cave: %s', prep[-1])
    budget, big_name, big_size = particles.gc_budget(gc_files)
    pdata, prep2, pstats = particles.build_scene(gc200, hd206, gtx, need, budget)
    common.write(os.path.join(out['particle_dir'], particles.STAGE_FILE), pdata)
    res['particles'] = {'need': need, 'need_src': need_src, 'stats': pstats, 'budget_ref': (big_name, big_size),
                        'actor_names': sorted(actor_names)}
    res['report'].extend(prep + prep2)
    log('cave: %s', prep[0])
    log('cave: %s (budget = GC %s %d B - %d)', prep2[-1], big_name, big_size, particles.SAFETY)
    if pstats['halved']:
        log('cave: particle textures stored at half size to fit the heap: %s', ', '.join(pstats['halved']))

    # object archives
    objs = list(OBJ_STAGE) + list(OBJ_ITEM) + (list(OBJ_HD_GATES) if hd_gates else [])
    odir = os.path.join(hd, 'res', 'Object')
    for o in objs:
        p = common.exists_ci(odir, o + '.arc')
        if p is None:
            raise FileNotFoundError('HD object %s.arc not found' % o)
        a, repl, st, rep = convert.convert_arc(p)
        res['report'].extend(rep)
        res['conv'][o + '.arc'] = st
        if o in OBJ_ITEM or o in OBJ_HD_GATES:
            # addressed by resource INDEX (item_resource; sWallShutter l_bmdIdx/l_dzbIdx, swpush M_bmd):
            # keep HD's entry layout so HD's index tables (Zelda.rpx / tphd branch) stay valid
            b = rarcx.rebuild_same_layout(a, repl)
            layout = 'HD entry layout kept'
        else:
            b = rarc_write.build_from_rarc(a, repl)
            layout = 'GC retail layout'
        fn = os.path.basename(p)
        common.write(os.path.join(out['obj_dir'], fn), b)
        res['objects'][fn] = {'size': len(b), 'layout': layout, 'models': st['models'], 'mat_fixed': st['mat_fixed']}
    log('cave: %d object archives: %s', len(objs), ', '.join(sorted(res['objects'])))

    # res/
    floors = floors_table(L, hd_records)
    nl = chr(10)
    txt = '[' + nl + (',' + nl).join(' ' + json.dumps(f) for f in floors) + nl + ']' + nl     # one floor per line
    common.write(os.path.join(out['res_dir'], 'floors.json'), txt.encode('utf-8'))
    res['floors'] = floors
    icon = _icon_png(hd, 'ni_saifu4_48')
    if icon:
        common.write(os.path.join(out['res_dir'], 'colossal_wallet.png'), icon)
    res['icon'] = bool(icon)
    tags = hd_tag_records(hd_dzr, hd_records)
    common.write(os.path.join(out['res_dir'], 'hd_tags.bin'), hd_tags_bin(tags))
    res['hd_tags'] = tags
    log('cave: hd_tags.bin, %d HD section-end talk tag record(s) (rooms %s)', len(tags),
        ', '.join(str(t[0]) for t in tags))
    kinds = {}
    for f in floors:
        kinds.setdefault(f['kind'], []).append(f['floor'])
    log('cave: floors.json %d floors (rest %s, final %s, reward %s); off-chain los rooms: %s',
        len(floors), kinds.get('rest'), kinds.get('final'), kinds.get('reward'),
        sorted(e['room'] for e in L if e['floor'] > 0 and e['room'] not in {f['room'] for f in floors}))
    return res
