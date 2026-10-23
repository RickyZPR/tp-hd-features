#!/usr/bin/env python3
"""gen_stamp_table.py - generate mods/hdfeatures/src/stamps/stamp_table.inc (and, optionally, the
message-box icons) for the stamps of the HD Features mod.

USAGE (run with `python -I`; every path is an argument, nothing is executed from the inputs)
  python -I tools/gen_stamp_table.py [--hd <TPHD content dir>] [--arc <Obj_Stamp.arc>]
                                     [--out <stamp_table.inc>] [--icons] [--check]

  --hd     TP HD dump `content` directory (read-only), optional: checks that each of the 7 HD-only
           chests is in the HD room data where the table says (layer, chest number, item). Their
           records are NOT written to the table: the player's builder copies them from the player's
           own HD copy into res/stamps/stamps/hd_chests.bin (tools/pipeline/stampbuild.py).
  --arc    converted Obj_Stamp.arc to read the model's file index from. Default: the mod's own
           overlay (mods/hdfeatures/overlay/res/Object/Obj_Stamp.arc), else
           res/stamps/stamps/stamps.json, else 4 (the HD entry layout tools/build_assets.py keeps).
  --out    output path (default mods/hdfeatures/src/stamps/stamp_table.inc).
  --icons  also build res/stamps/stamp_icons/stampNN.bti (48x48 GX CMPR, <= 0xC00 bytes) from the
           generated res/stamps/stamps/stampNN.png. These are derived from the user's HD dump: local
           use only (git-ignored). tools/build_assets.py builds them too (build_icons below), right
           after the stamp PNGs.
  --icons-only  only build the icons (no table regeneration).
  --check  regenerate in memory and fail (exit 1) if the checked-in .inc differs.

INPUTS (data facts only; no game text or stage records are copied: the stamp names and the HD-only
chests' records are not written to the table)
  research/stamps_all50.csv   index, item id, source of all 50 stamps
  research/stamp_chests.csv   one row per physical stamp chest (stage/room/layer/tboxNo/...)

Location hints are written by hand below (HINTS) in our own words.
"""
import csv
import json
import os
import re
import struct
import sys
import zlib

sys.dont_write_bytecode = True  # keep __pycache__ out of tools/formats (not ours)
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, "formats"))

DEFAULT_HD = None  # pass --hd (developer use only)
MOD_DIR = os.path.join(ROOT, "mods", "hdfeatures")   # HD Features (the stamps were mods/stamps before 2.0.0)
DEFAULT_OUT = os.path.join(MOD_DIR, "src", "stamps", "stamp_table.inc")
DEFAULT_ARC = os.path.join(MOD_DIR, "overlay", "res", "Object", "Obj_Stamp.arc")
ICON_SRC_DIR = os.path.join(MOD_DIR, "res", "stamps", "stamps")       # stampNN.png (tools/build_assets.py)
ICON_DST_DIR = os.path.join(MOD_DIR, "res", "stamps", "stamp_icons")  # stampNN.bti

# File-entry index of archive/bmdr/stamp.bmd. HD's archive has it at entry 4 (HD item_resource
# rows: {"Obj_Stamp", bmd 4, ...}) and tools/build_assets.py (pipeline/stampbuild.py) rebuilds the
# converted archive with that same layout. The older hd2gc_out conversion re-laid it out to 3, so
# the index is read from the real archive (or its stamps.json manifest) whenever available; the
# mod also re-checks it at runtime (items.cpp, CreateItemHeap hook).
FALLBACK_BMD_INDEX = 4
DEFAULT_MANIFEST = os.path.join(MOD_DIR, "res", "stamps", "stamps", "stamps.json")

