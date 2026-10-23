#!/usr/bin/env python3
"""rarc_write.py - build a GameCube-style RARC (JKRArchive) from files, mirroring the layout of
retail GC TP archives (VERIFIED against D_MN05/R00_00.arc and Object/@bg0050.arc):
  header 0x20 + info 0x20, nodes at 0x40, entries 32-aligned, string table 32-aligned
  (starts with ".\\0..\\0"), file data 32-aligned; per directory: file/subdir entries then
  '.' (-> own node) and '..' (-> parent node, 0xFFFFFFFF for root); file id == entry index,
  sync flag 1, flags 0x11 (FILE|MRAM) for files, 0x02 for dirs; name hash = h*3 + c (u16).

LIBRARY
  data = rarc_write.build(root_name, entries)   entries: list of ("dir/sub/name.ext", bytes)
         (paths relative to the root; directory order = first appearance)
  rarc_write.build_from_rarc(rarc_obj, replace={member_path: bytes}) -> bytes
CLI
  python -I rarc_write.py pack <src_dir> <out.arc> [--yaz0-none]
        Pack a directory tree (as produced by 'rarc.py extract') back into a RARC. The single
        top-level folder becomes the root node. Output is NOT Yaz0-compressed (JKRArchive
        accepts raw RARC; see report).
  python -I rarc_write.py roundtrip <in.arc> <out.arc>   re-emit an archive (for testing)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def name_hash(s):
    h = 0
    for c in s.encode("shift_jis"):
        h = (h * 3 + c) & 0xFFFF
    return h


def _align(v, a=32):
    return (v + a - 1) // a * a


def build(root_name, entries):
    # directory tree
    dirs = {"": []}          # dir path -> ordered list of (kind, name)
    order = [""]
    for path, _ in entries:
        parts = path.strip("/").split("/")
        cur = ""
        for p in parts[:-1]:
            nxt = (cur + "/" + p) if cur else p
            if nxt not in dirs:
                dirs[nxt] = []
                dirs[cur].append(("dir", p))
                order.append(nxt)
            cur = nxt
        dirs[cur].append(("file", parts[-1]))
    data_of = {p.strip("/"): d for p, d in entries}
    # BFS node order like retail archives
    node_order = [""]
    q = [""]
    while q:
        d = q.pop(0)
        for kind, nm in dirs[d]:
            if kind == "dir":
                sub = (d + "/" + nm) if d else nm
                node_order.append(sub)
                q.append(sub)
    node_index = {d: i for i, d in enumerate(node_order)}
    # strings
    strtab = bytearray(b".\0..\0")
    str_off = {".": 0, "..": 2}

    def sid(s):
        if s not in str_off:
            str_off[s] = len(strtab)
            strtab.extend(s.encode("shift_jis") + b"\0")
        return str_off[s]
    # retail string order: for each node (BFS): node name, then the names of its files
    for d in node_order:
        sid(root_name if d == "" else d.split("/")[-1])
        for kind, n in dirs[d]:
            if kind == "file":
                sid(n)
    nodes = []
    ents = []
    file_blobs = []
    data_cursor = 0
    for d in node_order:
        nm = root_name if d == "" else d.split("/")[-1]
        first = len(ents)
        for kind, n in dirs[d]:
            if kind == "dir":
                sub = (d + "/" + n) if d else n
                ents.append([0xFFFF, name_hash(n), 0x02, sid(n), node_index[sub], 0x10])
            else:
                blob = data_of[(d + "/" + n) if d else n]
                ents.append([None, name_hash(n), 0x11, sid(n), data_cursor, len(blob)])
                file_blobs.append((data_cursor, blob))
                data_cursor = _align(data_cursor + len(blob))
        ents.append([0xFFFF, name_hash("."), 0x02, 0, node_index[d], 0x10])
        parent = 0xFFFFFFFF if d == "" else node_index[d.rsplit("/", 1)[0] if "/" in d else ""]
        ents.append([0xFFFF, name_hash(".."), 0x02, 2, parent, 0x10])
        typ = "ROOT" if d == "" else (nm.upper() + "    ")[:4]
        nodes.append((typ.encode("ascii", "replace"), sid(nm), name_hash(nm), len(ents) - first, first))
    for i, e in enumerate(ents):
        if e[0] is None:
            e[0] = i
    node_off = 0x20
    ent_off = _align(0x20 + len(nodes) * 0x10)          # relative to info block (0x20)
    str_rel = _align(ent_off + len(ents) * 0x14)
    strtab_p = bytes(strtab) + b"\0" * (_align(len(strtab)) - len(strtab))
    data_rel = _align(str_rel + len(strtab_p))
    data_len = _align(data_cursor) if file_blobs else 0
    total = 0x20 + data_rel + data_len
    out = bytearray(total)
    struct.pack_into(">4sIIIIIII", out, 0, b"RARC", total, 0x20, data_rel, data_len, data_len, 0, 0)
    struct.pack_into(">IIIIIIHB", out, 0x20, len(nodes), node_off, len(ents), ent_off, len(strtab_p), str_rel,
                     len(ents), 1)
    for i, (typ, so, h, cnt, first) in enumerate(nodes):
        struct.pack_into(">4sIHHI", out, 0x20 + node_off + i * 0x10, typ, so, h, cnt, first)
    for i, (eid, h, fl, so, off, sz) in enumerate(ents):
        struct.pack_into(">HHIIII", out, 0x20 + ent_off + i * 0x14, eid, h, (fl << 24) | so, off, sz, 0)
    out[0x20 + str_rel:0x20 + str_rel + len(strtab_p)] = strtab_p
    base = 0x20 + data_rel
    for off, blob in file_blobs:
        out[base + off:base + off + len(blob)] = blob
    return bytes(out)


def build_from_rarc(arc, replace=None):
    replace = replace or {}
    root = arc.nodes[0]["name"]
    entries = []
    for m in arc.files():
        rel = m.path.split("/", 1)[1]
        entries.append((rel, replace.get(m.path, replace.get(rel, m.data()))))
    return build(root, entries)


def main(argv):
    if len(argv) < 4 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    if argv[1] == "pack":
        src = argv[2]
        tops = [d for d in os.listdir(src) if os.path.isdir(os.path.join(src, d))]
        if len(tops) != 1:
            raise SystemExit("expected exactly one top-level folder in %s" % src)
        root = tops[0]
        entries = []
        for dp, dn, fn in os.walk(os.path.join(src, root)):
            dn.sort()
            for f in sorted(fn):
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, os.path.join(src, root)).replace("\\", "/")
                entries.append((rel, open(p, "rb").read()))
        data = build(root, entries)
    elif argv[1] == "roundtrip":
        import rarc
        data = build_from_rarc(rarc.Rarc.from_file(argv[2]))
        argv = argv[:2] + [argv[2], argv[3]]
    else:
        print(__doc__)
        return 1
    with open(argv[3], "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (argv[3], len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
