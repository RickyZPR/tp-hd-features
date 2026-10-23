"""HD stamps -> Dusklight stamps mod assets.

overlay/res/Object/Obj_Stamp.arc
    HD res/Object/Obj_Stamp.arc (+ .pack.gz) through hd2gc + mat4fix, written with the HD entry layout so that
    stamp.bmd stays at file-entry index 4: HD item_resource[] rows for all 50 stamp items are
    {"Obj_Stamp", bmd 4, btk/bck/brk/btp -1, ...} (Zelda.rpx .data 0x1011F288). New disc path (the GC disc has
    no Obj_Stamp.arc), so the overlay replaces nothing.
res/stamps/stamps/stampNN.png (NN = 00..49 = HD stamp index = research/stamps_all50.csv stamp_index)
    HD res/LayoutRevo/stampResR.pack.gz timg/stampN.bti.gtx (RGBA8 100x100 or 200x200; the ink-coloured
    art the HD collection screen draws). Fallback: timg/stampN.tga from stampResR.arc.
res/stamps/stamps/stamp_frame.png, stamp_empty.png, stamp_background.png
    HD collection-screen tile frame (box), unknown-stamp tile (icon_empty) and paper backdrop (paper).
res/stamps/stamps/stamps.json
    manifest: per stamp index/file/size (+ item id from facts.py; label: "Letter A".."Letter Z" or HD's own name from the dump),
    the Obj_Stamp resource index, and the frame files.
res/stamps/stamps/hd_chests.bin
    the stage records (TRES, 0x20 bytes) of the 7 HD-only stamp chests (facts.HD_CHESTS), read from the HD room data;
    the mod adds these chests with StageService::add_actor. Layout: 'HDFC' u8 version=1 u8 count u16 0, then per
    chest u8 stamp index, s8 layer (-1 = TRES, n = TREn), u8 room, u8 0, u8 record[0x20].
Not built: item icons. GC res/Layout/itemicon.arc has 118 textures and its next free entry index is 123,
which is exactly where HD puts stamp0..49 (texture ids 123..172), but adding them needs a whole-file overlay
of a GC archive other mods may also replace; the stamps mod does not need HUD icons.
"""

import os
import struct

import common
import convert
import rarcx
import rarc
import hdpack
import gtx2img

FRAMES = {'stamp_frame.png': 'box', 'stamp_empty.png': 'icon_empty', 'stamp_background.png': 'paper'}
HD_STAMP_BMD_INDEX = 4


def _gtx_png(tm, name):
    g = tm.get(name + '.bti.gtx')
    if g is None:
        return None
    s = hdpack.gtx_surfaces(g)['surfaces'][0]
    w, h, lin = gtx2img.deswizzle_level(s, g, 0)
    s2 = dict(s)
    s2['w'], s2['h'] = w, h
    return w, h, gtx2img.to_rgba(s2, lin)


def _tga_rgba(t):
    idl, cmt, it = t[0], t[1], t[2]
    w, h = struct.unpack_from('<HH', t, 12)
    bpp, desc = t[16], t[17]
    if it != 2 or bpp != 32 or cmt:
        raise ValueError('unsupported TGA type %d bpp %d' % (it, bpp))
    px = t[18 + idl:18 + idl + w * h * 4]
    out = bytearray(w * h * 4)
    for y in range(h):
        sy = y if (desc & 0x20) else (h - 1 - y)
        row = px[sy * w * 4:(sy + 1) * w * 4]
        for x in range(w):
            o = x * 4
            d = (y * w + x) * 4
            out[d:d + 4] = bytes((row[o + 2], row[o + 1], row[o], row[o + 3]))
    return w, h, bytes(out)


def _stamp_rows(hd):
    """{index: {'item_id', 'stamp_name'}} for stamps.json: the item ids from facts.STAMP_ITEM_IDS, the labels
    "Letter A".."Letter Z" for the alphabet stamps 0-25 (this mod's wording) and HD's own names (zel_00 2890 +
    index, read from the player's dump) for the others. Nothing is stored in the tools."""
    import facts
    import hdtext
    names = {}
    try:
        b = hdtext.load_bmg(common.exists_ci(os.path.join(hd, 'res', 'Msgus'), 'bmgres.arc'), 'zel_00')
        for i in range(26, 50):
            e = b.by_id.get(2890 + i)
            if e is not None and len(e.text) > 1:
                names[i] = hdtext.preview(e.text).strip()
    except (OSError, ValueError, TypeError):
        names = {}
    rows = {}
    for i, item in enumerate(facts.STAMP_ITEM_IDS):
        rows[i] = {'item_id': '0x%02X' % item,
                   'stamp_name': ('Letter ' + chr(ord('A') + i)) if i < 26 else names.get(i, 'Stamp %d' % (i + 1))}
    return rows


