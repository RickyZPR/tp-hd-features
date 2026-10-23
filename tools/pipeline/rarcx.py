"""RARC helpers on top of tools/formats/rarc.py + rarc_write.py.

rebuild_same_layout(arc, replace)
    Re-emit an archive with EXACTLY the source's node/entry/string tables (same entry indices, ids, names,
    order, '.'/'..' handling) and only the file data replaced. Needed for archives the game addresses by
    resource INDEX: dItem_data::item_resource[] stores a file-entry index (dRes_info_c::loadResource fills
    mRes[node->first_file_index + k]; JKRArchive::findIdxResource indexes mFiles directly). HD's
    Obj_Stamp.arc keeps stamp.bmd at entry 4 and O_gD_puL4.arc keeps its bmd at entry 1, and HD's
    item_resource rows use 4 and 1; a GC-retail re-layout (rarc_write) would move them to 3.
    Output: raw (not Yaz0) RARC, file flags FILE|MRAM (0x11), data 32-byte aligned.

entry_index(arc, member_path) -> file-entry index of a member (what item_resource / getIdxResource use).

check(buf) -> list of problems (empty = structurally OK): header sizes, node/entry/string bounds, every
    file inside the data block, directory entries pointing at valid nodes, '.'/'..' only as directories
    or HD-style placeholders, unique file ids, no Yaz0 left inside (members must be stored expanded).
"""
import struct

import common  # noqa: F401  (sys.path setup)
import rarc


def _entry_paths(arc):
    """entry index -> member path (root-name prefixed, like rarc.Member.path) for every file entry."""
    out = {}
    seen = set()

    def walk(ni, prefix):
        if ni in seen or ni >= len(arc.nodes):
            return
        seen.add(ni)
        n = arc.nodes[ni]
        for k in range(n['count']):
            ei = n['first'] + k
            e = arc.entries[ei]
            if e['name'] in ('.', '..'):
                continue
            if e['flags'] & 0x02:
                walk(e['off'], prefix + '/' + e['name'])
            elif e['flags'] & 0x01:
                out[ei] = prefix + '/' + e['name']
    if arc.nodes:
        walk(0, arc.nodes[0]['name'])
    return out


def entry_index(arc, member_path):
    low = member_path.lower()
    for ei, p in _entry_paths(arc).items():
        if p.lower() == low or p.lower().endswith('/' + low):
            return ei
    return None


def rebuild_same_layout(arc, replace=None):
    replace = replace or {}
    buf = arc.buf
    ib = arc.hdr_size
    data_off_rel = struct.unpack_from('>I', buf, 0xC)[0]
    head = bytearray(buf[:ib + data_off_rel])          # header + info + nodes + entries + strings
    paths = _entry_paths(arc)
    data = bytearray()
    for ei in sorted(paths):
        path = paths[ei]
        rel = path.split('/', 1)[1]
        m = None
        for mm in arc.files():
            if mm.path == path:
                m = mm
                break
        blob = replace.get(path, replace.get(rel))
        if blob is None:
            blob = m.data()
        while len(data) % 32:
            data.append(0)
        off = len(data)
        data.extend(blob)
        eo = arc.entry_base + ei * 0x14
        eid, h, fl_name = struct.unpack_from('>HHI', head, eo)
        flags = (fl_name >> 24) & 0xFF
        flags = (flags & ~0x84) | 0x11                 # stored expanded: clear COMP/YAZ0, FILE|MRAM
        struct.pack_into('>HHIIII', head, eo, eid, h, (flags << 24) | (fl_name & 0xFFFFFF), off, len(blob), 0)
    while len(data) % 32:
        data.append(0)
    out = head + data
    total = len(out)
    struct.pack_into('>IIIIIII', out, 4, total, ib, data_off_rel, len(data), len(data), 0, 0)
    return bytes(out)


def check(buf):
    probs = []
    if buf[:4] != b'RARC':
        return ['magic %r (not a raw RARC)' % buf[:4]]
    fsize, hsz, doff, dlen, mram, aram, dvd = struct.unpack_from('>7I', buf, 4)
    if fsize != len(buf):
        probs.append('header file_size %d != %d' % (fsize, len(buf)))
    if hsz != 0x20:
        probs.append('header size 0x%x' % hsz)
    if hsz + doff + dlen > len(buf):
        probs.append('data block past EOF')
    try:
        a = rarc.Rarc(buf, 'check')
    except Exception as e:  # noqa: BLE001
        return probs + ['parse error: %r' % e]
    data_base = hsz + doff
    ids = {}
    for i, n in enumerate(a.nodes):
        if n['first'] + n['count'] > a.num_entries:
            probs.append('node %d entries out of range' % i)
    for i, e in enumerate(a.entries):
        if e['flags'] & 0x02:
            if e['name'] not in ('.', '..') and e['off'] >= a.num_nodes:
                probs.append('dir entry %d -> node %d out of range' % (i, e['off']))
        elif e['flags'] & 0x01:
            if e['off'] + e['size'] > dlen:
                probs.append('file entry %d (%s) past data block' % (i, e['name']))
            if e['id'] in ids:
                probs.append('duplicate file id %d (%s, %s)' % (e['id'], ids[e['id']], e['name']))
            ids[e['id']] = e['name']
            if e['flags'] & 0x04:
                probs.append('file entry %d (%s) still flagged compressed' % (i, e['name']))
            blob = buf[data_base + e['off']:data_base + e['off'] + 4]
            if blob in (b'Yaz0', b'Yay0'):
                probs.append('file entry %d (%s) is Yaz0/Yay0 data' % (i, e['name']))
            if e['off'] % 32:
                probs.append('file entry %d (%s) data not 32-byte aligned' % (i, e['name']))
        elif e['name'] not in ('.', '..'):
            probs.append('entry %d (%s) is neither file nor dir' % (i, e['name']))
    if not a.files():
        probs.append('no files')
    return probs
