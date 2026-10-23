"""Scene particles for the Cave of Shadows: a GC-format res/Particle/Pscene206.jpc built from the user's dumps.

Why:
  D_SB11's STAG asks for scene particle file 206 (HD-only). The first pipeline swapped it for GC Pscene200
  (the Cave of Ordeals file), but 130 of HD 206's 341 particle resources are missing from 200 and some cave
  actors need them: kytag10 "Sparks" (rooms 18/33/47) stores dComIfGp_particle_set(0x852B/0x852C) and
  dereferences the emitters every frame without a NULL check (d_a_kytag10.cpp:99-114, 198-199) -> crash on
  floor 10/11; Zant Head projectiles (E_zm, 0x8861-0x886C) and the magma poles (yfire, 0x84E4-0x84EC) would be
  invisible; the twilight-enemy dissolve, the exit portal swirl and more would be missing.
  Shipping HD's whole 206 is not possible either: with its textures at GC size it is 391 KB, but scene
  particle files are loaded into dPa_control_c::m_resHeap, a fixed 0x96000-byte JKRExpHeap that also holds
  the resident common.jpc (315,808 bytes on the GC disc) for the whole game (d_particle.cpp:1171-1177,
  1268; d_s_logo.cpp:1699). A file that does not fit makes mDoDvdThd_toMainRam_c fail (JKRDvdRipper
  returns NULL) and the stage gets no scene particles at all.

What this builds:
  - resources: every particle id that the code of the actors which can exist in D_SB11 references (hex
    literals 0x8000-0xFFFF and dPa_RM(ID_...) names in their source files, see needed_ids()), taken from
    GC Pscene200 when it has the id (GC engine data, GC textures), otherwise from HD Pscene206;
  - textures: the ones those resources reference (TDB1 indices remapped). GC 200 texture blocks are copied
    byte for byte; HD-only ones get GX pixels converted from tex/Particle/Pscene206.jpc.gtx.gz with the
    existing hd2gc code (GTX surface i <-> JPC texture i, as tphd's register_hd_particle_textures pairs them);
  - size budget: the largest scene particle file on the user's GC disc minus SAFETY bytes (271,136 B
    Pscene020 on GZ2E01), i.e. a size vanilla Dusklight is known to load into the same heap. If the file is
    larger, the largest HD-converted textures are re-encoded at half resolution (logged) until it fits.
The STAG particle number stays 206, so /res/Particle/Pscene206.jpc (a path the GC disc does not have) is
loaded from the mod overlay; no GC file is replaced and the real Cave of Ordeals keeps its Pscene200.
"""
import glob
import os
import re
import struct

import common  # noqa: F401  (paths)
import hdpack
import hd2gc
import j3d

MAGIC = b'JPAC2-10'
SAFETY = 4096
STAGE_FILE = 'Pscene206.jpc'

# Source files always included in the reference scan besides the actors placed in the rooms: the player
# (wolf), Midna, enemy death/dissolve effects, drops, chests, generic ground/polygon effects, the enemy
# spawner's targets and enemies' child actors (arrows, bombs, leaves, shells).
SUPPORT = ['d/actor/d_a_alink.cpp', 'd/actor/d_a_alink_*.inc', 'd/actor/d_a_alink_dusk.cpp',
           'd/actor/d_a_midna.cpp', 'd/actor/d_a_disappear.cpp', 'd/actor/d_a_obj_item.cpp',
           'd/actor/d_a_obj_drop.cpp', 'd/actor/d_a_obj_life_container.cpp', 'd/actor/d_a_tbox*.cpp',
           'd/actor/d_a_e_arrow.cpp', 'd/actor/d_a_nbomb.cpp', 'd/actor/d_grass.inc', 'd/actor/d_flower.inc',
           'd/d_particle.cpp', 'd/d_particle_copoly.cpp']