def hd_chest_records(hd):
    """[(stamp index, layer, room, 0x20-byte TRES record)] of facts.HD_CHESTS, from the HD room archives."""
    import dzx
    import facts
    out = []
    for idx, stage, room, tbox, item in facts.HD_CHESTS:
        sdir = common.exists_ci(os.path.join(hd, 'res', 'Stage'), stage)
        p = common.exists_ci(sdir, 'R%02d_00.arc' % room) if sdir else None
        if p is None:
            raise FileNotFoundError('HD room %s R%02d_00.arc not found (HD-only chest of stamp %d)' % (stage, room, idx))
        hit = None
        for m in rarc.Rarc.from_file(p).files():
            if not m.name.lower().endswith('.dzr'):
                continue
            b = m.data()
            for c in dzx.parse(b):
                if c.canon != 'TRES':
                    continue
                for k in range(c.count):
                    o = c.offset + k * c.stride
                    t = dzx.dec_actor(b, o).get('tbox')
                    if t and t['tboxNo'] == tbox and t['item'] == item:
                        hit = (-1 if c.layer is None else c.layer, bytes(b[o:o + 0x20]))
                        break
                if hit:
                    break
            if hit:
                break
        if hit is None:
            raise ValueError('HD-only chest of stamp %d (chest %d, item 0x%02X) not in %s R%02d' % (idx, tbox, item,
                                                                                                  stage, room))
        out.append((idx, hit[0], room, hit[1]))
    return out


def hd_chests_bin(records):
    b = bytearray(b'HDFC' + bytes((1, len(records), 0, 0)))
    for idx, layer, room, rec in records:
        b += bytes((idx, layer & 0xFF, room, 0)) + rec
    return bytes(b)


def build(hd, out, log, project=None):
    res = {'pngs': {}, 'frames': {}}
    # 1. Obj_Stamp.arc
    p = common.exists_ci(os.path.join(hd, 'res', 'Object'), 'Obj_Stamp.arc')
    a, repl, st, rep = convert.convert_arc(p)
    b = rarcx.rebuild_same_layout(a, repl)
    common.write(os.path.join(out['obj_dir'], 'Obj_Stamp.arc'), b)
    idx_hd = rarcx.entry_index(a, 'stamp.bmd')
    idx_out = rarcx.entry_index(rarc.Rarc(b, 'out'), 'stamp.bmd')
    res['obj_stamp'] = {'size': len(b), 'bmd_index_hd': idx_hd, 'bmd_index': idx_out, 'conv': st, 'report': rep}
    log('stamps: Obj_Stamp.arc %d bytes, stamp.bmd entry index %s (HD %s), %d model(s), MAT repack %d',
        len(b), idx_out, idx_hd, st['models'], st['mat_fixed'])

    # 2. stamp images
    lr = os.path.join(hd, 'res', 'LayoutRevo')
    tm = hdpack.Tmpk(hdpack.read_any(common.exists_ci(lr, 'stampResR.pack.gz')))
    arc = rarc.Rarc.from_file(common.exists_ci(lr, 'stampResR.arc'))
    rows = _stamp_rows(hd)
    manifest = {'version': 1,
                'note': 'Generated by tools/build_assets.py from the TP HD dump; local use only.',
                'obj_stamp': {'disc_path': 'res/Object/Obj_Stamp.arc', 'bmd_member': 'bmdr/stamp.bmd',
                              'bmd_resource_index': idx_out},
                'frames': {}, 'stamps': []}
    for i in range(50):
        src = 'gtx'
        r = _gtx_png(tm, 'stamp%d' % i)
        if r is None:
            m = arc.get('timg/stamp%d.tga' % i)
            r = _tga_rgba(m.data())
            src = 'tga'
        w, h, rgba = r
        fn = 'stamp%02d.png' % i
        common.write(os.path.join(out['stamps_dir'], fn), common.png_bytes(w, h, rgba))
        ent = {'index': i, 'file': fn, 'w': w, 'h': h}
        row = rows.get(i)
        if row:
            ent['item_id'] = row['item_id']
            ent['label'] = row['stamp_name']
        manifest['stamps'].append(ent)
        res['pngs'][fn] = (w, h, src)
    for fn, srcname in FRAMES.items():
        r = _gtx_png(tm, srcname)
        if r is None:
            continue
        w, h, rgba = r
        common.write(os.path.join(out['stamps_dir'], fn), common.png_bytes(w, h, rgba))
        manifest['frames'][fn[:-4]] = {'file': fn, 'w': w, 'h': h}
        res['frames'][fn] = (w, h)
    common.write_json(os.path.join(out['stamps_dir'], 'stamps.json'), manifest)
    res['manifest'] = manifest
    chests = hd_chest_records(hd)
    common.write(os.path.join(out['stamps_dir'], 'hd_chests.bin'), hd_chests_bin(chests))
    res['hd_chests'] = chests
    log('stamps: hd_chests.bin, %d HD-only chest records (stamps %s)', len(chests), ', '.join(str(c[0]) for c in chests))
    log('stamps: %d stamp PNGs (%s), %d frame PNGs, stamps.json', len(res['pngs']),
        ', '.join(sorted({'%dx%d' % (v[0], v[1]) for v in res['pngs'].values()})), len(res['frames']))
    return res
