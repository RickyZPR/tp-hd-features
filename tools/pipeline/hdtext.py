#!/usr/bin/env python3
"""hdtext.py - TP HD's own in-game text, HD-only message flows and the amiibo scan-screen art, read at build
time from the user's own TP HD dump into the mods' local res/ folders (git-ignored, never distributed).

CLI USAGE (normally run by tools/build_assets.py, which stages, validates and installs the output)
  python -I tools/pipeline/hdtext.py --hd <TP HD content dir> --gc <GC extraction dir>
                                     [--src <dusklight checkout>] --out <dir>
        writes <dir>/hdfeatures/res/cave/{hdtext,amiibo}/ and <dir>/hdfeatures/res/stamps/hdtext/ (the HD
        Features layout) and checks them.
  python -I tools/pipeline/hdtext.py --hd ... --gc ... show <zel_NN> <id>[-<id>] ...
        prints HD vs GC text (control codes as {group,id:args}) for a quick look.

INPUTS (read only)
  HD  res/Msgus/bmgres{,4,5}.arc (zel_00/04/05.bmg), res/LayoutRevo/amiibo.arc + amiibo.pack.gz,
      res/LayoutRevo/main2DR.pack.gz (amiibo / NFC glyphs), res/Layout/saveres.arc + .pack.gz
      (Wolf Link amiibo icon, "Cave of Shadows Record" screen), res/Layout/itemicon.pack.gz (st_figure).
  GC  res/Msgus/bmgres*.arc: the text the game really loads (override vs register, the GC charset and the
      per-box-kind line widths); res/Fontus/fontres.arc rodan_b_24_22.bfn (message font widths);
      res/Stage/<stage>/STG_00.arc STAG for the stage message groups.
  src (optional) Dusklight include/d/d_save_bit_labels.inc + src/d/d_save.cpp: label names for flows.json.

=====================================================================================================
OUTPUT SCHEMA (version 1). Every .json file is one UTF-8 JSON array, one element per line.

res/<feature>/hdtext/messages.json : [ message, ... ]   (feature = cave: amiibo/record/cave/flow text;
  stamps: stamp text; both in the HD Features mod, mods/hdfeatures)
  message = {
    "key":        "zel_05:5057"           unique: file + HD message id
    "file":       "zel_00" | "zel_04" | "zel_05"
    "group":      MessageService/FlowService group the text belongs to. 0 = zel_00 (common, ids <= 5000);
                  ids > 5000 live in the CURRENT STAGE's group (dMsgObject_c::getMessageGroup): 5 = zel_05 =
                  D_SB11 (STAG msg group 5), 4 = zel_04 = R_SP160 Castle Town shops/houses (both VERIFIED
                  from the GC/HD STAG bytes by this tool, see "stage_groups" in the build report).
    "id":         HD message id (INF1 entry +4). For "override" this is also the GC id to override.
    "hd_index":   INF1 entry index in the HD file (HD flow message nodes store this, not the id).
    "entry_hex":  40 hex chars = the 20-byte HD INF1 entry (MessageEntryData) with bytes 0-5 (DAT1 offset +
                  message id) zeroed, exactly what MessageService::register_message requires
                  (svc/flow.cpp register_message rejects a non-zero offset/id). Attribute conversions are
                  listed in "conversions".
    "text_hex":   the converted DAT1 text, GC encoding (BMG encoding byte 1, single-byte CP1252-style
                  characters, 0x1A <size> <group> <u16 id> <args> control tags), only control codes v2.0.3's
                  dMsgObject renders, terminated by exactly one NUL: valid for override_message /
                  register_message (svc/flow.cpp valid_encoded_text).
    "preview":    plain UTF-8 rendering with HD's own line breaks, for logs / RmlUi (tags as [A],
                  [Control Stick], {player}, ...; '%d' left for the caller's own formatting).
    "use":        who looks the text up: "item-get" (101 + item, the get-item demo), "item-text" (item name /
                  description ids), "shop" (shop-item tag), "flow" / "npc" (an HD flow node), "ui" (the mod's
                  own screens: show "preview" in RmlUi, or put it in a game message box via "action").
    "action":     how to get it into dMsgObject:
                  "native"   GC already has this exact text + attributes: use the GC message (custom flow nodes:
                             the GC INF1 entry index "gc.index"), install nothing.
                  "override" GC has an entry with this id and the same used attributes (INF1 bytes 0x0C item_no,
                             0x12 lines_per_page, 0x13 pad are unused by v2.0.3) but other/empty text:
                             override_message(group, id, language, text); flow nodes can then use gc.index.
                             Also chosen for item-get / item-text / shop ids even when attributes differ (the
                             game looks those ids up itself, so override is the only way in; see attr_diff).
                  "register" GC has no entry with this id or its attributes differ: register_message(group,
                             {language, entry, text}) and use the returned MessageId (>= 0x8000) in custom
                             flow nodes / setMessageIndex.
    "gc":         {"index": GC INF1 index or null, "empty": GC text empty, "same_text": bool,
                   "same_attrs": bool (used bytes only), "attr_diff": ["speaker 0x15->0x00", ...] (HD->GC)}
    "conversions": ["strip {3,0} Wii message-id override", "HD {7,6} (ZL) -> GC {0,13} L", ...]
    "category":   "amiibo" | "record" | "cave" | "wallet" | "stamp-get" | "stamp-ui" | "stamp-name" | "stamp-npc"
    "flows":      [HD flow ids whose nodes show this message]   (present when non-empty)
    "context":    {"zel04_nodes": [focus nodes of stamps/flows.json whose sub-graph shows it]} (stamps NPC)
  }
res/<feature>/hdtext/messages.bin : the same records for C++ without a JSON parser (little-endian):
  'HDTX' u32 version=1 u32 count, then per record (4-byte aligned):
  u8 file(0/4/5) u8 group u16 id u8 action(0 native,1 override,2 register) u8 language(0 English)
  u16 text_size  u8 entry[20]  u8 text[text_size]  pad to 4.
res/cave/hdtext/flows.json : [ flow, ... ]   (HD zel_05 flows 1111 Midna menu = tphd 0x457,
  1120 cave start, 1121 tier end, 1122 Great Fairy reward; none exists in GC zel_05 -> build them with
  FlowService::GraphBuilder in group 5 and start them with dMsgFlow_c::init(actor, <custom node id>);
  getInitNodeIndex() passes ids >= 0x8000 through)
res/stamps/hdtext/flows.json : the zel_04 sub-graphs around HD nodes 1755 / 2686 / 2690 (the stamp gives:
  Agitha flow 1900, Malo Mart flow 21, Gengle flow 1145) and 1711 / 1728 (Agitha flows 1813 / 1814 set temp
  flag 18, which HD's new head of flow 1900 tests): partial graphs (the focus node, all it leads to, 6 steps
  of predecessors); targets outside "nodes" are ordinary nodes of that flow. Message nodes with a GC
  equivalent carry "gc_nodes" = the GC zel_04 node indices showing the same GC entry, and "gc_start" is the
  GC FLI1 start node of the same flow id (anchor points for FlowService::patch_node on the GC graph).
  flow = {"flow_id": FLI1 id, "flows_reaching": [...], "file", "group", "start": HD node index,
          "gc_has_flow": bool, "gc_start": GC node | null, "focus": [node], "focus_note": text,
          "hd_only": [...summary...], "nodes": [node, ...]}
  node (HD node indices; null = END 0xFFFF; "raw" = the 8 FLW1 bytes):
    {"n": idx, "type": "message", "subtype": u8, "message": "zel_05:5057" (key into messages.json),
     "msg_id": 5057, "hd_entry": HD INF1 index, "action": the message's action, "gc_index": GC INF1 index
     usable directly in a node when action is native/override, else null (use the registered id),
     "next": idx, "unknown": u16 bytes 6-7, "raw": hex}
    {"n": idx, "type": "branch", "query": idx, "query_name": "FLOW_QUERY_EVENT_FLAG" | "HD_QUERY055" ...,
     "param": u16, "results": [idx, ...], "hd_only": bool, "label": {...}?, "raw": hex}
    {"n": idx, "type": "event", "event": idx, "event_name": "FLOW_EVENT_GIVE_ITEM" | "HD_EVENT044" ...,
     "params": hex8, "p0": u16, "p1": u16, "next": idx, "hd_only": bool, "labels": [{...}]?, "raw": hex}
    label = {"index": n, "hd_only": n >= 822, "bit": "0x7008", "meaning" (HD labels, INFERRED) |
             "name": "M_045", "desc" (GC labels, from --src d_save_bit_labels.inc)}
    Query / event names are FlowService's FlowGameQuery / FlowGameEvent enums (dMsgFlow_c table index;
    query001 = index 1 returns 0 when the flag IS set, critic correction #6). HD added query indices 53/54
    (= tphd query054/query055) and events 43/44/45 (event044 = Midna "Withdraw": wolf warp-out sound +
    return); labels 822-829 exist only in HD (saveBitLabels has 822 entries in v2.0.3, critic #8): the
    "bit" given is HD's saveBitLabels[n] (Zelda.rpx .rodata 0x1009F88C), for the mod's blob emulation.
res/cave/amiibo/ : PNGs of every texture zelda_amiibo_progress.blo uses (HD GTX, full HD
  resolution, RGBA8) + amiibo_wolflink.png, font_amiibo.png, font_nfc.png, st_figure.png, and
  layout.json = {"screens": {"zelda_amiibo_progress": screen, "zelda_file_select_los_record": screen},
  "textures": {name: {"file", "w", "h", "source"}}, "animations": [...], "note"}
  screen = {"width", "height", "panes": [pane, ...]} in tree order, pane =
    {"name", "kind": "PAN2"|"PIC2"|"TBX2"|"WIN2", "parent", "depth", "visible", "anchor": [ax, ay] (0/0.5/1),
     "pos": [x, y] (anchor position in the parent's anchor space, J2D mTranslate), "size": [w, h],
     "scale", "rotate", "rect": [x0, y0, x1, y1] absolute in the screen (608x448 base, rotation ignored),
     "alpha", "material": name, "textures": [...], "mat_color": "#rrggbbaa", "uv": [[u, v] x4] (PIC2),
     "corner_colors", "black"/"white" (PIC2: J2DPicture colour = black + (white - black) * texel),
     "font_size", "char_space", "line_space", "char_color", "grad_color", "text" (TBX2: designer
     placeholder; HD code fills the real text at runtime, e.g. msg_text from zel_00 1524-1573)}
=====================================================================================================

TEXT CONVERSIONS (HD -> GC, all VERIFIED against both message sets unless marked)
  * Encoding: GC and HD zel_*.bmg are both MESGbmg1 with encoding byte 1 and the same 0x1A tag syntax.
  * {3,0:u32} (MSGTAG_WII_MSGID_OVERRIDE, "use message N on Wii"): stripped. The id is HD-numbered; v2.0.3
    ignores it while g_MsgObject_HIO_c.mMessageDisplay == 0, but GC's numbering differs.
  * Group 7 = HD-only Wii U button glyphs (no case in d_msg_class.cpp; the tphd branch has none either), mapped
    to what GC's own text uses at the same spot (difflib alignment over every Msgus bmg):
      {7,1} L stick -> {0,19} Control Stick   {7,2} R stick -> {0,12} C Stick   {7,4} R-stick view -> {0,12}
      {7,5} gyro aim -> {0,19}                 {7,6} ZL -> {0,13} L              {7,7} ZR -> {0,14} R
      {7,8} map -> {3,12} (GC zel_00 #136)     {7,9} + -> "START" (GC wording)   {7,13} NFC touchpoint ->
      "the NFC touchpoint" (no GC glyph; the virtual amiibo has no reader). Any other group-7 id: removed.
  * "{0,16}, {0,15}, or {0,14}" (HD's three item buttons Y/X/R) -> "{0,16} or {0,15}" (GC wording).
  * Characters >= 0x80 GC's English text never uses -> ASCII (NFKD; (c), x). Tags outside the v2.0.3 set
    (d_msg_class.h groups 0-6/255) or with an invalid size -> removed.
  * INF1 entry: event_label_id >= 822 -> 0 (would index past saveBitLabels[822]).
  * Line fitting: HD's windows are wider, so for message-window box kinds (0/1/2/5/8/9/13/14/15/16) every
    line wider than GC's own text for that box kind (99th percentile of GC's corpus, measured like
    jmessage_tRenderingProcessor::getLineLength with GC's rodan_b_24_22.bfn) is re-wrapped, page by page
    (4 lines, getLineMax): sentence breaks at line ends are kept if the page still fits, else the page is
    reflowed, else pages break between sentences; item-get titles ({0,1}..{0,2}) and shop price headers stay
    on their own lines and "50 Rupees" never splits. Texts identical to GC's are never touched. "preview" keeps
    HD's own line breaks (the amiibo texts were laid out for HD's amiibo popup, not the message window).
"""
import argparse
import csv
import json
import os
import re
import struct
import sys
import unicodedata

