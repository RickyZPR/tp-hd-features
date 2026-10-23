#!/usr/bin/env python3
"""golden_text.py - the golden list of module TX (text) for `hdf-convert test text`, made by the Python pipeline
(tools/pipeline/hdtext.py, the executable spec) from YOUR OWN game files.

The list holds numbers and hashes only: no game text, no names from inside the files. It is still made from
your games, so it goes to a build-*/ folder of the project (or a scratch folder) and is never committed or shared.

USAGE (always with -I -B: no .pyc is written into tools/)
  python -I -B tools/dev/golden_text.py --hd <TP HD title or content folder> --gc <GC 'files' folder>
                                        --out build-conv/golden/text.tsv
  python -I -B tools/dev/golden_text.py vectors
        prints the unit-test vectors of tests/text_test.cpp (the block between the "vectors" markers): the Python
        spec run on SYNTHETIC inputs only (a made-up font, made-up English texts and message graphs; no game file
        is read).

One line per fact, tab-separated:
  limit    <box kind> <px> <GC samples>                 gc_line_limits (sorted by box kind)
  group    <name> <STAG msg group or -1>                stage_msg_group (D_SB11, R_SP160_GC, R_SP160_HD)
  chars    <count> <sha256 of the sorted code bytes>    _gc_chars
  font     <cell> <cell_w> <name width repr>            FontMetrics
  rewrap   <part> <file> <id> <box> <limit> <text size> <sha256 of the re-wrapped text> <note>
           (part = cave | stamps; note = hdtext's own note, numbers only)
  message  <part> <n> <file> <group> <id> <action> <text size> <sha256 of text> <sha256 of entry>
           <sha256 of the UTF-8 preview>
  file     <rel> <size> <sha256>                         the 3 kept outputs (+ the dropped stamps flows.json, an
                                                         extra check), as hdtext writes them
"""
import hashlib
import json
import os
import struct
import sys

sys.dont_write_bytecode = True
DEV = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(DEV)
sys.path.insert(0, DEV)
sys.path.insert(0, os.path.join(TOOLS, 'pipeline'))
import golden  # noqa: E402  (check_out)
import hdtext  # noqa: E402


def sha(b):
    return hashlib.sha256(b).hexdigest()


def bin_bytes(msgs):
    """hdtext._write_bin into memory"""
    b = bytearray(b'HDTX' + struct.pack('<II', hdtext.SCHEMA_VERSION, len(msgs)))
    for m in msgs:
        t = bytes.fromhex(m['text_hex'])
        b += struct.pack('<BBHBBH', hdtext.FILE_NO[m['file']], m['group'], m['id'], hdtext.ACTION_NO[m['action']], 0,
                         len(t))
        b += bytes.fromhex(m['entry_hex']) + t
        while len(b) % 4:
            b.append(0)
    return bytes(b)


def json_lines(items):
    """hdtext._write_json_lines into memory"""
    body = '[\n' + ',\n'.join(json.dumps(x, ensure_ascii=False, separators=(',', ':')) for x in items) + '\n]\n'
    return body.encode('utf-8')


# ---------------------------------------------------------------------------------------------- unit-test vectors
MASK = (1 << 64) - 1


class Lcg(object):
    """the generator tests/text_test.cpp repeats (PCG multiplier/increment, top bits)"""

    def __init__(self, seed):
        self.s = seed & MASK

    def __call__(self, n):
        self.s = (self.s * 6364136223846793005 + 1442695040888963407) & MASK
        return (self.s >> 33) % n


WORDS = [b'the', b'a', b'Link', b'Rupee', b'Rupees', b'50', b'1,000', b'sword', b'and', b'shield', b'cave', b'of',
         b'shadows', b'go', b'now', b'fairy', b'heart', b'piece', b'you', b'got', b'it', b'wolf', b'Midna', b'is',
         b'very', b'longerwordhere', b'x', b'\xe9t\xe9']


