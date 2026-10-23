#!/usr/bin/env python3
"""dzx.py - parse Twilight Princess stage/room data (stage.dzs, roomN.dzs, room.dzr).

Works on raw .dzs/.dzr files OR directly on a stage/room .arc (finds dzs/dzr members
via rarc.py). Same format on GC (GZ2E01) and Wii U HD (AZAP01): big-endian.

CLI USAGE
  python -I dzx.py <file.dzr|file.dzs|R00_00.arc|STG_00.arc> [options]
     --chunks        only print the chunk table (tag, count, offset, stride)
     --actors        only print actor-like entries (ACTR/ACTn/TGOB/PLYR/TRES/TREn/SCOB/SCOn/
                     TGSC/TGDR/Door/Doon), one per line, with layer and decoded fields
     --tag XXXX      only chunks whose canonical tag or raw tag matches (repeatable)
     --json          machine-readable output (list of chunks with decoded entries)
     --member NAME   when given an .arc, only parse that member (e.g. room.dzr, stage.dzs)
  Examples
     python -I dzx.py STG_00.arc --member stage.dzs --chunks
     python -I dzx.py R00_00.arc --actors
     python -I dzx.py room.dzr --tag TRES --tag SCLS

LIBRARY USAGE
  import dzx;  chunks = dzx.parse(bytes)  -> list of Chunk(tag, canon, layer, count, offset,
                                              stride, entries=[dict,...])
  dzx.iter_actors(chunks) -> yields dicts {tag, layer, name, params, pos, angle, setid, [scale]}

FORMAT (VERIFIED against zeldaret/tp / Dusklight include/d/d_stage.h + src/d/d_stage.cpp)
  header: s32 chunkCount, then chunkCount x dStage_nodeHeader {char tag[4]; s32 entryNum;
          u32 offset (from file start)}.  The game matches tags with memcmp of all 4 chars.
  Layered tags: 3-char prefix + layer char; layer = '0'..'9' -> 0..9, 'a'..'e' -> 10..14
          (dStage_setLayerTagName: layerNo + (layerNo < 10 ? '0' : 'W')).
          ACT0..e (actors), SCO0.. (scaled objs), TRE0.. (chests), Doo0.. (doors),
          LGT0/Env0/Col0/PAL0/VRB0 (lighting/env), MEM0/MEC0, SON0.., MPA0.., UNI0...
  entries:
     ACTR/ACTn/TGOB/PLYR/TRES(room dzr)/TREn  0x20 stage_actor_data_class / stage_tresure_data_class
          char name[8]; u32 params; f32 pos[3]; s16 angle[3]; u16 setID   (fopAcM_prmBase_class)
     SCOB/SCOn/TGSC/TGDR/Door/Doon            0x24 stage_tgsc_data_class = actor + u8 scale[3] + pad
     SCLS 0x0D  char stage[8]; u8 start; s8 room; u8 f_a; u8 f_b; u8 wipe   (exits)
     STAG 0x3C?, RTBL 4 (offset table -> {u8 num,u8,u8,pad,u32 off->u8 rooms[]}),
     CAMR/RCAM 0x18, AROB/RARO 0x14, LGHT 0x1C, LGTn 0x20, PALn 0x34, Coln 0x0C, Envn 0x41,
     MULT 0x0C, FILI 0x20, RPAT/PATH 0x0C, RPPN/PPNT 0x10, SOND/SONn 0x1C, REVT 0x1C,
     FLOR 0x14, DMAP 0x10, MEMn 4, MECn 2.  Unknown tags: stride inferred from layout.
  NOTE: TRES inside the *stage* archive's roomN.dzs (minimap data for dTres_c) uses a different
        0x14 record dTres_c::data_s {u8 no(=tboxNo); s8 room; u8 status; u8 arg1; f32 pos[3];
        u8 swBit; u8 type; u8 arg2; s8 angleY} (VERIFIED: D_MN05 room0.dzs no=20/37/39 match
        the tboxNo + positions of room.dzr TRES). TRES inside room.dzr is the 0x20 actor form.
  tbox (chest) params (d_a_tbox.h): event=params>>24, shape=(params>>20)&0xF,
        sw=(params>>12)&0xFF, tboxNo=(params>>6)&0x3F, funcType=params&0x3F;
        item = (angle.z >> 8) & 0xFF ; swType = angle.x & 0xF.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

LAYER_CHARS = "0123456789abcde"
LAYER_PREFIX = {"ACT": "ACTR", "SCO": "SCOB", "TRE": "TRES", "Doo": "Door", "LGT": "LGT_",
                "Env": "Env_", "Col": "Col_", "PAL": "PAL_", "VRB": "VRB_", "MEM": "MEM_",
                "MEC": "MEC_", "SON": "SOND", "MPA": "MPAT", "UNI": "UNI_"}

ACTOR_TAGS = {"ACTR", "TGOB", "PLYR", "TRES"}
TGSC_TAGS = {"SCOB", "TGSC", "TGDR", "Door"}
STRIDE = {"ACTR": 0x20, "TGOB": 0x20, "PLYR": 0x20, "TRES": 0x20,
          "SCOB": 0x24, "TGSC": 0x24, "TGDR": 0x24, "Door": 0x24,
          "SCLS": 0x0D, "RTBL": 4, "CAMR": 0x18, "RCAM": 0x18, "AROB": 0x14, "RARO": 0x14,
          "LGHT": 0x1C, "LGT_": 0x20, "PAL_": 0x34, "Col_": 0x0C, "Env_": 0x41,
          "MULT": 0x0C, "FILI": 0x20, "RPAT": 0x0C, "PATH": 0x0C, "RPPN": 0x10, "PPNT": 0x10,
          "SOND": 0x1C, "REVT": 0x1C, "FLOR": 0x14, "DMAP": 0x10, "MEM_": 4, "MEC_": 2}

_OBJNAMES = None


def objnames():
    global _OBJNAMES
    if _OBJNAMES is None:
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "objnames.json")
        try:
            with open(p) as f:
                _OBJNAMES = json.load(f)
        except Exception:
            _OBJNAMES = {}
    return _OBJNAMES


def canon(tag):
    """Return (canonical_tag, layer or None)."""
    if len(tag) == 4 and tag[:3] in LAYER_PREFIX and tag[3] in LAYER_CHARS:
        return LAYER_PREFIX[tag[:3]], LAYER_CHARS.index(tag[3])
    return tag, None


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def dec_actor(buf, o, scaled=False):
    name = cstr(buf[o:o + 8])
    params, x, y, z, ax, ay, az, sid = struct.unpack_from(">I3f3hH", buf, o + 8)
    d = {"name": name, "params": params, "pos": [round(x, 3), round(y, 3), round(z, 3)],
         "angle": [ax, ay, az], "setid": sid}
    if scaled:
        d["scale"] = list(buf[o + 0x20:o + 0x23])
    proc = objnames().get(name)
    if proc:
        d["proc"] = proc[0]
    if name.startswith("tbox") or (proc and proc[0] in ("fpcNm_TBOX_e", "fpcNm_TBOX2_e")):
        d["tbox"] = {"item": (az >> 8) & 0xFF, "tboxNo": (params >> 6) & 0x3F,
                     "sw": (params >> 12) & 0xFF, "shape": (params >> 20) & 0xF,
                     "funcType": params & 0x3F, "event": params >> 24, "swType": ax & 0xF}
    return d


def dec_scls(buf, o):
    st = cstr(buf[o:o + 8])
    start, room, fa, fb, wipe = struct.unpack_from(">BbBBB", buf, o + 8)
    return {"stage": st, "start": start, "room": room, "f_a": fa, "f_b": fb, "wipe": wipe}


def dec_mult(buf, o):
    tx, ty, ang, room = struct.unpack_from(">ffhB", buf, o)
    return {"transX": tx, "transY": ty, "angle": ang, "room": room}


def dec_cam(buf, o):
    return {"type": cstr(buf[o:o + 16]), "arrow": buf[o + 16], "raw": buf[o + 17:o + 0x18].hex()}


def dec_arrow(buf, o):
    x, y, z, ax, ay, az, f = struct.unpack_from(">3f4h", buf, o)
    return {"pos": [round(x, 3), round(y, 3), round(z, 3)], "angle": [ax, ay, az], "f12": f}


def dec_rtbl(buf, o):
    off = struct.unpack_from(">I", buf, o)[0]
    if off + 8 > len(buf):
        return {"offset": off}
    num, f1, f2 = struct.unpack_from(">BBB", buf, off)
    roff = struct.unpack_from(">I", buf, off + 4)[0]
    rooms = list(buf[roff:roff + num]) if roff + num <= len(buf) else []
    return {"num": num, "f1": f1, "f2": f2, "rooms": rooms}


def dec_stag(buf, o):
    near, far, camtype = struct.unpack_from(">ffB", buf, o)
    d = {"near": near, "far": far, "camType": camtype, "f09": buf[o + 9],
         "f0a": struct.unpack_from(">H", buf, o + 0xA)[0],
         "f0c": "%08x" % struct.unpack_from(">I", buf, o + 0xC)[0],
         "f10": "%08x" % struct.unpack_from(">I", buf, o + 0x10)[0]}
    gap, up, down = struct.unpack_from(">hhh", buf, o + 0x1A)
    d.update({"gapLevel": gap, "rangeUp": up, "rangeDown": down, "msgGroup": buf[o + 0x28],
              "stageTitleNo": struct.unpack_from(">H", buf, o + 0x2A)[0],
              "particleNo": list(buf[o + 0x2C:o + 0x3C])})
    return d


def dec_fili(buf, o):
    p, sea = struct.unpack_from(">If", buf, o)
    return {"params": "%08x" % p, "seaLevel": sea, "defCam": buf[o + 0x1A], "bitSw": buf[o + 0x1B],
            "msg": struct.unpack_from(">H", buf, o + 0x1C)[0]}


def dec_path(buf, o):
    num, nxt, sw, loop = struct.unpack_from(">HhBB", buf, o)
    ptr = struct.unpack_from(">I", buf, o + 8)[0]
    return {"num": num, "next": nxt, "sw": sw, "loop": loop, "pntOff": ptr}


def dec_pnt(buf, o):
    a = list(buf[o:o + 4])
    x, y, z = struct.unpack_from(">3f", buf, o + 4)
    return {"arg": a, "pos": [round(x, 3), round(y, 3), round(z, 3)]}


def dec_revt(buf, o):
    return {"type": buf[o], "priority": buf[o + 6], "name": cstr(buf[o + 0xD:o + 0x1A]),
            "switch": buf[o + 0x1B], "raw": buf[o:o + 0x1C].hex()}


def dec_maptres(buf, o):
    no, room, status, arg1, x, y, z, sw, typ, arg2, angy = struct.unpack_from(">BbBB3fBBBb", buf, o)
    return {"no": no, "room": room, "status": status, "arg1": arg1,
            "pos": [round(x, 3), round(y, 3), round(z, 3)], "sw": sw, "type": typ, "arg2": arg2,
            "angleY": angy}


DECODERS = {"SCLS": dec_scls, "MULT": dec_mult, "CAMR": dec_cam, "RCAM": dec_cam,
            "AROB": dec_arrow, "RARO": dec_arrow, "RTBL": dec_rtbl, "STAG": dec_stag,
            "FILI": dec_fili, "RPAT": dec_path, "PATH": dec_path, "RPPN": dec_pnt,
            "PPNT": dec_pnt, "REVT": dec_revt}


class Chunk(object):
    def __init__(self, tag, count, offset):
        self.tag = tag
        self.canon, self.layer = canon(tag)
        self.count = count
        self.offset = offset
        self.avail = 0
        self.stride = 0
        self.entries = []
        self.note = ""

    def to_dict(self):
        return {"tag": self.tag, "canon": self.canon, "layer": self.layer, "count": self.count,
                "offset": self.offset, "stride": self.stride, "note": self.note, "entries": self.entries}


def parse(buf, file_kind=None):
    """file_kind: 'map_dzs' to mark TRES as map-treasure (dTres) records."""
    n = struct.unpack_from(">i", buf, 0)[0]
    if n < 0 or n > 512 or 4 + n * 12 > len(buf):
        raise ValueError("not a dzs/dzr (chunk count %d)" % n)
    chunks = []
    for i in range(n):
        tag, cnt, off = struct.unpack_from(">4siI", buf, 4 + i * 12)
        chunks.append(Chunk(tag.decode("latin-1"), cnt, off))
    offs = sorted(set([c.offset for c in chunks] + [len(buf)]))
    for c in chunks:
        nxt = [x for x in offs if x > c.offset]
        c.avail = (nxt[0] - c.offset) if nxt else 0
        known = STRIDE.get(c.canon)
        if c.canon == "TRES" and file_kind == "map_dzs":
            known = 0x14
            c.note = "map treasure (dTres_c::data_s) record"
        if c.count > 0 and known is None:
            c.stride = c.avail // c.count if c.count else 0
        else:
            c.stride = known or 0
        if c.count and known and c.avail and known * c.count > c.avail:
            c.note = (c.note + " WARN: stride*count > available bytes(%d)" % c.avail).strip()
        for k in range(max(c.count, 0)):
            o = c.offset + k * c.stride
            if c.stride == 0 or o + c.stride > len(buf):
                break
            if c.canon == "TRES" and file_kind == "map_dzs":
                e = dec_maptres(buf, o)
            elif c.canon in ACTOR_TAGS:
                e = dec_actor(buf, o)
            elif c.canon in TGSC_TAGS:
                e = dec_actor(buf, o, scaled=True)
            elif c.canon in DECODERS:
                try:
                    e = DECODERS[c.canon](buf, o)
                except Exception as ex:  # keep going on odd data
                    e = {"error": str(ex), "raw": buf[o:o + c.stride].hex()}
            else:
                e = {"raw": buf[o:o + min(c.stride, 0x40)].hex()}
            c.entries.append(e)
    return chunks


def iter_actors(chunks):
    for c in chunks:
        if c.canon in ACTOR_TAGS or c.canon in TGSC_TAGS:
            for e in c.entries:
                if "name" in e:
                    d = dict(e)
                    d["tag"] = c.tag
                    d["layer"] = c.layer
                    yield d


def fmt_actor(a):
    s = "%-4s L%-2s %-8s prm=%08x pos=(%9.1f,%9.1f,%9.1f) ang=(%6d,%6d,%6d) set=%04x" % (
        a["tag"], "-" if a["layer"] is None else a["layer"], a["name"], a["params"],
        a["pos"][0], a["pos"][1], a["pos"][2], a["angle"][0], a["angle"][1], a["angle"][2], a["setid"])
    if "scale" in a:
        s += " scl=%s" % ",".join(str(x) for x in a["scale"])
    if "proc" in a:
        s += " [%s]" % a["proc"]
    if "tbox" in a:
        t = a["tbox"]
        s += " {item=0x%02x tboxNo=%d sw=0x%02x shape=%d}" % (t["item"], t["tboxNo"], t["sw"], t["shape"])
    return s


def load_inputs(path, member=None):
    """Yield (label, bytes, kind) for a raw dzx file or every dzs/dzr inside an .arc."""
    with open(path, "rb") as f:
        raw = f.read()
    import yaz0
    data = yaz0.maybe_decompress(raw)
    if data[:4] == b"RARC":
        import rarc
        arc = rarc.Rarc(data, os.path.basename(path))
        for m in arc.files():
            ln = m.name.lower()
            if member and ln != member.lower():
                continue
            if ln.endswith(".dzr") or ln.endswith(".dzs"):
                kind = "map_dzs" if (ln.endswith(".dzs") and ln != "stage.dzs") else None
                yield "%s:%s" % (os.path.basename(path), m.path), m.data(), kind
    else:
        ln = os.path.basename(path).lower()
        kind = "map_dzs" if (ln.endswith(".dzs") and ln != "stage.dzs") else None
        yield path, data, kind


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    path = argv[1]
    opts = argv[2:]
    tags = [opts[i + 1] for i, o in enumerate(opts) if o == "--tag" and i + 1 < len(opts)]
    member = None
    if "--member" in opts:
        member = opts[opts.index("--member") + 1]
    out_json = []
    for label, data, kind in load_inputs(path, member):
        chunks = parse(data, kind)
        if tags:
            chunks_f = [c for c in chunks if c.tag in tags or c.canon in tags]
        else:
            chunks_f = chunks
        if "--json" in opts:
            out_json.append({"file": label, "chunks": [c.to_dict() for c in chunks_f]})
            continue
        print("=== %s  (%d chunks, %d bytes)" % (label, len(chunks), len(data)))
        if "--actors" in opts:
            for a in iter_actors(chunks_f):
                print("  " + fmt_actor(a))
            continue
        for c in chunks_f:
            print("  %-4s canon=%-4s layer=%-2s count=%-4d off=0x%05x stride=0x%02x avail=%d %s" % (
                c.tag, c.canon, "-" if c.layer is None else c.layer, c.count, c.offset, c.stride, c.avail, c.note))
            if "--chunks" in opts:
                continue
            for i, e in enumerate(c.entries):
                if "name" in e and "params" in e:
                    a = dict(e)
                    a["tag"], a["layer"] = c.tag, c.layer
                    print("     [%3d] %s" % (i, fmt_actor(a)))
                else:
                    print("     [%3d] %s" % (i, json.dumps(e)))
    if "--json" in opts:
        print(json.dumps(out_json, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