sys.dont_write_bytecode = True
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import common  # noqa: E402  (puts tools/formats, tools/cave, tools/critic on sys.path)
import rarc  # noqa: E402
import hdpack  # noqa: E402
import gtx2img  # noqa: E402

SCHEMA_VERSION = 1
FILES = {'zel_00': ('bmgres.arc', 0), 'zel_04': ('bmgres4.arc', 4), 'zel_05': ('bmgres5.arc', 5)}
FILE_NO = {'zel_00': 0, 'zel_04': 4, 'zel_05': 5}
ACTION_NO = {'native': 0, 'override': 1, 'register': 2}
LABELS_GC = 822
# HD saveBitLabels[822..829] (Zelda.rpx .rodata 0x1009F88C; re-read for this tool from the decompressed
# section). Meanings VERIFIED in HD's code (research/cos_sections_spec.md §1): 824/826/828 are set only by
# the l8warp exit (by the cave controller's tier), 827 only by HD's own flow 811, 829 by the entry bookkeeping.
HD_LABELS = {822: (0x7008, 'Great Fairy reward given (flow 1122)'),
             823: (0x0610, 'amiibo use unlocked = GC M_019 (Faron light spirit revived)'),
             824: (0x7001, 'section 1 cleared (floor-6 portal used; flow 1121, query055, tier ALLdie)'),
             825: (0x5420, '= GC F_0685 (first Mirror shard scene)'),
             826: (0x7002, 'section 2 cleared (floor-20 portal used)'),
             827: (0x7101, 'Sols power in the Master Sword (HD flow 811 node 1173; flow 1121, tier ALLdie)'),
             828: (0x7004, 'section 3 cleared (floor-41 portal used)'),
             829: (0x7080, 'arrival intro pending (entry bookkeeping; R19 TagEv valid flag; flow 1120 clears it)')}

QUERY_NAMES = ['SELECT_2', 'EVENT_FLAG', 'PLAYER_FORM', 'RANDOM', 'SELECT_3', 'TALK_DISTANCE', 'RUPEES',
               'SWORD_TUTORIAL_STEP', 'SWORD_TUTORIAL_RESULT', 'SWORD_TUTORIAL_COUNT', 'TEMP_FLAG',
               'CHEST_FLAG', 'SAVE_SWITCH', 'SAVE_ITEM_FLAG', 'DUNGEON_SWITCH', 'DUNGEON_ITEM_FLAG',
               'ZONE_SWITCH', 'ZONE_ITEM_FLAG', 'ONE_ZONE_SWITCH', 'ONE_ZONE_ITEM_FLAG', 'EQUIPPED',
               'ITEM_OWNED', 'BOMB_BAG_COUNT', 'ARROWS', 'EMPTY_BOTTLES', 'SHOP_CLERK', 'TEARS_OF_LIGHT',
               'HERDING_TIME', 'LANTERN_OIL', 'REGISTER', 'GOATS_CAUGHT', 'HEARTS', 'HOLDING_LANTERN',
               'TIME_OF_DAY', 'MAGIC', 'SELECT_2_CANCEL', 'SELECT_3_CANCEL', 'BOMB_BAG_CONTENTS',
               'BOMBS_FIT', 'BOMB_BAG_FILL', 'WATER_BOMBS_FIT', 'TRANSFORM_BLOCKED', 'BOMBLINGS_FIT',
               'WARP_ALLOWED', 'GOLDEN_BUGS', 'UNDELIVERED_BUG', 'UNUSED_46', 'NEW_LETTERS', 'POE_SOULS',
               'DONATION_TOTAL', 'BALLOON_SCORE', 'IN_WATER', 'IRON_BOOTS']           # flow.h, 53 entries
EVENT_NAMES = ['SET_EVENT_FLAG', 'CLEAR_EVENT_FLAG', 'ADD_RUPEES', 'REMOVE_RUPEES', 'ADD_HEARTS',
               'REMOVE_HEARTS', 'ADD_MAGIC', 'REMOVE_MAGIC', 'START_EVENT', 'JUMP_FLOW', 'SET_TEMP_FLAG',
               'CLEAR_TEMP_FLAG', 'OPEN_DOOR', 'SELECT_VERTICAL', 'SET_SWITCH', 'CLEAR_SWITCH',
               'SHOP_SELECT', 'GIVE_ITEM', 'STAGE_DIRECTION', 'SET_SPEAKER', 'WARP_PLAYER', 'WAIT',
               'FILL_LANTERN', 'FILL_BOTTLE', 'SHOP_SOLD_OUT', 'SET_REGISTER', 'TENT_PURCHASE', 'FILL_BOMBS',
               'SELL_BOMBS', 'SELECT_HORIZONTAL', 'FILL_ARROWS', 'RETURN_RENTAL_BOMB_BAG', 'FADE_IN',
               'FADE_OUT', 'SET_TRADE_ITEM', 'REMOVE_ITEM', 'SET_SAVE_SWITCH', 'CLEAR_SAVE_SWITCH',
               'RECEIVE_LETTER', 'UNLOCK_MAP_REGION', 'EMPTY_BOTTLE', 'ADD_DONATION', 'UNUSED_42']  # 43
HD_QUERY_NOTES = {53: 'HD_QUERY054 (tphd stub returns 1; semantics INFERRED)',
                  54: 'HD_QUERY055 (HD 0x02a64650: 1 if !0x7001, 2 if 0x7001&&0x5420&&!0x7002, '
                      '3 if 0x7002&&0x7101&&!0x7004, else 0)'}
HD_EVENT_NOTES = {43: 'HD_EVENT043 (HD 0x02a65c30; only on an unreachable node)',
                  44: 'HD_EVENT044 Withdraw (HD 0x02a65c84: warp sounds, then the game-over/reset path; no flags)',
                  45: 'HD_EVENT045 (HD 0x02a65f74: demo unsuspend by its parameter; flow 1120 no-message path)'}

# ---------------------------------------------------------------------------------------------- what to extract
STAMP_ITEMS = [0x13, 0x14, 0x15, 0x3B, 0x3C, 0x4D, 0x4E, 0x52, 0x57] + list(range(0x85, 0x90)) + \
    list(range(0x92, 0x9C)) + list(range(0xA8, 0xB0)) + list(range(0xB6, 0xC0)) + [0xD8, 0xD9]
#   stamp index 0-49 order = research/stamps_all50.csv (checked by _check_stamp_csv); get message = 101 + item
STAMP_NPC_NODES = {1755: 'Agitha gives the Agitha stamp (event008 1, 0xBD)',
                   2686: 'Malo Mart sells the Malo Mart stamp (event008 1, 0xBE)',
                   2690: 'Gengle gives the Rupee stamp (event008 1, 0x98)'}
# HD nodes that set Agitha's temp flag 18, which HD's new head of flow 1900 (branch 1753) tests to hand over
# the Agitha stamp (5468/5469/6138 -> clear flag 18 -> node 1755)
STAMP_NPC_SETTERS = {1711: 'sets temp flag 18 (Agitha stamp pending) in flow 1813',
                     1728: 'sets temp flag 18 (Agitha stamp pending) in flow 1814'}
CAVE_FLOWS = [1111, 1120, 1121, 1122]


def _cave_spec():
    """(file, id, category, use, required): explicit ids are required, range members only if HD has them."""
    out = [('zel_00', 177, 'wallet', 'item-get', True), ('zel_00', 412, 'wallet', 'item-text', True),
           ('zel_00', 413, 'cave', 'ui', True), ('zel_00', 668, 'wallet', 'item-text', True)]
    # HD's pre-entry "Cave of Shadows" screen (src/cave/confirm.cpp, _open type 6 0x02a0614c): the buttons
    # 414 "Save and begin" / 415 "Cancel" and the plate's level value 855 "B%d" (read with text_or only).
    out += [('zel_00', i, 'cave', 'ui', True) for i in (414, 415, 855)]
    out += [('zel_00', i, 'record', 'ui', False) for i in range(1254, 1264)]
    # The "Cave of Shadows Record" screen (src/cave/record.cpp, dLos_record_c 0x029a2224/0x029a24a0, dFile_amiibo_c
    # 0x0294b028): 854 "x%d" (whole hearts of the damage / heart difference, Zelda amiibo count; GC's 854 is empty).
    out += [('zel_00', 854, 'record', 'ui', True)]
    out += [('zel_00', i, 'amiibo', 'ui', i in (1528, 1530, 1568)) for i in range(1524, 1574)]
    out += [('zel_05', i, 'cave' if i >= 5056 else 'other', 'flow', False) for i in range(5048, 5069)]
    return out


def _stamps_spec():
    out = [('zel_00', 101 + it, 'stamp-get', 'item-get', True) for it in STAMP_ITEMS]
    out += [('zel_00', i, 'stamp-ui', 'ui', i in (2800, 2801, 2806)) for i in range(2800, 2807)]
    out += [('zel_00', i, 'stamp-name', 'ui', True) for i in range(2890, 2940)]
    out += [('zel_04', 5464, 'stamp-npc', 'shop', True), ('zel_04', 5465, 'stamp-npc', 'shop', True)]
    return out


# ---------------------------------------------------------------------------------------------- BMG
def _u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def _u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


