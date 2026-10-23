"""Synthetic test vectors for mods/hdfeatures/convert/tests/io_vectors.inc (module C1 unit tests), made by the
Python spec (tools/formats, tools/pipeline). No game data: every input is made up here (fixed random seed).
usage: python -I -B tools/dev/gen_io_vectors.py <project root> > mods/hdfeatures/convert/tests/io_vectors.inc"""
import gzip
import io
import json
import math
import os
import random
import struct
import sys
import zlib

ROOT = sys.argv[1]
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, 'tools', 'formats'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'pipeline'))
import common  # noqa: E402
import rarc  # noqa: E402
import rarc_write  # noqa: E402
import rarcx  # noqa: E402
import yaz0  # noqa: E402

out = []


def emit(s=''):
    out.append(s)


def cstr(b):
    """bytes -> C++ string literal (octal escapes for everything not plain ASCII)"""
    s = ''
    for c in b:
        if 0x20 <= c < 0x7f and c not in (0x22, 0x5c, 0x3f):
            s += chr(c)
        else:
            s += '\\%03o' % c
    return '"' + s + '"'


def chex(b):
    return '"' + b.hex() + '"'


def bits(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


rng = random.Random(20261022)

# ------------------------------------------------------------------ repr
vals = [0.1, 1 / 3, 2 / 3, 1e16, 1e15, 9999999999999998.0, 1e-4, 1e-5, 1.5e-5, 0.00012345, 123.456, -299.0, 369.5,
        5e-324, 1.7976931348623157e308, 2.2250738585072014e-308, 1e22, 1e21, 0.30000000000000004, 100.0,
        1234567890123456.7, -1e-7, 12345678901234567890.0, 1.0, -1.0, 0.5, 1e100, 1.234e-300, 123456789.0,
        1e-06, 0.001, 2.5, 3.0e15, 4.35, -0.75, 65504.0, 16777217.0, 0.2, 7.0e-3]
for _ in range(40):
    vals.append(struct.unpack('<d', struct.pack('<Q', rng.getrandbits(64)))[0])
for _ in range(30):
    vals.append(round(rng.uniform(-1000, 1000), rng.randint(0, 4)))
vals = [v for v in vals if math.isfinite(v)]
emit('// repr(float): {bits, repr}')
emit('const ReprCase kRepr[] = {')
for v in vals:
    emit('    {0x%016xull, "%s"},' % (bits(v), repr(v)))
emit('};')

# ------------------------------------------------------------------ round
cases = []
for x in [0.125, 0.375, 2.675, 1.005, 2.5, 3.5, -2.5, 0.5, 1.5, -0.125, 1e-10, 123.456789, 0.0, -0.0, -0.001,
          1234.5, 150.0, 250.0, -250.0, 0.045, 1e300, 5e-324, 0.6, 99.995, -99.995, 1e16 + 2, 0.285, 1.115]:
    for n in (0, 1, 2, 9, -1, -2):
        cases.append((x, n))
for _ in range(60):
    a = rng.randint(-32768, 32767) * math.pi / 32768
    cases.append((math.cos(a), 9))
    cases.append((math.sin(a), 9))
for _ in range(60):
    cases.append((rng.uniform(-700, 700), 2))
    cases.append((rng.randint(-70000, 70000) / 200.0, 2))  # exact .xx5 ties appear
emit('// round(x, n): {bits x, n, bits result}')
emit('const RoundCase kRound[] = {')
for x, n in cases:
    emit('    {0x%016xull, %d, 0x%016xull},' % (bits(x), n, bits(round(x, n))))
emit('};')

emit('// int(round(x)): {bits x, result}')
emit('const RoundIntCase kRoundInt[] = {')
for x in [0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 1e15 + 0.5, 3.4999999999999996, 7.0, -7.49, 1023.5, 4503599627370495.5]:
    emit('    {0x%016xull, %d},' % (bits(x), int(round(x))))
emit('};')

emit('// a // b, a % b')
emit('const DivCase kDiv[] = {')
for a, b in [(7, 2), (-7, 2), (7, -2), (-7, -2), (0, 3), (-1, 3), (6, 3), (-6, 3), (-0x8000, 0x4000), (5, 7), (-5, 7)]:
    emit('    {%d, %d, %d, %d},' % (a, b, a // b, a % b))
emit('};')

# ------------------------------------------------------------------ JSON
obj = {
    'version': 3,
    'note': 'quote" back\\slash /solidus',
    'ctrl': '\b\f\n\r\t\x01\x1f\x7f',
    'uni': 'é日本\U0001F600 ',
    'empty_s': '',
    'list': [0, -1, 123456789012, True, False, None, [], {}, [[]], {'a': []}],
    'floats': [0.0, -0.0, 1.0, 1e16, 1e-05, 0.1, -299.0, 369.5, 2.5e-7, 1e22, 123.25, float('nan'),
               float('inf'), float('-inf')],
    'nested': {'k': {'z': 1, 'a': [1.5, 'x', {'deep': None}]}, 'ü': 'key non-ascii'},
}
styles = [
    ('indent1', dict(indent=1, sort_keys=False)),
    ('compact_utf8', dict(ensure_ascii=False, separators=(',', ':'))),
    ('default', dict()),
    ('indent2_utf8', dict(indent=2, ensure_ascii=False)),
]
for name, kw in styles:
    emit('const char kJson_%s[] = %s;' % (name, cstr(json.dumps(obj, **kw).encode('utf-8'))))
items = [{'id': 1, 'text': 'café 日'}, [1, 2.5, None], 'str', {}]
emit('const char kJsonLines[] = %s;' % cstr(
    ('[\n' + ',\n'.join(json.dumps(x, ensure_ascii=False, separators=(',', ':')) for x in items) + '\n]\n')
    .encode('utf-8')))

# ------------------------------------------------------------------ Yaz0 (stream made here, decoded by yaz0.py)


def yaz0_encode(data):
    """naive greedy encoder (both short and long back-references, overlapping copies)"""
    outb = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + b'\0' * 8)
    i = 0
    while i < len(data):
        code_pos = len(outb)
        outb.append(0)
        code = 0
        for bit in range(8):
            if i >= len(data):
                break
            best_len, best_dist = 0, 0
            for dist in range(1, min(i, 0x1000) + 1):
                ln = 0
                while ln < 0x111 and i + ln < len(data) and data[i + ln - dist] == data[i + ln]:
                    ln += 1
                if ln > best_len:
                    best_len, best_dist = ln, dist
            if best_len >= 3:
                d = best_dist - 1
                if best_len >= 0x12:
                    outb += bytes([(d >> 8) & 0x0F, d & 0xFF, best_len - 0x12])
                else:
                    outb += bytes([((best_len - 2) << 4) | ((d >> 8) & 0x0F), d & 0xFF])
                i += best_len
            else:
                code |= 0x80 >> bit
                outb.append(data[i])
                i += 1
        outb[code_pos] = code
    return bytes(outb)


plain = bytearray()
for k in range(300):
    r = rng.random()
    if r < 0.3:
        plain += bytes([rng.randrange(256)])
    elif r < 0.6:
        plain += b'ABCD'[: rng.randint(1, 4)]
    else:
        plain += bytes([plain[-1] if plain else 0x41]) * rng.randint(1, 40)
plain = bytes(plain)
stream = yaz0_encode(plain)
assert yaz0.decompress_yaz0(stream) == plain
emit('const char kYaz0Stream[] = %s;' % chex(stream))
emit('const char kYaz0Plain[] = %s;' % chex(plain))
# declared size shorter than the data the stream holds: the last back-reference is clamped
short = bytearray(stream)
struct.pack_into('>I', short, 4, len(plain) - 5)
emit('const char kYaz0Clamped[] = %s;' % chex(yaz0.decompress_yaz0(bytes(short))))

# ------------------------------------------------------------------ RARC
entries = [('dzb/room.dzb', bytes(range(40))), ('bmdr/model.bmd', bytes(rng.randrange(256) for _ in range(70))),
           ('bmdr/sub/x.bti', b'BTI!!'), ('readme.txt', b'abc'), ('dzb/a.bin', bytes(33)),
           ('Kcl/Kcl.kcl', b'K' * 64)]
built = rarc_write.build('arc', entries)
emit('const char kRarcBuilt[] = %s;' % chex(built))
a = rarc.Rarc(built)
emit('// rarc.Rarc(kRarcBuilt).files(): {path, flags, offset, size, id}')
emit('const MemberCase kRarcMembers[] = {')
for m in a.files():
    emit('    {"%s", 0x%02x, %d, %d, %d},' % (m.path, m.flags, m.offset, m.size, m.eid))
emit('};')
emit('const EntryIndexCase kEntryIndex[] = {')
for key in ['arc/dzb/room.dzb', 'room.dzb', 'BMDR/MODEL.BMD', 'x.bti', 'sub/x.bti', 'kcl.kcl', 'nothing', 'arc']:
    ei = rarcx.entry_index(a, key)
    emit('    {"%s", %d},' % (key, -1 if ei is None else ei))
emit('};')
# HD-style archive: model.bmd stored as Yaz0 with COMPRESSED|YAZ0 flags; rebuilt with the same layout
hd = bytearray(built)
m = a.get('model.bmd')
yz = yaz0_encode(m.data())
# put the Yaz0 blob at the end of the data block and point the entry at it
data_base = a.data_base
new_off = len(hd) - data_base
hd += yz
while len(hd) % 32:
    hd.append(0)
for i, e in enumerate(a.entries):
    if e['name'] == 'model.bmd':
        eo = a.entry_base + i * 0x14
        eid, h, fl_name = struct.unpack_from('>HHI', hd, eo)
        fl = (fl_name >> 24) | 0x84
        struct.pack_into('>HHIIII', hd, eo, eid, h, (fl << 24) | (fl_name & 0xFFFFFF), new_off, len(yz), 0)
struct.pack_into('>I', hd, 4, len(hd))
struct.pack_into('>I', hd, 0x10, len(hd) - data_base)
hd = bytes(hd)
ah = rarc.Rarc(hd)
same = rarcx.rebuild_same_layout(ah, {'arc/readme.txt': b'NEW-README', 'dzb/a.bin': b'Z' * 7})
gcw = rarc_write.build_from_rarc(ah, {'arc/readme.txt': b'NEW-README', 'dzb/a.bin': b'Z' * 7})
emit('const char kRarcHd[] = %s;' % chex(hd))
emit('const char kRarcHdSameLayout[] = %s;' % chex(same))
emit('const char kRarcHdGcLayout[] = %s;' % chex(gcw))
emit('const char kRarcHdPlain[] = %s;' % chex(rarcx.rebuild_same_layout(ah)))
# Yaz0-compressed archive as a whole
emit('const char kRarcYaz0[] = %s;' % chex(yaz0_encode(built)))
emit('const unsigned kNameHash[] = {0x%04x, 0x%04x, 0x%04x, 0x%04x};' % (
    rarc_write.name_hash('.'), rarc_write.name_hash('..'), rarc_write.name_hash('room.dzb'),
    rarc_write.name_hash('a_much_longer_file_name_for_the_hash.bmd')))

# ------------------------------------------------------------------ PNG
rgba = bytes(rng.randrange(256) for _ in range(5 * 3 * 4))
emit('const char kPngRgba[] = %s;' % chex(rgba))
emit('const char kPngBytes[] = %s;' % chex(common.png_bytes(5, 3, rgba)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'dist'))
import hashlib  # noqa: E402
png = common.png_bytes(5, 3, rgba)
o, idat, ihdr = 8, b'', b''
while o + 8 <= len(png):
    n = int.from_bytes(png[o:o + 4], 'big')
    t = png[o + 4:o + 8]
    if t == b'IHDR':
        ihdr = png[o + 8:o + 8 + n]
    elif t == b'IDAT':
        idat += png[o + 8:o + 8 + n]
    elif t == b'IEND':
        break
    o += 12 + n
emit('const char kPngPixelHash[] = "%s";' % hashlib.sha256(ihdr + zlib.decompress(idat)).hexdigest())

# ------------------------------------------------------------------ zlib / gzip
for name, data in [('Empty', b''), ('Hello', b'hello hello hello hello'), ('Bytes', bytes(range(256)) * 3)]:
    emit('const char kDeflate9%s[] = %s;' % (name, chex(zlib.compress(data, 9))))
g1 = gzip.compress(b'first member ' * 10, mtime=0)
g2 = gzip.compress(b'second', mtime=0)
emit('const char kGzipTwo[] = %s;' % chex(g1 + g2 + b'\0\0\0'))
emit('const char kGzipTwoPlain[] = %s;' % chex(gzip.decompress(g1 + g2 + b'\0\0\0')))
emit('const unsigned kCrc123456789 = 0x%08x;' % (zlib.crc32(b'123456789') & 0xFFFFFFFF))

print('// Generated by tools/dev/gen_io_vectors.py from the Python spec (synthetic data only, no game data).')
print('// Remake: python -I -B tools/dev/gen_io_vectors.py <project root> > mods/hdfeatures/convert/tests/io_vectors.inc')
print('\n'.join(out))
