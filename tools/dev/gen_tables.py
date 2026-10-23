"""Write the converter's embedded tables (plan research/dropin/plan.md 2.4, 2.5, 3.3 C2 and 7.2 #3, #17).

  mods/hdfeatures/convert/data/inputs.inc            the 136 TP HD + 13 GC inputs: path, size, SHA-256
  mods/hdfeatures/convert/data/expected_outputs.inc  the 176 kept outputs (rel, size, byte SHA-256, overlay),
                                                     the 30-file drop list, the set id, the oracle pin

Facts only: paths, sizes and hashes. No game bytes are written. The inputs are only read.

usage (from the project root; the inputs are your own games, read-only):
  python -I -B tools/dev/gen_tables.py --hd "<TP HD title or content folder>" --gc "<extracted GZ2E01 files folder>"
      --oracle cache/oracle/hd_features-oracle-2.0.0.dusk [--hd-list tools/dist/hd_files.txt]
      [--out mods/hdfeatures/convert/data] [--check]

--check compares the freshly computed tables with the .inc files on disk and writes nothing.
The oracle must be the pinned file (Correction #3); any other oracle is refused. A change to the Python pipeline
means: rebuild the oracle (without --src), copy it to cache/oracle, update ORACLE_SHA256 here and in
include/hdf/outputs.hpp, and run this script again (new set id).
"""
import argparse
import hashlib
import os
import sys
import zipfile

ORACLE_SHA256 = 'a7a48bf643e621b89e3860fe9d7c48b26e76ec9b6cf518af8c5ff2b42bc68f59'
ORACLE_SIZE = 41757577

# hd_files.txt entries used only for the Mods-menu art (art/hdfeatures/make.py), not by the converter.
HD_ART_ONLY = ('res/object/logous.pack.gz', 'res/object/title.pack.gz')
HD_COUNT = 136

# Plan 2.5: the 13 GC inputs (D_SB01/STG_00.arc only fed a log line and is left out).
GC_FILES = ['res/msgus/bmgres.arc'] + ['res/msgus/bmgres%d.arc' % i for i in range(1, 9)] + [
    'res/msgus/bmgres99.arc', 'res/fontus/fontres.arc', 'res/stage/r_sp160/stg_00.arc',
    'res/particle/pscene200.jpc']
GC_COUNT = 13
PSCENE_BUDGET = 267040          # largest res/Particle/Pscene*.jpc (271,136) - particles.SAFETY (4,096)

# The package files that are not converter outputs.
PACKAGE_ONLY = ('mod.json', 'lib/windows-amd64/mod.dll', 'res/icon.png', 'res/banner.png')
# Plan 4 "Drop list" (VERIFIED by grep of src/): files no C++ code reads.
DROP_FIXED = (
    'res/cave/floors.json', 'res/cave/colossal_wallet.png',
    'res/cave/hdtext/messages.json', 'res/cave/hdtext/README.md',
    'res/stamps/hdtext/messages.json', 'res/stamps/hdtext/flows.json', 'res/stamps/hdtext/README.md',
    'res/cave/confirm/confirm.json', 'res/stamps/stampbook/book.json',
    'res/stamps/stamps/stamp_background.png', 'res/stamps/stamps/stamps.json',
)
AMIIBO_DIR = 'res/cave/amiibo/'
AMIIBO_KEPT = ('popup.png', 'tt_input_glow.png', 'tt_input_prompt.png', 'amiibo_wolflink.png', 'font_amiibo.png',
               'los_heart.png')
# Plan 7.1 numbers (VERIFIED by listing the oracle).
KEPT_COUNT, KEPT_BYTES = 176, 74408567
OVERLAY_COUNT, OVERLAY_BYTES = 56, 50327200
DROP_COUNT, DROP_BYTES = 30, 1165694


def die(msg):
    sys.stderr.write('gen_tables: %s\n' % msg)
    sys.exit(1)


def sha256_file(path):
    h = hashlib.sha256()
    n = 0
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                break
            h.update(b)
            n += len(b)
    return n, h.hexdigest()