class Entry(object):
    __slots__ = ('index', 'id', 'entry', 'text')

    def __init__(self, index, mid, entry, text):
        self.index, self.id, self.entry, self.text = index, mid, entry, text


class Bmg(object):
    """MESGbmg1 reader (layout: tools/formats/bmg.py, Dusklight svc/flow.cpp parse_resource)."""

    def __init__(self, buf, name):
        if buf[:8] != b'MESGbmg1':
            raise ValueError('%s: not MESGbmg1' % name)
        self.name, self.buf = name, buf
        self.encoding = buf[0x10]
        self.sec = {}
        o = 0x20
        for _ in range(_u32(buf, 0xC)):
            tag = buf[o:o + 4].decode('latin-1')
            sz = _u32(buf, o + 4)
            if sz < 8 or o + 8 > len(buf):
                raise ValueError('%s: bad section %r' % (name, tag))
            sz = min(sz, len(buf) - o)          # HD's last section size counts padding past the file end
            self.sec[tag] = (o, sz)
            o += sz
        io, isz = self.sec['INF1']
        cnt, esz = struct.unpack_from('>HH', buf, io + 8)
        if esz != 20 or 0x10 + cnt * esz > isz:
            raise ValueError('%s: INF1 entry size %d' % (name, esz))
        do, dsz = self.sec['DAT1']
        dat, dend = do + 8, do + dsz
        self.entries = []
        for i in range(cnt):
            e = buf[io + 0x10 + i * esz:io + 0x10 + i * esz + esz]
            p = dat + _u32(e, 0)
            q = p
            while q < dend and buf[q] != 0:
                q += buf[q + 1] if (buf[q] == 0x1A and buf[q + 1] >= 2) else 1
            self.entries.append(Entry(i, _u16(e, 4), bytes(e), bytes(buf[p:min(q, dend - 1) + 1])))
        self.by_id = {}
        for e in self.entries:
            self.by_id.setdefault(e.id, e)
        self.nodes, self.edges, self.flows = [], [], {}
        if 'FLW1' in self.sec:
            fo, _ = self.sec['FLW1']
            nn, ne = struct.unpack_from('>HH', buf, fo + 8)
            self.nodes = [bytes(buf[fo + 0x10 + i * 8:fo + 0x18 + i * 8]) for i in range(nn)]
            eb = fo + 0x10 + nn * 8
            self.edges = [_u16(buf, eb + 2 * i) for i in range(ne)]
        if 'FLI1' in self.sec:
            lo, _ = self.sec['FLI1']
            for i in range(_u16(buf, lo + 8)):
                v, idx = struct.unpack_from('>IH', buf, lo + 0x10 + i * 8)
                self.flows.setdefault(v >> 16, idx)


def load_bmg(arc_path, member):
    a = rarc.Rarc.from_file(arc_path)
    m = a.get(member + '.bmg')
    if m is None:
        raise ValueError('%s has no %s.bmg' % (arc_path, member))
    return Bmg(m.data(), os.path.basename(arc_path) + ':' + member)


# ---------------------------------------------------------------------------------------------- text tokens
def tokens(text):
    """('c', byte) | ('t', group, id, args). Stops at the NUL. Raises on a truncated tag."""
    out = []
    q = 0
    while q < len(text) and text[q] != 0:
        c = text[q]
        if c == 0x1A:
            ln = text[q + 1] if q + 1 < len(text) else 0
            if ln < 5 or q + ln > len(text):
                raise ValueError('truncated control tag at %d' % q)
            out.append(('t', text[q + 2], _u16(text, q + 3), bytes(text[q + 5:q + ln])))
            q += ln
        else:
            out.append(('c', c))
            q += 1
    return out


def encode(toks):
    b = bytearray()
    for t in toks:
        if t[0] == 'c':
            b.append(t[1])
        else:
            b += bytes((0x1A, 5 + len(t[3]), t[1])) + struct.pack('>H', t[2]) + t[3]
    return bytes(b) + b'\0'


def show(text):
    out = []
    for t in tokens(text):
        if t[0] == 'c':
            out.append('|' if t[1] == 10 else (chr(t[1]) if 32 <= t[1] < 127 else '<%02x>' % t[1]))
        else:
            out.append('{%d,%d%s}' % (t[1], t[2], (':' + t[3].hex()) if t[3] else ''))
    return ''.join(out)


def gc_tag_known(g, i):
    """Control tags v2.0.3's d_msg_class.cpp processors handle (include/d/d_msg_class.h)."""
    if g in (1, 2):
        return True
    return (g == 0 and i <= 63) or (g == 3 and i <= 20) or (g == 4 and i <= 13) or \
        (g == 5 and i in (0, 3, 7, 8, 9, 10, 11, 12, 13, 14)) or (g == 6 and i <= 11) or (g == 255 and i <= 2)


def valid_encoded_text(t):
    """Python port of svc/flow.cpp valid_encoded_text (what override/register accept)."""
    if not t:
        return False
    o = 0
    while o < len(t):
        if t[o] == 0:
            return o + 1 == len(t)
        if t[o] == 0x1A:
            if o + 2 > len(t) or t[o + 1] < 5 or t[o + 1] > len(t) - o:
                return False
            if t[o + 2] == 255 and _u16(t, o + 3) == 0 and t[o + 1] - 5 not in (1, 4, 8):
                return False
            o += t[o + 1]
        else:
            o += 1
    return False


def _lit(s):
    return [('c', ord(ch)) for ch in s]


TAG7 = {1: ([('t', 0, 19, b'')], 'HD {7,1} (L stick) -> GC {0,19} Control Stick'),
        2: ([('t', 0, 12, b'')], 'HD {7,2} (R stick) -> GC {0,12} C Stick'),
        4: ([('t', 0, 12, b'')], 'HD {7,4} (R stick view) -> GC {0,12} C Stick'),
        5: ([('t', 0, 19, b'')], 'HD {7,5} (gyro aim) -> GC {0,19} Control Stick'),
        6: ([('t', 0, 13, b'')], 'HD {7,6} (ZL) -> GC {0,13} L'),
        7: ([('t', 0, 14, b'')], 'HD {7,7} (ZR) -> GC {0,14} R'),
        8: ([('t', 3, 12, b'')], 'HD {7,8} (map button) -> GC {3,12} (GC zel_00 #136 map prompt)'),
        9: (_lit('START'), 'HD {7,9} (+ button) -> "START" (GC wording)'),
        13: (_lit('the NFC touchpoint'), 'HD {7,13} (GamePad NFC touchpoint glyph) -> "the NFC touchpoint"')}
_ITEM3 = [('t', 0, 16, b''), ('c', 44), ('c', 32), ('t', 0, 15, b''), ('c', 44), ('c', 32), ('c', 111),
          ('c', 114), ('c', 32), ('t', 0, 14, b'')]
_ITEM2 = [('t', 0, 16, b''), ('c', 32), ('c', 111), ('c', 114), ('c', 32), ('t', 0, 15, b'')]


def _ascii(c):
    ch = bytes((c,)).decode('cp1252', 'replace')
    special = {'©': '(c)', '®': '(R)', '×': 'x', '™': 'TM', '…': '...',
               '‘': "'", '’': "'", '“': '"', '”': '"', '–': '-', '—': '-'}
    if ch in special:
        return special[ch]
    s = ''.join(x for x in unicodedata.normalize('NFKD', ch) if not unicodedata.combining(x))
    s = s.encode('ascii', 'ignore').decode('ascii')
    return s or '?'


def convert_text(text, gc_chars):
    """HD DAT1 text -> (GC text bytes incl. NUL, [conversion notes])."""
    notes = []
    toks = tokens(text)
    out = []
    for t in toks:
        if t[0] == 't' and t[1] == 3 and t[2] == 0:
            notes.append('strip {3,0} Wii message-id override (%s)' % t[3].hex())
            continue
        if t[0] == 't' and t[1] == 7:
            rep = TAG7.get(t[2])
            if rep is None:
                notes.append('remove unknown HD {7,%d}' % t[2])
                continue
            notes.append(rep[1])
            out.extend(rep[0])
            continue
        out.append(t)
    i = 0
    res = []
    while i < len(out):
        if out[i:i + len(_ITEM3)] == _ITEM3:
            res.extend(_ITEM2)
            notes.append('HD "{0,16}, {0,15}, or {0,14}" (Y/X/R) -> GC "{0,16} or {0,15}"')
            i += len(_ITEM3)
            continue
        res.append(out[i])
        i += 1
    fin = []
    for t in res:
        if t[0] == 'c' and t[1] >= 0x80 and t[1] not in gc_chars:
            s = _ascii(t[1])
            notes.append('char 0x%02x -> %r' % (t[1], s))
            fin.extend(_lit(s))
        elif t[0] == 't' and (not gc_tag_known(t[1], t[2]) or
                              (t[1] == 255 and t[2] == 0 and len(t[3]) not in (1, 4, 8))):
            notes.append('remove tag {%d,%d} unknown to v2.0.3' % (t[1], t[2]))
        else:
            fin.append(t)
    return encode(fin), notes


BTN = {10: 'A', 11: 'B', 12: 'C Stick', 13: 'L', 14: 'R', 15: 'X', 16: 'Y', 17: 'Z', 18: '+Control Pad',
       19: 'Control Stick', 20: '<', 21: '>', 22: '^', 23: 'v', 24: 'Control Stick Up', 25: 'Control Stick Down',
       26: 'Control Stick Left', 27: 'Control Stick Right', 28: 'Control Stick Up/Down',
       29: 'Control Stick Left/Right', 35: 'red target', 36: 'yellow target', 42: 'white target', 57: 'heart'}
BTN3 = {1: 'A', 2: 'B', 6: 'D-Pad Up', 7: 'D-Pad Down', 9: 'D-Pad Up', 12: 'D-Pad Right', 20: 'Z'}


def preview(gc_text):
    out = []
    for t in tokens(gc_text):
        if t[0] == 'c':
            out.append(bytes((t[1],)).decode('cp1252', 'replace'))
            continue
        _, g, i, a = t
        if g == 0 and i == 0:
            out.append('{player}')
        elif g == 0 and i == 34:
            out.append('{horse}')
        elif g == 0 and i in BTN:
            out.append('[%s]' % BTN[i])
        elif g == 3 and i in BTN3:
            out.append('[%s]' % BTN3[i])
        elif g == 6 and i in (10, 11):
            out.append('* ' if i == 10 else '  ')
    return ''.join(out)


UNUSED_ATTRS = ('item_no', 'lines_per_page', 'pad')   # d_msg_class.h: "unused" INF1 bytes 0x0C/0x12/0x13


def _attr_diff(hd_e, gc_e):
    names = {6: 'event_label', 8: 'speaker', 9: 'box_kind', 10: 'draw_type', 11: 'box_position', 12: 'item_no',
             13: 'line_alignment', 14: 'speaker_mood', 15: 'camera_attr', 16: 'talk_anim', 17: 'face_anim',
             18: 'lines_per_page', 19: 'pad'}
    d = []
    for off, n in sorted(names.items()):
        if off == 6:
            a, b = _u16(hd_e, 6), _u16(gc_e, 6)
            if a != b:
                d.append('%s %d->%d' % (n, a, b))
            continue
        if off == 7:
            continue
        if hd_e[off] != gc_e[off]:
            d.append('%s 0x%02x->0x%02x' % (n, hd_e[off], gc_e[off]))
    return d


