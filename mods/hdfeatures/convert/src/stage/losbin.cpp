// Module ST: TP HD content/los.bin (the Cave of Shadows per-room table).
//
// Ports tools/cave/losbin.py (load) and tools/pipeline/cavebuild.py (floor_chain).
// Layout (losbin.py, from tphd LosTable.cpp): u32 count, u32, u32; count x 0x20
// {u32 room, u32 next, u32 prev, u32 floorIdx, f32 x, y, z, s16 unk1C, s16 angleY}, big-endian.
#include "stage_internal.hpp"

#include <set>

namespace hdf::stage {

using namespace detail;

const LosEntry* LosTable::by_room(u32 room) const {
    const LosEntry* hit = nullptr;  // {e['room']: e for e in L}: the last one wins
    for (const LosEntry& e : entries)
        if (e.room == room) hit = &e;
    return hit;
}

// losbin.load(p)
Result<LosTable> read_los(View d) {
    if (!has(d, 0, 12)) return std::unexpected(bad("los.bin: no header"));
    LosTable t;
    t.size = d.size();
    t.count = be32_at(d, 0);  // cnt, u4, u8 = struct.unpack_from('>III', d, 0)
    t.unk4 = be32_at(d, 4);
    t.unk8 = be32_at(d, 8);
    for (u32 i = 0; i < t.count; ++i) {
        const u64 o = 12 + u64(i) * 32;  // '>IIIIfffhh'
        if (!has(d, o, 32)) return std::unexpected(bad(fmt("los.bin: entry %u is outside the data", i)));
        LosEntry e;
        e.index = i;
        e.room = be32_at(d, o);
        // n if n < 0x80000000 else n - (1 << 32)
        e.next = static_cast<i32>(be32_at(d, o + 4));
        e.prev = static_cast<i32>(be32_at(d, o + 8));
        e.floor = static_cast<i32>(be32_at(d, o + 12));
        e.x = bef32_at(d, o + 16);
        e.y = bef32_at(d, o + 20);
        e.z = bef32_at(d, o + 24);
        e.unk1c = bes16_at(d, o + 28);
        e.angle_y = bes16_at(d, o + 30);
        t.entries.push_back(e);
    }
    const View tail = slice(d, 12 + u64(t.count) * 32, d.size());  // d[12 + cnt * 32:]
    t.tail.assign(tail.begin(), tail.end());
    return t;
}

// cavebuild.floor_chain(L)
Result<std::vector<LosEntry>> floor_chain(const LosTable& los) {
    std::vector<const LosEntry*> start;
    for (const LosEntry& e : los.entries)
        if (e.floor == 1) start.push_back(&e);
    if (start.size() != 1) return std::unexpected(bad(fmt("los.bin: %zu rooms with floor 1", start.size())));
    std::vector<LosEntry> chain;
    std::set<u32> seen;
    const LosEntry* e = start[0];
    while (e != nullptr && !seen.contains(e->room)) {
        seen.insert(e->room);
        chain.push_back(*e);
        e = e->next >= 0 ? los.by_room(static_cast<u32>(e->next)) : nullptr;  // by_room.get(e['next'])
    }
    return chain;
}

}  // namespace hdf::stage