# Location hints, written for this mod (own wording). Keep them short: they render in the
# stamp book's detail pane.
HINTS = {
    0: "Forest Temple, room 1: down at water level on the way toward the big key.",
    1: "Snowpeak Ruins ground floor: the beam room you reach after the first cannonball.",
    2: "Eldin side of Hyrule Field (field room 0): a dig-in grotto; light its torches.",
    3: "Arbiter's Grounds basement: the Spinner room watched by two Stalfos.",
    4: "Eldin Field north of the little bridge toward Kakariko, past a Dominion Rod statue.",
    5: "Lanayru Field north of Hyrule Castle: a caged chest sitting in the water.",
    6: "City in the Sky, upper outdoor level where the flying plants are.",
    7: "Goron Mines archer room (room 7): under the water on the east side.",
    8: "Lanayru Field north of the castle: grotto on the east bank under a grass ring (HD-only chest).",
    9: "Lake Hylia's cliffside cave: climb the ladder, blast the rocks, light the torches.",
    10: "Lakebed Temple, the central chamber on the lower floor (room 3).",
    11: "Temple of Time, fifth floor: the room with the spiders.",
    12: "Far south of Lanayru Field below Lake Hylia: Clawshot over, then dig in as the wolf.",
    13: "Forest Temple, the room farthest east: the chest sits up on a ledge.",
    14: "Eldin Field: the cave just north of the bridge leading to Kakariko.",
    15: "Far north Lanayru Field, east shore: dig into the grotto and light three torches.",
    16: "Snowpeak Ruins ground-floor hall, hidden behind a statue.",
    17: "Gerudo Desert, north-west of the Cave of Ordeals entrance.",
    18: "South-west Lanayru Field past the watchtower ladder, inside a grotto.",
    19: "Temple of Time, seventh floor, where a Poe keeps watch.",
    20: "Goron Mines archer room, north-east corner: Clawshot up the iron grate.",
    21: "Lanayru Field south of Castle Town, close to a large pit.",
    22: "Lake Hylia near the portal: sink with the Iron Boots to a Poe platform.",
    23: "Sacred Grove side of the Lost Woods: the room beside the statue room (Spinner rails).",
    24: "Lakebed Temple, fourth floor, up a wall you can Clawshot.",
    25: "City in the Sky, fourth floor outdoors: the rope room near a Poe.",
    26: "Jovani's house in Castle Town: once all 60 Poe souls are in, talk to his cat Gengle.",
    27: "Lake Hylia's cliffside cave, the longer branch behind several bombable rocks.",
    28: "Eldin Field: the cave north of Eldin Bridge (room 10 of the stockcave).",
    29: "Cave of Ordeals: rest floor after the fourth set of ten floors (HD-only chest).",
    30: "Ordon Ranch: a dig-in grotto; light the two torches.",
    31: "Lanayru Field by Castle Town's west gate: dig inside a ring of stones.",
    32: "Eldin Field near Eldin Bridge, inside a grotto.",
    33: "Cave of Ordeals: rest floor after the first ten floors (HD-only chest).",
    34: "Southern Faron Field: a hidden grotto you have to dig into (HD-only chest).",
    35: "Cave of Ordeals: rest floor after the third set of ten floors (HD-only chest).",
    36: "Palace of Twilight first floor, after you deal with Zant's floating head.",
    37: "Gerudo Desert, north-east: a grotto with Poes, a bomb rock and torches.",
    38: "Arbiter's Grounds first floor: the chandelier room, after the third Poe.",
    39: "City in the Sky first floor, west rooms: bring the Double Clawshot.",
    40: "Hyrule Castle: the chest room right next to the boss door.",
    41: "Cave of Ordeals: rest floor after the second set of ten floors (HD-only chest).",
    42: "Hyrule Castle after the map: climb the ladder to the right of a chest.",
    43: "Lanayru Field at Castle Town's south exit (Clawshot and Spinner needed).",
    44: "Palace of Twilight first floor, second room on the right (Clawshot targets).",
    45: "Agitha's Castle in Castle Town: she hands it over once you have brought her 12 golden bugs.",
    46: "Castle Town's Malo Mart branch: make a purchase there.",
    47: "Lake Hylia: a prize from the Flight-by-Fowl minigame.",
    48: "Cave of Ordeals: the chest after the final floor 50 (HD-only chest).",
    49: "Appears by itself once the other 49 stamps are collected.",
}