# --------------------------------------------------------------------------------------------- JPC I/O
def parse(d):
    if d[:8] != MAGIC:
        raise ValueError('not a JPAC2-10 file')
    nres, ntex, texoff = struct.unpack_from('>HHI', d, 8)
    o = 0x10
    res = []
    for _ in range(nres):
        rid, nblk, nfld, nkey, ntdb = struct.unpack_from('>HHBBB', d, o)
        st = o
        o += 8
        tdb_off = None
        for _ in range(nblk):
            tag = d[o:o + 4]
            sz = struct.unpack_from('>I', d, o + 4)[0]
            if sz < 8:
                raise ValueError('bad block size in resource %04x' % rid)
            if tag == b'TDB1':
                tdb_off = o + 8 - st
            o += sz
        raw = d[st:o]
        tdb = list(struct.unpack_from('>%dH' % ntdb, raw, tdb_off)) if (tdb_off is not None and ntdb) else []
        res.append({'id': rid, 'raw': raw, 'tdb_off': tdb_off, 'tdb': tdb})
    if o > texoff:
        raise ValueError('resources overrun the texture table')
    tex = []
    o = texoff
    for i in range(ntex):
        if d[o:o + 4] != b'TEX1':
            raise ValueError('texture %d: no TEX1 block' % i)
        sz = struct.unpack_from('>I', d, o + 4)[0]
        name = d[o + 0xC:o + 0x20].split(b'\0')[0].decode('latin-1')
        tex.append({'name': name, 'raw': d[o:o + sz], 'h': j3d.bti_header(d, o + 0x20)})
        o += sz
    return {'res': res, 'tex': tex, 'end': o}


def build(resources, textures):
    """resources: [raw bytes with TDB1 already remapped]; textures: [raw TEX1 blocks]"""
    body = bytearray(b''.join(resources))
    texoff = common.align(0x10 + len(body), 0x20)
    out = bytearray(MAGIC + struct.pack('>HHI', len(resources), len(textures), texoff))
    out += body
    out += b'\0' * (texoff - len(out))
    for t in textures:
        out += t
    return bytes(out)


def tex_block(name_hdr, timg, pixels):
    """TEX1 block: 'TEX1', u32 size, u32 0, char name[20], ResTIMG (imageOffset 0x20), pixels (32-aligned)"""
    pix = pixels + b'\0' * ((-len(pixels)) % 32)
    blk = bytearray(name_hdr[:0x20]) + bytearray(timg) + pix
    struct.pack_into('>I', blk, 4, len(blk))
    return bytes(blk)


def verify(d, need=()):
    """structural check of a built file -> list of problems"""
    probs = []
    try:
        p = parse(d)
    except Exception as e:  # noqa: BLE001
        return ['parse: %r' % e]
    nt = len(p['tex'])
    ids = set()
    for r in p['res']:
        ids.add(r['id'])
        bad = [i for i in r['tdb'] if i >= nt]
        if bad:
            probs.append('resource %04x references textures %s of %d' % (r['id'], bad, nt))
    for i, t in enumerate(p['tex']):
        h = t['h']
        exp = j3d.tex_size(h['fmt'], h['w'], h['h'], max(1, h['mips']))
        if h['img_off'] != 0x20 or len(t['raw']) < 0x40 + exp:
            probs.append('texture %d %s: no pixels (block %d, needs %d)' % (i, t['name'], len(t['raw']), 0x40 + exp))
    miss = sorted(set(need) - ids)
    if miss:
        probs.append('missing ids %s' % ' '.join('%04x' % x for x in miss[:12]))
    if p['end'] != len(d):
        probs.append('trailing bytes after the texture table (%d != %d)' % (p['end'], len(d)))
    return probs


# --------------------------------------------------------------------------------------------- id scan
def _profiles(src_root):
    """{fpcNm_*: [source files]} from the '/* Proc Name */' entries of every actor profile, plus the
    process -> module table of src/c/c_dylink.cpp ({fpcNm_Obj_FireWood2_e, "d_a_obj_fireWood2"}: some profiles
    carry a different, decomp-placeholder proc name, e.g. d_a_obj_fireWood2.cpp says fpcNm_Obj_Lv1Cdl00_e)."""
    out = {}
    actor_dir = os.path.join(src_root, 'src', 'd', 'actor')
    by_lower = {}
    for p in glob.glob(os.path.join(glob.escape(actor_dir), '*.cpp')):
        by_lower[os.path.basename(p)[:-4].lower()] = p
        t = common.read(p).decode('utf-8', 'replace')
        for m in re.finditer(r'/\*\s*Proc Name\s*\*/\s*(fpcNm_\w+)', t):
            out.setdefault(m.group(1), []).append(p)
    dyl = os.path.join(src_root, 'src', 'c', 'c_dylink.cpp')
    if os.path.exists(dyl):
        t = common.read(dyl).decode('utf-8', 'replace')
        for m in re.finditer(r'\{\s*(fpcNm_\w+)\s*,\s*"(\w+)"\s*\}', t):
            p = by_lower.get(m.group(2).lower())
            if p and p not in out.setdefault(m.group(1), []):
                out[m.group(1)].append(p)
    return out


