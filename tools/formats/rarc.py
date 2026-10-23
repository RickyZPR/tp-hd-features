#!/usr/bin/env python3
"""rarc.py - list / extract Nintendo RARC (JKRArchive) archives, GC TP and Wii U TP HD.
(formats-agent version; lives in tools/formats/ together with yaz0.py, dzx.py, stage_iter.py ...)

Handles Yaz0/Yay0-compressed archives transparently, and Yaz0/Yay0-compressed members
(flag 0x04) are decompressed on extract unless --raw is given.

CLI USAGE
  python -I rarc.py list    <file.arc> [--json]
        Print every member: size, flags, magic of the (decompressed) data, path.
  python -I rarc.py tree    <file.arc>
        Print the directory node table (node type tag, name, entry counts).
  python -I rarc.py extract <file.arc> <out_dir> [--raw]
        Extract all members preserving directory tree (root node name becomes top dir).
  python -I rarc.py cat     <file.arc> <member_path_or_basename> <out_file>
        Extract a single member (match by full path, path suffix, or basename).

LIBRARY USAGE
  sys.path.insert(0, '<...>/tools/formats'); import rarc
  arc = rarc.Rarc.from_file(path)        # or rarc.Rarc(bytes)
  for m in arc.files():                  # m.path, m.name, m.size, m.flags, m.data(), m.raw()
      ...
  arc.get('room.dzr').data()             # basename / path lookup; None if missing

FORMAT (verified on GC GZ2E01 + HD AZAP01 archives; both big-endian, identical header layout)
  0x00 'RARC' | u32 file_size | u32 header_size(0x20) | u32 data_off(rel 0x20)
  0x10 u32 data_len | u32 mram_size | u32 aram_size | u32 dvd_size
  0x20 info: u32 num_nodes, u32 node_off, u32 num_entries, u32 entry_off,
             u32 strtab_len, u32 strtab_off, u16 next_free_id, u8 sync_ids   (offsets rel 0x20)
  node (0x10): char type[4] ('ROOT', 'DZR ', 'BMDR', ...), u32 name_off, u16 hash,
               u16 entry_count, u32 first_entry
  entry (0x14): u16 id, u16 hash, u32 (flags<<24 | name_off), u32 data_off|node_idx,
                u32 size, u32 runtime_ptr(0)
  flags: 0x01 FILE 0x02 DIR 0x04 COMPRESSED 0x10 MRAM 0x20 ARAM 0x40 DVD 0x80 YAZ0(else YAY0)
  HD difference (VERIFIED): HD archives omit the '.'/'..' entries from node entry_counts and
  order nodes differently (e.g. ROOT, PLC, KCL, EVT, DZR, BMDR) - walking by node/entry works.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yaz0  # noqa: E402

FLAG_NAMES = [(0x01, "FILE"), (0x02, "DIR"), (0x04, "COMP"), (0x10, "MRAM"),
              (0x20, "ARAM"), (0x40, "DVD"), (0x80, "YAZ0")]


def flag_str(f):
    return "|".join(n for b, n in FLAG_NAMES if f & b) or "0"


def sniff(data):
    """Short description of a blob's magic (used by list)."""
    if len(data) < 4:
        return ""
    m = data[:8]
    if m[:4] in (b"Yaz0", b"Yay0", b"RARC"):
        return m[:4].decode()
    if m.startswith(b"J3D2") or m.startswith(b"J3D1"):
        return m[:8].decode("ascii", "replace")
    if m.startswith(b"MESGbmg1"):
        return "MESGbmg1"
    if m.startswith(b"SCRN") or m[:4] in (b"Gfx2", b"FLYT", b"RLYT", b"BNTX", b"FRES", b"TMPK", b"GMX2"):
        return m[:8].decode("ascii", "replace")
    if all(32 <= c < 127 for c in m[:4]):
        return m[:4].decode("ascii")
    return m[:4].hex()


class Member(object):
    __slots__ = ("arc", "path", "name", "flags", "offset", "size", "eid")

    def __init__(self, arc, path, name, flags, offset, size, eid):
        self.arc, self.path, self.name = arc, path, name
        self.flags, self.offset, self.size, self.eid = flags, offset, size, eid

    def raw(self):
        base = self.arc.data_base + self.offset
        return self.arc.buf[base:base + self.size]

    def data(self):
        return yaz0.maybe_decompress(self.raw())

    def __repr__(self):
        return "<Member %s %d %s>" % (self.path, self.size, flag_str(self.flags))