def rand_text(r):
    out = bytearray()
    for _ in range(3 + r(70)):
        k = r(100)
        if k < 55:
            out += WORDS[r(len(WORDS))] + b' '
        elif k < 63:
            out += (b'.', b'!', b'?', b',')[r(4)] + b' '
        elif k < 71:
            out += b'\n'
        elif k < 74:
            out += b'\x1a\x05\x00\x00\x00'
        elif k < 76:
            out += b'\x1a\x05\x00\x00\x01'
        elif k < 78:
            out += b'\x1a\x05\x00\x00\x02'
        elif k < 80:
            out += b'\x1a\x06\xff\x00\x00\x06'
        elif k < 82:
            out += b'\x1a\x05\x00\x00\x0a'
        elif k < 84:
            out += b'\x1a\x06\xff\x00\x00\x01'
        elif k < 85:
            out += b'\x1a\x05\x00\x00\x08'
        elif k < 90:
            out += b'  '
        else:
            out += b'\n\n'
    return bytes(out) + b'\0'


def synth_bfn():
    """a made-up BFN: INF1 (cell 22), GLY1 (cell width 24), WID1 for glyphs 0-223, MAP1 type 1 (A-Z, ignored),
    MAP1 type 2 (digits -> glyphs 200-209), MAP1 type 0 (0x20-0xFF)."""
    def blk(tag, body):
        b = tag + struct.pack('>I', 0) + body
        b += b'\0' * (-len(b) % 4)
        return b[:4] + struct.pack('>I', len(b)) + b[8:]
    inf = blk(b'INF1', struct.pack('>HHHHHH', 0, 20, 4, 22, 26, 0x20) + b'\0' * 12)
    gly = blk(b'GLY1', struct.pack('>HHHH', 0x20, 0xFF, 24, 24) + b'\0' * 16)
    wid = blk(b'WID1', struct.pack('>HH', 0, 223) + b''.join(bytes((0, 6 + (g * 7) % 17)) for g in range(224)))
    m1 = blk(b'MAP1', struct.pack('>HHHH', 1, 0x41, 0x5A, 0))
    m2 = blk(b'MAP1', struct.pack('>HHHH', 2, 0x30, 0x39, 10) + b''.join(struct.pack('>H', 200 + i) for i in range(10)))
    m3 = blk(b'MAP1', struct.pack('>HHHH', 0, 0x20, 0xFF, 0))
    body = inf + gly + wid + m1 + m2 + m3
    head = b'FONTbfn1' + struct.pack('>II', 0x20 + len(body), 6) + b'\0' * 16
    return head + body


def synth_fm(bfn):
    """hdtext.FontMetrics on the made-up BFN (its real __init__, the archive lookup stubbed)"""
    class M(object):
        def data(self):
            return bfn

    class A(object):
        def get(self, name):
            return M()
    old_from, old_ci = hdtext.rarc.Rarc.from_file, hdtext.common.exists_ci
    hdtext.rarc.Rarc.from_file = staticmethod(lambda p: A())
    hdtext.common.exists_ci = lambda d, n: 'x'
    try:
        return hdtext.FontMetrics('gc')
    finally:
        hdtext.rarc.Rarc.from_file, hdtext.common.exists_ci = old_from, old_ci


def synth_bmg(entries, nodes=None, edges=None, flows=None):
    """MESGbmg1 with INF1 (entries: (id, attrs[14], text incl. NUL)), DAT1, optional FLW1 / FLI1"""
    def sec(tag, body):
        b = tag + struct.pack('>I', 0) + body
        b += b'\0' * (-len(b) % 32)
        return b[:4] + struct.pack('>I', len(b)) + b[8:]
    dat = bytearray(b'\0')
    inf = bytearray()
    for mid, at, text in entries:
        inf += struct.pack('>IH', len(dat), mid) + at
        dat += text
    secs = [sec(b'INF1', struct.pack('>HHI', len(entries), 20, 0) + bytes(inf)), sec(b'DAT1', bytes(dat))]
    if nodes is not None:
        secs.append(sec(b'FLW1', struct.pack('>HHI', len(nodes), len(edges), 0) + b''.join(nodes) +
                        b''.join(struct.pack('>H', e) for e in edges)))
    if flows is not None:
        secs.append(sec(b'FLI1', struct.pack('>HHI', len(flows), 8, 0) +
                        b''.join(struct.pack('>IHH', v, i, 0) for v, i in flows)))
    body = b''.join(secs)
    return b'MESGbmg1' + struct.pack('>II', 0x20 + len(body), len(secs)) + b'\x01' + b'\0' * 15 + body


def attrs(box=0, speaker=0, label=0, item_no=0):
    a = bytearray(14)
    a[0:2] = struct.pack('>H', label)
    a[2] = speaker
    a[3] = box
    a[6] = item_no
    return bytes(a)


