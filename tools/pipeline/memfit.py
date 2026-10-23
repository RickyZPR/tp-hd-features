"""Room memory-block (STAG MEM0) sizing for the converted D_SB11 rooms.

How Dusklight v2.0.3 uses MEM0 (src/d/d_stage.cpp, d_s_room.cpp, d_resorce.cpp, actor/d_a_bg.cpp):
  - dStage_memaInfoInit: block i = JKRCreateExpHeap((MEM0[i] + 0x380) * 2, archive heap)   (x2: TARGET_PC)
  - MEC0 maps each room to a block; RTBL loads <= 3 rooms at once (self, next, prev[, prev-prev]); the
    cave patch makes MEC0 = los floor % 3 so those always use three different blocks.
  - In a block: the room .arc mounted as a JKRMemArchive (whole file, 32-aligned, TAIL), the
    dRes_info_c solid heap (mRes[] + J3DModelData / material / shape objects / shared DLs / anim objects;
    KCL and vertex arrays are converted in place), and daBg_c's solid heap (J3DModel instances, packets,
    matrix buffers). Solid heaps take the free space then shrink (adjustSolidHeap), so there is no
    fragmentation to account for; room_of_scene_class waits until the block is empty before mounting.

Estimate per room (bytes, PC):  align32(arc) + ARC_OVH + sum over models of
      MDL + MAT * materials + SHP * shapes + TEX * textures + JNT * joints  +  ANM * animations
      + DL copies + VCDVAT * shapes
The object constants are deliberately generous PC (64-bit) upper bounds (J3DMaterial + its 5 blocks + tex
matrices ~1.5 KB on GC, a shared DL and a material packet with differed DLs < 1.5 KB more; a J3DShape with
its matrix/draw objects and packet < 1 KB). calibrate() checks them against the GC disc: for every GC room
that has models and a tight block (slack = MEM0+0x380 - arc < 64 KB) the estimate must be >= 2 x slack.
Note: the PC build does NOT use the display lists in place:
every J3DShapeDraw copies its packet DL into the room block's resource heap after aurora's optimizer
expands strips/fans/quads into indexed triangles (J3DShapeFactory.cpp:135, J3DShapeDraw.cpp
set_display_list_copy, extern/aurora/lib/gx/dl.cpp optimize), and J3DShapeFactory allocates
J3DShape::kVcdVatDLSize (0x180 on PC) per shape. Those copies are often as large as the room archive's whole
model, so they are counted exactly (j3d_dlcheck.pc_dl_bytes) instead of being hidden in the x2 assumption
(the earlier "PC objects are at most twice their GC size" note does not hold for DLs: GC copies nothing).
Block size = max estimate of its rooms * (1 + MARGIN).
"""
import struct

import common
import rarc
import j3d
import j3d_dlcheck

ARC_OVH = 0x4000      # JKRMemArchive object, heap block headers, mRes[], dBgWKCol / room misc
MDL = 0x2000          # per model: J3DModelData + J3DModel + matrix buffers + joint tree
MAT = 0x1800          # per material (6 KB)
SHP = 0x800           # per shape (2 KB)
TEX = 0x100           # per texture entry
JNT = 0x200           # per joint
ANM = 0x1000          # per btk/brk/bck/btp/bpk/bva/bla animation resource
VCDVAT = 0x180        # J3DShape::kVcdVatDLSize on TARGET_PC (J3DShape.h), one buffer per shape
MARGIN = 0.10         # extra headroom on top of the largest estimate per block
PC_FACTOR = 2         # createMemoryBlock() doubles MEM0 + 0x380 on TARGET_PC


def model_stats(d):
    st = {'mat': 0, 'shp': 0, 'jnt': 0, 'tex': 0, 'dl': 0}
    try:
        _, _, secs = j3d.sections(d)
    except Exception:  # noqa: BLE001
        return st
    try:
        st['dl'] = j3d_dlcheck.pc_dl_bytes(d)['copy']
    except Exception:  # noqa: BLE001
        st['dl'] = 0
    for tag, o, s in secs:
        if tag in ('MAT3', 'MAT4', 'MAT2'):
            st['mat'] += struct.unpack_from('>H', d, o + 8)[0]
        elif tag == 'SHP1':
            st['shp'] += struct.unpack_from('>H', d, o + 8)[0]
        elif tag == 'JNT1':
            st['jnt'] += struct.unpack_from('>H', d, o + 8)[0]
        elif tag == 'TEX1':
            st['tex'] += struct.unpack_from('>H', d, o + 8)[0]
    return st