SOURCES = {
    "chest": "StampSource::Chest",
    "hd_chest": "StampSource::HdChest",
    26: "StampSource::Gengle",
    45: "StampSource::Agitha",
    46: "StampSource::MaloMart",
    49: "StampSource::Automatic",
}


def die(msg):
    sys.stderr.write("gen_stamp_table: error: %s\n" % msg)
    sys.exit(2)


def warn(msg):
    sys.stderr.write("gen_stamp_table: warning: %s\n" % msg)


def read_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def gc_item_of(row):
    """Vanilla GC item of a re-stocked chest, from the 'gc_chest_here' column."""
    m = re.search(r"item=0x([0-9A-Fa-f]{2})", row.get("gc_chest_here", ""))
    return int(m.group(1), 16) if m else None


def layer_of(row):
    lay = row["layer"].strip()
    if lay in ("-", "") or "/" in lay:
        return -1  # non-layered TRES, or the same chest repeated in several layers
    return int(lay)


def load_tres_from_hd(hd_root, stage, room, tbox_no, item):
    import rarc
    import dzx
    path = os.path.join(hd_root, "res", "Stage", stage, "R%02d_00.arc" % room)
    if not os.path.isfile(path):
        return None
    arc = rarc.Rarc.from_file(path)
    for m in arc.files():
        if not m.name.lower().endswith(".dzr"):
            continue
        b = m.data()
        for c in dzx.parse(b):
            if c.canon != "TRES":
                continue
            for k in range(c.count):
                o = c.offset + k * c.stride
                e = dzx.dec_actor(b, o)
                t = e.get("tbox")
                if t and t["tboxNo"] == tbox_no and t["item"] == item:
                    return bytes(b[o:o + 0x20]), c.layer
    return None


def bmd_index_from_arc(path):
    import rarc
    arc = rarc.Rarc.from_file(path)
    for i, e in enumerate(arc.entries):
        if e["name"].lower() == "stamp.bmd" and e["id"] != 0xFFFF:
            if e["id"] != i:
                warn("%s: stamp.bmd has file id %d but entry index %d; the mod uses the entry "
                     "index (dRes_info_c::mRes order)" % (path, e["id"], i))
            return i
    die("%s has no stamp.bmd" % path)


