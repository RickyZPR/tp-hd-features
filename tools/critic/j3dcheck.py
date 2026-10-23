"""critic: for every .bmd/.bdl inside the given .arc files, report
 - MAT block tag and real material init-data stride (0x14C GC, 0x14E HD MAT4)
 - VTX1 arrays present without a format entry (v2.0.3 J3DModelLoader::getFmt -> OSPanic)
 - polygon-offset usage (MAT4 only)
usage: python -I j3dcheck.py <formats_tools_dir> arc [arc...]

library: import j3dcheck; res = j3dcheck.check_bytes(bmd_bytes, label)
         -> {'label', 'mat', 'stride', 'bad_stride', 'vtx_missing'}

CHANGES (assets pipeline, 2026-10-18):
  - importable (CLI code moved under main()).
  - the stride verdict now comes from the entry contents (matstride.effective_stride), not from the
    span alone. mat4fix repacks the entries but keeps every table offset, so the span of a FIXED block is
    still N*0x14E + pad; the old span-only test reported every repacked model as BAD_STRIDE. A block is
    bad when its entries are really laid out at 0x14E (or are inconsistent at both strides) and it has
    more than one material.
"""
import os
import struct
import sys

_here = os.path.dirname(os.path.abspath(__file__))
if _here not in sys.path:
    sys.path.insert(0, _here)
import matstride  # noqa: E402

ATTR = [9, 10, 25, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]
NAMES = {9:'POS',10:'NRM',25:'NBT',11:'CLR0',12:'CLR1'}
for i in range(8): NAMES[13+i] = 'TEX%d' % i


def check_bytes(b, label=''):
    res = {'label': label}
    if b[:4] != b'J3D2':
        res['not_j3d'] = True
        return res
    n = struct.unpack_from('>I', b, 0xC)[0]; o = 0x20
    for _ in range(n):
        tag = b[o:o+4].decode('latin1'); size = struct.unpack_from('>I', b, o+4)[0]
        if tag in ('MAT3', 'MAT4'):
            num = struct.unpack_from('>H', b, o+8)[0]
            offs = struct.unpack_from('>30I', b, o+0xC)
            ids = struct.unpack_from('>%dH' % num, b, o+offs[1])
            uniq = max(ids)+1; span = offs[1]-offs[0]
            eff = matstride.effective_stride(b, o)
            stride = ('0x%X' % eff) if eff else ('?span=0x%x' % span)
            if uniq == 1: stride += '(1 mat: harmless)'
            res['mat'] = '%s stride=%s mats=%d' % (tag, stride, uniq)
            res['stride'] = eff
            res['tag'] = tag
            if uniq > 1 and eff != 0x14C:
                res['bad_stride'] = True
        if tag == 'VTX1':
            fo = struct.unpack_from('>I', b, o+8)[0]
            arr = struct.unpack_from('>13I', b, o+0xC)
            fmts = set(); p = o+fo
            while True:
                a = struct.unpack_from('>I', b, p)[0]
                if a == 0xFF: break
                fmts.add(a); p += 0x10
            missing = [NAMES[ATTR[i]] for i in range(13) if arr[i] and ATTR[i] not in fmts]
            if missing: res['vtx_missing'] = missing
        if size == 0:
            break
        o += size
    return res


def check(b, label, out):
    """old signature kept for compatibility"""
    if b[:4] != b'J3D2': return
    out.append(check_bytes(b, label))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    sys.path.insert(0, argv[1])
    import rarc
    tot = {'models': 0, 'bad_stride': 0, 'vtx_missing': 0}
    for arcp in argv[2:]:
        try:
            a = rarc.Rarc.from_file(arcp)
        except Exception as e:
            print('ERR', arcp, e); continue
        for m in a.files():
            if m.name.lower().endswith(('.bmd', '.bdl')):
                out = []
                check(m.data(), os.path.basename(arcp) + ':' + m.path, out)
                for r in out:
                    tot['models'] += 1
                    flag = []
                    if r.get('bad_stride'): tot['bad_stride'] += 1; flag.append('BAD_STRIDE')
                    if r.get('vtx_missing'): tot['vtx_missing'] += 1; flag.append('VTX_NOFMT=' + ','.join(r['vtx_missing']))
                    print('%-60s %-40s %s' % (r['label'][:60], r.get('mat', '-'), ' '.join(flag)))
    print('TOTAL', tot)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
