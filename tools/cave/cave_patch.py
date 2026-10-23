"""cave_patch.py - data-only fixes so the HD Cave of Shadows (D_SB11) stage/room data behaves with *unmodified*
GC/Dusklight actor code. Operates on (converted, optionally los-baked) D_SB11 archives; reads in_dir, writes out_dir.

usage: python -I cave_patch.py <los.bin> <in_stage_dir> <out_stage_dir> [options] [--report out.txt]
  --no-rtbl         keep the file RTBL (linear placeholder) instead of rebuilding it from the los floor chain
  --no-meco         keep MEC0 (roomNo-based memory blocks) instead of floor_index % 3
  --cam NAME        stage RCAM camera type replacing HD-only "BonusDungeon" (default: Hall; GC camtype.dat lacks it)
  --mem BYTES[,BYTES...]  set the MEM0 memory block sizes (one value = every block, or one value per
                    block; default: keep 0x15a180; Dusklight doubles it on PC)
  --gates           rGate (Obj_SwallShutter) model types >=2 -> 1 (SDGate)  [GC l_resNameIdx has 2 entries]
  --kbota           Kbota_A (Obj_Swpush) model 3 -> 1 (S_lv3bota)            [GC M_arcname has 3 entries]
  --swc             SwAreaC/S (swc00) HD types 10/11 -> 15 (GC type with the same 'sw2 on -> set sw1, then
                    delete' rule) and the HD type kept in angle.x bits 12-15 (0xA/0xB), which neither GC nor HD
                    code reads (type = bits 8-11, condition = bits 0-7). The HD Features mod reads it back
                    (hooks.cpp post_swc00Execute) for HD's cave timer: type 10 starts it, type 11 stops it.
  --tagev           TagEv valid/invalid event labels >= 822 (HD-only saveBitLabels) -> none
  --exit S,START,ROOM  add a room SCLS[0] to the rest/final floors (R25, R34, R14) and point their l8warp at it.
                    The rest floors' AND_SW2 chains stay HD's (C1&C2->C4 portal, C2&C3->0x99/0xA2 descent gate):
                    C1 or C3 comes from the floor's tier ALLdie, which the mod runs with HD's rule
                    (hooks.cpp pre_alldieActionTimer). Until 2026-10-19 the portal's AND_SW2 was rewritten to
                    C2->C4 and the mod refused 0x99/0xA2 instead.
  --particle N      STAG scene particle number (field_0x0a bits 3-10; HD 206 = Pscene206.jpc, HD-only file).
                    Default: keep 206 (fix-cave, 2026-10-18). tools/build_assets.py generates a GC-format
                    Pscene206.jpc (tools/pipeline/particles.py). Do NOT use 200: GC Pscene200 lacks particles the
                    cave actors need (kytag10 Sparks 0x852B/0x852C are dereferenced without a NULL check ->
                    crash on floor 10/11; Zant Head shots and magma poles become invisible).
  --tbox-unique     give the 3 cave chests (all tboxNo 63 = one shared save bit) tboxNo 60/61/62
  --fairy-bypass    R49 rGate (to the reward room) opens on the floor-40 ALLdie switch 0xB1 instead of 0xC0
  --reward-item N   item byte of the R14 reward chest (HD: 0x07 silver rupee; e.g. 0x36 = giant wallet)
  --banks-gc        STG bnk/name.bin @bg0084/@bg0085 -> GC @bg0050/@bg0051 (room preload list; HD arcs not needed)
  --fseirei         delete FSeirei (NPC_FAIRY_SEIREI) records of HD type 4 (R49 end fairy). GC isDelete() keeps it
                    alive once F_0505 (Cave of Ordeals B50 cleared) is set, and _to_FairyCave then indexes
                    sTempBit[4] out of bounds; its flow 1122 is not in GC text either. Use with --fairy-bypass.
  --drop-tagms F,F  delete Tag_ms (TAG_MSG) records whose flow (angle.z) is in the list, e.g. the HD-only flow
                    1121 of R10/R48 (a missing flow -> getInitNodeIndex 0xFFFF -> out-of-bounds node read)
  --all             = --gates --kbota --swc --tagev --fseirei

CHANGES (assets pipeline, 2026-10-18): --mem takes per-block values; added --fseirei and --drop-tagms
(record deletion: the chunk's later records move up, the count drops, the freed tail is zero-filled; no
other chunk offset changes); --all now includes --fseirei.
CHANGES (HD sections, 2026-10-19): --swc keeps the HD type in angle.x bits 12-15; --exit no longer rewrites the
rest floors' AND_SW2 c1c40002 (research/cos_sections_spec.md M6).
Everything defaults to OFF except RTBL/MEC0/camera, which are needed for the stage to work at all.
"""
import sys, os, glob, struct
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, '..', 'formats'))
import yaz0, rarc, rarc_write, losbin

