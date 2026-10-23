"""Readers for stage.dzs / room.dzr chunk data (layouts as in tools/formats/dzx.py and tools/cave/cave_patch.py,
both checked against Dusklight include/d/d_stage.h)."""
import struct

import common  # noqa: F401
import dzx
import cave_patch

LAYER = '0123456789abcde'


def chunks(b):
    return cave_patch.chunks(b)          # [tag, count, offset, header_offset]


def chunk_map(b):
    return {c[0]: c for c in chunks(b)}


def layer_of(tag):
    if len(tag) == 4 and tag[3] in LAYER and tag[:3] in ('ACT', 'TRE', 'SCO', 'Doo'):
        return LAYER.index(tag[3])
    return -1


def records(b):
    """every actor-like record (ACTR/ACTn/TGOB/PLYR/TRES/TREn = 0x20, SCOB/SCOn/TGSC/TGDR/Door/Doon = 0x24)"""
    out = []
    for tag, cnt, off, _ in chunks(b):
        if cave_patch.is_actor_tag(tag):
            stride = 0x20
        elif cave_patch.is_scob_tag(tag):
            stride = 0x24
        else:
            continue
        for i in range(cnt):
            o = off + i * stride
            name = b[o:o + 8].split(b'\0')[0].decode('latin-1')
            prm, x, y, z, ax, ay, az, sid = struct.unpack_from('>I3f3hH', b, o + 8)
            out.append({'tag': tag, 'layer': layer_of(tag), 'i': i, 'off': o, 'name': name, 'prm': prm,
                        'pos': (x, y, z), 'ang': (ax, ay, az), 'setid': sid})
    return out


def scls(b):
    c = chunk_map(b).get('SCLS')
    if not c:
        return []
    _, cnt, off, _ = c
    return [dzx.dec_scls(b, off + i * 0xD) for i in range(cnt)]


def rcam(b):
    c = chunk_map(b).get('RCAM')
    if not c:
        return []
    _, cnt, off, _ = c
    return [b[off + i * 0x18:off + i * 0x18 + 16].split(b'\0')[0].decode('latin-1') for i in range(cnt)]


def stag(b):
    c = chunk_map(b).get('STAG')
    return dzx.dec_stag(b, c[2]) if c else None


def rtbl(b):
    """{room: [raw bytes of its load list]}"""
    c = chunk_map(b).get('RTBL')
    if not c:
        return {}
    _, cnt, off, _ = c
    out = {}
    for r in range(cnt):
        e = struct.unpack_from('>I', b, off + 4 * r)[0]
        num = b[e]
        roff = struct.unpack_from('>I', b, e + 4)[0]
        out[r] = list(b[roff:roff + num])
    return out


def objnames():
    return dzx.objnames()
