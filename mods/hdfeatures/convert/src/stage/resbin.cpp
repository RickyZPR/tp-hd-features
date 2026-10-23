// Module ST: the two small res/ binaries made from HD stage records.
//
// Ports tools/pipeline/cavebuild.py (hd_tag_records, hd_tags_bin) and tools/pipeline/stampbuild.py
// (hd_chest_records, hd_chests_bin), with the parts of tools/formats/dzx.py they use (parse: the chunk table
// check and canon(); dec_actor: name, params, angle.z, the tbox fields).
//
// dzx.dec_actor marks a record as a chest when its name starts with "tbox" or objnames.json gives it the proc
// fpcNm_TBOX_e / fpcNm_TBOX2_e. Every one of the 13 names with those procs starts with "tbox" (VERIFIED wave 2 ST,
// tools/formats/objnames.json: tboxA0 A1 A2 B0 B00 B01 B1 B2 EL0 EL1 J0 P0 W0), so the name test alone is the same.
#include "stage_internal.hpp"

#include "hdf/facts.hpp"

namespace hdf::stage {

using namespace detail;

namespace {

constexpr std::string_view kCaveStage = "D_SB11";  // cavebuild.STAGE

std::string room_arc(std::string_view stage, unsigned room) {
    return "res/Stage/" + std::string(stage) + "/" + fmt("R%02u_00.arc", room);
}

Result<io::Rarc> read_arc(const Source& hd, const std::string& path) {
    auto raw = hd.read(path);
    if (!raw) return std::unexpected(raw.error());
    auto arc = io::rarc_read(*raw);
    if (!arc) return std::unexpected(arc.error());
    return arc;
}

struct TagRecord {
    u8 room;
    Bytes record;  // the 0x24-byte HD SCOB record
};

// cavebuild.hd_tag_records(hd_dzr, hd_records): HD's section-end talk tags (Tag_ms with facts.CAVE_TAG_FLOW in
// facts.CAVE_TAG_ROOMS). hd_dzr / hd_records are the room.dzr of res/Stage/D_SB11/R<room>_00.arc.
Result<std::vector<TagRecord>> hd_tag_records(const Source& hd) {
    std::vector<TagRecord> out;
    for (u8 room : facts::kCaveTagRooms) {
        auto arc = read_arc(hd, room_arc(kCaveStage, room));
        if (!arc) return std::unexpected(arc.error());
        Bytes dzr;
        std::vector<Record> recs;  // hd_records.get(room, [])
        if (const io::RarcMember* m = arc->get("room.dzr")) {
            auto d = arc->data(*m);
            if (!d) return std::unexpected(d.error());
            dzr = std::move(*d);
            auto r = records(dzr);
            if (!r) return std::unexpected(r.error());
            recs = std::move(*r);
        }
        std::vector<const Record*> hits;
        for (const Record& r : recs)
            if (r.name == "Tag_ms" && r.tag.substr(0, 3) == "SCO" && (r.ang[2] & 0xFFFF) == facts::kCaveTagFlow)
                hits.push_back(&r);
        if (hits.size() != 1)
            return std::unexpected(bad(fmt("D_SB11 room %d: %zu Tag_ms records with flow %d (expected 1)", int(room),
                                           hits.size(), int(facts::kCaveTagFlow))));
        const u64 o = hits[0]->offset;
        const View rec = slice(dzr, o, o + 0x24);  // bytes(hd_dzr[room][o:o + 0x24])
        out.push_back(TagRecord{room, Bytes(rec.begin(), rec.end())});
    }
    return out;
}

struct ChestRecord {
    u8 stamp;
    int layer;     // -1 = TRES, n = TREn
    u8 room;
    Bytes record;  // the 0x20-byte TRES record
};

// dzx.canon(tag) == 'TRES': the layer (-1 for "TRES" itself), or nullopt for another tag
std::optional<int> tres_layer(std::string_view tag) {
    if (tag == "TRES") return -1;
    if (tag.size() == 4 && tag.substr(0, 3) == "TRE") {
        const std::size_t k = kLayer.find(tag[3]);
        if (k != std::string_view::npos) return static_cast<int>(k);
    }
    return std::nullopt;
}

// stampbuild.hd_chest_records(hd): [(stamp index, layer, room, 0x20-byte TRES record)] of facts.HD_CHESTS
Result<std::vector<ChestRecord>> hd_chest_records(const Source& hd) {
    std::vector<ChestRecord> out;
    for (const facts::HdChest& ch : facts::kHdChests) {
        auto arc = read_arc(hd, room_arc(ch.stage, ch.room));
        if (!arc) return std::unexpected(arc.error());
        std::optional<std::pair<int, Bytes>> hit;
        for (const io::RarcMember& m : arc->members) {
            std::string low = m.name;
            for (char& c : low)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (low.size() < 4 || low.substr(low.size() - 4) != ".dzr") continue;
            auto d = arc->data(m);
            if (!d) return std::unexpected(d.error());
            const View b = *d;
            // dzx.parse(b): n < 0 or n > 512 or 4 + n * 12 > len(buf) -> ValueError
            if (!has(b, 0, 4)) return std::unexpected(bad("dzx: not a dzs/dzr"));
            const i32 n = static_cast<i32>(be32_at(b, 0));
            if (n < 0 || n > 512 || !has(b, 4, u64(n) * 12))
                return std::unexpected(bad(fmt("dzx: not a dzs/dzr (chunk count %d)", n)));
            for (i32 ci = 0; ci < n && !hit; ++ci) {
                const u64 ho = 4 + u64(ci) * 12;
                const std::string tag(reinterpret_cast<const char*>(b.data() + ho), 4);
                const std::optional<int> layer = tres_layer(tag);
                if (!layer) continue;
                const i64 cnt = static_cast<i32>(be32_at(b, ho + 4));
                const u64 off = be32_at(b, ho + 8);
                for (i64 k = 0; k < cnt; ++k) {
                    const u64 o = off + u64(k) * 0x20;  // c.stride = STRIDE['TRES'] = 0x20
                    if (!has(b, o + 8, 24))              // dec_actor: struct.unpack_from('>I3f3hH', buf, o + 8)
                        return std::unexpected(bad("dzx: TRES record is outside the data"));
                    if (!starts_with(cstr(b, o, 8), "tbox")) continue;  // .get('tbox')
                    const u32 params = be32_at(b, o + 8);
                    const i16 az = bes16_at(b, o + 28);
                    const int item = (az >> 8) & 0xFF, tbox_no = static_cast<int>((params >> 6) & 0x3F);
                    if (tbox_no == ch.chest && item == ch.item) {
                        const View rec = slice(b, o, o + 0x20);
                        hit.emplace(*layer, Bytes(rec.begin(), rec.end()));
                        break;
                    }
                }
            }
            if (hit) break;
        }
        if (!hit)
            return std::unexpected(bad(fmt("HD-only chest of stamp %d (chest %d, item 0x%02X) not in %.*s R%02d",
                                           int(ch.stamp), int(ch.chest), unsigned(ch.item), int(ch.stage.size()),
                                           ch.stage.data(), int(ch.room))));
        out.push_back(ChestRecord{ch.stamp, hit->first, ch.room, std::move(hit->second)});
    }
    return out;
}

}  // namespace

// cavebuild.hd_tags_bin(hd_tag_records(...)): 'HDFT' u8 version=1 u8 count u16 0, then per tag u8 room,
// u8[3] 0, u8 record[0x24].
Result<Bytes> hd_tags_bin(const Source& hd) {
    auto tags = hd_tag_records(hd);
    if (!tags) return std::unexpected(tags.error());
    Bytes b{'H', 'D', 'F', 'T', 1, static_cast<u8>(tags->size()), 0, 0};
    for (const TagRecord& t : *tags) {
        const u8 head[4] = {t.room, 0, 0, 0};
        b.insert(b.end(), head, head + 4);
        b.insert(b.end(), t.record.begin(), t.record.end());
    }
    return b;
}

// stampbuild.hd_chests_bin(hd_chest_records(hd)): 'HDFC' u8 version=1 u8 count u16 0, then per chest u8 stamp
// index, s8 layer (-1 = TRES, n = TREn), u8 room, u8 0, u8 record[0x20].
Result<Bytes> hd_chests_bin(const Source& hd) {
    auto chests = hd_chest_records(hd);
    if (!chests) return std::unexpected(chests.error());
    Bytes b{'H', 'D', 'F', 'C', 1, static_cast<u8>(chests->size()), 0, 0};
    for (const ChestRecord& c : *chests) {
        const u8 head[4] = {c.stamp, static_cast<u8>(c.layer & 0xFF), c.room, 0};
        b.insert(b.end(), head, head + 4);
        b.insert(b.end(), c.record.begin(), c.record.end());
    }
    return b;
}

}  // namespace hdf::stage
