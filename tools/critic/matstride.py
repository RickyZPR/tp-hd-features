"""critic: compute J3D MAT3/MAT4 material init-data stride from a raw .bmd/.bdl file.
usage: python -I matstride.py file.bmd [...]

library (added by the assets pipeline, 2026-10-18):
  matstride.effective_stride(buf, mat_block_offset) -> 0x14C | 0x14E | None
      Content-based: an entry layout is accepted only if every material entry read at that stride has
      a sane mMaterialMode byte and every table index (cull mode, colours, tex gens, tex numbers, TEV
      orders/stages/colours, swap modes, fog, alpha compare, blend, z mode/comp, dither, NBT scale) is
      either the 'none' value (0xFF / 0xFFFF) or inside its table. Reading a 0x14E-stride HD block at
      0x14C shifts every later entry by 2*i bytes, which breaks these invariants immediately.
  matstride.material_errors(buf, mat_block_offset, stride) -> list of (entry, field, value, limit)
  matstride.analyze_bytes(buf) -> list of dicts (one per MAT block)
CHANGES: the span-based numbers printed by the CLI are unchanged; the CLI now also prints the
content-based 'effective stride', which is what tells a mat4fix-repacked block (span still
N*0x14E, entries at 0x14C) from an unrepacked one.
"""
import struct
import sys

# J3DMaterialBlock offset table index -> element size (J3DModelLoader.h J3DMaterialBlock, MAT3 order)
_ELEM = {4: 4, 5: 4, 6: 1, 7: 8, 8: 4, 9: 0x34, 10: 1, 11: 4, 12: 4, 13: 0x64, 14: 0x64, 15: 2, 16: 4,
         17: 8, 18: 4, 19: 1, 20: 0x14, 21: 4, 22: 4, 23: 0x2C, 24: 8, 25: 4, 26: 4, 27: 1, 28: 1, 29: 0x10}
# J3DMaterialInitData (J3DMaterialFactory.h): u8 index fields (offset, table) and u16 arrays (offset, n, table)
_U8 = [(1, 4), (2, 6), (3, 10), (4, 19), (5, 27), (6, 26), (7, 28)]
_U16 = [(0x08, 2, 5), (0x0C, 4, 7), (0x14, 2, 8), (0x28, 8, 11), (0x48, 8, 13), (0x84, 8, 15), (0x94, 4, 18),
        (0xBC, 16, 16), (0xDC, 4, 17), (0xE4, 16, 20), (0x104, 16, 21), (0x124, 4, 22), (0x144, 1, 23),
        (0x146, 1, 24), (0x148, 1, 25), (0x14A, 1, 29)]
# mMaterialMode is a draw-mode bitfield (J3DMaterial::isDrawModeOpaTexEdge = mode & 3). Retail GC TP uses
# 1/2/4; HD also uses higher bits (0x10, 0x21 seen in D_SB11), which the GC engine ignores except for the
# opaque/edge test. Misaligned reads typically give 0 or 0xFF here; the index checks below do the rest.
_MODES = set(range(1, 0x80))


def sections(b):
    assert b[:4] == b'J3D2', b[:8]
    n = struct.unpack_from('>I', b, 0xC)[0]
    o = 0x20
    for _ in range(n):
        tag = b[o:o+4].decode('latin1'); size = struct.unpack_from('>I', b, o+4)[0]
        yield tag, o, size
        if size == 0:
            break
        o += size


def _block_info(b, o):
    tag = b[o:o+4].decode('latin1')
    size = struct.unpack_from('>I', b, o+4)[0]
    num = struct.unpack_from('>H', b, o+8)[0]
    offs = list(struct.unpack_from('>30I', b, o+0xC))
    ids = struct.unpack_from('>%dH' % num, b, o+offs[1])
    uniq = max(ids)+1
    return tag, size, num, offs, uniq


def _counts(offs, size):
    nz = sorted(set(x for x in offs if x))
    cnt = {}
    for k, es in _ELEM.items():
        if not offs[k]:
            cnt[k] = 0
            continue
        nxt = min([x for x in nz if x > offs[k]] + [size])
        cnt[k] = (nxt - offs[k]) // es
    return cnt