LAYER = '0123456789abcde'


def chunks(b):
    n = struct.unpack_from('>i', b, 0)[0]
    out = []
    for i in range(n):
        tag, cnt, off = struct.unpack_from('>4siI', b, 4 + i * 12)
        out.append([tag.decode('latin-1'), cnt, off, 4 + i * 12])
    return out


def is_actor_tag(t):
    return t in ('ACTR', 'TGOB', 'PLYR', 'TRES') or (t[:3] in ('ACT', 'TRE') and t[3] in LAYER)


def is_scob_tag(t):
    return t in ('SCOB', 'TGSC', 'TGDR', 'Door') or (t[:3] in ('SCO', 'Doo') and t[3] in LAYER)


def patch_stage(b, L, opts, rep):
    b = bytearray(b)
    ch = {c[0]: c for c in chunks(b)}
    floor = {e['room']: e['floor'] for e in L}
    nxt = {e['room']: e['next'] for e in L}
    prv = {e['room']: e['prev'] for e in L}
    if not opts.get('no_rtbl'):
        # Neighbours are loaded with flag 0x80 (create BG), exactly like GC's own Cave of Ordeals RTBL (D_SB01 r1 =
        # [c1, 80, 82]): the next floor's actors are created and run one floor early. HD/tphd only set 0x80 on
        # the next floor once that floor's gate switch (0x80 + room) is on (dStage_LOSRoomReadOverride); that
        # needs code, and the GC behaviour is known to work, so it is kept (documented in the mod README).
        tag, cnt, off, _ = ch['RTBL']
        ents = [struct.unpack_from('>I', b, off + 4 * i)[0] for i in range(cnt)]
        roff = [struct.unpack_from('>I', b, e + 4)[0] for e in ents]
        for r in range(cnt):
            cap = (roff[r + 1] - roff[r]) if r + 1 < cnt else 3
            want = [(r & 0x3F) | (b[roff[r]] & 0xC0)]
            if nxt.get(r, -1) >= 0:
                want.append(nxt[r] | 0x80)
            if prv.get(r, -1) >= 0:
                want.append(prv[r] | 0x80)
            if nxt.get(r, -1) < 0 and prv.get(r, -1) >= 0 and prv.get(prv[r], -1) >= 0:
                want.append(prv[prv[r]] | 0x80)
            if len(want) > cap:
                raise ValueError('RTBL room %d: no room for %d entries (cap %d)' % (r, len(want), cap))
            old = list(b[roff[r]:roff[r] + b[ents[r]]])
            b[ents[r]] = len(want)
            b[roff[r]:roff[r] + len(want)] = bytes(want)
            if old != want:
                rep.append('stage RTBL r%-2d %s -> %s' % (r, ['%02x' % x for x in old], ['%02x' % x for x in want]))
    if not opts.get('no_meco'):
        tag, cnt, off, _ = ch['MEC0']
        for i in range(cnt):
            rn, blk = b[off + 2 * i], b[off + 2 * i + 1]
            if floor.get(rn, 0) > 0:
                nb = floor[rn] % 3
                if nb != blk:
                    b[off + 2 * i + 1] = nb
                    rep.append('stage MEC0 room %d block %d -> %d (floor %d)' % (rn, blk, nb, floor[rn]))
    if opts.get('cam'):
        tag, cnt, off, _ = ch['RCAM']
        for i in range(cnt):
            o = off + i * 0x18
            name = bytes(b[o:o + 16]).split(b'\0')[0]
            if name == b'BonusDungeon':
                b[o:o + 16] = opts['cam'].encode().ljust(16, b'\0')
                rep.append('stage RCAM[%d] BonusDungeon -> %s' % (i, opts['cam']))
    if opts.get('particle') is not None:
        tag, cnt, off, _ = ch['STAG']
        v = struct.unpack_from('>H', b, off + 0xA)[0]
        nv = (v & ~(0xFF << 3)) | ((opts['particle'] & 0xFF) << 3)
        if nv != v:
            struct.pack_into('>H', b, off + 0xA, nv)
            rep.append('stage STAG particleNo %d -> %d (field_0x0a 0x%04x -> 0x%04x)' % ((v >> 3) & 0xFF, opts['particle'], v, nv))
    if opts.get('mem') is not None:
        tag, cnt, off, _ = ch['MEM0']
        mem = opts['mem'] if isinstance(opts['mem'], (list, tuple)) else [opts['mem']] * cnt
        if len(mem) != cnt:
            raise ValueError('MEM0 has %d blocks, got %d sizes' % (cnt, len(mem)))
        for i in range(cnt):
            old = struct.unpack_from('>I', b, off + 4 * i)[0]
            struct.pack_into('>I', b, off + 4 * i, mem[i])
            rep.append('stage MEM0[%d] 0x%x -> 0x%x' % (i, old, mem[i]))
    return bytes(b)