def build(args):
    research = os.path.join(ROOT, "research")
    for name in ("stamps_all50.csv", "stamp_chests.csv"):
        if not os.path.isfile(os.path.join(research, name)):
            die("research/%s is missing: it is a developer-only input and is not in the public repository. "
                "stamp_table.inc is committed ready-made; only --icons-only works without it." % name)
    all50 = read_csv(os.path.join(research, "stamps_all50.csv"))
    chests = read_csv(os.path.join(research, "stamp_chests.csv"))
    if len(all50) != 50:
        die("stamps_all50.csv has %d rows, expected 50" % len(all50))
    by_index = {int(r["stamp_index"]): r for r in chests}

    hd_root = args.get("hd") or DEFAULT_HD
    if hd_root and not os.path.isdir(os.path.join(hd_root, "res", "Stage")):
        warn("HD dump not found at %s; the HD-only chests are not checked" % hd_root)
        hd_root = None

    arc_path = args.get("arc") or (DEFAULT_ARC if os.path.isfile(DEFAULT_ARC) else None)
    if arc_path:
        bmd_index = bmd_index_from_arc(arc_path)
        bmd_src = ("overlay archive" if os.path.abspath(arc_path) == os.path.abspath(DEFAULT_ARC)
                   else "--arc " + os.path.basename(arc_path))
    elif os.path.isfile(DEFAULT_MANIFEST):
        with open(DEFAULT_MANIFEST, encoding="utf-8") as f:
            bmd_index = int(json.load(f)["obj_stamp"]["bmd_resource_index"])
        bmd_src = "res/stamps/stamps/stamps.json"
    else:
        warn("no Obj_Stamp.arc or stamps.json to inspect; using the HD layout index %d"
             % FALLBACK_BMD_INDEX)
        bmd_index = FALLBACK_BMD_INDEX
        bmd_src = "HD entry layout, as written by tools/build_assets.py"

    lines = []
    out = lines.append
    out("// GENERATED by tools/gen_stamp_table.py - do not edit by hand.")
    out("// Data facts only (ids, flags, stage and room numbers); hints are this mod's own wording.")
    out("// Sources: research/stamps_all50.csv, research/stamp_chests.csv. The 7 HD-only chests' stage records")
    out("// are not here: the builder makes res/stamps/stamps/hd_chests.bin from the player's own HD copy.")
    out("// clang-format off")
    out("")
    out("// File index of stamp.bmd in overlay/res/Object/Obj_Stamp.arc (%s)." % bmd_src)
    out("inline constexpr int16_t kGeneratedStampBmdIndex = %d;" % bmd_index)
    out("")
    out("inline constexpr StampDef kStamps[kStampCount] = {")
    hd_only_count = 0
    chest_count = 0
    for r in sorted(all50, key=lambda r: int(r["stamp_index"])):
        idx = int(r["stamp_index"])
        item = int(r["item_id"], 16)
        hint = HINTS[idx]
        if int(r["get_msg_id"]) != item + 101:
            die("stamp %d: message id %s != 101 + item" % (idx, r["get_msg_id"]))
        stage, room, layer, tbox, tbl, gc_item = "nullptr", -1, -1, -1, -1, 0xFF
        if r["source"] == "chest":
            c = by_index.get(idx)
            if c is None:
                die("stamp %d is a chest stamp but has no stamp_chests.csv row" % idx)
            chest_count += 1
            stage = cstr(c["stage"])
            room = int(c["room"])
            layer = layer_of(c)
            tbox = int(c["tboxNo"])
            tbl = int(c["save_tbl"], 16)
            hd_only = c["flag_status"] == "FREE"
            if hd_only:
                src = SOURCES["hd_chest"]
                hd_only_count += 1
                if hd_root:
                    got = load_tres_from_hd(hd_root, c["stage"], room, tbox, item)
                    if got is None:
                        die("stamp %d: TRES record not found in the HD dump" % idx)
                    rec, hd_layer = got
                    want_layer = -1 if hd_layer is None else hd_layer
                    if want_layer != layer:
                        die("stamp %d: layer mismatch (csv %d, HD chunk %s)" % (idx, layer, hd_layer))
                    # Sanity: the record decodes to the stamp item and this tboxNo.
                    params = struct.unpack_from(">I", rec, 8)[0]
                    az = struct.unpack_from(">h", rec, 0x1C)[0]
                    if ((params >> 6) & 0x3F) != tbox or ((az >> 8) & 0xFF) != item:
                        die("stamp %d: TRES record does not decode to tboxNo %d / item 0x%02X" % (idx, tbox, item))
            else:
                src = SOURCES["chest"]
                g = gc_item_of(c)
                if g is None:
                    die("stamp %d: no GC chest item in stamp_chests.csv" % idx)
                gc_item = g
        else:
            src = SOURCES.get(idx)
            if src is None:
                die("stamp %d: unknown non-chest source" % idx)
        out("    {%2d, 0x%02X, %s, %s, %d, %d, %d, %d, 0x%02X,"
            % (idx, item, src, stage, room, layer, tbox, tbl, gc_item))
        out("        %s}," % cstr(hint))
    out("};")
    out("")
    out("// clang-format on")
    out("")
    if chest_count != 46 or hd_only_count != 7:
        die("expected 46 chests / 7 HD-only, got %d / %d" % (chest_count, hd_only_count))
    return "\n".join(lines)


