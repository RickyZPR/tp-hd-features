"""HD archive -> GC-engine archive members, in memory.

convert_arc(arc_path, pack_path=None) -> (rarc.Rarc, {member_path: new_bytes}, stats)
    For every .bmd/.bdl/.bti member: tools/formats/hd2gc.py (GTX -> GX texture pixels in TEX1 / BTI,
    MAT4 tag -> MAT3), then tools/critic/mat4fix.fix_j3d (material init data 0x14E -> 0x14C repack).
    Other members are untouched (callers patch them). Same per-member logic as hd2gc.convert_archive,
    which only writes files; this returns bytes so the later steps (bake, patch) chain in memory.
"""
import os

import common  # noqa: F401
import rarc
import hd2gc
import hdpack
import mat4fix


def convert_arc(arc_path, pack_path=None, scale=1):
    a = rarc.Rarc.from_file(arc_path)
    pack_path = pack_path or (arc_path[:-4] + '.pack.gz')
    tm = hdpack.Tmpk(hdpack.read_any(pack_path)) if os.path.exists(pack_path) else None
    report = []
    repl = {}
    st = {'models': 0, 'mat_fixed': 0, 'textures': 0, 'no_source': 0, 'bti': 0, 'mdl3': 0}
    for m in a.files():
        ln = m.name.lower()
        if not ln.endswith(('.bmd', '.bdl', '.bti')):
            continue
        gtx = None
        if tm:
            for e in tm.entries:
                if e['name'].lower().endswith('/' + m.path.lower() + '.gtx'):
                    gtx = tm.data(e)
                    break
        d = m.data()
        lab = '%s:%s' % (os.path.basename(arc_path), m.path)
        n0 = len(report)
        if ln.endswith('.bti'):
            nd = hd2gc.convert_bti(d, gtx, scale, report, lab)
            st['bti'] += 1
        else:
            nd = hd2gc.convert_j3d(d, gtx, scale, {}, None, report, lab)
            nd, fixed = mat4fix.fix_j3d(nd)
            st['models'] += 1
            st['mat_fixed'] += fixed
            if fixed:
                report.append('%s: MAT 0x14E -> 0x14C repack (%d block)' % (lab, fixed))
        for line in report[n0:]:
            if 'NO SOURCE' in line or 'left as is' in line:
                st['no_source'] += 1
            elif 'MDL3' in line:
                st['mdl3'] += 1
            elif '<- GTX' in line:
                st['textures'] += 1
        repl[m.path] = nd
    return a, repl, st, report