def find_ci(root, lower):
    """common.exists_ci: the real path of a '/'-separated lower-case path under root, or None."""
    d = root
    for part in lower.split('/'):
        try:
            names = {n.lower(): n for n in os.listdir(d)}
        except OSError:
            return None
        if part not in names:
            return None
        d = os.path.join(d, names[part])
    return d if os.path.isfile(d) else None


def hd_content(path):
    """A content folder, or a title folder holding content/ (as hdf_inputs.open_hd)."""
    if os.path.isfile(os.path.join(path, 'los.bin')):
        return path
    c = os.path.join(path, 'content')
    if os.path.isfile(os.path.join(c, 'los.bin')):
        return c
    die('%s is not a TP HD content or title folder (no los.bin)' % path)


def read_hd_list(path):
    with open(path, encoding='utf-8') as f:
        names = [l.strip().lower() for l in f if l.strip() and not l.startswith('#')]
    names = [n for n in names if n not in HD_ART_ONLY]
    if len(names) != HD_COUNT or len(set(names)) != HD_COUNT:
        die('%s: expected %d converter inputs, found %d' % (path, HD_COUNT, len(names)))
    return sorted(names)


def input_rows(root, names, kind):
    rows = []
    for n in sorted(names):
        p = find_ci(root, n)
        if p is None:
            die('%s input missing: %s' % (kind, n))
        size, sha = sha256_file(p)
        rows.append((n, size, sha))
    return rows


def pscene_budget(gc_root):
    d = os.path.join(gc_root, 'res', 'Particle')
    sizes = [os.path.getsize(os.path.join(d, n)) for n in os.listdir(d)
             if n.lower().startswith('pscene') and n.lower().endswith('.jpc')]
    return max(sizes) - 4096


def oracle_rows(oracle):
    size, sha = sha256_file(oracle)
    if size != ORACLE_SIZE or sha != ORACLE_SHA256:
        die('%s is not the pinned oracle (size %d, sha256 %s); see Correction #3' % (oracle, size, sha))
    kept, dropped, other = [], [], []
    with zipfile.ZipFile(oracle) as z:
        for info in z.infolist():
            name = info.filename
            if name.endswith('/'):
                continue
            if name in PACKAGE_ONLY:
                other.append(name)
                continue
            data = z.read(info)
            if name in DROP_FIXED or (name.startswith(AMIIBO_DIR) and name.endswith('.png')
                                      and name[len(AMIIBO_DIR):] not in AMIIBO_KEPT):
                dropped.append((name, len(data)))
            else:
                kept.append((name, len(data), hashlib.sha256(data).hexdigest(), name.startswith('overlay/')))
    if sorted(other) != sorted(PACKAGE_ONLY):
        die('oracle package files differ: %s' % other)
    kept.sort(key=lambda r: r[0].encode('utf-8'))
    dropped.sort(key=lambda r: r[0].encode('utf-8'))
    checks = (
        ('kept files', len(kept), KEPT_COUNT), ('kept bytes', sum(r[1] for r in kept), KEPT_BYTES),
        ('overlay files', sum(1 for r in kept if r[3]), OVERLAY_COUNT),
        ('overlay bytes', sum(r[1] for r in kept if r[3]), OVERLAY_BYTES),
        ('dropped files', len(dropped), DROP_COUNT), ('dropped bytes', sum(r[1] for r in dropped), DROP_BYTES),
    )
    for what, got, want in checks:
        if got != want:
            die('oracle %s: %d, plan says %d' % (what, got, want))
    for name in DROP_FIXED:
        if name not in [d[0] for d in dropped]:
            die('drop-list file not in the oracle: %s' % name)
    return kept, dropped


def set_id(kept):
    """Plan 2.4 + Correction #17: SHA-256 over the sorted lines 'rel \\t size \\t sha256 \\n' (byte hashes)."""
    lines = sorted('%s\t%d\t%s\n' % (r[0], r[1], r[2]) for r in kept)
    return hashlib.sha256(''.join(lines).encode('utf-8')).hexdigest()


def cstr(s):
    assert all(32 <= ord(c) < 127 and c not in '"\\' for c in s), s
    return '"%s"' % s


HEADER = ('// Generated by tools/dev/gen_tables.py (python -I -B). Do not edit by hand.',
          '// Facts only: paths, sizes and SHA-256 values. No game bytes.')