def _id_names(src_root):
    names = {}
    for l in common.read(os.path.join(src_root, 'include', 'd', 'd_particle_name.h')).decode('utf-8', 'replace').splitlines():
        m = re.match(r'\s*/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*(ID_\w+)', l)
        if m:
            names[m.group(2)] = int(m.group(1), 16)
    return names


def is_checkout(src_root):
    """True when src_root is a Dusklight source checkout the reference scan can read."""
    return bool(src_root) and os.path.isdir(os.path.join(src_root, 'src', 'd', 'actor')) and \
        os.path.exists(os.path.join(src_root, 'include', 'd', 'd_particle_name.h'))


def needed_ids(src_root, actor_names, objnames, universe):
    """Particle ids (subset of `universe`) referenced by the code of the given actor names (room records),
    their child actors and SUPPORT. Returns (ids, report lines, {id: [files]}).
    Without a source checkout (src_root None: the player builder) the ids come from facts.CAVE_PARTICLE_IDS,
    the result of this scan for the converted D_SB11 rooms."""
    if not is_checkout(src_root):
        import facts
        ids = set(facts.CAVE_PARTICLE_IDS) & set(universe)
        return ids, ['particles: %d ids from the built-in table (facts.CAVE_PARTICLE_IDS; no source scan)' %
                     len(ids)], {}
    prof = _profiles(src_root)
    srcd = os.path.join(src_root, 'src')
    files = set()
    unmapped = []
    procs = set()
    for n in sorted(actor_names):
        proc = (objnames.get(n) or [None])[0]
        if not proc or proc not in prof:
            unmapped.append('%s(%s)' % (n, proc))
            continue
        procs.add(proc)
    # child actors created by enemies / the enemy spawner (fpcNm_E_* referenced in their code)
    for proc in sorted(procs):
        for p in prof[proc]:
            base = os.path.basename(p)
            if base.startswith('d_a_e_') or base == 'd_a_obj_enemy_create.cpp':
                t = common.read(p).decode('utf-8', 'replace')
                procs.update(x for x in re.findall(r'\b(fpcNm_E_\w+_e)\b', t) if x in prof)
    for proc in procs:
        files.update(prof[proc])
    # same-prefix helper sources of every enemy (d_a_e_hb_leaf.cpp for d_a_e_hb.cpp, ...)
    for p in list(files):
        b = os.path.basename(p)
        if b.startswith('d_a_e_') and b.endswith('.cpp'):
            files.update(glob.glob(os.path.join(glob.escape(os.path.dirname(p)), b[:-4] + '_*.cpp')))
    for pat in SUPPORT:
        files.update(glob.glob(os.path.join(glob.escape(srcd), *pat.split('/'))))
    names = _id_names(src_root)
    ids = {}
    for p in sorted(files):
        t = common.read(p).decode('utf-8', 'replace')
        rel = os.path.relpath(p, srcd).replace(os.sep, '/')
        for m in re.finditer(r'\b0x([89A-Fa-f][0-9A-Fa-f]{3})\b', t):
            v = int(m.group(1), 16)
            if v in universe:
                ids.setdefault(v, set()).add(rel)
        for m in re.finditer(r'dPa_RM\(\s*(ID_\w+)\s*\)', t):
            v = names.get(m.group(1))
            if v is not None and (v | 0x8000) in universe:
                ids.setdefault(v | 0x8000, set()).add(rel)
    rep = ['particles: scanned %d source files of %d actor classes (+ support); unmapped actor names: %s' % (
        len(files), len(procs), ', '.join(unmapped) or 'none')]
    return set(ids), rep, {k: sorted(v) for k, v in ids.items()}


