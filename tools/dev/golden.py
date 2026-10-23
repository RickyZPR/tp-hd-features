#!/usr/bin/env python3
"""golden.py - golden lists for the C++ converter's oracle tests (`hdf-convert test <module>`), made by the Python
pipeline (the executable spec) from YOUR OWN game files.

The lists hold hashes, sizes, counts and input paths only: no game bytes, no names from inside the files. They
are still made from your games, so they go to a build-*/ folder of the project (or a scratch folder) and are
never committed or shared. --out is refused anywhere else.

USAGE (always with -I -B: no .pyc is written into tools/)
  python -I -B tools/dev/golden.py io --hd <TP HD title or content folder> --gc <GC 'files' folder>
                                      --out build-conv/golden/io.tsv

io (module C1, plan 3.3 (c)):
  Every HD input of tools/dist/hd_files.txt (minus the 2 icon-only packs) and the 14 GC inputs of the pipeline
  (pipeline.md 3). One line per fact, tab-separated:
    input   <src> <path> <size> <sha256 of the file>
    yaz0    <src> <path> <in size> <out size> <sha256 of yaz0.decompress_yaz0>
    rarc    <src> <path> <nodes> <entries> <members> <sha256 of the member table>
    nested  <src> <path> <entry index> <raw size> <out size> <sha256 of the decoded member>
    gzip    <src> <path> <in size> <out size> <sha256 of gzip.decompress>
    tmpk    <src> <path> <count> <alignment> <unk> <sha256 of the entry table>
  member table = for each rarc.Rarc member in walk order: path \\t flags \\t offset \\t size \\t id \\t entry index \\n
  (path as the archive's Shift-JIS bytes). TMPK entry table = name \\t off \\t size \\n per entry (raw name bytes).
"""
import gzip
import hashlib
import os
import struct
import sys

sys.dont_write_bytecode = True
TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.dirname(TOOLS)
sys.path.insert(0, os.path.join(TOOLS, 'formats'))
import hdpack  # noqa: E402
import rarc  # noqa: E402
import yaz0  # noqa: E402

ICON_ONLY = {'res/object/logous.pack.gz', 'res/object/title.pack.gz'}
GC_INPUTS = ['res/msgus/bmgres.arc'] + ['res/msgus/bmgres%d.arc' % i for i in (1, 2, 3, 4, 5, 6, 7, 8, 99)] + [
    'res/fontus/fontres.arc', 'res/stage/r_sp160/stg_00.arc', 'res/stage/d_sb01/stg_00.arc',
    'res/particle/pscene200.jpc']


def sha(b):
    return hashlib.sha256(b).hexdigest()


def norm(p):
    return os.path.normcase(os.path.realpath(os.path.abspath(p)))


def within(child, parent):
    c, p = norm(child), norm(parent)
    return c == p or c.startswith(p.rstrip('\\/') + os.sep)


def check_out(out, inputs):
    if within(out, PROJECT):
        first = os.path.relpath(norm(out), norm(PROJECT)).split(os.sep)[0]
        if not first.startswith('build-') or first == 'build-hdf' or len(first) <= 6:
            raise SystemExit('golden.py: --out must be in a build-*/ folder of the project (not build-hdf) or a '
                             'scratch folder: the lists are made from your games')
    for i in inputs:
        if within(out, i) or within(i, out):
            raise SystemExit('golden.py: --out overlaps the input %s' % i)
    appdata = os.environ.get('APPDATA')
    if appdata and within(out, os.path.join(appdata, 'TwilitRealm')):
        raise SystemExit('golden.py: refusing to write into %APPDATA%\\TwilitRealm')


def find_ci(root, rel):
    """case-insensitive lookup, one component at a time (common.exists_ci)"""
    cur = root
    for part in rel.split('/'):
        if not os.path.isdir(cur):
            return None
        low = part.lower()
        hit = None
        for n in sorted(os.listdir(cur)):
            if n.lower() == low:
                hit = n
                break
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur if os.path.isfile(cur) else None