# --- PNG -> BTI (CMPR) icons -------------------------------------------------------------

def png_decode(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    o = 8
    idat = b""
    plte = None
    trns = None
    w = h = depth = ctype = interlace = None
    while o < len(data):
        ln, = struct.unpack_from(">I", data, o)
        tag = data[o + 4:o + 8]
        body = data[o + 8:o + 8 + ln]
        o += 12 + ln
        if tag == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            plte = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
    if depth != 8 or interlace != 0 or ctype not in (0, 2, 3, 4, 6):
        raise ValueError("unsupported PNG (depth %s, color type %s, interlace %s)" % (depth, ctype, interlace))
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows = []
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ft == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ft == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        rows.append(bytes(line))
        prev = line
    out = bytearray(w * h * 4)
    for y, line in enumerate(rows):
        for x in range(w):
            d = (y * w + x) * 4
            if ctype == 6:
                out[d:d + 4] = line[x * 4:x * 4 + 4]
            elif ctype == 2:
                out[d:d + 4] = line[x * 3:x * 3 + 3] + b"\xff"
            elif ctype == 0:
                g = line[x]
                out[d:d + 4] = bytes((g, g, g, 255))
            elif ctype == 4:
                g, a = line[x * 2], line[x * 2 + 1]
                out[d:d + 4] = bytes((g, g, g, a))
            else:
                k = line[x]
                rgb = plte[k * 3:k * 3 + 3]
                a = trns[k] if trns and k < len(trns) else 255
                out[d:d + 4] = rgb + bytes((a,))
    return w, h, bytes(out)


PAPER = (0xF4, 0xEC, 0xD6)
EDGE = (0x7A, 0x5A, 0x30)


def make_icon_rgba(w, h, rgba, size=48, margin=4):
    """Composite the stamp art onto a 'paper' card, area-resampled to size x size."""
    inner = size - 2 * margin
    scale = max(w, h) / float(inner)
    out = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            border = x == 0 or y == 0 or x == size - 1 or y == size - 1
            r, g, b = EDGE if border else PAPER
            ix, iy = x - margin, y - margin
            # center the art inside the inner square
            ox = (inner - w / scale) / 2.0
            oy = (inner - h / scale) / 2.0
            sx0, sy0 = (ix - ox) * scale, (iy - oy) * scale
            sx1, sy1 = sx0 + scale, sy0 + scale
            if not border and 0 <= ix < inner and 0 <= iy < inner and sx1 > 0 and sy1 > 0 and sx0 < w and sy0 < h:
                acc = [0.0, 0.0, 0.0, 0.0]
                tot = 0.0
                yy = max(0, int(sy0))
                while yy < min(h, int(sy1) + 1):
                    wy = min(sy1, yy + 1) - max(sy0, yy)
                    xx = max(0, int(sx0))
                    while xx < min(w, int(sx1) + 1):
                        wx = min(sx1, xx + 1) - max(sx0, xx)
                        wgt = wx * wy
                        if wgt > 0:
                            o = (yy * w + xx) * 4
                            a = rgba[o + 3] / 255.0
                            acc[0] += rgba[o] * a * wgt
                            acc[1] += rgba[o + 1] * a * wgt
                            acc[2] += rgba[o + 2] * a * wgt
                            acc[3] += a * wgt
                            tot += wgt
                        xx += 1
                    yy += 1
                if tot > 0 and acc[3] > 0:
                    a = acc[3] / tot
                    cr, cg, cb = acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3]
                    r = int(round(cr * a + r * (1 - a)))
                    g = int(round(cg * a + g * (1 - a)))
                    b = int(round(cb * a + b * (1 - a)))
            d = (y * size + x) * 4
            out[d:d + 4] = bytes((r, g, b, 255))
    return bytes(out)


def _to565(p):
    return ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3)


