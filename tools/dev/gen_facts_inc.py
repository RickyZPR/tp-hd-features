"""Write mods/hdfeatures/convert/data/facts.inc from tools/pipeline/facts.py (numbers and short own strings only).
usage: python -I -B tools/dev/gen_facts_inc.py tools/pipeline/facts.py mods/hdfeatures/convert/data/facts.inc
The .inc is included by include/hdf/facts.hpp inside namespace hdf::facts, after the struct definitions.
facts.py is executed from its source text (no import, so no .pyc is written next to it)."""
import hashlib
import sys

src, out = sys.argv[1], sys.argv[2]
raw = open(src, 'rb').read()
ns = {'__name__': 'facts'}
exec(compile(raw, src, 'exec'), ns)


class F(object):
    pass


f = F()
for k in ('CAVE_PARTICLE_IDS', 'SAVE_LABELS', 'HD_CHESTS', 'CAVE_TAG_FLOW', 'CAVE_TAG_ROOMS', 'LABEL_COUNT_GC',
          'STAMP_ITEM_IDS'):
    setattr(f, k, ns[k])

BAD = set('"' + chr(92))


def cstr(s):
    assert all(32 <= ord(c) < 127 and c not in BAD for c in s), s
    return '"%s"' % s


ids = sorted(f.CAVE_PARTICLE_IDS)
assert len(ids) == 216 and len(f.STAMP_ITEM_IDS) == 50 and len(f.HD_CHESTS) == 7 and len(f.SAVE_LABELS) == 4
L = []
L.append('// Generated from tools/pipeline/facts.py (sha256 %s)' % hashlib.sha256(raw).hexdigest())
L.append('// by tools/dev/gen_facts_inc.py (python -I -B). Do not edit by hand.')
L.append("// Facts only: numbers, ids and this project's own short descriptions. No game data, no game text.")
L.append('// Included by include/hdf/facts.hpp inside namespace hdf::facts.')
L.append('')
L.append('// CAVE_PARTICLE_IDS: scene-particle resource ids of the cave room actors (Pscene206.jpc), sorted.')
L.append('inline constexpr std::array<u16, %d> kCaveParticleIds = {' % len(ids))
for i in range(0, len(ids), 12):
    L.append('    ' + ', '.join('0x%04X' % v for v in ids[i:i + 12]) + ',')
L.append('};')
L.append('')
L.append('// SAVE_LABELS: GC event-bit labels named in flows.json (index -> name, bit, description).')
L.append('inline constexpr std::array<SaveLabel, %d> kSaveLabels = {{' % len(f.SAVE_LABELS))
for i, (name, bit, desc) in sorted(f.SAVE_LABELS.items()):
    L.append('    {%d, %s, 0x%04X, %s},' % (i, cstr(name), bit, cstr(desc)))
L.append('}};')
L.append('')
L.append('// HD_CHESTS: the 7 HD-only stamp chests (stamp index, stage, room, chest number, item id), facts.py order.')
L.append('inline constexpr std::array<HdChest, %d> kHdChests = {{' % len(f.HD_CHESTS))
for st, stage, room, chest, item in f.HD_CHESTS:
    L.append('    {%d, %s, %d, %d, 0x%02X},' % (st, cstr(stage), room, chest, item))
L.append('}};')
L.append('')
L.append('// STAMP_ITEM_IDS: the HD item id of each stamp, by stamp index 0-49 (HD book order).')
L.append('inline constexpr std::array<u8, %d> kStampItemIds = {' % len(f.STAMP_ITEM_IDS))
for i in range(0, len(f.STAMP_ITEM_IDS), 10):
    L.append('    ' + ', '.join('0x%02X' % v for v in f.STAMP_ITEM_IDS[i:i + 10]) + ',')
L.append('};')
L.append('')
L.append("// CAVE_TAG_FLOW / CAVE_TAG_ROOMS: HD's section-end talk tags (Tag_ms) of the Cave of Shadows.")
L.append('inline constexpr u16 kCaveTagFlow = %d;' % f.CAVE_TAG_FLOW)
L.append('inline constexpr std::array<u8, %d> kCaveTagRooms = {%s};' % (
    len(f.CAVE_TAG_ROOMS), ', '.join(str(r) for r in f.CAVE_TAG_ROOMS)))
L.append('')
L.append('// LABEL_COUNT_GC: saveBitLabels[822], the size of facts.label_table().')
L.append('inline constexpr u32 kLabelCountGc = %d;' % f.LABEL_COUNT_GC)
L.append('')
with open(out, 'w', newline='\n') as fh:
    fh.write('\n'.join(L))
print('wrote', out, len(ids), 'particle ids')
