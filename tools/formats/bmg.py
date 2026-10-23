#!/usr/bin/env python3
"""bmg.py - dump / diff Twilight Princess BMG message files (MESGbmg1), GC and HD.

CLI USAGE
  python -I bmg.py dump <file.bmg | bmgres.arc[:zel_00.bmg]> [--raw]
        One line per INF1 entry: index, message id, attrs, text (control codes shown as {..}).
  python -I bmg.py diff <gc.bmg|arc> <hd.bmg|arc>
        Messages whose id is new in B, removed from B, or whose text changed.
  Inputs: a .bmg, or an .arc (all .bmg members are processed, labelled by member name),
          or 'archive.arc:member.bmg' (use Windows-style C:/... paths in that form).

FORMAT (VERIFIED on Msgus/bmgres*.arc zel_*.bmg, GC + HD)
  0x00 'MESGbmg1' u32 size u32 nsections u8 encoding(1) ...pad to 0x20
  sections {tag[4], u32 size}: INF1, DAT1, FLW1, FLI1, ...
  INF1: u16 count, u16 entry_size(0x14), u32 (group/default color); entries:
        u32 dat1_text_offset, u16 message_id, u16 + 12 attribute bytes
  DAT1: NUL-terminated strings; 0x1A <len> <payload len-2 bytes> = control sequence.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def parse(buf):
    if buf[:8] != b"MESGbmg1":
        raise ValueError("not MESGbmg1")
    nsec = struct.unpack_from(">I", buf, 0xC)[0]
    enc = buf[0x10]
    o = 0x20
    secs = {}
    for _ in range(nsec):
        tag = buf[o:o + 4].decode("ascii", "replace")
        sz = struct.unpack_from(">I", buf, o + 4)[0]
        secs[tag] = (o, sz)
        if sz == 0:
            break
        o += sz
    io, isz = secs["INF1"]
    cnt, esz = struct.unpack_from(">HH", buf, io + 8)
    do = secs.get("DAT1", (0, 0))[0] + 8
    msgs = []
    for i in range(cnt):
        eo = io + 0x10 + i * esz
        toff, mid = struct.unpack_from(">IH", buf, eo)
        attrs = buf[eo + 6:eo + esz].hex()
        msgs.append({"index": i, "id": mid, "attrs": attrs, "text": decode(buf, do + toff, enc)})
    return {"encoding": enc, "sections": list(secs), "messages": msgs}


def decode(buf, p, enc):
    out = []
    while p < len(buf):
        c = buf[p]
        if c == 0:
            break
        if c == 0x1A and p + 1 < len(buf):
            ln = buf[p + 1]
            out.append("{%s}" % buf[p + 2:p + ln].hex())
            p += max(ln, 2)
            continue
        out.append(chr(c) if 32 <= c < 127 else ("\\n" if c == 10 else "\\x%02x" % c))
        p += 1
    return "".join(out)


def load_all(spec):
    """Yield (label, bytes) for each bmg in spec."""
    import yaz0
    arcp, member = spec, None
    if ":" in spec[2:] and not os.path.exists(spec):
        i = spec.rindex(":")
        arcp, member = spec[:i], spec[i + 1:]
    with open(arcp, "rb") as f:
        d = yaz0.maybe_decompress(f.read())
    if d[:4] == b"RARC":
        import rarc
        a = rarc.Rarc(d)
        for m in a.files():
            if m.name.lower().endswith(".bmg") and (member is None or m.name.lower() == member.lower()):
                yield os.path.basename(arcp) + ":" + m.name, m.data()
    else:
        yield os.path.basename(arcp), d


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    if argv[1] == "dump":
        for label, d in load_all(argv[2]):
            r = parse(d)
            print("=== %s enc=%d sections=%s messages=%d" % (label, r["encoding"], r["sections"], len(r["messages"])))
            for m in r["messages"]:
                if m["id"] == 0 and not m["text"]:
                    continue
                print("%5d id=%5d %s %s" % (m["index"], m["id"], m["attrs"] if "--raw" in argv else "", m["text"]))
    elif argv[1] == "diff":
        a = {}
        for label, d in load_all(argv[2]):
            for m in parse(d)["messages"]:
                a[(label.split(":")[-1], m["id"])] = m
        b = {}
        for label, d in load_all(argv[3]):
            for m in parse(d)["messages"]:
                b[(label.split(":")[-1], m["id"])] = m
        new = [k for k in b if b[k]["text"] and (k not in a or not a[k]["text"])]
        gone = [k for k in a if a[k]["text"] and (k not in b or not b[k]["text"])]
        chg = [k for k in b if k in a and a[k]["text"] and b[k]["text"] and a[k]["text"] != b[k]["text"]]
        print("# A=%s B=%s : new-in-B=%d removed-in-B=%d text-changed=%d" % (argv[2], argv[3], len(new), len(gone), len(chg)))
        for k in sorted(new):
            print("NEW  %s id=%d : %s" % (k[0], k[1], b[k]["text"][:300]))
        for k in sorted(gone):
            print("GONE %s id=%d : %s" % (k[0], k[1], a[k]["text"][:300]))
        for k in sorted(chg):
            print("CHG  %s id=%d :\n   A: %s\n   B: %s" % (k[0], k[1], a[k]["text"][:300], b[k]["text"][:300]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