def arc_stats(buf):
    a = rarc.Rarc(buf, 'x')
    s = {'arc': len(a.buf), 'models': 0, 'mat': 0, 'shp': 0, 'jnt': 0, 'tex': 0, 'dl': 0, 'anm': 0, 'files': 0}
    for m in a.files():
        s['files'] += 1
        n = m.name.lower()
        if n.endswith(('.bmd', '.bdl')):
            ms = model_stats(m.data())
            s['models'] += 1
            for k in ('mat', 'shp', 'jnt', 'tex', 'dl'):
                s[k] += ms[k]
        elif n.endswith(('.btk', '.brk', '.bck', '.btp', '.bpk', '.bva', '.bla', '.bxa')):
            s['anm'] += 1
    return s


def estimate(s):
    return (common.align(s['arc'], 32) + ARC_OVH + MDL * s['models'] + MAT * s['mat'] + SHP * s['shp'] +
            TEX * s['tex'] + JNT * s['jnt'] + ANM * s['anm'] + s.get('dl', 0) + VCDVAT * s['shp'])


def mem0_for(need):
    """smallest MEM0 (multiple of 0x20) with (MEM0 + 0x380) * 2 >= need * (1 + MARGIN)"""
    want = int(need * (1.0 + MARGIN) + 0.999)
    m = (want + PC_FACTOR - 1) // PC_FACTOR - 0x380
    return common.align(max(m, 0x20), 0x20)


def block_bytes_pc(mem0):
    return (mem0 + 0x380) * PC_FACTOR


def plan(room_stats, room_block, nblocks=3):
    """room_stats: {room: arc_stats}; room_block: {room: block}. Returns (mem0 list, per-room rows)."""
    need = {r: estimate(s) for r, s in room_stats.items()}
    mem0 = []
    for b in range(nblocks):
        rooms = [r for r in need if room_block.get(r) == b]
        mx = max([need[r] for r in rooms] + [0x8000])
        mem0.append(mem0_for(mx))
    rows = []
    for r in sorted(need):
        b = room_block.get(r)
        cap = block_bytes_pc(mem0[b]) if b is not None and b < nblocks else None
        rows.append({'room': r, 'block': b, 'arc': room_stats[r]['arc'], 'need': need[r], 'cap': cap,
                     'headroom': (cap - need[r]) if cap else None, 'stats': room_stats[r]})
    return mem0, rows


def stage_mem_tables(dzs):
    """(MEM0 list, MEC0 {room: block}) from a stage.dzs"""
    n = struct.unpack_from('>i', dzs, 0)[0]
    ch = {}
    for i in range(n):
        tag, cnt, off = struct.unpack_from('>4siI', dzs, 4 + i * 12)
        ch[tag.decode('latin-1')] = (cnt, off)
    mem, mec = [], {}
    if 'MEM0' in ch:
        cnt, off = ch['MEM0']
        mem = [struct.unpack_from('>I', dzs, off + 4 * i)[0] for i in range(cnt)]
    if 'MEC0' in ch:
        cnt, off = ch['MEC0']
        mec = {dzs[off + 2 * i]: dzs[off + 2 * i + 1] for i in range(cnt)}
    return mem, mec


def calibrate(gc_files_dir, tight=0x10000):
    """GC disc check of the constants (see module doc). Returns (rows, failures)."""
    import glob
    import os
    rows = []
    for sd in sorted(glob.glob(os.path.join(glob.escape(os.path.join(gc_files_dir, 'res', 'Stage')), '*'))):
        stg = os.path.join(sd, 'STG_00.arc')
        if not os.path.exists(stg):
            continue
        m = rarc.Rarc.from_file(stg).get('stage.dzs')
        if m is None:
            continue
        mem, mec = stage_mem_tables(m.data())
        if not mem:
            continue
        for rp in sorted(glob.glob(os.path.join(glob.escape(sd), 'R*_00.arc'))):
            rn = int(os.path.basename(rp)[1:3])
            if rn not in mec or mec[rn] >= len(mem):
                continue
            s = arc_stats(common.read(rp))
            cap_gc = mem[mec[rn]] + 0x380
            slack = cap_gc - s['arc']
            est = estimate(s) - common.align(s['arc'], 32)
            rows.append({'stage': os.path.basename(sd), 'room': rn, 'arc': s['arc'], 'cap_gc': cap_gc,
                         'slack': slack, 'est_ovh': est, 'stats': s})
    tight_rows = [r for r in rows if r['stats']['models'] and r['slack'] < tight]
    fails = [r for r in tight_rows if r['est_ovh'] < 2 * r['slack']]
    tight_rows.sort(key=lambda r: r['slack'])
    return rows, tight_rows, fails
