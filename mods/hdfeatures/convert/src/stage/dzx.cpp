// Module ST: stage.dzs / room.dzr chunk readers.
//
// Ports tools/cave/cave_patch.py (chunks, is_actor_tag, is_scob_tag), tools/pipeline/stagedata.py (chunks,
// chunk_map, layer_of, records, stag) and tools/formats/dzx.py (dec_stag). Layouts as dzx.py's docstring
// (checked there against Dusklight include/d/d_stage.h).
#include "stage_internal.hpp"

namespace hdf::stage {

using namespace detail;

// cave_patch.chunks(b)
Result<std::vector<Chunk>> chunks(View b) {
    if (!has(b, 0, 4)) return std::unexpected(bad("dzx: no chunk count"));
    const i32 n = static_cast<i32>(be32_at(b, 0));  // '>i'
    std::vector<Chunk> out;
    if (n <= 0) return out;                           // range(n) is empty
    if (!has(b, 4, u64(n) * 12)) return std::unexpected(bad(fmt("dzx: %d chunks do not fit in %zu bytes", n, b.size())));
    out.reserve(static_cast<std::size_t>(n));
    for (i32 i = 0; i < n; ++i) {
        const u64 o = 4 + u64(i) * 12;  // tag, cnt, off = struct.unpack_from('>4siI', b, 4 + i * 12)
        Chunk c;
        c.tag.assign(reinterpret_cast<const char*>(b.data() + o), 4);  // latin-1: the raw bytes
        c.count = be32_at(b, o + 4);
        c.offset = be32_at(b, o + 8);
        c.header_offset = static_cast<u32>(o);
        out.push_back(std::move(c));
    }
    return out;
}

// {c[0]: c for c in chunks(b)}.get(tag)
const Chunk* find_chunk(std::span<const Chunk> cs, std::string_view tag) {
    const Chunk* hit = nullptr;
    for (const Chunk& c : cs)
        if (c.tag == tag) hit = &c;
    return hit;
}

// stagedata.records(b)
Result<std::vector<Record>> records(View b) {
    auto cs = chunks(b);
    if (!cs) return std::unexpected(cs.error());
    std::vector<Record> out;
    for (const Chunk& c : *cs) {
        u64 stride;
        if (is_actor_tag(c.tag)) {
            stride = 0x20;
        } else if (is_scob_tag(c.tag)) {
            stride = 0x24;
        } else {
            continue;
        }
        const int layer = layer_of(c.tag);
        for (i64 i = 0; i < count_of(c); ++i) {
            const u64 o = u64(c.offset) + u64(i) * stride;
            // prm, x, y, z, ax, ay, az, sid = struct.unpack_from('>I3f3hH', b, o + 8)
            if (!has(b, o + 8, 24))
                return std::unexpected(bad(fmt("dzx: %s record %lld is outside the data", c.tag.c_str(), (long long)i)));
            Record r;
            r.tag = c.tag;
            r.layer = layer;
            r.index = static_cast<u32>(i);
            r.offset = static_cast<u32>(o);
            r.name = cstr(b, o, 8);
            r.params = be32_at(b, o + 8);
            r.pos = {bef32_at(b, o + 12), bef32_at(b, o + 16), bef32_at(b, o + 20)};
            r.ang = {bes16_at(b, o + 24), bes16_at(b, o + 26), bes16_at(b, o + 28)};
            r.set_id = be16_at(b, o + 30);
            out.push_back(std::move(r));
        }
    }
    return out;
}

// stagedata.stag(b) = dzx.dec_stag(b, chunk_map(b)['STAG'][2])
Result<Stag> stag(View b) {
    auto cs = chunks(b);
    if (!cs) return std::unexpected(cs.error());
    const Chunk* c = find_chunk(*cs, "STAG");
    if (!c) return std::unexpected(bad("dzx: no STAG chunk"));
    const u64 o = c->offset;
    if (!has(b, o, 0x2C)) return std::unexpected(bad("dzx: STAG is outside the data"));
    Stag s;
    s.near_plane = bef32_at(b, o);
    s.far_plane = bef32_at(b, o + 4);
    s.cam_type = b[o + 8];
    s.f09 = b[o + 9];
    s.f0a = be16_at(b, o + 0xA);
    s.f0c = be32_at(b, o + 0xC);
    s.f10 = be32_at(b, o + 0x10);
    s.gap_level = bes16_at(b, o + 0x1A);
    s.range_up = bes16_at(b, o + 0x1C);
    s.range_down = bes16_at(b, o + 0x1E);
    s.msg_group = b[o + 0x28];
    s.stage_title_no = be16_at(b, o + 0x2A);
    const View p = slice(b, o + 0x2C, o + 0x3C);
    s.particle_no.assign(p.begin(), p.end());
    return s;
}

}  // namespace hdf::stage