def inputs_inc(hd, gc):
    L = list(HEADER)
    L.append('// Included by src/base/inputs.cpp inside namespace hdf::inputs. Sorted by path (byte order).')
    L.append('// HD: under the title\'s content/ folder (TP HD USA v81 = the [AZAP01] bytes). GC: GZ2E01 disc paths.')
    L.append('')
    for name, rows in (('kHdTable', hd), ('kGcTable', gc)):
        L.append('inline constexpr InputFile %s[%d] = {' % (name, len(rows)))
        for p, size, sha in rows:
            L.append('    {%s, %d, %s},' % (cstr(p), size, cstr(sha)))
        L.append('};')
        L.append('inline constexpr u64 %sBytes = %d;' % (name, sum(r[1] for r in rows)))
        L.append('')
    return '\n'.join(L)


def outputs_inc(kept, dropped, sid):
    L = list(HEADER)
    L.append('// Included by src/base/outputs.cpp inside namespace hdf::outputs. Sorted by rel (byte order).')
    L.append('// Source: the pinned oracle (Correction #3); byte SHA-256 for every file, PNGs included (Correction #17).')
    L.append('')
    L.append('inline constexpr std::string_view kTableOracleSha256 = %s;' % cstr(ORACLE_SHA256))
    L.append('inline constexpr u64 kTableOracleSize = %d;' % ORACLE_SIZE)
    L.append('// SHA-256 over the sorted lines "rel \\t size \\t sha256 \\n" of kExpectedTable.')
    L.append('inline constexpr std::string_view kTableSetId = %s;' % cstr(sid))
    L.append('')
    L.append('inline constexpr ExpectedFile kExpectedTable[%d] = {' % len(kept))
    for rel, size, sha, ov in kept:
        L.append('    {%s, %d, %s, %s},' % (cstr(rel), size, cstr(sha), 'true' if ov else 'false'))
    L.append('};')
    L.append('inline constexpr u64 kExpectedTableBytes = %d;' % sum(r[1] for r in kept))
    L.append('')
    L.append('// The drop list (plan 4): made by the Python pipeline, read by no C++ code.')
    L.append('inline constexpr std::string_view kDropTable[%d] = {' % len(dropped))
    for rel, _ in dropped:
        L.append('    %s,' % cstr(rel))
    L.append('};')
    L.append('inline constexpr u64 kDropTableBytes = %d;' % sum(r[1] for r in dropped))
    L.append('')
    return '\n'.join(L)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hd', required=True)
    ap.add_argument('--gc', required=True)
    ap.add_argument('--oracle', required=True)
    ap.add_argument('--hd-list', default=os.path.join(root, 'tools', 'dist', 'hd_files.txt'))
    ap.add_argument('--out', default=os.path.join(root, 'mods', 'hdfeatures', 'convert', 'data'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()

    hd_names = read_hd_list(a.hd_list)
    if len(GC_FILES) != GC_COUNT:
        die('GC list')
    budget = pscene_budget(a.gc)
    if budget != PSCENE_BUDGET:
        die('Pscene budget %d != %d: not a GZ2E01 extraction?' % (budget, PSCENE_BUDGET))
    hd = input_rows(hd_content(a.hd), hd_names, 'HD')
    gc = input_rows(a.gc, GC_FILES, 'GC')
    kept, dropped = oracle_rows(a.oracle)
    sid = set_id(kept)

    files = {'inputs.inc': inputs_inc(hd, gc), 'expected_outputs.inc': outputs_inc(kept, dropped, sid)}
    rc = 0
    for name, text in files.items():
        path = os.path.join(a.out, name)
        if a.check:
            try:
                with open(path, encoding='utf-8', newline='') as f:
                    same = f.read() == text
            except OSError:
                same = False
            print('%s: %s' % (name, 'same' if same else 'DIFFERENT'))
            rc |= 0 if same else 1
        else:
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
            print('wrote %s' % path)
    print('hd %d files %d B; gc %d files %d B; kept %d files %d B; dropped %d; set id %s'
          % (len(hd), sum(r[1] for r in hd), len(gc), sum(r[1] for r in gc), len(kept), sum(r[1] for r in kept),
             len(dropped), sid))
    return rc


if __name__ == '__main__':
    sys.exit(main())