def add_scls(b, scls_rec):
    """Insert an SCLS chunk (1 entry) into a dzr: 12-byte header growth -> shift all chunk offsets by 12."""
    b = bytearray(b)
    cl = chunks(b)
    if any(c[0] == 'SCLS' for c in cl):
        return bytes(b), False
    shift = 12
    # chunk offsets are absolute file offsets; RPAT/PATH point offsets are relative to the RPPN/PPNT chunk
    # (dStage_rpatInfoInit adds getPnt2Inf()->m_pnt_offset), so they must NOT be shifted.
    for tag, cnt, off, ho in cl:
        struct.pack_into('>I', b, ho + 8, off + shift)
    n = len(cl)
    hdr_end = 4 + n * 12
    body = bytes(b[hdr_end:])
    tail_off = len(b) + shift
    tail_off = (tail_off + 3) & ~3
    newhdr = bytes(b[:hdr_end]) + struct.pack('>4siI', b'SCLS', 1, tail_off)
    out = bytearray(newhdr + body)
    out += b'\0' * (tail_off - len(out))
    out += scls_rec + b'\0' * ((-len(scls_rec)) % 4)
    struct.pack_into('>i', out, 0, n + 1)
    return bytes(out), True


def drop_records(rn, b, should_drop, rep):
    """Delete actor/SCOB records for which should_drop(tag, name, params, angles) is true. Records after a
    deleted one move up inside their chunk, the chunk count drops and the freed tail is zero-filled, so no
    chunk offset (and nothing else in the file) moves."""
    b = bytearray(b)
    for tag, cnt, off, ho in chunks(b):
        if not (is_actor_tag(tag) or is_scob_tag(tag)):
            continue
        stride = 0x20 if is_actor_tag(tag) else 0x24
        keep = []
        for i in range(cnt):
            o = off + i * stride
            rec = bytes(b[o:o + stride])
            name = rec[:8].split(b'\0')[0].decode('latin-1')
            p = struct.unpack_from('>I', rec, 8)[0]
            ang = struct.unpack_from('>hhh', rec, 0x18)
            if should_drop(tag, name, p, ang):
                rep.append('R%02d %s %s prm %08x ang %s deleted' % (rn, tag, name, p, ang))
            else:
                keep.append(rec)
        if len(keep) != cnt:
            blob = b''.join(keep)
            b[off:off + cnt * stride] = blob + b'\0' * (cnt * stride - len(blob))
            struct.pack_into('>i', b, ho + 4, len(keep))
    return bytes(b)