def _from565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def bc1_encode(rgba, w, h):
    """Small opaque-only BC1 encoder (endpoints = darkest/brightest pixel). Linear LE blocks."""
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            px = []
            for y in range(4):
                for x in range(4):
                    o = ((by + y) * w + (bx + x)) * 4
                    px.append(tuple(rgba[o:o + 3]))
            lum = [p[0] * 77 + p[1] * 150 + p[2] * 29 for p in px]
            hi = px[lum.index(max(lum))]
            lo = px[lum.index(min(lum))]
            c0, c1 = _to565(hi), _to565(lo)
            if c0 < c1:
                c0, c1 = c1, c0
            if c0 == c1:
                out += struct.pack("<HHI", c0, c1, 0)
                continue
            p0, p1 = _from565(c0), _from565(c1)
            pal = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
                   tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
            bits = 0
            for i, p in enumerate(px):
                best = min(range(4), key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
                bits |= best << (2 * i)
            out += struct.pack("<HHI", c0, c1, bits)
    return bytes(out)


def bti_cmpr(rgba, size):
    import gxtex
    img = gxtex.bc1_to_cmpr(bc1_encode(rgba, size, size), size, size)
    hdr = struct.pack(">BBHHBBBBHIBBBBBBbbBBhI",
                      14, 0, size, size,   # format CMPR, no alpha, w, h
                      0, 0,                # wrap S/T clamp
                      0, 0, 0, 0,          # no palette
                      0, 0, 0, 0,          # mipmap/edgeLOD/bias/aniso
                      1, 1, 0, 0,          # min/mag linear, LOD 0..0
                      1, 0, 0, 0x20)       # 1 image, LOD bias, image at 0x20
    return hdr + img


def build_icons(src_dir=None, dst_dir=None):
    """stampNN.png in src_dir -> stampNN.bti in dst_dir (defaults: the mod's res/stamps/stamps and
    res/stamps/stamp_icons). Returns the number of icons written."""
    src_dir = src_dir or ICON_SRC_DIR
    dst_dir = dst_dir or ICON_DST_DIR
    if not os.path.isdir(src_dir):
        warn("no %s; skipping icons" % src_dir)
        return 0
    os.makedirs(dst_dir, exist_ok=True)
    made = 0
    for i in range(50):
        p = os.path.join(src_dir, "stamp%02d.png" % i)
        if not os.path.isfile(p):
            warn("missing %s" % p)
            continue
        with open(p, "rb") as f:
            w, h, rgba = png_decode(f.read())
        icon = make_icon_rgba(w, h, rgba)
        bti = bti_cmpr(icon, 48)
        assert len(bti) <= 0xC00, len(bti)
        with open(os.path.join(dst_dir, "stamp%02d.bti" % i), "wb") as f:
            f.write(bti)
        made += 1
    return made


def main(argv):
    args = {}
    i = 1
    flags = set()
    while i < len(argv):
        a = argv[i]
        if a in ("--hd", "--arc", "--out"):
            if i + 1 >= len(argv):
                die("%s needs a value" % a)
            args[a[2:]] = argv[i + 1]
            i += 2
        elif a in ("--icons", "--icons-only", "--check"):
            flags.add(a)
            i += 1
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            die("unknown argument %s" % a)
    if "--icons-only" in flags:
        print("gen_stamp_table: built %d icon(s)" % build_icons())
        return 0
    text = build(args)
    out = args.get("out") or DEFAULT_OUT
    if "--check" in flags:
        try:
            with open(out, encoding="utf-8") as f:
                cur = f.read()
        except OSError:
            cur = None
        if cur != text:
            sys.stderr.write("gen_stamp_table: %s is out of date\n" % out)
            return 1
        print("gen_stamp_table: %s is up to date" % out)
    else:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("gen_stamp_table: wrote %s" % out)
    if "--icons" in flags:
        print("gen_stamp_table: built %d icon(s)" % build_icons())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