# --------------------------------------------------------------------------------------------- builder
def _convert_hd_texture(t, gtx, surf, half, report):
    h = dict(t['h'])
    if half:
        h['w'], h['h'] = max(8, h['w'] // 2), max(8, h['h'] // 2)
    fmt, w, hh, nm, pix = hd2gc.convert_texture(h, gtx, surf, 1, report, 'Pscene206 tex %s' % t['name'])
    timg = hd2gc.patch_header(t['raw'][0x20:0x40], fmt, w, hh, nm, 0x20)
    return tex_block(t['raw'][:0x20], timg, pix)


def build_scene(gc200, hd206, hd_gtx, need, budget):
    """-> (bytes, report lines, stats)"""
    G, H = parse(gc200), parse(hd206)
    gres = {r['id']: r for r in G['res']}
    surfaces = hdpack.gtx_surfaces(hd_gtx)['surfaces']
    if len(surfaces) != len(H['tex']):
        raise ValueError('Pscene206: %d JPC textures but %d GTX surfaces' % (len(H['tex']), len(surfaces)))
    gtex_by_name = {t['name']: t for t in G['tex']}
    order = [r for r in H['res'] if r['id'] in need]
    picked = []
    tex_names = []
    src_kind = {}
    differing = 0
    for r in order:
        if r['id'] in gres:
            g = gres[r['id']]
            names = [G['tex'][i]['name'] for i in g['tdb']]
            if g['raw'][:8] != r['raw'][:8] or len(g['raw']) != len(r['raw']):
                differing += 1
            picked.append((g, names))
        else:
            names = [H['tex'][i]['name'] for i in r['tdb']]
            picked.append((r, names))
        for n in names:
            if n not in src_kind:
                tex_names.append(n)
                src_kind[n] = 'GC' if n in gtex_by_name else 'HD'
    hd_index = {t['name']: i for i, t in enumerate(H['tex'])}
    new_index = {n: i for i, n in enumerate(tex_names)}
    resources = []
    for r, names in picked:
        raw = bytearray(r['raw'])
        for k, n in enumerate(names):
            struct.pack_into('>H', raw, r['tdb_off'] + 2 * k, new_index[n])
        resources.append(bytes(raw))
    report = []
    half = set()

    def blocks():
        out = []
        for n in tex_names:
            if src_kind[n] == 'GC':
                out.append(gtex_by_name[n]['raw'])
            else:
                i = hd_index[n]
                out.append(_convert_hd_texture(H['tex'][i], hd_gtx, surfaces[i], n in half, report))
        return out

    tb = blocks()
    data = build(resources, tb)
    # over budget: halve the largest HD-converted textures first, then stop (GC pixels are never touched)
    while len(data) > budget:
        cands = sorted(((len(b), n) for b, n in zip(tb, tex_names) if src_kind[n] == 'HD' and n not in half
                        and len(b) > 0x40 + 512), reverse=True)
        if not cands:
            break
        half.add(cands[0][1])
        report.append('Pscene206: %s re-encoded at half size (file %d B > budget %d B)' % (cands[0][1], len(data), budget))
        tb = blocks()
        data = build(resources, tb)
    stats = {'resources': len(resources), 'from_gc': sum(1 for r, _ in picked if r['id'] in gres),
             'from_hd': sum(1 for r, _ in picked if r['id'] not in gres), 'textures': len(tex_names),
             'tex_gc': sum(1 for n in tex_names if src_kind[n] == 'GC'),
             'tex_hd': sum(1 for n in tex_names if src_kind[n] == 'HD'), 'halved': sorted(half),
             'size': len(data), 'budget': budget, 'shared_header_differs': differing,
             'hd_total': len(H['res']), 'gc_total': len(G['res'])}
    report.append('Pscene206: %d resources (%d from GC Pscene200, %d HD-only) of HD %d, %d textures (%d GC, %d HD), '
                  '%d bytes (budget %d)' % (stats['resources'], stats['from_gc'], stats['from_hd'], stats['hd_total'],
                                            stats['textures'], stats['tex_gc'], stats['tex_hd'], stats['size'], budget))
    return data, report, stats


def gc_budget(gc_files):
    """(budget bytes, largest GC scene file name, its size)"""
    best = (0, None)
    for p in glob.glob(os.path.join(glob.escape(os.path.join(gc_files, 'res', 'Particle')), 'Pscene*.jpc')):
        sz = os.path.getsize(p)
        if sz > best[0]:
            best = (sz, os.path.basename(p))
    if not best[1]:
        raise FileNotFoundError('no res/Particle/Pscene*.jpc on the GC disc')
    return best[0] - SAFETY, best[1], best[0]