def _strip_revo(text):
    return encode([t for t in tokens(text) if not (t[0] == 't' and t[1] == 3 and t[2] == 0)])


# ---------------------------------------------------------------------------------------------- line fitting
# HD's message windows are wider than GC's: many HD lines are wider than any GC line of the same box kind
# (measured with GC's own message font), so they would run past the GC window. Lines are re-wrapped to the
# width GC's own text uses for that box kind (99th percentile of the GC corpus, same metric), keeping TP's
# 4-line pages (jmessage_tReference::getLineMax, non-JP talk/item boxes) and the item-get title break.
# Metric = jmessage_tRenderingProcessor::getLineLength: sum(glyph width / cell width * font size X) +
# char space per character, with the 4-line text panes of GC msgcom.arc zelda_message_window_text.blo
# 'mg_e4lin' (443 px wide, font 23x22, char space 1) / msgres03.arc zelda_item_get_window_text.blo (400 px,
# 23x23, 1). GC's own lines reach ~1.07x the pane width (the panes are resized to 1.2x in d_msg_scrn_*), so
# the limit is taken from the GC corpus, not the pane width.
WRAP_BOX_KINDS = (0, 1, 2, 5, 8, 9, 13, 14, 15, 16)   # message windows; 11/12/19 = names/banners, 7 credits
LINE_MAX = 4
NAME_CHARS = 8                                         # player / horse name budget
FONT_SIZE_X, CHAR_SPACE = 23, 1


class FontMetrics(object):
    """Advance widths of GC res/Fontus/fontres.arc rodan_b_24_22.bfn (mDoExt_initFont0, the message font).
    BFN: 'FONTbfn1' header 0x20, blocks INF1 (u16 type, ascent, descent, width, leading, default char),
    WID1 (u16 first, last; {u8 kerning, u8 width} each), MAP1 (u16 type, first, last, count, table),
    GLY1 (u16 first, last, cell width, cell height, ...). Widths are returned in screen px (see above)."""

    def __init__(self, gc):
        p = common.exists_ci(os.path.join(gc, 'res', 'Fontus'), 'fontres.arc')
        f = rarc.Rarc.from_file(p).get('rodan_b_24_22.bfn').data()
        if f[:8] != b'FONTbfn1':
            raise ValueError('rodan_b_24_22.bfn: not a BFN')
        self.wid, self.maps, self.cell = {}, [], 22
        self.cell_w = None
        o = 0x20
        while o + 8 <= len(f):
            tag, sz = f[o:o + 4], _u32(f, o + 4)
            if sz < 8:
                break
            if tag == b'INF1':
                self.cell = _u16(f, o + 0xE)
            elif tag == b'GLY1' and self.cell_w is None:
                self.cell_w = _u16(f, o + 0xC)              # JUTResFont::getCellWidth (first glyph block)
            elif tag == b'WID1':
                s, e = struct.unpack_from('>HH', f, o + 8)
                for i in range(e - s + 1):
                    self.wid[s + i] = f[o + 13 + 2 * i]
            elif tag == b'MAP1':
                mt, s, e, n = struct.unpack_from('>HHHH', f, o + 8)
                self.maps.append((mt, s, e, [_u16(f, o + 16 + 2 * i) for i in range(n)]))
            o += sz
        self.cell_w = self.cell_w or self.cell
        self.name = NAME_CHARS * self.char(ord('n'))

    def char(self, c):
        """one character's advance in screen px."""
        w = self.cell
        for mt, s, e, tab in self.maps:
            if s <= c <= e:
                g = c - s if mt == 0 else (tab[c - s] if mt == 2 and c - s < len(tab) else None)
                if g is not None and g in self.wid:
                    w = self.wid[g]
                    break
        return w * FONT_SIZE_X / float(self.cell_w) + CHAR_SPACE

    def tok(self, t):
        if t[0] == 'c':
            return self.char(t[1])
        g, i = t[1], t[2]
        if g == 0 and i in (0, 34):
            return self.name
        if (g == 0 and (10 <= i <= 29 or i in (35, 36, 42, 46, 47, 48, 57))) or (g == 3 and 1 <= i <= 20) or \
                (g == 6 and 2 <= i <= 11):
            return FONT_SIZE_X + CHAR_SPACE         # outfont icon / glyph (do_outfont / do_space)
        return 0

    def width(self, toks):
        # A plain left-to-right sum: Python 3.12+ sum() of floats is compensated and can round differently.
        w = 0
        for t in toks:
            w += self.tok(t)
        return int(round(w))


def gc_line_limits(gc_bmgs, fm):
    """box kind -> 99th percentile of the widest line per GC message of that kind (same metric)."""
    per = {}
    for b in gc_bmgs:
        for e in b.entries:
            if len(e.text) <= 1:
                continue
            try:
                w = max(fm.width(l) for l in _lines(tokens(e.text)))
            except ValueError:
                continue
            per.setdefault(e.entry[9], []).append(w)
    out = {}
    for k, v in per.items():
        v.sort()
        out[k] = v[min(len(v) - 1, int(len(v) * 0.99))]
    talk = out.get(0, 450)
    for k in WRAP_BOX_KINDS:
        if len(per.get(k, [])) < 50:
            out[k] = min(out.get(k, talk), talk)        # too few GC samples: no wider than talk boxes
    return out


def _lines(toks):
    lines = [[]]
    for t in toks:
        if t == ('c', 10):
            lines.append([])
        else:
            lines[-1].append(t)
    return lines


def _visible(line):
    return any(t[0] == 'c' and t[1] != 32 for t in line) or any(t[0] == 't' and t[1] == 0 and t[2] in (0, 34)
                                                                 for t in line)


def _vis_bytes(word):
    return bytes(t[1] for t in word if t[0] == 'c')


def _words(line):
    """split at spaces; tags stay glued to their word; a number stays glued to a following "Rupee(s)"."""
    words, cur = [], []
    for t in line:
        if t == ('c', 32):
            if cur:
                words.append(cur)
                cur = []
        else:
            cur.append(t)
    if cur:
        words.append(cur)
    out = []
    for w in words:
        prev = _vis_bytes(out[-1]) if out else b''
        if out and prev and all(ch in b'0123456789,' for ch in prev) and _vis_bytes(w).startswith(b'Rupee'):
            out[-1] = out[-1] + [('c', 32)] + w                       # "50 Rupees" never split
        else:
            out.append(w)
    return out


def _join_words(words):
    line = []
    for w in words:
        if line:
            line.append(('c', 32))
        line.extend(w)
    return line


def _wrap_para(lines, limit, fm):
    words = [w for l in lines for w in _words(l)]
    out, cur, cw = [], [], 0
    sp = fm.char(32)
    for w in words:
        ww = fm.width(w)
        if cur and cw + sp + ww > limit:
            out.append(cur)
            cur, cw = [], 0
        if cur:
            cur.append(('c', 32))
            cw += sp
        cur.extend(w)
        cw += ww
    out.append(cur)
    return out


def _title_end(line):
    """item-get title ({0,1}...{0,2}) or shop header (a {255,0:06} Rupee price, GC zel_04 5435/5440 style)."""
    vis = [t for t in line if not (t[0] == 't' and t[1] == 255)]
    if bool(vis) and vis[-1][0] == 't' and vis[-1][1] == 0 and vis[-1][2] == 2:        # ...{0,2} INSTANT off
        return True
    return ('t', 255, 0, b'\x06') in line and b'Rupee' in _vis_bytes(line)


def _sentence_end(line):
    vis = _vis_bytes(line).rstrip(b' ')
    return _title_end(line) or vis[-1:] in (b'!', b'.', b'?')


def _split_sentences(lines):
    """one paragraph (lines) -> one pseudo-line per sentence (breaks after a word ending in . ! ?)."""
    out, cur = [], []
    words = [w for l in lines for w in _words(l)]
    for i, w in enumerate(words):
        cur.append(w)
        if _vis_bytes(w)[-1:] in (b'!', b'.', b'?') and i + 1 < len(words):
            out.append([_join_words(cur)])
            cur = []
    if cur:
        out.append([_join_words(cur)])
    return out


def _wrap_chunk(chunk, limit, fm, hard, sentences=False):
    """re-wrap one 4-line page -> list of paragraphs (each a list of lines; [[]] = a kept blank line).
    Paragraphs end at blank lines and where `hard(line)` is true; with `sentences` also inside lines."""
    k = len(chunk)
    while k > 0 and not _visible(chunk[k - 1]) and not chunk[k - 1]:
        k -= 1
    paras, cur = [], []
    for l in chunk[:k]:
        if not _visible(l):
            if cur:
                paras.append(cur)
                cur = []
            paras.append(None if not l else [l])
            continue
        cur.append(l)
        if hard(l):
            paras.append(cur)
            cur = []
    if cur:
        paras.append(cur)
    out = []
    for p in paras:
        if p is None:
            out.append([[]])
        elif len(p) == 1 and not _visible(p[0]):
            out.append([p[0]])
        elif sentences:
            out.extend(_wrap_para(s, limit, fm) for s in _split_sentences(p))
        else:
            out.append(_wrap_para(p, limit, fm))
    return out


def _pack(paras):
    """lay paragraphs out on 4-line pages; a paragraph that would straddle a page break starts a new page
    (blank padding) when it fits on one page by itself."""
    out = []
    for p in paras:
        used = len(out) % LINE_MAX
        if p == [[]]:
            if used:
                out.append([])
            continue
        if used and used + len(p) > LINE_MAX and len(p) <= LINE_MAX:
            out.extend([] for _ in range(LINE_MAX - used))
        out.extend(p)
    return out


def rewrap(text, limit, fm):
    """-> (text, note or None). Only pages with a line wider than `limit` are touched. Per page: keep the
    line-end sentence breaks if the page still fits 4 lines; else reflow the sentences together if that
    fits; else (the text needs another page anyway) break pages between sentences."""
    toks = tokens(text)
    lines = _lines(toks)
    if max(fm.width(l) for l in lines) <= limit:
        return text, None
    if any(t[0] == 't' and t[1] == 0 and t[2] in (8, 9) for t in toks):
        return text, 'selection message wider than GC (%d > %d px): left as is' % (
            max(fm.width(l) for l in lines), limit)
    chunks = [lines[i:i + LINE_MAX] for i in range(0, len(lines), LINE_MAX)]
    out = []
    for ci, ch in enumerate(chunks):
        if max(fm.width(l) for l in ch) <= limit:
            new = list(ch)
        else:
            new = _pack(_wrap_chunk(ch, limit, fm, _sentence_end))
            if len(new) > LINE_MAX:
                alt = _pack(_wrap_chunk(ch, limit, fm, _title_end))
                new = alt if len(alt) <= LINE_MAX else _pack(_wrap_chunk(ch, limit, fm, _sentence_end, True))
        out.extend(new)
        if ci + 1 < len(chunks):
            while len(out) % LINE_MAX:
                out.append([])
    while out and not out[-1]:
        out.pop()
    res = []
    for i, l in enumerate(out):
        if i:
            res.append(('c', 10))
        res.extend(l)
    new_text = encode(res)
    pages = (len(out) + LINE_MAX - 1) // LINE_MAX
    return new_text, 'lines re-wrapped to the GC window width (widest %d -> %d px, limit %d; %d -> %d lines, %d page%s)' % (
        max(fm.width(l) for l in lines), max(fm.width(l) for l in out), limit, len(lines), len(out), pages,
        '' if pages == 1 else 's')