def patch_room(rn, b, opts, rep):
    b = bytearray(b)
    for tag, cnt, off, _ in chunks(b):
        if is_actor_tag(tag) or is_scob_tag(tag):
            stride = 0x20 if is_actor_tag(tag) else 0x24
            for i in range(cnt):
                o = off + i * stride
                name = bytes(b[o:o + 8]).split(b'\0')[0].decode('latin-1')
                p = struct.unpack_from('>I', b, o + 8)[0]
                ax, ay, az = struct.unpack_from('>hhh', b, o + 0x18)
                np_ = p
                if opts.get('gates') and name == 'rGate' and ((p >> 8) & 0xF) >= 2:
                    np_ = (p & ~0xF00) | 0x100
                if opts.get('kbota') and name == 'Kbota_A' and ((p >> 16) & 0xF) == 3:
                    np_ = (p & ~0xF0000) | 0x10000
                if opts.get('exit') and name == 'l8warp':
                    np_ = (p & ~0xFF00)
                # floor 40: the gate to the reward room opens on zone switch 0xC0, which only the HD end-fairy
                # flow 1122 sets (event014 zone 0). GC FSeirei is deleted unless F_0505 and has no flow 1122.
                if opts.get('fairy_bypass') and rn == 49 and name == 'rGate' and (p & 0xFF) == 0xC0:
                    np_ = (np_ & ~0xFF) | 0xB1
                if opts.get('reward_item') is not None and rn == 14 and name.startswith('tbox'):
                    naz = (az & 0xFF) | (opts['reward_item'] << 8)
                    naz = naz - 0x10000 if naz >= 0x8000 else naz
                    if naz != az:
                        struct.pack_into('>h', b, o + 0x1C, naz)
                        rep.append('R14 %s %s item 0x%02x -> 0x%02x' % (tag, name, (az >> 8) & 0xFF, opts['reward_item']))
                        az = naz
                if opts.get('tbox_unique') and name.startswith('tbox') and ((p >> 6) & 0x3F) == 63 and rn in (25, 34, 14):
                    np_ = (p & ~(0x3F << 6)) | ({25: 60, 34: 61, 14: 62}[rn] << 6)
                if np_ != p:
                    struct.pack_into('>I', b, o + 8, np_)
                    rep.append('R%02d %s %s prm %08x -> %08x' % (rn, tag, name, p, np_))
                if opts.get('swc') and name in ('SwAreaC', 'SwAreaS'):
                    t = ((ax & 0xFFFF) >> 8) & 0xF
                    if t in (10, 11):
                        # GC type 15 + the HD type in the free top nibble (idempotent: a patched record has type 15)
                        nax = (ax & 0x00FF) | (15 << 8) | (t << 12)
                        nax = nax - 0x10000 if nax >= 0x8000 else nax
                        struct.pack_into('>h', b, o + 0x18, nax)
                        rep.append('R%02d %s %s type %d -> 15, HD type kept in bits 12-15 (angle.x 0x%04x -> 0x%04x)'
                                   % (rn, tag, name, t, ax & 0xFFFF, nax & 0xFFFF))
                if opts.get('tagev') and name == 'TagEv':
                    inv = ax & 0x7FFF
                    val = az & 0xFFFF
                    nax, naz = ax, az
                    if 822 <= inv < 0x7FFF:
                        nax = (ax & 0x8000) | 0x7FFF
                        nax = nax - 0x10000 if nax >= 0x8000 else nax
                    if 822 <= val < 0xFFFF:
                        naz = -1
                    if (nax, naz) != (ax, az):
                        struct.pack_into('>hhh', b, o + 0x18, nax, ay, naz)
                        rep.append('R%02d %s TagEv labels inv %d val %d -> inv %d val %d' % (rn, tag, inv, val, nax & 0x7FFF, naz & 0xFFFF))
    out = bytes(b)
    drop_flows = set(opts.get('drop_tagms') or ())
    if opts.get('fseirei') or drop_flows:
        def _drop(tag, name, p, ang):
            if opts.get('fseirei') and name == 'FSeirei' and (p & 0xFF) == 4:
                return True
            if name == 'Tag_ms' and (ang[2] & 0xFFFF) in drop_flows:
                return True
            return False
        out = drop_records(rn, out, _drop, rep)
    if opts.get('exit') and rn in (25, 34, 14):
        st, start, room = opts['exit']
        rec = st.encode().ljust(8, b'\0') + struct.pack('>BbBBB', start, room, 0xF0, 0x1F, 1)
        out, added = add_scls(out, rec)
        if added:
            rep.append('R%02d added room SCLS[0] -> %s start %d room %d' % (rn, st, start, room))
    return out