def hd_inputs():
    out = []
    with open(os.path.join(TOOLS, 'dist', 'hd_files.txt'), encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#') and line not in ICON_ONLY:
                out.append(line)
    return out


def walk_with_index(arc):
    """rarc.Rarc._walk, also giving the entry index of each member"""
    res = []
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
            else:
                res.append((prefix + '/' + e['name'], ei))
    if arc.nodes:
        walk(0, arc.nodes[0]['name'])
    return res


def tmpk_table(buf, t):
    lines = []
    for i, e in enumerate(t.entries):
        no = struct.unpack_from('>I', buf, 0x10 + i * 16)[0]
        name = buf[no:buf.index(b'\0', no)]
        assert name.decode('ascii', 'replace') == e['name']
        lines.append(name + b'\t%d\t%d\n' % (e['off'], e['size']))
    return b''.join(lines)


def arc_lines(src, path, buf, rows, counts):
    a = rarc.Rarc(buf, path)
    walked = walk_with_index(a)
    files = a.files()
    assert [p for p, _ in walked] == [m.path for m in files]
    table = b''.join(b'%s\t%d\t%d\t%d\t%d\t%d\n' % (m.path.encode('shift_jis'), m.flags, m.offset, m.size, m.eid, ei)
                     for m, (_, ei) in zip(files, walked))
    rows.append(('rarc', src, path, a.num_nodes, a.num_entries, len(files), sha(table)))
    counts['rarc'] += 1
    for m, (_, ei) in zip(files, walked):
        raw = m.raw()
        if yaz0.is_yaz0(raw):
            d = yaz0.decompress_yaz0(raw)
            rows.append(('nested', src, path, ei, len(raw), len(d), sha(d)))
            counts['nested'] += 1


def io_rows(src, root, paths, rows, counts):
    for path in paths:
        p = find_ci(root, path)
        if p is None:
            raise SystemExit('golden.py: %s input missing: %s' % (src, path))
        with open(p, 'rb') as f:
            raw = f.read()
        rows.append(('input', src, path, len(raw), sha(raw)))
        if raw[:2] == b'\x1f\x8b':
            d = gzip.decompress(raw)
            rows.append(('gzip', src, path, len(raw), len(d), sha(d)))
            counts['gzip'] += 1
            if d[:4] == b'TMPK':
                t = hdpack.Tmpk(d)
                rows.append(('tmpk', src, path, t.n, t.align, t.unk, sha(tmpk_table(d, t))))
                counts['tmpk'] += 1
        elif yaz0.is_yaz0(raw):
            d = yaz0.decompress_yaz0(raw)
            rows.append(('yaz0', src, path, len(raw), len(d), sha(d)))
            counts['yaz0'] += 1
            if d[:4] == b'RARC':
                arc_lines(src, path, raw, rows, counts)
        elif raw[:4] == b'RARC':
            arc_lines(src, path, raw, rows, counts)


def cmd_io(args):
    hd, gc, out = args['hd'], args['gc'], args['out']
    if os.path.isdir(os.path.join(hd, 'content')):
        hd = os.path.join(hd, 'content')
    check_out(out, [args['hd'], gc])
    rows = []
    counts = {'yaz0': 0, 'rarc': 0, 'nested': 0, 'gzip': 0, 'tmpk': 0}
    io_rows('hd', hd, hd_inputs(), rows, counts)
    io_rows('gc', gc, GC_INPUTS, rows, counts)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# hdf golden io 1: made by tools/dev/golden.py from your own game files (hashes and numbers only;'
                ' never commit or share)\n')
        f.write('# counts %s\n' % ' '.join('%s=%d' % kv for kv in counts.items()))
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print('golden io: %d lines -> %s (%s)' % (len(rows), out, ', '.join('%s %d' % kv for kv in counts.items())))
    return 0


def parse(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(__doc__)
        raise SystemExit(1)
    args = {'cmd': argv[1]}
    i = 2
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            args[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            raise SystemExit('golden.py: unexpected argument %r' % argv[i])
    return args


def main(argv):
    args = parse(argv)
    if args['cmd'] == 'io':
        for k in ('hd', 'gc', 'out'):
            if k not in args:
                raise SystemExit('golden.py io: --%s is required' % k)
        return cmd_io(args)
    raise SystemExit('golden.py: unknown list %r (known: io)' % args['cmd'])


if __name__ == '__main__':
    sys.exit(main(sys.argv))