class Ctx(object):
    def __init__(self, gc_chars, fm, limits):
        self.gc_chars, self.fm, self.limits = gc_chars, fm, limits


def build_message(hd, gc, fname, mid, category, use, group, ctx):
    he = hd.by_id.get(mid)
    if he is None or len(he.text) <= 1:
        return None
    text, notes = convert_text(he.text, ctx.gc_chars)
    text_hd_lines = text                     # HD's own line breaks (for "preview": RmlUi / logs)
    box = he.entry[9]
    ge0 = gc.by_id.get(mid)
    same_as_gc = ge0 is not None and len(ge0.text) > 1 and _strip_revo(ge0.text) == text
    if ctx.fm is not None and box in WRAP_BOX_KINDS and not same_as_gc:      # GC's own text already fits
        text, note = rewrap(text, ctx.limits.get(box, ctx.limits.get(0, 450)), ctx.fm)
        if note:
            notes.append(note)
    entry = bytearray(he.entry)
    entry[0:6] = b'\0' * 6
    if _u16(entry, 6) >= LABELS_GC:
        notes.append('INF1 event_label %d >= 822 -> 0' % _u16(entry, 6))
        entry[6:8] = b'\0\0'
    ge = gc.by_id.get(mid)
    info = {'index': None, 'empty': True, 'same_text': False, 'same_attrs': False, 'attr_diff': []}
    if ge is not None:
        gtext = _strip_revo(ge.text) if len(ge.text) > 1 else b'\0'
        diff = _attr_diff(bytes(entry), ge.entry)
        info = {'index': ge.index, 'empty': len(ge.text) <= 1, 'same_text': gtext == text,
                'same_attrs': not [d for d in diff if d.split(' ')[0] not in UNUSED_ATTRS], 'attr_diff': diff}
    if ge is not None and info['same_text'] and info['same_attrs']:
        action = 'native'
    elif ge is not None and (info['same_attrs'] or use in ('item-get', 'item-text', 'shop')):
        action = 'override'          # (item-get/text, shop: the game looks the id up itself: override only)
    else:
        action = 'register'
    return {'key': '%s:%d' % (fname, mid), 'file': fname, 'group': group, 'id': mid, 'hd_index': he.index,
            'entry_hex': bytes(entry).hex(), 'text_hex': text.hex(), 'preview': preview(text_hd_lines), 'use': use,
            'action': action, 'gc': info, 'conversions': notes, 'category': category}


# ---------------------------------------------------------------------------------------------- flows
def _label(idx, names):
    d = {'index': idx, 'hd_only': idx >= LABELS_GC}
    if idx >= LABELS_GC:
        bit, meaning = HD_LABELS.get(idx, (None, 'unknown HD label'))
        d['bit'] = None if bit is None else '0x%04x' % bit
        d['meaning'] = meaning
    elif names and idx < len(names) and names[idx]:
        d['name'], d['bit'] = names[idx][0], '0x%04x' % names[idx][1]
        if len(names[idx]) > 2 and names[idx][2]:
            d['desc'] = names[idx][2]
    return d


def decode_node(b, i, label_names, msg_ref):
    n = b.nodes[i]
    t = n[0]
    end = lambda v: None if v == 0xFFFF else v  # noqa: E731
    d = {'n': i}
    if t == 1:
        mi = _u16(n, 2)
        mid = b.entries[mi].id if mi < len(b.entries) else None
        d.update({'type': 'message', 'subtype': n[1], 'hd_entry': mi, 'msg_id': mid, 'next': end(_u16(n, 4)),
                  'unknown': _u16(n, 6)})
        d.update(msg_ref(mid))
    elif t == 2:
        cnt, q, prm, first = n[1], _u16(n, 2), _u16(n, 4), _u16(n, 6)
        res = [end(b.edges[first + k]) if first + k < len(b.edges) else None for k in range(cnt)]
        d.update({'type': 'branch', 'query': q,
                  'query_name': ('FLOW_QUERY_' + QUERY_NAMES[q]) if q < len(QUERY_NAMES) else
                  HD_QUERY_NOTES.get(q, 'HD_QUERY%03d' % (q + 1)).split(' ')[0],
                  'param': prm, 'results': res, 'hd_only': q >= len(QUERY_NAMES)})
        if q >= len(QUERY_NAMES):
            d['note'] = HD_QUERY_NOTES.get(q, 'HD-only query')
        if q == 1:
            d['label'] = _label(prm, label_names)
            if prm >= LABELS_GC:
                d['hd_only'] = True
    elif t == 3:
        ev = n[1]
        p0, p1 = _u16(n, 4), _u16(n, 6)
        nx = _u16(n, 2)
        d.update({'type': 'event', 'event': ev,
                  'event_name': ('FLOW_EVENT_' + EVENT_NAMES[ev]) if ev < len(EVENT_NAMES) else
                  HD_EVENT_NOTES.get(ev, 'HD_EVENT%03d' % ev).split(' ')[0],
                  'params': n[4:8].hex(), 'p0': p0, 'p1': p1, 'hd_only': ev >= len(EVENT_NAMES)})
        if ev == 9:
            d['next'] = None
            d['jump_flow'] = _u32(n, 4)
        else:
            d['next'] = None if nx == 0xFFFF else end(b.edges[nx]) if nx < len(b.edges) else None
        if ev >= len(EVENT_NAMES):
            d['note'] = HD_EVENT_NOTES.get(ev, 'HD-only event')
        if ev in (0, 1):
            d['labels'] = [_label(x, label_names) for x in (p0, p1) if x]
            if any(x >= LABELS_GC for x in (p0, p1)):
                d['hd_only'] = True
    else:
        d['type'] = 'type%d' % t
    d['raw'] = n.hex()
    return d


def reachable(b, start):
    seen, order, stack = set(), [], [start]
    while stack:
        i = stack.pop()
        if i is None or i == 0xFFFF or i in seen or i >= len(b.nodes):
            continue
        seen.add(i)
        order.append(i)
        n = b.nodes[i]
        if n[0] == 1:
            stack.append(_u16(n, 4))
        elif n[0] == 2:
            first = _u16(n, 6)
            stack.extend(reversed([b.edges[first + k] for k in range(n[1]) if first + k < len(b.edges)]))
        elif n[0] == 3 and n[1] != 9:
            nx = _u16(n, 2)
            if nx < len(b.edges):
                stack.append(b.edges[nx])
    return order


def predecessors(b):
    pred = {}
    for i in range(len(b.nodes)):
        n = b.nodes[i]
        succ = []
        if n[0] == 1:
            succ = [_u16(n, 4)]
        elif n[0] == 2:
            first = _u16(n, 6)
            succ = [b.edges[first + k] for k in range(n[1]) if first + k < len(b.edges)]
        elif n[0] == 3 and n[1] != 9 and _u16(n, 2) < len(b.edges):
            succ = [b.edges[_u16(n, 2)]]
        for s in succ:
            pred.setdefault(s, []).append(i)
    return pred


def neighbourhood(b, node, back=6):
    """the focus node, everything it leads to, and up to `back` steps of predecessors (sorted)."""
    pred = predecessors(b)
    keep = set(reachable(b, node))
    front = {node}
    for _ in range(back):
        nxt = set()
        for x in front:
            nxt.update(pred.get(x, []))
        nxt -= keep
        keep |= nxt
        front = nxt
    return sorted(keep)


def flows_reaching(b, node):
    return sorted(f for f, s in b.flows.items() if node in set(reachable(b, s)))


# ---------------------------------------------------------------------------------------------- labels (names)
def label_names(src):
    """index -> (name, bit) for the 822 GC labels, from the Dusklight checkout (None if unavailable)."""
    try:
        inc = common.read(os.path.join(src, 'include', 'd', 'd_save_bit_labels.inc')).decode('utf-8', 'replace')
        cpp = common.read(os.path.join(src, 'src', 'd', 'd_save.cpp')).decode('utf-8', 'replace')
    except (OSError, TypeError):
        return None
    vals, desc = {}, {}
    for m in re.finditer(r'^\s*([A-Z][A-Z0-9_]*)\s*=\s*(0x[0-9a-fA-F]+),?[ \t]*(?://[ \t]*([^\n]*))?', inc, re.M):
        vals.setdefault(m.group(1), int(m.group(2), 16))
        desc.setdefault(m.group(1), re.sub(r'\s+', ' ', (m.group(3) or '')).strip())
    m = re.search(r'saveBitLabels\[822\]\s*=\s*\{(.*?)\};', cpp, re.S)
    if not m:
        return None
    names = [x.strip() for x in re.sub(r'//[^\n]*', '', m.group(1)).split(',') if x.strip()]
    if len(names) != LABELS_GC:
        return None
    return [(nm, vals.get(nm, 0xFFFF if nm == 'UNUSED' else 0), desc.get(nm, '')) for nm in names]


# ---------------------------------------------------------------------------------------------- stage groups
def stage_msg_group(stage_dir):
    """STAG mMsgGroup (d_stage.h stage_stag_info_class +0x28) of res/Stage/<x>/STG_00.arc."""
    import stagedata
    p = common.exists_ci(stage_dir, 'STG_00.arc')
    if p is None:
        return None
    a = rarc.Rarc.from_file(p)
    m = a.get('stage.dzs')
    if m is None:
        return None
    d = m.data()
    c = stagedata.chunk_map(d).get('STAG')
    return d[c[2] + 0x28] if c else None


# ---------------------------------------------------------------------------------------------- amiibo art
def _gtx_rgba(g):
    s = hdpack.gtx_surfaces(g)['surfaces'][0]
    w, h, lin = gtx2img.deswizzle_level(s, g, 0)
    s2 = dict(s)
    s2['w'], s2['h'] = w, h
    return w, h, gtx2img.to_rgba(s2, lin)


def _tmpk_find(tm, name):
    low = '/' + name.lower()
    for e in tm.entries:
        if e['name'].lower().endswith(low):
            return tm.data(e)
    return None


def _name_tab(b, o):
    n = _u16(b, o)
    out = []
    for i in range(n):
        off = _u16(b, o + 4 + i * 4 + 2)
        e = b.index(b'\0', o + off)
        out.append(b[o + off:e].decode('latin-1'))
    return out


def _res_ref(b, blk):
    """TEX1 / FNT1 block -> names (J2DScreen::getResReference + J2DResReference::getName)."""
    size, off = _u32(b, blk + 4), _u32(b, blk + 12)
    base = blk + off
    if off >= size:
        return []
    n = _u16(b, base)
    out = []
    for i in range(n):
        r = base + _u16(b, base + 2 + 2 * i)
        typ, ln = b[r], b[r + 1]
        name = b[r + 2:r + 2 + ln].decode('latin-1').replace('\\', '/').split('/')[-1]
        out.append(re.sub(r'\.(bti|bfn)$', '', name, flags=re.I) if typ in (2, 3, 4) else name)
    return out