class Rarc(object):
    def __init__(self, buf, name="<mem>"):
        self.was_compressed = buf[:4] in (b"Yaz0", b"Yay0")
        buf = yaz0.maybe_decompress(buf)
        if buf[:4] != b"RARC":
            raise ValueError("%s: not a RARC archive (magic %r)" % (name, buf[:4]))
        self.buf = buf
        self.name = name
        (self.file_size, self.hdr_size, data_off, self.data_len, self.mram, self.aram,
         self.dvd) = struct.unpack_from(">7I", buf, 4)
        ib = self.hdr_size
        (self.num_nodes, node_off, self.num_entries, entry_off, self.str_len, str_off
         ) = struct.unpack_from(">6I", buf, ib)
        self.next_id, self.sync = struct.unpack_from(">HB", buf, ib + 0x18)
        self.node_base = ib + node_off
        self.entry_base = ib + entry_off
        self.str_base = ib + str_off
        self.data_base = ib + data_off
        self.nodes = []
        for i in range(self.num_nodes):
            t, noff, h, cnt, first = struct.unpack_from(">4sIHHI", buf, self.node_base + i * 0x10)
            self.nodes.append({"type": t.decode("ascii", "replace"), "name": self._str(noff),
                               "hash": h, "count": cnt, "first": first})
        self.entries = []
        for i in range(self.num_entries):
            eid, h, fl_name, doff, size, _ = struct.unpack_from(">HHIIII", buf, self.entry_base + i * 0x14)
            flags = fl_name >> 24
            self.entries.append({"id": eid, "hash": h, "flags": flags, "name": self._str(fl_name & 0xFFFFFF),
                                 "off": doff, "size": size})
        self._members = []
        if self.nodes:
            self._walk(0, self.nodes[0]["name"], set())

    @classmethod
    def from_file(cls, path):
        with open(path, "rb") as f:
            return cls(f.read(), os.path.basename(path))

    def _str(self, off):
        p = self.str_base + off
        e = self.buf.index(b"\0", p)
        return self.buf[p:e].decode("shift_jis", "replace")

    def _walk(self, ni, prefix, seen):
        if ni in seen or ni >= len(self.nodes):
            return
        seen.add(ni)
        n = self.nodes[ni]
        for k in range(n["count"]):
            e = self.entries[n["first"] + k]
            if e["name"] in (".", ".."):
                continue
            if e["flags"] & 0x02:
                self._walk(e["off"], prefix + "/" + e["name"], seen)
            else:
                self._members.append(Member(self, prefix + "/" + e["name"], e["name"], e["flags"],
                                            e["off"], e["size"], e["id"]))

    def files(self):
        return list(self._members)

    def get(self, key):
        key_l = key.lower()
        hits = [m for m in self._members if m.path.lower() == key_l or m.path.lower().endswith("/" + key_l)]
        if not hits:
            hits = [m for m in self._members if m.name.lower() == key_l]
        return hits[0] if hits else None


def cmd_list(path, as_json=False):
    arc = Rarc.from_file(path)
    rows = []
    for m in arc.files():
        d = m.data()
        rows.append({"path": m.path, "size": m.size, "dec_size": len(d), "flags": flag_str(m.flags),
                     "offset": m.offset, "magic": sniff(d)})
    if as_json:
        print(json.dumps({"archive": path, "yaz0": arc.was_compressed, "files": rows}, indent=1))
        return
    print("# %s  (outer %s, %d nodes, %d entries, %d files)" % (
        path, "Yaz0" if arc.was_compressed else "raw", arc.num_nodes, arc.num_entries, len(rows)))
    for r in rows:
        extra = "" if r["dec_size"] == r["size"] else " (dec %d)" % r["dec_size"]
        print("%9d%s  %-16s %-10s %s" % (r["size"], extra, r["flags"], r["magic"], r["path"]))


def cmd_tree(path):
    arc = Rarc.from_file(path)
    for i, n in enumerate(arc.nodes):
        print("node %2d type=%-4s name=%-12s entries=%d first=%d" % (i, n["type"], n["name"], n["count"], n["first"]))


def cmd_extract(path, out_dir, raw=False):
    arc = Rarc.from_file(path)
    n = 0
    for m in arc.files():
        rel = m.path.lstrip("/").replace("\\", "/")
        parts = [p for p in rel.split("/") if p not in ("", ".", "..")]
        dest = os.path.join(out_dir, *parts)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(m.raw() if raw else m.data())
        n += 1
    print("extracted %d files from %s -> %s" % (n, path, out_dir))


def cmd_cat(path, member, out_file):
    arc = Rarc.from_file(path)
    m = arc.get(member)
    if m is None:
        sys.stderr.write("member %s not found\n" % member)
        return 2
    with open(out_file, "wb") as f:
        f.write(m.data())
    print("%s:%s -> %s (%d bytes)" % (path, m.path, out_file, len(m.data())))
    return 0


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == "list":
        cmd_list(argv[2], "--json" in argv)
    elif cmd == "tree":
        cmd_tree(argv[2])
    elif cmd == "extract":
        cmd_extract(argv[2], argv[3], "--raw" in argv)
    elif cmd == "cat":
        return cmd_cat(argv[2], argv[3], argv[4])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