def material_errors(b, o, stride):
    tag, size, num, offs, uniq = _block_info(b, o)
    cnt = _counts(offs, size)
    base = o + offs[0]
    errs = []
    for i in range(uniq):
        e = base + i * stride
        if e + 0x14C > o + size:
            errs.append((i, 'entry', e - o, size))
            break
        if b[e] not in _MODES:
            errs.append((i, 'mode', b[e], '1..0x7F'))
        for fo, t in _U8:
            v = b[e + fo]
            if v != 0xFF and v >= cnt[t]:
                errs.append((i, 'u8@0x%x' % fo, v, cnt[t]))
        for fo, n, t in _U16:
            for k in range(n):
                v = struct.unpack_from('>H', b, e + fo + 2 * k)[0]
                if v != 0xFFFF and v >= cnt[t]:
                    errs.append((i, 'u16@0x%x[%d]' % (fo, k), v, cnt[t]))
    return errs


def effective_stride(b, o):
    """0x14C / 0x14E by content (see module doc); None if neither layout is consistent."""
    tag, size, num, offs, uniq = _block_info(b, o)
    span = offs[1] - offs[0]
    if uniq == 1:
        base = o + offs[0]
        hd_span = 0 <= span - 0x14E < 0x20
        trailing = b[base + 0x14C:base + 0x14E] if hd_span else b'\0\0'
        if material_errors(b, o, 0x14C):
            return None
        return 0x14E if (hd_span and trailing != b'\0\0') else 0x14C
    ok_c = not material_errors(b, o, 0x14C)
    ok_e = (span >= uniq * 0x14E) and not material_errors(b, o, 0x14E)
    if ok_c and not ok_e:
        return 0x14C
    if ok_e and not ok_c:
        return 0x14E
    if ok_c and ok_e:          # both plausible (should not happen with >=2 materials) -> trust the span
        return 0x14E if 0 <= span - uniq * 0x14E < 0x20 else 0x14C
    return None


def analyze_bytes(b):
    out = []
    for tag, o, size in sections(b):
        if tag[:3] != 'MAT':
            continue
        _, _, num, offs, uniq = _block_info(b, o)
        span = offs[1] - offs[0]
        out.append({'tag': tag, 'num': num, 'uniq': uniq, 'span': span,
                    'rem14C': span - uniq * 0x14C, 'rem14E': span - uniq * 0x14E,
                    'effective_stride': effective_stride(b, o),
                    'errors14C': len(material_errors(b, o, 0x14C))})
    return out


def analyze(path):
    b = open(path, 'rb').read()
    for tag, o, size in sections(b):
        if tag[:3] != 'MAT': continue
        num = struct.unpack_from('>H', b, o+8)[0]
        noffs = 31 if tag == 'MAT4' else 30
        offs = list(struct.unpack_from('>%dI' % noffs, b, o+0xC))
        ids = struct.unpack_from('>%dH' % num, b, o+offs[1])
        uniq = max(ids)+1
        span = offs[1]-offs[0]
        cands = {s: span - s*uniq for s in (0x14C, 0x14E)}
        # check: first byte (mMaterialMode) of each candidate entry should be small (1/2/4)
        def modes(stride):
            return [b[o+offs[0]+i*stride] for i in range(uniq)]
        print("%s %s num=%d uniq=%d span=0x%x  rem@0x14C=%d rem@0x14E=%d" % (path.split('/')[-4] if path.count('/')>4 else path, tag, num, uniq, span, cands[0x14C], cands[0x14E]))
        for s in (0x14C, 0x14E):
            m = modes(s)
            print("   stride 0x%X matModes=%s" % (s, m[:24]))
        if tag == 'MAT4':
            print("   off[30](polyoffs)=0x%x size=0x%x" % (offs[30], size))
            # read polygon-offset idx (u16 at +0x14C) for stride 0x14E
            idx = [struct.unpack_from('>H', b, o+offs[0]+i*0x14E+0x14C)[0] for i in range(uniq)]
            print("   polyOffIdx@0x14E-stride:", idx[:24])
        es = effective_stride(b, o)
        print("   effective stride (content): %s" % (('0x%X' % es) if es else 'INCONSISTENT'))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyze(p)