def _mat1(b, blk, tex_names):
    """MAT1 -> [{name, textures, mat_color}] (J2DMaterialFactory layout, init data 0xE8 per material)."""
    cnt = _u16(b, blk + 8)
    hdr = struct.unpack_from('>24I', b, blk + 0xC)
    init, ids, names_off = blk + hdr[0], blk + hdr[1], hdr[2]
    matcolor, texno = blk + hdr[5], blk + hdr[11]
    names = _name_tab(b, blk + names_off) if names_off else [''] * cnt
    out = []
    for i in range(cnt):
        mi = _u16(b, ids + 2 * i)
        d = init + mi * 0xE8
        tex = []
        for k in range(8):
            ti = _u16(b, d + 0x38 + 2 * k)
            if ti != 0xFFFF:
                tn = _u16(b, texno + 2 * ti)
                tex.append(tex_names[tn] if tn < len(tex_names) else '#%d' % tn)
        mc = _u16(b, d + 0x8)
        col = None if mc == 0xFFFF else '#' + b[matcolor + 4 * mc:matcolor + 4 * mc + 4].hex()
        tg = b[blk + hdr[8] + b[d + 3]] if b[d + 3] != 0xFF else 0           # newTexGenNum
        st = b[blk + hdr[16] + b[d + 4]] if b[d + 4] != 0xFF else 0xFF      # newTevStageNum
        tev = []
        for k in range(2):                                                  # newTevColor (GXColorS10)
            ci = _u16(b, d + 0x92 + 2 * k)
            rgba = struct.unpack_from('>4h', b, blk + hdr[14] + 8 * ci) if ci != 0xFFFF else (0, 0, 0, 0)
            tev.append('#' + bytes(v & 0xFF for v in rgba).hex())
        # J2DPicture(stream, materials): black/white interpolation colours (output = black + (white-black)*tex)
        if (tg == 1 and st != 1) or (tg != 1 and st != tg + 1):
            black, white = tev
        else:
            black, white = '#00000000', '#ffffffff'
        out.append({'name': names[i] if i < len(names) else '', 'textures': tex, 'mat_color': col,
                    'black': black, 'white': white})
    return out