def main(argv):
    losp, ind, outd = argv[1:4]
    a = argv[4:]
    opts = {'no_rtbl': '--no-rtbl' in a, 'no_meco': '--no-meco' in a, 'cam': 'Hall', 'mem': None, 'particle': None,
            'tbox_unique': '--tbox-unique' in a, 'banks_gc': '--banks-gc' in a,
            'fairy_bypass': '--fairy-bypass' in a, 'reward_item': None}
    if '--reward-item' in a:
        opts['reward_item'] = int(a[a.index('--reward-item') + 1], 0)
    if '--particle' in a:
        v = a[a.index('--particle') + 1]
        opts['particle'] = None if v == 'keep' else int(v, 0)
    if '--cam' in a:
        opts['cam'] = a[a.index('--cam') + 1]
    if '--mem' in a:
        v = [int(x, 0) for x in a[a.index('--mem') + 1].split(',')]
        opts['mem'] = v[0] if len(v) == 1 else v
    for k in ('gates', 'kbota', 'swc', 'tagev', 'fseirei'):
        opts[k] = ('--' + k in a) or ('--all' in a)
    if '--drop-tagms' in a:
        opts['drop_tagms'] = [int(x, 0) for x in a[a.index('--drop-tagms') + 1].split(',')]
    if '--exit' in a:
        s, st, r = a[a.index('--exit') + 1].split(',')
        opts['exit'] = (s, int(st), int(r))
    L = losbin.load(losp)['entries']
    os.makedirs(outd, exist_ok=True)
    rep = []
    for p in sorted(glob.glob(os.path.join(glob.escape(ind), '*.arc'))):
        bn = os.path.basename(p)
        arc = rarc.Rarc(open(p, 'rb').read(), bn)
        repl = {}
        for m in arc.files():
            if bn.upper().startswith('STG') and m.name == 'stage.dzs':
                repl[m.path] = patch_stage(m.data(), L, opts, rep)
            elif bn.upper().startswith('STG') and m.name == 'name.bin' and opts.get('banks_gc'):
                nb = m.data().replace(b'@bg0084', b'@bg0050').replace(b'@bg0085', b'@bg0051')
                if nb != m.data():
                    repl[m.path] = nb
                    rep.append('stage bnk/name.bin @bg0084/@bg0085 -> @bg0050/@bg0051')
            elif m.name == 'room.dzr':
                rn = int(bn[1:3])
                nd = patch_room(rn, m.data(), opts, rep)
                if nd != m.data():
                    repl[m.path] = nd
        data = rarc_write.build_from_rarc(arc, repl) if repl else yaz0.maybe_decompress(open(p, 'rb').read())
        open(os.path.join(outd, bn), 'wb').write(data)
    txt = '\n'.join(rep) + '\n'
    if '--report' in a:
        open(a[a.index('--report') + 1], 'w').write(txt)
    print(txt)


if __name__ == '__main__':
    main(sys.argv)
