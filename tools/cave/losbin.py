"""losbin.py - dump TP HD content/los.bin (Cave of Shadows per-room table).
usage: python -I losbin.py <los.bin> [--json]
layout (from tphd LosTable.cpp + verified by size): hdr {u32 count,u32,u32}; count x 0x20
 {u32 room, u32 next, u32 prev, u32 floorIdx, f32 x,y,z, s16 unk1C, s16 angleY}"""
import struct, sys, json
def load(p):
    d = open(p, 'rb').read()
    cnt, u4, u8 = struct.unpack_from('>III', d, 0)
    es = []
    for i in range(cnt):
        r, n, pv, fi, x, y, z, u1c, ay = struct.unpack_from('>IIIIfffhh', d, 12 + i * 32)
        es.append(dict(i=i, room=r, next=n if n < 0x80000000 else n - (1 << 32),
                       prev=pv if pv < 0x80000000 else pv - (1 << 32), floor=fi if fi < 0x80000000 else fi - (1 << 32),
                       x=x, y=y, z=z, unk1c=u1c, angleY=ay))
    return dict(size=len(d), count=cnt, unk4=u4, unk8=u8, entries=es, tail=d[12 + cnt * 32:].hex())
if __name__ == '__main__':
    L = load(sys.argv[1])
    if '--json' in sys.argv:
        print(json.dumps(L, indent=1)); sys.exit()
    print('size=%d count=%d unk4=0x%x unk8=0x%x tail=%s' % (L['size'], L['count'], L['unk4'], L['unk8'], L['tail']))
    for e in L['entries']:
        print('[%2d] room=%2d next=%3d prev=%3d floor=%3d pos=(%9.1f,%9.1f,%9.1f) unk1c=%6d angY=%6d' % (
            e['i'], e['room'], e['next'], e['prev'], e['floor'], e['x'], e['y'], e['z'], e['unk1c'], e['angleY']))