def rand_bmg(seed, count, kinds):
    r = Lcg(seed)
    entries = []
    for i in range(count):
        box = kinds[r(len(kinds))]
        entries.append((i + 1, attrs(box=box), rand_text(r)))
    entries.append((count + 1, attrs(box=kinds[0]), b'abc\x1a\x09\x00'))   # truncated tag: skipped by the limits
    entries.append((count + 2, attrs(box=kinds[0]), b'\0'))
    return synth_bmg(entries)


def node(*b):
    return bytes(b)


def u16b(v):
    return (v >> 8, v & 0xFF)


FLOW_NODES = [
    node(1, 0, *u16b(0), *u16b(1), *u16b(0)),                   # 0 message entry 0 -> 1
    node(2, 2, *u16b(1), *u16b(110), *u16b(0)),                 # 1 branch query 1 (label 110), edges 0-1
    node(3, 0, *u16b(2), *u16b(829), *u16b(110)),               # 2 event 0 (set flag 829, 110), edge 2
    node(3, 9, 0, 0, 0, 1, 0, 2),                               # 3 event 9 jump flow 0x00010002
    node(2, 3, *u16b(54), *u16b(78), *u16b(3)),                 # 4 branch query 54 (HD), edges 3-5
    node(1, 0, *u16b(5), *u16b(0xFFFF), *u16b(0xFF00)),         # 5 message entry 5 (none) -> END
    node(3, 44, *u16b(6), 0, 0, 0, 0),                          # 6 event 44 (HD), edge 6
    node(3, 1, *u16b(0xFFFF), 0, 0, *u16b(822)),                # 7 event 1 (clear 822), no next
    node(7, 1, 2, 3, 4, 5, 6, 7),                               # 8 type 7
    node(2, 2, *u16b(60), *u16b(1), *u16b(7)),                  # 9 branch query 60, edges 7-8
    node(1, 1, *u16b(1), *u16b(9), *u16b(0)),                   # 10 message entry 1 -> 9
    node(2, 1, *u16b(1), *u16b(822), *u16b(9)),                 # 11 branch query 1 (label 822), edge 9
    node(3, 45, *u16b(10), *u16b(1), *u16b(2)),                 # 12 event 45 (HD), edge 10
    node(3, 5, *u16b(99), 0, 0, 0, 0),                          # 13 event 5, edge out of range
    node(2, 2, *u16b(5), 0, 0, *u16b(11)),                      # 14 branch query 5, edges 11-12 (12 missing)
    node(3, 43, *u16b(0xFFFF), 0, 0, 0, 0),                     # 15 event 43 (HD), END
    node(3, 0, *u16b(0xFFFF), 0, 78, 0, 0),                     # 16 event 0 (set 78)
]
FLOW_EDGES = [2, 3, 4, 5, 0xFFFF, 6, 7, 9, 1, 10, 11, 13]
FLOW_FLI = [(100 << 16 | 1, 0), (200 << 16, 12), (300 << 16 | 7, 14), (100 << 16, 5), (50 << 16, 8), (60 << 16, 16)]


def cstr(s):
    if isinstance(s, str):
        s = s.encode('utf-8')
    out = '"'
    for ch in s:
        c = chr(ch)
        if c in '"\\':
            out += '\\' + c
        elif 32 <= ch < 127:
            out += c
        else:
            out += '\\x%02x""' % ch
    return out + '"'


def hexs(b):
    return cstr(bytes(b).hex())