def parse_blo(b):
    """SCRN blo2 -> {width, height, panes} (J2DScreen::makeHierarchyPanes / J2DPane::makePaneExStream)."""
    if b[:8] not in (b'SCRNblo2', b'SCRNblo1'):
        raise ValueError('not a SCRN blo')
    o = 0x20
    inf_w = inf_h = None
    tex, mats, panes = [], [], []
    stack = [None]
    last = None
    while o + 8 <= len(b):
        tag, sz = b[o:o + 4].decode('latin-1'), _u32(b, o + 4)
        if sz < 8:
            raise ValueError('bad block size at 0x%x' % o)
        if tag == 'INF1':
            inf_w, inf_h = struct.unpack_from('>HH', b, o + 8)
        elif tag == 'TEX1':
            tex = _res_ref(b, o)
        elif tag == 'MAT1':
            mats = _mat1(b, o, tex)
        elif tag == 'BGN1':
            stack.append(last)
        elif tag == 'END1':
            stack.pop()
        elif tag == 'EXT1':
            break
        elif tag in ('PAN2', 'PIC2', 'TBX2', 'WIN2'):
            p = o if tag == 'PAN2' else o + 8
            psz = _u32(b, p + 4)
            vis, basep = b[p + 0xC], b[p + 0xD]
            name = b[p + 0x10:p + 0x18].strip(b'\0').decode('latin-1')
            w, h, sx, sy, rx, ry, rz, tx, ty = struct.unpack_from('>9f', b, p + 0x20)
            parent = stack[-1]
            ax, ay = (basep % 3) / 2.0, (basep // 3) / 2.0
            if parent is None:
                anc = (tx, ty)
            else:
                pa = panes[parent]['_anchor_abs']
                anc = (pa[0] + tx, pa[1] + ty)
            pane = {'name': name, 'kind': tag, 'parent': None if parent is None else panes[parent]['name'],
                    'depth': len(stack) - 1, 'visible': bool(vis), 'anchor': [ax, ay], 'pos': [tx, ty],
                    'size': [w, h], 'scale': [sx, sy], 'rotate': [rx, ry, rz],
                    'rect': [round(anc[0] - ax * w, 2), round(anc[1] - ay * h, 2),
                             round(anc[0] - ax * w + w, 2), round(anc[1] - ay * h + h, 2)],
                    '_anchor_abs': anc}
            q = p + psz
            if tag == 'PIC2':
                matno = _u16(b, q + 4)
                uv = [[_u16(b, q + 0x10 + 4 * k) / 256.0, _u16(b, q + 0x12 + 4 * k) / 256.0] for k in range(4)]
                pane['uv'] = uv
                pane['corner_colors'] = ['#' + b[q + 0x20 + 4 * k:q + 0x24 + 4 * k].hex() for k in range(4)]
                pane['_mat'] = matno
            elif tag == 'TBX2':
                matno, cs, ls, fx, fy, hb, vb = struct.unpack_from('>HhhHHBB', b, q + 4)
                pane.update({'font_size': [fx, fy], 'char_space': cs, 'line_space': ls, 'hbind': hb, 'vbind': vb,
                             'char_color': '#' + b[q + 0x10:q + 0x14].hex(),
                             'grad_color': '#' + b[q + 0x14:q + 0x18].hex()})
                slen = _u16(b, q + 0x1E)
                raw = b[q + 0x20:q + 0x20 + slen].split(b'\0')[0]
                try:                                   # designer placeholder text (EN or Shift-JIS)
                    pane['text'] = raw.decode('shift_jis')
                except UnicodeDecodeError:
                    pane['text'] = raw.decode('cp1252', 'replace')
                pane['_mat'] = matno
            elif tag == 'WIN2':
                pane['_mat'] = 0xFFFF
            m = pane.pop('_mat', 0xFFFF)
            if m != 0xFFFF and m < len(mats):
                pane['material'] = mats[m]['name']
                pane['textures'] = mats[m]['textures']
                pane['mat_color'] = mats[m]['mat_color']
                if tag == 'PIC2':
                    pane['black'], pane['white'] = mats[m]['black'], mats[m]['white']
                if mats[m]['mat_color']:
                    pane['alpha'] = int(mats[m]['mat_color'][-2:], 16)
            last = len(panes)
            panes.append(pane)
        o += sz
    for pn in panes:
        pn.pop('_anchor_abs', None)
    return {'width': inf_w, 'height': inf_h, 'textures_used': sorted({t for p in panes for t in p.get('textures', [])}),
            'panes': panes}


def build_amiibo(hd, out_dir, log):
    res = {'pngs': {}, 'screens': {}, 'missing': []}
    lr = os.path.join(hd, 'res', 'LayoutRevo')
    ly = os.path.join(hd, 'res', 'Layout')
    textures = {}

    def export(tm, name, out_name, source):
        g = _tmpk_find(tm, name + '.bti.gtx') if tm is not None else None
        if g is None:
            res['missing'].append(source + ':' + name)
            return
        w, h, rgba = _gtx_rgba(g)
        data = common.png_bytes(w, h, rgba)
        common.write(os.path.join(out_dir, out_name), data)
        res['pngs'][out_name] = (w, h)
        textures[name] = {'file': out_name, 'w': w, 'h': h, 'source': source}

    def tmpk(d, n):
        p = common.exists_ci(d, n)
        return hdpack.Tmpk(hdpack.read_any(p)) if p else None

    arc_p = common.exists_ci(lr, 'amiibo.arc')
    tm = tmpk(lr, 'amiibo.pack.gz')
    if arc_p is None:
        res['missing'].append('LayoutRevo/amiibo.arc')
    else:
        a = rarc.Rarc.from_file(arc_p)
        blo = a.get('zelda_amiibo_progress.blo')
        scr = parse_blo(blo.data())
        res['screens']['zelda_amiibo_progress'] = scr
        names = sorted({m.name[:-4] for m in a.files() if m.name.lower().endswith('.bti')})
        for n in names:
            export(tm, n, n + '.png', 'LayoutRevo/amiibo.pack.gz')
        res['animations'] = sorted(m.path for m in a.files() if m.name.lower().split('.')[-1] in
                                   ('bck', 'bpk', 'btk', 'brk', 'btp', 'bca', 'bla', 'bva'))
    sv = common.exists_ci(ly, 'saveres.arc')
    stm = tmpk(ly, 'saveres.pack.gz')
    for n in ('amiibo_wolflink',):
        export(stm, n, n + '.png', 'Layout/saveres.pack.gz')
    if sv is not None:
        sa = rarc.Rarc.from_file(sv)
        m = sa.get('zelda_file_select_los_record.blo')
        if m is not None:
            scr = parse_blo(m.data())
            res['screens']['zelda_file_select_los_record'] = scr
            for n in scr['textures_used']:
                if n not in textures:
                    export(stm, n, 'los_' + n + '.png', 'Layout/saveres.pack.gz')
    mtm = tmpk(lr, 'main2DR.pack.gz')
    for n in ('font_amiibo', 'font_nfc'):
        export(mtm, n, n + '.png', 'LayoutRevo/main2DR.pack.gz')
    itm = tmpk(ly, 'itemicon.pack.gz')
    export(itm, 'st_figure', 'st_figure.png', 'Layout/itemicon.pack.gz')
    for scr in res['screens'].values():
        for t in scr['textures_used']:
            if t not in textures:
                res['missing'].append('texture ' + t)
    layout = {'version': SCHEMA_VERSION,
              'note': 'Generated by tools/pipeline/hdtext.py from the TP HD dump (zelda_amiibo_progress.blo, '
                      'zelda_file_select_los_record.blo); local use only. Coordinates: J2D 608x448 base screen.',
              'animations': res.get('animations', []),
              'animation_note': 'amiibo.arc has no J2D animation files: HD code drives the scan screen '
                                '(dFile_amiibo_c / AmiiboInterfaceThread; INFERRED)',
              'textures': textures, 'screens': res['screens']}
    data = json.dumps(layout, indent=1, sort_keys=False).encode('utf-8') + b'\n'
    common.write(os.path.join(out_dir, 'layout.json'), data)
    res['textures'] = textures
    log('hdtext: amiibo art: %d PNGs, %d screens (%s), %d missing', len(res['pngs']), len(res['screens']),
        ', '.join('%s %d panes' % (k, len(v['panes'])) for k, v in res['screens'].items()), len(res['missing']))
    return res


# ---------------------------------------------------------------------------------------------- build
def _gc_chars(bmgs):
    s = set()
    for b in bmgs:
        for e in b.entries:
            try:
                for t in tokens(e.text):
                    if t[0] == 'c' and t[1] >= 0x80:
                        s.add(t[1])
            except ValueError:
                pass
    return s


def _check_stamp_csv(project):
    """STAMP_ITEMS against the developer's research CSV when there is one, else against facts.STAMP_ITEM_IDS."""
    p = os.path.join(project, 'research', 'stamps_all50.csv') if project else None
    if not p or not os.path.exists(p):
        import facts
        return list(facts.STAMP_ITEM_IDS) == STAMP_ITEMS
    with open(p, newline='', encoding='utf-8') as f:
        rows = {int(r['stamp_index']): int(r['item_id'], 16) for r in csv.DictReader(f)}
    return [rows.get(i) for i in range(50)] == STAMP_ITEMS


def _write_json_lines(path, items):
    body = '[\n' + ',\n'.join(json.dumps(x, ensure_ascii=False, separators=(',', ':')) for x in items) + '\n]\n'
    common.write(path, body.encode('utf-8'))


def _write_bin(path, msgs):
    b = bytearray(b'HDTX' + struct.pack('<II', SCHEMA_VERSION, len(msgs)))
    for m in msgs:
        t = bytes.fromhex(m['text_hex'])
        b += struct.pack('<BBHBBH', FILE_NO[m['file']], m['group'], m['id'], ACTION_NO[m['action']], 0, len(t))
        b += bytes.fromhex(m['entry_hex']) + t
        while len(b) % 4:
            b.append(0)
    common.write(path, bytes(b))


def read_bin(data):
    if data[:4] != b'HDTX':
        raise ValueError('not HDTX')
    ver, n = struct.unpack_from('<II', data, 4)
    o, out = 12, []
    for _ in range(n):
        f, g, mid, act, lang, ts = struct.unpack_from('<BBHBBH', data, o)
        o += 8
        e = data[o:o + 20]
        t = data[o + 20:o + 20 + ts]
        o += 20 + ts
        o = (o + 3) & ~3
        out.append((f, g, mid, act, lang, e, t))
    if o != len(data):
        raise ValueError('trailing bytes')
    return ver, out


def build(hd, gc, out, log, src=None, project=common.PROJECT):
    """out = {'cave_text', 'cave_amiibo', 'stamps_text'} directories (a missing key = that part is not written,
    e.g. build_assets.py --only stamps). Returns a result dict for validate()."""
    msg_hd = os.path.join(hd, 'res', 'Msgus')
    msg_gc = os.path.join(gc, 'res', 'Msgus')
    H, G = {}, {}
    for f, (arc, _) in FILES.items():
        H[f] = load_bmg(common.exists_ci(msg_hd, arc), f)
        G[f] = load_bmg(common.exists_ci(msg_gc, arc), f)
    gc_all = []                                      # every GC zel_*.bmg: charset + window widths
    for n in sorted(os.listdir(msg_gc)):
        if n.lower().startswith('bmgres') and n.lower().endswith('.arc'):
            for m in rarc.Rarc.from_file(os.path.join(msg_gc, n)).files():
                if m.name.lower().startswith('zel_') and m.name.lower().endswith('.bmg') and 'unit' not in m.name:
                    gc_all.append(Bmg(m.data(), n + ':' + m.name))
    gc_chars = _gc_chars(gc_all)
    fm = FontMetrics(gc)
    limits = gc_line_limits(gc_all, fm)
    ctx = Ctx(gc_chars, fm, limits)
    groups = {'D_SB11': stage_msg_group(os.path.join(hd, 'res', 'Stage', 'D_SB11')),
              'R_SP160 (GC)': stage_msg_group(os.path.join(gc, 'res', 'Stage', 'R_SP160')),
              'R_SP160 (HD)': stage_msg_group(os.path.join(hd, 'res', 'Stage', 'R_SP160')),
              'D_SB01 (GC)': stage_msg_group(os.path.join(gc, 'res', 'Stage', 'D_SB01'))}
    fgroup = {'zel_00': 0, 'zel_04': groups['R_SP160 (GC)'] if groups['R_SP160 (GC)'] is not None else 4,
              'zel_05': groups['D_SB11'] if groups['D_SB11'] is not None else 5}
    names = label_names(src) if src else None
    if names is None:                                # no source checkout (player builder): built-in names
        import facts
        names = facts.label_table()
    res = {'encodings': {f: (H[f].encoding, G[f].encoding) for f in FILES}, 'stage_groups': groups,
           'file_groups': fgroup, 'label_names': names is not None, 'stamp_csv_ok': _check_stamp_csv(project),
           'missing': [], 'gc_chars': sorted(gc_chars), 'line_limits': limits, '_fm': fm}
    log('hdtext: encodings HD/GC %s; stage message groups %s', res['encodings'], groups)

    # --- cave
    def collect(spec):
        msgs, seen = [], set()
        for f, mid, cat, use, required in spec:
            m = build_message(H[f], G[f], f, mid, cat, use, fgroup[f], ctx)
            if m is None:
                if required:
                    res['missing'].append('%s:%d' % (f, mid))
                continue
            if m['key'] not in seen:
                seen.add(m['key'])
                msgs.append(m)
        return msgs, seen

    cave_msgs, seen = collect(_cave_spec())
    by_key = {m['key']: m for m in cave_msgs}
    b5, g5 = H['zel_05'], G['zel_05']

    def msg_ref_for(b, g, fname, sink, bykey, sinkseen, cat, flow_id):
        def ref(mid):
            if mid is None:
                return {'message': None, 'gc_index': None}
            key = '%s:%d' % (fname, mid)
            m = bykey.get(key)
            if m is None:
                m = build_message(b, g, fname, mid, cat, 'flow', fgroup[fname], ctx)
                if m is None:
                    return {'message': None, 'gc_index': None, 'note': 'HD entry %d has no text' % mid}
                if m['key'] not in sinkseen:
                    sinkseen.add(m['key'])
                    sink.append(m)
                bykey[key] = m
            m.setdefault('flows', [])
            if flow_id not in m['flows']:
                m['flows'].append(flow_id)
            return {'message': key, 'action': m['action'],
                    'gc_index': m['gc']['index'] if m['action'] in ('native', 'override') else None}
        return ref

    cave_flows = []
    for fid in CAVE_FLOWS:
        if fid not in b5.flows:
            res['missing'].append('flow %d' % fid)
            continue
        ref = msg_ref_for(b5, g5, 'zel_05', cave_msgs, by_key, seen, 'cave', fid)
        start = b5.flows[fid]
        nodes = [decode_node(b5, i, names, ref) for i in reachable(b5, start)]
        hd_only = ['%s %s @%d' % (n['type'], n.get('query_name') or n.get('event_name'), n['n'])
                   for n in nodes if n.get('hd_only')]
        cave_flows.append({'flow_id': fid, 'file': 'zel_05', 'group': fgroup['zel_05'], 'start': start,
                           'gc_has_flow': fid in g5.flows, 'hd_only': hd_only, 'nodes': nodes})
    res['flow_0x457'] = {'flow_1111_in_hd_zel05': 0x457 in b5.flows, 'flow_1111_in_gc_zel05': 0x457 in g5.flows,
                         'message_1111_in_hd_zel00': 1111 in H['zel_00'].by_id and len(H['zel_00'].by_id[1111].text) > 1}

    # --- stamps
    stamp_msgs, sseen = collect(_stamps_spec())
    sby = {m['key']: m for m in stamp_msgs}
    b4, g4 = H['zel_04'], G['zel_04']
    stamp_flows = []
    for node, why in sorted(list(STAMP_NPC_NODES.items()) + list(STAMP_NPC_SETTERS.items())):
        if node >= len(b4.nodes):
            res['missing'].append('zel_04 node %d' % node)
            continue
        fl = flows_reaching(b4, node)
        keep = neighbourhood(b4, node)
        ref = msg_ref_for(b4, g4, 'zel_04', stamp_msgs, sby, sseen, 'stamp-npc', fl[0] if fl else None)
        nodes = [decode_node(b4, i, names, ref) for i in keep]
        for n in nodes:
            if n['type'] == 'message' and n.get('gc_index') is not None:
                n['gc_nodes'] = [k for k, gn in enumerate(g4.nodes) if gn[0] == 1 and _u16(gn, 2) == n['gc_index']]
        for n in nodes:
            if n['type'] == 'message' and n.get('message'):
                cx = sby[n['message']].setdefault('context', {})
                cx.setdefault('zel04_nodes', [])
                if node not in cx['zel04_nodes']:
                    cx['zel04_nodes'].append(node)
        stamp_flows.append({'flow_id': fl[0] if fl else None, 'flows_reaching': fl, 'file': 'zel_04',
                            'group': fgroup['zel_04'], 'start': b4.flows.get(fl[0]) if fl else None,
                            'gc_has_flow': bool(fl) and fl[0] in g4.flows,
                            'gc_start': g4.flows.get(fl[0]) if fl else None, 'focus': [node], 'focus_note': why,
                            'hd_only': ['%s %s @%d' % (n['type'], n.get('query_name') or n.get('event_name'), n['n'])
                                        for n in nodes if n.get('hd_only')], 'nodes': nodes})
    for m in stamp_msgs:
        if m.get('use') == 'flow' and m['category'] == 'stamp-npc':
            m['use'] = 'npc'

    # --- write (only the parts asked for)
    for path in out.values():
        os.makedirs(path, exist_ok=True)
    readme = (README % {'version': SCHEMA_VERSION}).encode('utf-8')
    for key, msgs, flows in (('cave_text', cave_msgs, cave_flows), ('stamps_text', stamp_msgs, stamp_flows)):
        if key in out:
            _write_json_lines(os.path.join(out[key], 'messages.json'), msgs)
            _write_bin(os.path.join(out[key], 'messages.bin'), msgs)
            _write_json_lines(os.path.join(out[key], 'flows.json'), flows)
            common.write(os.path.join(out[key], 'README.md'), readme)
    res.update({'cave_msgs': cave_msgs, 'cave_flows': cave_flows, 'stamp_msgs': stamp_msgs,
                'stamp_flows': stamp_flows, 'parts': sorted(out)})
    res['amiibo'] = build_amiibo(hd, out['cave_amiibo'], log) if 'cave_amiibo' in out else None
    conv = sum(1 for m in cave_msgs + stamp_msgs if m['conversions'])
    log('hdtext: cave %d messages (%s), %d flows (%d nodes); stamps %d messages (%s), %d NPC sub-graphs; '
        '%d texts converted; missing %s', len(cave_msgs), _count(cave_msgs), len(cave_flows),
        sum(len(f['nodes']) for f in cave_flows), len(stamp_msgs), _count(stamp_msgs), len(stamp_flows), conv,
        res['missing'] or 'none')
    return res


def _count(msgs):
    c = {}
    for m in msgs:
        c[m['action']] = c.get(m['action'], 0) + 1
    return ', '.join('%s %d' % kv for kv in sorted(c.items()))


README = """# hdtext (generated, local use only)

Generated by `tools/pipeline/hdtext.py` (run from `tools/build_assets.py`) from the user's own TP HD dump.
Contains Nintendo's text: never commit or distribute. Schema version %(version)d; the full schema is the
docstring at the top of `tools/pipeline/hdtext.py`.

- `messages.json`: one message per line: key, file, group, id, hd_index, entry_hex (20-byte INF1 entry,
  offset + id zeroed for MessageService::register_message), text_hex (GC-encoded DAT1 text incl. NUL),
  preview (plain UTF-8, HD line breaks), use, action (native / override / register), gc, conversions, category.
- `messages.bin`: the same records, little-endian, for C++ without a JSON parser:
  `'HDTX' u32 version u32 count`, then per record `u8 file u8 group u16 id u8 action u8 language
  u16 text_size u8 entry[20] u8 text[text_size]`, padded to 4 bytes. Load it with
  `svc_resource->load(mod_ctx, "<feature>/hdtext/messages.bin", &buf)` (feature = `cave` or `stamps`, the HD
  Features layout); MOD_UNAVAILABLE (file missing) means "use the mod's own fallback wording". Images for RmlUi:
  `mod://local.ricky.hdfeatures/res/cave/amiibo/<name>.png`.
- `flows.json`: HD message-flow nodes (message / branch / event) with HD node indices, resolved targets
  (null = END), query/event names (FlowService enums; HD-only ones flagged) and label info (labels >= 822
  are HD-only: emulate them with custom queries/events).
"""


# ---------------------------------------------------------------------------------------------- validation
def validate(rep, res, out):
    """Adds rows to a pipeline validate.Report."""
    g = 'hdtext'
    enc_ok = all(h == 1 and c == 1 for h, c in res['encodings'].values())
    rep.add(g, 'BMG encoding', enc_ok, 'HD/GC encoding byte %s (1 = GC single-byte)' % res['encodings'])
    rep.add(g, 'stage message groups', res['file_groups'] == {'zel_00': 0, 'zel_04': 4, 'zel_05': 5},
            'D_SB11 %s, R_SP160 GC %s / HD %s' % (res['stage_groups']['D_SB11'], res['stage_groups']['R_SP160 (GC)'],
                                                  res['stage_groups']['R_SP160 (HD)']))
    rep.add(g, 'nothing missing', not res['missing'], ', '.join(res['missing']) or 'all requested text present')
    if res['stamp_csv_ok'] is not None:
        rep.add(g, 'stamp item ids', res['stamp_csv_ok'], 'STAMP_ITEMS matches the stamp table')
    parts = [(lb, ms, out[k]) for lb, ms, k in (('cave', res['cave_msgs'], 'cave_text'),
                                                 ('stamps', res['stamp_msgs'], 'stamps_text')) if k in out]
    for label, msgs, d in parts:
        bad = []
        for m in msgs:
            t = bytes.fromhex(m['text_hex'])
            e = bytes.fromhex(m['entry_hex'])
            if not valid_encoded_text(t):
                bad.append(m['key'] + ' invalid encoding')
                continue
            for tk in tokens(t):
                if tk[0] == 't' and not gc_tag_known(tk[1], tk[2]):
                    bad.append('%s tag {%d,%d}' % (m['key'], tk[1], tk[2]))
                if tk[0] == 'c' and tk[1] >= 0x80 and tk[1] not in res['gc_chars']:
                    bad.append('%s char 0x%02x' % (m['key'], tk[1]))
            if len(e) != 20 or e[:6] != b'\0' * 6 or _u16(e, 6) >= LABELS_GC:
                bad.append(m['key'] + ' entry')
            if m['group'] != (0 if m['id'] <= 5000 else res['file_groups'][m['file']]):
                bad.append(m['key'] + ' group')
            if m['action'] == 'override' and m['gc']['index'] is None:
                bad.append(m['key'] + ' override without GC entry')
            box = e[9]
            if box in WRAP_BOX_KINDS and not any(tk[0] == 't' and tk[1] == 0 and tk[2] in (8, 9) for tk in tokens(t)):
                lim = res['line_limits'].get(box, res['line_limits'].get(0, 450))
                for line in _lines(tokens(t)):
                    if res['_fm'].width(line) > lim and len(_words(line)) > 1:
                        bad.append('%s line %d px > GC window %d px' % (m['key'], res['_fm'].width(line), lim))
        rep.add(g, '%s texts GC-renderable' % label, not bad,
                '%d messages: valid_encoded_text, v2.0.3 tag set, GC charset, entry, group, lines fit GC window%s' % (
                    len(msgs), (': ' + '; '.join(bad[:4])) if bad else ''))
        try:
            ver, recs = read_bin(common.read(os.path.join(d, 'messages.bin')))
            js = json.loads(common.read(os.path.join(d, 'messages.json')).decode('utf-8'))
            same = ver == SCHEMA_VERSION and len(recs) == len(js) == len(msgs) and all(
                r[2] == m['id'] and r[6].hex() == m['text_hex'] and r[5].hex() == m['entry_hex'] for r, m in zip(recs, js))
            rep.add(g, '%s json/bin round trip' % label, same, '%d records' % len(recs))
        except Exception as ex:  # noqa: BLE001
            rep.add(g, '%s json/bin round trip' % label, False, str(ex))
    if 'cave_text' in out:
        _validate_cave_flows(rep, g, res)
    if 'stamps_text' in out:
        _validate_stamp_flows(rep, g, res)
    if 'cave_amiibo' in out:
        _validate_amiibo(rep, g, res, out)


def _validate_cave_flows(rep, g, res):
    """every node target inside the flow, every message node has text, label >= 822 flagged, not in GC,
    native/override message nodes carry a GC entry index."""
    probs = []
    by_key = {m['key']: m for m in res['cave_msgs']}
    for f in res['cave_flows']:
        ids = {n['n'] for n in f['nodes']}
        for n in f['nodes']:
            targets = n.get('results', []) + ([n.get('next')] if 'next' in n else [])
            for t in targets:
                if t is not None and t not in ids:
                    probs.append('flow %d node %d -> %s outside' % (f['flow_id'], n['n'], t))
            if n['type'] == 'message':
                m = by_key.get(n.get('message'))
                if m is None:
                    probs.append('flow %d node %d message %s' % (f['flow_id'], n['n'], n.get('msg_id')))
                elif (m['action'] in ('native', 'override')) != (n.get('gc_index') is not None):
                    probs.append('flow %d node %d gc_index' % (f['flow_id'], n['n']))
            if n['type'] == 'branch' and n['query'] == 1 and n['param'] >= LABELS_GC and not n['hd_only']:
                probs.append('label not flagged')
        if f['gc_has_flow']:
            probs.append('flow %d unexpectedly in GC' % f['flow_id'])
    rep.add(g, 'cave flows complete', not probs and len(res['cave_flows']) == len(CAVE_FLOWS),
            '%d flows, %d nodes; HD-only: %s%s' % (len(res['cave_flows']), sum(len(f['nodes']) for f in res['cave_flows']),
                                                   sum(len(f['hd_only']) for f in res['cave_flows']),
                                                   (': ' + '; '.join(probs[:4])) if probs else ''))


def _validate_stamp_flows(rep, g, res):
    give = {1755: 0xBD, 2686: 0xBE, 2690: 0x98}
    ok = []
    for f in res['stamp_flows']:
        node = f['focus'][0]
        nd = [n for n in f['nodes'] if n['n'] == node]
        if node in give:
            ok.append(bool(nd) and nd[0]['type'] == 'event' and nd[0]['event'] == 8 and nd[0]['p0'] == 1 and
                      nd[0]['p1'] == give[node])
        else:
            ok.append(bool(nd) and nd[0]['type'] == 'event' and nd[0]['event'] == 10 and nd[0]['p0'] == 18)
    rep.add(g, 'stamp NPC give nodes', all(ok) and len(ok) == len(give) + len(STAMP_NPC_SETTERS),
            'zel_04 nodes %s = event008(1, item) as in the stamps report; %s = event010 temp flag 18' % (
                sorted(give), sorted(STAMP_NPC_SETTERS)))


def _validate_amiibo(rep, g, res, out):
    a = res['amiibo']
    pngs_ok = True
    for fn, (w, h) in a['pngs'].items():
        try:
            pw, ph, rgba = common.png_read(common.read(os.path.join(out['cave_amiibo'], fn)))
            pngs_ok &= (pw, ph) == (w, h) and any(rgba[3::4])
        except Exception:  # noqa: BLE001
            pngs_ok = False
    scr = a['screens'].get('zelda_amiibo_progress')
    rep.add(g, 'amiibo PNGs', pngs_ok and len(a['pngs']) >= 16, '%d PNGs decode, non-empty alpha' % len(a['pngs']))
    lay_ok = False
    try:
        lay = json.loads(common.read(os.path.join(out['cave_amiibo'], 'layout.json')).decode('utf-8'))
        lay_ok = all(os.path.exists(os.path.join(out['cave_amiibo'], t['file'])) for t in lay['textures'].values()) and \
            all(t in lay['textures'] for s in lay['screens'].values() for t in s['textures_used'])
    except Exception:  # noqa: BLE001
        pass
    rep.add(g, 'amiibo layout', lay_ok and bool(scr) and scr['width'] == 608 and len(scr['panes']) >= 40 and
            not a['missing'],
            'zelda_amiibo_progress: %d panes, %d textures, every texture exported; missing %s' % (
                len(scr['panes']) if scr else 0, len(scr['textures_used']) if scr else 0, a['missing'] or 'none'))


# ---------------------------------------------------------------------------------------------- CLI
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hd', required=True)
    ap.add_argument('--gc', required=True)
    ap.add_argument('--src', default=os.path.join(common.PROJECT, 'dusklight'))
    ap.add_argument('--out')
    ap.add_argument('cmd', nargs='*')
    a = ap.parse_args(argv)
    gc = a.gc if os.path.isdir(os.path.join(a.gc, 'res')) else os.path.join(a.gc, 'files')
    if a.cmd and a.cmd[0] == 'show':
        f = a.cmd[1]
        hb = load_bmg(common.exists_ci(os.path.join(a.hd, 'res', 'Msgus'), FILES[f][0]), f)
        gb = load_bmg(common.exists_ci(os.path.join(gc, 'res', 'Msgus'), FILES[f][0]), f)
        for r in a.cmd[2:]:
            lo, _, hi = r.partition('-')
            for mid in range(int(lo), int(hi or lo) + 1):
                for tag, b in (('HD', hb), ('GC', gb)):
                    e = b.by_id.get(mid)
                    print('%5d %s %s %s' % (mid, tag, e.entry[4:].hex() if e else '-', show(e.text) if e else '<absent>'))
        return 0
    if not a.out:
        ap.error('--out is required to build')
    import validate as V
    log = common.Log()
    out = {'cave_text': os.path.join(a.out, 'hdfeatures', 'res', 'cave', 'hdtext'),
           'cave_amiibo': os.path.join(a.out, 'hdfeatures', 'res', 'cave', 'amiibo'),
           'stamps_text': os.path.join(a.out, 'hdfeatures', 'res', 'stamps', 'hdtext')}
    res = build(os.path.abspath(a.hd), os.path.abspath(gc), out, log, src=a.src)
    rep = V.Report()
    validate(rep, res, out)
    print(rep.table())
    return 1 if rep.failed() else 0


if __name__ == '__main__':
    sys.exit(main())
