"""critic: repack HD MAT4 material init data (0x14E stride, trailing u16 mPolygonOffsetIdx) into the
GC MAT3 layout (0x14C stride) in place, and retag MAT4->MAT3. Offsets of all other MAT tables are
block-relative and stay valid (the freed 2*N bytes before table[1] become padding).
Works on MAT blocks already renamed to MAT3 by hd2gc.py (detects stride from span).
usage: python -I mat4fix.py <formats_tools_dir> in.arc out.arc     (writes a NEW file; never in place)

library: import mat4fix; new_bytes, n_fixed = mat4fix.fix_j3d(bmd_bytes)

CHANGES (assets pipeline, 2026-10-18):
  - importable: the CLI code moved under main(); fix_j3d() unchanged in behaviour for HD input.
  - idempotent: the span test alone cannot tell an already-repacked block from an unrepacked one (the
    repack keeps all offsets, so the span stays N*0x14E + pad). fix_j3d() now also checks the entry
    contents (matstride.effective_stride) and only repacks when the entries really are laid out at
    0x14E. Running it twice no longer shifts the entries a second time.
"""
import os
import struct
import sys

_here = os.path.dirname(os.path.abspath(__file__))
if _here not in sys.path:
    sys.path.insert(0, _here)
import matstride  # noqa: E402


def fix_j3d(d):
    b = bytearray(d); n = struct.unpack_from('>I', b, 0xC)[0]; o = 0x20; changed = 0
    for _ in range(n):
        tag = bytes(b[o:o+4]); size = struct.unpack_from('>I', b, o+4)[0]
        if tag in (b'MAT3', b'MAT4'):
            num = struct.unpack_from('>H', b, o+8)[0]
            offs = struct.unpack_from('>30I', b, o+0xC)
            ids = struct.unpack_from('>%dH' % num, b, o+offs[1])
            uniq = max(ids)+1; span = offs[1]-offs[0]
            if 0 <= span - uniq*0x14E < 0x20 and span - uniq*0x14C >= 2*uniq \
                    and matstride.effective_stride(bytes(b), o) == 0x14E:
                base = o+offs[0]
                ents = [bytes(b[base+i*0x14E: base+i*0x14E+0x14C]) for i in range(uniq)]
                blob = b''.join(ents)
                b[base:base+uniq*0x14E] = blob + b'\0'*(2*uniq)
                changed += 1
            b[o:o+4] = b'MAT3'
        if size == 0:
            break
        o += size
    return bytes(b), changed


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 1
    sys.path.insert(0, argv[1])
    import rarc, rarc_write
    arc = rarc.Rarc.from_file(argv[2]); rep = {}; tot = 0
    for m in arc.files():
        if m.name.lower().endswith(('.bmd', '.bdl')):
            nd, c = fix_j3d(m.data())
            if c: rep[m.path] = nd; tot += c
    open(argv[3], 'wb').write(rarc_write.build_from_rarc(arc, rep))
    print(argv[2].split('/')[-1], 'fixed MAT blocks:', tot)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