def cmd_vectors():
    o = []
    w = o.append
    w('// ---- vectors: made by `python -I -B tools/dev/golden_text.py vectors` (Python %s; synthetic data only)'
      % sys.version.split()[0])
    bfn = synth_bfn()
    fm = synth_fm(bfn)
    w('constexpr const char* kBfnHex =')
    h = bfn.hex()
    for i in range(0, len(h), 100):
        w('    ' + cstr(h[i:i + 100]))
    w('    ;')
    w('constexpr int kFontCell = %d, kFontCellW = %d;' % (fm.cell, fm.cell_w))
    w('constexpr double kFontName = %s;' % fm.name.hex())
    w('constexpr double kFontChars[256] = {')
    for i in range(0, 256, 4):
        w('    ' + ' '.join('%s,' % fm.char(c).hex() for c in range(i, i + 4)))
    w('};')
    # width: a plain sequential sum and a compensated one (math.fsum) round differently here
    import math
    r = Lcg(4242)
    found = []
    tries = 0
    while len(found) < 4 and tries < 2000000:
        tries += 1
        t = bytes(0x20 + r(0x5F) for _ in range(1 + r(40)))
        toks = [('c', c) for c in t]
        seq = fm.width(toks)
        comp = int(round(math.fsum(fm.tok(x) for x in toks)))
        if seq != comp:
            found.append((t, seq, comp))
    w('struct WidthCase { const char* hex; int sequential; int compensated; };')
    w('const WidthCase kWidthCases[] = {')
    for t, seq, comp in found:
        w('    {%s, %d, %d},' % (hexs(t), seq, comp))
    w('};')
    # rewrap: handcrafted
    def T(*parts):
        return b''.join(p if isinstance(p, bytes) else p.encode('latin-1') for p in parts) + b'\0'
    NAME, TITLE0, TITLE1, PRICE, SEL = (b'\x1a\x05\x00\x00\x00', b'\x1a\x05\x00\x00\x01', b'\x1a\x05\x00\x00\x02',
                                        b'\x1a\x06\xff\x00\x00\x06', b'\x1a\x05\x00\x00\x08')
    hand = [
        (T('Short line.'), 300),
        (T('This is one very long line that will certainly not fit into the narrow window at all.'), 200),
        (T('First sentence here.\nSecond sentence is a bit longer than the window allows.\nThird.'), 220),
        (T('One two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen '
           'seventeen eighteen nineteen twenty twentyone twentytwo twentythree.'), 150),
        (T(TITLE0, 'Big Shiny Title', TITLE1, '\nYou got the very big shiny thing! It is shiny and big and it '
           'goes on.'), 180),
        (T(PRICE, '50 Rupees ', NAME, ' wants it\nThe shop has a long description of the item for sale.'), 160),
        (T('It costs 1,000 Rupees and more and more 1,000 Rupees again.'), 150),
        (T('A selection that is far too wide for the window ', SEL, ' yes'), 120),
        (T('Top line that is long enough to wrap around.\n\nAfter a blank line, more long text follows here.'), 170),
        (T('Spaces only next:\n                                                  \nEnd of the text, which is long.'), 140),
        (T('Sentence one is long. Sentence two is long too. Sentence three is long. Sentence four. Five. Six '
           'goes on and on. Seven is here. Eight.'), 120),
        (T('Line one.\nLine two.\nLine three.\nLine four.\nLine five is the start of page two and is long.'), 160),
        (T(NAME, ' and ', NAME, ' went to the cave of shadows and found a heart piece there.'), 150),
        (T('Trailing newlines that are long enough to need a wrap.\n\n\n'), 150),
    ]
    w('struct RewrapCase { const char* in; int limit; const char* out; const char* note; };')
    w('const RewrapCase kRewrapHand[] = {')
    for text, limit in hand:
        new, note = hdtext.rewrap(text, limit, fm)
        w('    {%s, %d,\n     %s,\n     %s},' % (hexs(text), limit, hexs(new), cstr(note) if note else 'nullptr'))
    w('};')
    # rewrap: random (the generator is repeated in C++; input and output as sha256 prefixes)
    w('struct RewrapRandom { u32 seed; const char* in16; int limit; const char* out16; const char* note; };')
    w('const RewrapRandom kRewrapRandom[] = {')
    changed = 0
    for seed in range(1, 201):
        r = Lcg(seed)
        text = rand_text(r)
        limit = 60 + r(300)
        try:
            new, note = hdtext.rewrap(text, limit, fm)
            changed += new != text
            w('    {%d, "%s", %d, "%s", %s},' % (seed, sha(text)[:16], limit, sha(new)[:16],
                                                cstr(note) if note else 'nullptr'))
        except ValueError as ex:
            w('    {%d, "%s", %d, "error", %s},' % (seed, sha(text)[:16], limit, cstr(str(ex))))
    w('};')
    w('constexpr int kRewrapRandomChanged = %d;' % changed)
    # gc_line_limits over two made-up GC files
    b1 = rand_bmg(7, 140, [0, 0, 0, 0, 9, 9, 13, 1, 2, 7, 5])
    b2 = rand_bmg(8, 40, [0, 14, 15])
    bm = [hdtext.Bmg(b1, 'b1'), hdtext.Bmg(b2, 'b2')]
    lim = hdtext.gc_line_limits(bm, fm)
    w('constexpr const char* kLimitBmgSha[2] = {"%s", "%s"};' % (sha(b1), sha(b2)))
    w('const std::pair<int, int> kLimits[] = {%s};' % ', '.join('{%d, %d}' % kv for kv in sorted(lim.items())))
    w('constexpr std::size_t kGcCharCount = %d;' % len(hdtext._gc_chars(bm)))
    # flows
    fb = synth_bmg([(7001, attrs(), b'One\0'), (7002, attrs(), b'Two\0')], FLOW_NODES, FLOW_EDGES, FLOW_FLI)
    b = hdtext.Bmg(fb, 'flows')
    w('constexpr const char* kFlowBmgSha = "%s";' % sha(fb))
    w('const std::pair<u32, u16> kFlows[] = {%s};' % ', '.join('{%d, %d}' % kv for kv in b.flows.items()))
    w('struct Reach { u32 start; std::vector<u16> order; };')
    w('const Reach kReach[] = {')
    for s in (0, 1, 4, 9, 12, 14, 8, 16, 99, 0xFFFF):
        w('    {%d, {%s}},' % (s, ', '.join(map(str, hdtext.reachable(b, s)))))
    w('};')
    pred = hdtext.predecessors(b)
    w('const std::pair<u32, std::vector<u32>> kPred[] = {')
    for k in sorted(pred):
        w('    {%d, {%s}},' % (k, ', '.join(map(str, pred[k]))))
    w('};')
    w('struct Hood { u32 node; int back; std::vector<u32> keep; std::vector<u32> flows; };')
    w('const Hood kHood[] = {')
    for nd, back in ((4, 6), (10, 6), (13, 6), (7, 1), (11, 0), (16, 6)):
        w('    {%d, %d, {%s}, {%s}},' % (nd, back, ', '.join(map(str, hdtext.neighbourhood(b, nd, back))),
                                       ', '.join(map(str, hdtext.flows_reaching(b, nd)))))
    w('};')
    import facts
    names = facts.label_table()

    def ref(mid):
        if mid is None:
            return {'message': None, 'gc_index': None}
        return {'message': 'k:%d' % mid, 'action': 'native', 'gc_index': 3}
    w('const char* const kNodeJson[] = {')
    for i in range(len(b.nodes)):
        d = hdtext.decode_node(b, i, names, ref)
        w('    %s,' % cstr(json.dumps(d, ensure_ascii=False, separators=(',', ':'))))
    w('};')
    nodes = [hdtext.decode_node(b, i, names, ref) for i in range(len(b.nodes))]
    hd_only = ['%s %s @%d' % (n['type'], n.get('query_name') or n.get('event_name'), n['n'])
               for n in nodes if n.get('hd_only')]
    w('constexpr const char* kHdOnlyJson = %s;' % cstr(json.dumps(hd_only, ensure_ascii=False, separators=(',', ':'))))
    # build_message
    long_txt = 'and this line is long enough to be re-wrapped by the window width rule'
    hd_e = [
        (10, attrs(box=0), b'Hello there.\0'),
        (11, attrs(box=0, item_no=3), b'Changed text.\0'),
        (12, attrs(box=9, speaker=4), b'Who are you?\0'),
        (13, attrs(box=0, speaker=2), b'You got it!\0'),
        (14, attrs(box=0, label=830), b'Label too high.\0'),
        (15, attrs(box=0), b'\x1a\x09\x03\x00\x00\x00\x00\x12\x34Press \x1a\x05\x07\x00\x06 caf\xe9 \xe8 '
         b'\x1a\x05\x07\x00\x63and \x1a\x05\x00\x00\x10, \x1a\x05\x00\x00\x0f, or \x1a\x05\x00\x00\x0e '
         b'\x1a\x05\x09\x00\x01\x1a\x07\xff\x00\x00\x01\x02 \x1a\x05\x07\x00\x09 \x93q\x94 \xa9 \x85 ' +
         long_txt.encode() + b'\0'),
        (16, attrs(box=0), b'\0'),
        (18, attrs(box=0), (long_txt + ' ' + long_txt).encode() + b'\0'),
        (19, attrs(box=7), (long_txt + ' ' + long_txt).encode() + b'\0'),
        (20, attrs(box=9, label=700), b'\x1a\x09\x03\x00\x00\x00\x00\x00\x01Revo tag only differs.\0'),
        (21, attrs(box=0), b'Icons \x1a\x05\x00\x00\x0a \x1a\x05\x03\x00\x06 \x1a\x05\x06\x00\x0a\x1a\x05\x00\x00\x22'
         b' \x81\x8d\0'),
    ]
    gc_e = [
        (10, attrs(box=0), b'Hello there.\0'),
        (11, attrs(box=0), b'Original text.\0'),
        (12, attrs(box=9, speaker=5), b'Who?\0'),
        (13, attrs(box=0, speaker=1), b'You got that!\0'),
        (18, attrs(box=0), b'\x1a\x09\x03\x00\x00\x00\x00\x00\x07' + (long_txt + ' ' + long_txt).encode() + b'\0'),
        (20, attrs(box=9, label=700), b'Revo tag only differs.\0'),
    ]
    hb, gb = synth_bmg(hd_e), synth_bmg(gc_e)
    H, G = hdtext.Bmg(hb, 'hd'), hdtext.Bmg(gb, 'gc')
    ctx = hdtext.Ctx({0xE8}, fm, {0: 160, 9: 140})
    w('constexpr const char* kMsgHdHex =')
    h = hb.hex()
    for i in range(0, len(h), 100):
        w('    ' + cstr(h[i:i + 100]))
    w('    ;')
    w('constexpr const char* kMsgGcHex =')
    h = gb.hex()
    for i in range(0, len(h), 100):
        w('    ' + cstr(h[i:i + 100]))
    w('    ;')
    w('struct MsgCase { u16 id; const char* use; bool none; const char* action; const char* entry; const char* text;')
    w('                 int gc_index; const char* preview; std::vector<const char*> notes; };')
    w('const MsgCase kMsgCases[] = {')
    msgs = []
    for mid, use in ((10, 'ui'), (11, 'ui'), (12, 'flow'), (13, 'item-get'), (14, 'ui'), (15, 'ui'), (16, 'ui'),
                     (17, 'ui'), (18, 'ui'), (19, 'ui'), (20, 'shop'), (21, 'ui')):
        m = hdtext.build_message(H, G, 'zel_00', mid, 'cave', use, 0, ctx)
        if m is None:
            w('    {%d, "%s", true, "", "", "", -1, "", {}},' % (mid, use))
            continue
        msgs.append(m)
        w('    {%d, "%s", false, "%s", %s,\n     %s,\n     %d, %s,\n     {%s}},' % (
            mid, use, m['action'], cstr(m['entry_hex']), cstr(m['text_hex']),
            -1 if m['gc']['index'] is None else m['gc']['index'], cstr(m['preview']),
            ', '.join(cstr(n) for n in m['conversions'])))
    w('};')
    w('constexpr const char* kBinHex =')
    h = bin_bytes(msgs).hex()
    for i in range(0, len(h), 100):
        w('    ' + cstr(h[i:i + 100]))
    w('    ;')
    # gc_tag_known, valid_encoded_text
    groups = (0, 1, 2, 3, 4, 5, 6, 7, 255)
    w('constexpr const char* kTagKnown =  // groups %s x ids 0-69' % (groups,))
    for g in groups:
        w('    "%s"' % ''.join('1' if hdtext.gc_tag_known(g, i) else '0' for i in range(70)))
    w('    ;')
    vt = [b'', b'\0', b'a\0', b'a', b'a\0b\0', b'\x1a\x05\x00\x00\x00\0', b'\x1a\x04\x00\x00\x00\0',
          b'\x1a\x06\xff\x00\x00\x01\0', b'\x1a\x07\xff\x00\x00\x01\x02\0', b'\x1a\x09\xff\x00\x00\x01\x02\x03\x04\0',
          b'\x1a\x05\xff\x00\x00\0', b'\x1a\x07\xff\x00\x01\x01\x02\0', b'\x1a\x20\x00\x00\x00\0', b'\x1a']
    w('const std::pair<const char*, bool> kValid[] = {%s};' % ', '.join(
        '{%s, %s}' % (hexs(t), 'true' if hdtext.valid_encoded_text(t) else 'false') for t in vt))
    w('// ---- end of vectors')
    sys.stdout.write('\n'.join(o) + '\n')
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == 'vectors':
        return cmd_vectors()
    args = golden.parse(['golden_text.py', 'text'] + argv[1:])
    for k in ('hd', 'gc', 'out'):
        if k not in args:
            raise SystemExit('golden_text.py: --%s is required' % k)
    hd, gc, out = args['hd'], args['gc'], args['out']
    if os.path.isdir(os.path.join(hd, 'content')):
        hd = os.path.join(hd, 'content')
    golden.check_out(out, [args['hd'], gc])
    res = hdtext.build(os.path.abspath(hd), os.path.abspath(gc), {}, lambda *a: None, src=None, project=None)
    fm = res['_fm']
    rows = []
    gc_all = []
    msg_gc = os.path.join(gc, 'res', 'Msgus')
    import rarc
    for n in sorted(os.listdir(msg_gc)):
        if n.lower().startswith('bmgres') and n.lower().endswith('.arc'):
            for m in rarc.Rarc.from_file(os.path.join(msg_gc, n)).files():
                if m.name.lower().startswith('zel_') and m.name.lower().endswith('.bmg') and 'unit' not in m.name:
                    gc_all.append(hdtext.Bmg(m.data(), n + ':' + m.name))
    samples = {}
    for b in gc_all:
        for e in b.entries:
            if len(e.text) <= 1:
                continue
            try:
                max(fm.width(l) for l in hdtext._lines(hdtext.tokens(e.text)))
            except ValueError:
                continue
            samples[e.entry[9]] = samples.get(e.entry[9], 0) + 1
    for k in sorted(res['line_limits']):
        rows.append(('limit', k, res['line_limits'][k], samples.get(k, 0)))
    g = res['stage_groups']
    for name, key in (('D_SB11', 'D_SB11'), ('R_SP160_GC', 'R_SP160 (GC)'), ('R_SP160_HD', 'R_SP160 (HD)')):
        rows.append(('group', name, -1 if g[key] is None else g[key]))
    rows.append(('chars', len(res['gc_chars']), sha(bytes(res['gc_chars']))))
    rows.append(('font', fm.cell, fm.cell_w, repr(fm.name)))
    nrew = 0
    for part, msgs in (('cave', res['cave_msgs']), ('stamps', res['stamp_msgs'])):
        for m in msgs:
            for note in m['conversions']:
                if note.startswith('lines re-wrapped') or note.startswith('selection message'):
                    e = bytes.fromhex(m['entry_hex'])
                    lim = res['line_limits'].get(e[9], res['line_limits'].get(0, 450))
                    t = bytes.fromhex(m['text_hex'])
                    rows.append(('rewrap', part, m['file'], m['id'], e[9], lim, len(t), sha(t), note))
                    nrew += 1
    for part, msgs in (('cave', res['cave_msgs']), ('stamps', res['stamp_msgs'])):
        for i, m in enumerate(msgs):
            t = bytes.fromhex(m['text_hex'])
            rows.append(('message', part, i, m['file'], m['group'], m['id'], m['action'], len(t), sha(t),
                         sha(bytes.fromhex(m['entry_hex'])), sha(m['preview'].encode('utf-8'))))
    files = [('res/cave/hdtext/messages.bin', bin_bytes(res['cave_msgs'])),
             ('res/cave/hdtext/flows.json', json_lines(res['cave_flows'])),
             ('res/stamps/hdtext/messages.bin', bin_bytes(res['stamp_msgs'])),
             ('res/stamps/hdtext/flows.json', json_lines(res['stamp_flows']))]
    for rel, data in files:
        rows.append(('file', rel, len(data), sha(data)))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# hdf golden text 1: made by tools/dev/golden_text.py from your own game files (hashes and numbers '
                'only; never commit or share)\n')
        f.write('# counts limits=%d rewraps=%d cave=%d stamps=%d flows=%d missing=%d\n' % (
            len(res['line_limits']), nrew, len(res['cave_msgs']), len(res['stamp_msgs']), len(res['cave_flows']),
            len(res['missing'])))
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print('golden text: %d lines -> %s (%d limits, %d re-wraps, %d + %d messages, missing %s)' % (
        len(rows), out, len(res['line_limits']), nrew, len(res['cave_msgs']), len(res['stamp_msgs']),
        res['missing'] or 'none'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
