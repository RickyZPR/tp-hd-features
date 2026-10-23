// Module ST: data-only fixes so the HD Cave of Shadows (D_SB11) stage/room data behaves with unmodified
// GC/Dusklight actor code.
//
// Ports tools/cave/cave_patch.py (patch_stage, add_scls, drop_records, patch_room) and the MEM0 step of
// tools/pipeline/cavebuild.py (build: a pass without mem, memfit.stage_mem_tables + memfit.plan, the final pass).
// The reasons for each fix are in cave_patch.py's docstring and in cavebuild.py's. Report lines are Python's,
// character for character.
#include "stage_internal.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace hdf::stage {

using namespace detail;

namespace {

// dict.get(key, default) over {e['room']: e[field] for e in L} (the last entry of a room wins)
struct RoomMap {
    std::map<u32, i32> m;
    i64 get(i64 key, i64 dflt) const {
        if (key < 0 || key > 0xFFFFFFFFll) return dflt;
        auto it = m.find(static_cast<u32>(key));
        return it == m.end() ? dflt : it->second;
    }
};

void add(std::vector<std::string>* rep, std::string line) {
    if (rep) rep->push_back(std::move(line));
}

// cave_patch.patch_stage(b, L, opts, rep)
Result<Bytes> patch_stage_impl(View in, const LosTable& los, const PatchOptions& opts,
                               const std::optional<std::vector<u64>>& mem, std::vector<std::string>* rep) {
    Bytes b(in.begin(), in.end());
    auto cl = chunks(b);
    if (!cl) return std::unexpected(cl.error());
    const std::vector<Chunk> ch = std::move(*cl);  // ch = {c[0]: c for c in chunks(b)}: find_chunk, last wins
    RoomMap floor, nxt, prv;
    for (const LosEntry& e : los.entries) {
        floor.m[e.room] = e.floor;
        nxt.m[e.room] = e.next;
        prv.m[e.room] = e.prev;
    }
    if (!opts.no_rtbl) {
        const Chunk* c = find_chunk(ch, "RTBL");
        if (!c) return std::unexpected(bad("stage: no RTBL chunk"));
        const i64 cnt = count_of(*c);
        std::vector<u64> ents, roff;
        for (i64 i = 0; i < cnt; ++i) {
            const u64 o = u64(c->offset) + 4 * u64(i);
            if (!has(b, o, 4)) return std::unexpected(bad("stage: RTBL table is outside the data"));
            ents.push_back(be32_at(b, o));
        }
        for (u64 e : ents) {
            if (!has(b, e + 4, 4)) return std::unexpected(bad("stage: RTBL entry is outside the data"));
            roff.push_back(be32_at(b, e + 4));
        }
        for (i64 r = 0; r < cnt; ++r) {
            const std::size_t ri = static_cast<std::size_t>(r);
            const i64 cap = (r + 1 < cnt) ? static_cast<i64>(roff[ri + 1]) - static_cast<i64>(roff[ri]) : 3;
            if (!has(b, roff[ri], 1)) return std::unexpected(bad("stage: RTBL room list is outside the data"));
            std::vector<i64> want{(r & 0x3F) | (b[roff[ri]] & 0xC0)};
            if (nxt.get(r, -1) >= 0) want.push_back(nxt.get(r, -1) | 0x80);
            if (prv.get(r, -1) >= 0) want.push_back(prv.get(r, -1) | 0x80);
            if (nxt.get(r, -1) < 0 && prv.get(r, -1) >= 0 && prv.get(prv.get(r, -1), -1) >= 0)
                want.push_back(prv.get(prv.get(r, -1), -1) | 0x80);
            if (static_cast<i64>(want.size()) > cap)
                return std::unexpected(bad(fmt("RTBL room %lld: no room for %zu entries (cap %lld)", (long long)r,
                                               want.size(), (long long)cap)));
            if (!has(b, ents[ri], 1)) return std::unexpected(bad("stage: RTBL entry is outside the data"));
            const View old_v = slice(b, roff[ri], roff[ri] + b[ents[ri]]);
            const Bytes old(old_v.begin(), old_v.end());
            Bytes w;
            for (i64 v : want) {
                if (v < 0 || v > 0xFF) return std::unexpected(bad("stage: RTBL room number does not fit in a byte"));
                w.push_back(static_cast<u8>(v));
            }
            b[ents[ri]] = static_cast<u8>(w.size());
            slice_assign(b, roff[ri], roff[ri] + w.size(), w);  // b[roff[r]:roff[r] + len(want)] = bytes(want)
            if (old != w) add(rep, fmt("stage RTBL r%-2lld %s -> %s", (long long)r, hex_list(old).c_str(), hex_list(w).c_str()));
        }
    }
    if (!opts.no_meco) {
        const Chunk* c = find_chunk(ch, "MEC0");
        if (!c) return std::unexpected(bad("stage: no MEC0 chunk"));
        for (i64 i = 0; i < count_of(*c); ++i) {
            const u64 o = u64(c->offset) + 2 * u64(i);
            if (!has(b, o, 2)) return std::unexpected(bad("stage: MEC0 is outside the data"));
            const u8 rn = b[o], blk = b[o + 1];
            const i64 f = floor.get(rn, 0);
            if (f > 0) {
                const i64 nb = io::floor_mod(f, 3);
                if (nb != blk) {
                    b[o + 1] = static_cast<u8>(nb);
                    add(rep, fmt("stage MEC0 room %d block %d -> %lld (floor %lld)", int(rn), int(blk), (long long)nb,
                                 (long long)f));
                }
            }
        }
    }
    if (!opts.cam.empty()) {
        const Chunk* c = find_chunk(ch, "RCAM");
        if (!c) return std::unexpected(bad("stage: no RCAM chunk"));
        Bytes cam(opts.cam.begin(), opts.cam.end());  // opts['cam'].encode().ljust(16, b'\0')
        if (cam.size() < 16) cam.resize(16, 0);
        for (i64 i = 0; i < count_of(*c); ++i) {
            const u64 o = u64(c->offset) + u64(i) * 0x18;
            // wave2_ST_verify L1: past the end cstr is empty, so nothing matches, and b only grows on a match;
            // stopping here is exact and bounds a damaged count (Python loops up to 2^31 times).
            if (o >= b.size()) break;
            if (cstr(b, o, 16) == "BonusDungeon") {
                slice_assign(b, o, o + 16, cam);
                add(rep, fmt("stage RCAM[%lld] BonusDungeon -> %s", (long long)i, opts.cam.c_str()));
            }
        }
    }
    if (opts.particle) {
        const Chunk* c = find_chunk(ch, "STAG");
        if (!c) return std::unexpected(bad("stage: no STAG chunk"));
        const u64 o = u64(c->offset) + 0xA;
        if (!has(b, o, 2)) return std::unexpected(bad("stage: STAG is outside the data"));
        const u32 v = be16_at(b, o);
        const u32 nv = (v & ~(0xFFu << 3)) | ((u32(*opts.particle) & 0xFF) << 3);
        if (nv != v) {
            io::put16(b, static_cast<std::size_t>(o), static_cast<u16>(nv));
            add(rep, fmt("stage STAG particleNo %u -> %u (field_0x0a 0x%04x -> 0x%04x)", (v >> 3) & 0xFF,
                         unsigned(*opts.particle), v, nv));
        }
    }
    if (mem) {
        const Chunk* c = find_chunk(ch, "MEM0");
        if (!c) return std::unexpected(bad("stage: no MEM0 chunk"));
        const i64 cnt = count_of(*c);
        if (static_cast<i64>(mem->size()) != cnt)
            return std::unexpected(bad(fmt("MEM0 has %lld blocks, got %zu sizes", (long long)cnt, mem->size())));
        for (i64 i = 0; i < cnt; ++i) {
            const u64 o = u64(c->offset) + 4 * u64(i);
            if (!has(b, o, 4)) return std::unexpected(bad("stage: MEM0 is outside the data"));
            const u64 v = (*mem)[static_cast<std::size_t>(i)];
            if (v > 0xFFFFFFFFull) return std::unexpected(bad("stage: MEM0 size does not fit in 32 bits"));
            const u32 old = be32_at(b, o);
            io::put32(b, static_cast<std::size_t>(o), static_cast<u32>(v));
            add(rep, fmt("stage MEM0[%lld] 0x%x -> 0x%llx", (long long)i, old, (unsigned long long)v));
        }
    }
    return b;
}

std::string ang_repr(const std::array<i16, 3>& a) { return fmt("(%d, %d, %d)", int(a[0]), int(a[1]), int(a[2])); }

i16 to_s16(i64 v) { return static_cast<i16>(v >= 0x8000 ? v - 0x10000 : v); }  // n - 0x10000 if n >= 0x8000

}  // namespace

Result<Bytes> patch_stage_mem(View hd_stage_dzs, const LosTable& los, const PatchOptions& opts,
                              const std::optional<std::vector<u32>>& mem, std::vector<std::string>* report) {
    std::optional<std::vector<u64>> m;
    if (mem) m.emplace(mem->begin(), mem->end());
    return patch_stage_impl(hd_stage_dzs, los, opts, m, report);
}

// cavebuild.build, "MEM0: block of every room after the MEC0 rewrite, then size each block for its largest room"
Result<Bytes> patch_stage(View hd_stage_dzs, std::span<const ModelStats> rooms, const LosTable& los,
                          const PatchOptions& opts, std::vector<std::string>* report) {
    auto nomem = patch_stage_impl(hd_stage_dzs, los, opts, std::nullopt, nullptr);  // dict(opts, mem=None)
    if (!nomem) return std::unexpected(nomem.error());
    auto old_t = stage_mem_tables(hd_stage_dzs);  // mem_old, mec = memfit.stage_mem_tables(dzs_m.data())
    if (!old_t) return std::unexpected(old_t.error());
    auto new_t = stage_mem_tables(*nomem);         // _, mec_new = memfit.stage_mem_tables(dzs_nomem)
    if (!new_t) return std::unexpected(new_t.error());
    const std::vector<u64> mem0 = plan_wide(rooms, new_t->mec, old_t->mem0.size());  // memfit.plan
    return patch_stage_impl(hd_stage_dzs, los, opts, mem0, report);                  // dict(opts, mem=mem0)
}

// cave_patch.add_scls(b, scls_rec)
Result<std::pair<Bytes, bool>> add_scls(View in, View scls_rec) {
    Bytes b(in.begin(), in.end());
    auto cl = chunks(b);
    if (!cl) return std::unexpected(cl.error());
    for (const Chunk& c : *cl)
        if (c.tag == "SCLS") return std::pair<Bytes, bool>{std::move(b), false};
    constexpr u64 shift = 12;
    // chunk offsets are absolute file offsets; RPAT/PATH point offsets are relative to the RPPN/PPNT chunk, so
    // they are not shifted (cave_patch.py)
    for (const Chunk& c : *cl) {
        if (u64(c.offset) + shift > 0xFFFFFFFFull) return std::unexpected(bad("add_scls: chunk offset overflows"));
        io::put32(b, c.header_offset + 8, static_cast<u32>(c.offset + shift));
    }
    const u64 n = cl->size();
    const u64 hdr_end = 4 + n * 12;
    u64 tail_off = b.size() + shift;
    tail_off = (tail_off + 3) & ~u64(3);
    if (tail_off > 0xFFFFFFFFull) return std::unexpected(bad("add_scls: file too large"));
    // newhdr = b[:hdr_end] + struct.pack('>4siI', b'SCLS', 1, tail_off); out = newhdr + body
    const View head = slice(b, 0, hdr_end), body = slice(b, hdr_end, b.size());
    Bytes out(head.begin(), head.end());
    const u8 ent[12] = {'S', 'C', 'L', 'S', 0, 0, 0, 1, u8(tail_off >> 24), u8(tail_off >> 16), u8(tail_off >> 8), u8(tail_off)};
    out.insert(out.end(), ent, ent + 12);
    out.insert(out.end(), body.begin(), body.end());
    if (out.size() < tail_off) out.resize(static_cast<std::size_t>(tail_off), 0);  // b'\0' * (tail_off - len(out))
    out.insert(out.end(), scls_rec.begin(), scls_rec.end());
    out.resize(out.size() + (4 - scls_rec.size() % 4) % 4, 0);  // b'\0' * ((-len(scls_rec)) % 4)
    if (out.size() < 4) return std::unexpected(bad("add_scls: no chunk count"));
    io::put32(out, 0, static_cast<u32>(n + 1));  // struct.pack_into('>i', out, 0, n + 1)
    return std::pair<Bytes, bool>{std::move(out), true};
}

// cave_patch.drop_records(rn, b, should_drop, rep)
Result<Bytes> drop_records(int rn, View in, const DropFn& should_drop, std::vector<std::string>* rep) {
    Bytes b(in.begin(), in.end());
    auto cl = chunks(b);  // evaluated once, before any change (Python's for loop)
    if (!cl) return std::unexpected(cl.error());
    for (const Chunk& c : *cl) {
        if (!(is_actor_tag(c.tag) || is_scob_tag(c.tag))) continue;
        const u64 stride = is_actor_tag(c.tag) ? 0x20 : 0x24;
        const i64 cnt = count_of(c);
        std::vector<Bytes> keep;
        for (i64 i = 0; i < cnt; ++i) {
            const u64 o = u64(c.offset) + u64(i) * stride;
            const View rv = slice(b, o, o + stride);  // rec = bytes(b[o:o + stride])
            Bytes rec(rv.begin(), rv.end());
            if (rec.size() < 0x1E) return std::unexpected(bad(fmt("drop_records: %s record %lld is outside the data",
                                                                  c.tag.c_str(), (long long)i)));
            const std::string name = cstr(rec, 0, 8);
            const u32 p = be32_at(rec, 8);
            const std::array<i16, 3> ang{bes16_at(rec, 0x18), bes16_at(rec, 0x1A), bes16_at(rec, 0x1C)};
            if (should_drop(c.tag, name, p, ang)) {
                if (rep) rep->push_back(fmt("R%02d %s %s prm %08x ang %s deleted", rn, c.tag.c_str(), name.c_str(), p,
                                            ang_repr(ang).c_str()));
            } else {
                keep.push_back(std::move(rec));
            }
        }
        if (static_cast<i64>(keep.size()) != cnt) {  // also true for a negative count, which becomes 0
            Bytes blob;
            for (const Bytes& r : keep) blob.insert(blob.end(), r.begin(), r.end());
            const i64 total = cnt * static_cast<i64>(stride);  // negative for a negative count
            if (total > 0 && blob.size() < static_cast<u64>(total)) blob.resize(static_cast<std::size_t>(total), 0);
            // b[off:off + cnt * stride] = blob + b'\0' * (cnt * stride - len(blob))
            slice_assign_py(b, static_cast<i64>(c.offset), static_cast<i64>(c.offset) + total, blob);
            if (!has(b, u64(c.header_offset) + 4, 4)) return std::unexpected(bad("drop_records: chunk table changed"));
            io::put32(b, c.header_offset + 4, static_cast<u32>(keep.size()));  // '>i', len(keep)
        }
    }
    return b;
}

// cave_patch.patch_room(rn, b, opts, rep)
Result<Bytes> patch_room(int rn, View in, const PatchOptions& opts, std::vector<std::string>* rep) {
    Bytes b(in.begin(), in.end());
    auto cl = chunks(b);
    if (!cl) return std::unexpected(cl.error());
    for (const Chunk& c : *cl) {
        if (!(is_actor_tag(c.tag) || is_scob_tag(c.tag))) continue;
        const u64 stride = is_actor_tag(c.tag) ? 0x20 : 0x24;
        for (i64 i = 0; i < count_of(c); ++i) {
            const u64 o = u64(c.offset) + u64(i) * stride;
            const std::string name = cstr(b, o, 8);
            // p = '>I' at o + 8; ax, ay, az = '>hhh' at o + 0x18
            if (!has(b, o + 8, 4) || !has(b, o + 0x18, 6))
                return std::unexpected(bad(fmt("R%02d %s record %lld is outside the data", rn, c.tag.c_str(), (long long)i)));
            const u32 p = be32_at(b, o + 8);
            const i16 ax = bes16_at(b, o + 0x18), ay = bes16_at(b, o + 0x1A);
            i16 az = bes16_at(b, o + 0x1C);
            u32 np = p;
            if (opts.gates && name == "rGate" && ((p >> 8) & 0xF) >= 2) np = (p & ~0xF00u) | 0x100;
            if (opts.kbota && name == "Kbota_A" && ((p >> 16) & 0xF) == 3) np = (p & ~0xF0000u) | 0x10000;
            if (opts.exit && name == "l8warp") np = (p & ~0xFF00u);
            // floor 40: the gate to the reward room opens on zone switch 0xC0, which only the HD end-fairy flow
            // 1122 sets (event014 zone 0). GC FSeirei is deleted unless F_0505 and has no flow 1122.
            if (opts.fairy_bypass && rn == 49 && name == "rGate" && (p & 0xFF) == 0xC0) np = (np & ~0xFFu) | 0xB1;
            if (opts.reward_item && rn == 14 && starts_with(name, "tbox")) {
                const i16 naz = to_s16((az & 0xFF) | (i64(*opts.reward_item) << 8));
                if (naz != az) {
                    put16s(b, o + 0x1C, naz);
                    add(rep, fmt("R14 %s %s item 0x%02x -> 0x%02x", c.tag.c_str(), name.c_str(), (az >> 8) & 0xFF,
                                 unsigned(*opts.reward_item)));
                    az = naz;
                }
            }
            if (opts.tbox_unique && starts_with(name, "tbox") && ((p >> 6) & 0x3F) == 63 &&
                (rn == 25 || rn == 34 || rn == 14)) {
                const u32 no = rn == 25 ? 60u : rn == 34 ? 61u : 62u;  // {25: 60, 34: 61, 14: 62}[rn]
                np = (p & ~(0x3Fu << 6)) | (no << 6);
            }
            if (np != p) {
                io::put32(b, static_cast<std::size_t>(o + 8), np);
                add(rep, fmt("R%02d %s %s prm %08x -> %08x", rn, c.tag.c_str(), name.c_str(), p, np));
            }
            if (opts.swc && (name == "SwAreaC" || name == "SwAreaS")) {
                const int t = ((ax & 0xFFFF) >> 8) & 0xF;
                if (t == 10 || t == 11) {
                    // GC type 15 + the HD type in the free top nibble (idempotent: a patched record has type 15)
                    const i16 nax = to_s16((ax & 0x00FF) | (15 << 8) | (t << 12));
                    put16s(b, o + 0x18, nax);
                    add(rep, fmt("R%02d %s %s type %d -> 15, HD type kept in bits 12-15 (angle.x 0x%04x -> 0x%04x)", rn,
                                 c.tag.c_str(), name.c_str(), t, ax & 0xFFFF, nax & 0xFFFF));
                }
            }
            if (opts.tagev && name == "TagEv") {
                const int inv = ax & 0x7FFF;
                const int val = az & 0xFFFF;
                i16 nax = ax, naz = az;
                if (822 <= inv && inv < 0x7FFF) nax = to_s16((ax & 0x8000) | 0x7FFF);
                if (822 <= val && val < 0xFFFF) naz = -1;
                if (nax != ax || naz != az) {
                    put16s(b, o + 0x18, nax);
                    put16s(b, o + 0x1A, ay);
                    put16s(b, o + 0x1C, naz);
                    add(rep, fmt("R%02d %s TagEv labels inv %d val %d -> inv %d val %d", rn, c.tag.c_str(), inv, val,
                                 nax & 0x7FFF, naz & 0xFFFF));
                }
            }
        }
    }
    Bytes out = std::move(b);
    const std::set<u16> drop_flows(opts.drop_tagms.begin(), opts.drop_tagms.end());
    if (opts.fseirei || !drop_flows.empty()) {
        auto dropped = drop_records(
            rn, out,
            [&](std::string_view, std::string_view name, u32 p, const std::array<i16, 3>& ang) {
                if (opts.fseirei && name == "FSeirei" && (p & 0xFF) == 4) return true;
                if (name == "Tag_ms" && drop_flows.contains(static_cast<u16>(ang[2] & 0xFFFF))) return true;
                return false;
            },
            rep);
        if (!dropped) return std::unexpected(dropped.error());
        out = std::move(*dropped);
    }
    if (opts.exit && (rn == 25 || rn == 34 || rn == 14)) {
        // rec = st.encode().ljust(8, b'\0') + struct.pack('>BbBBB', start, room, 0xF0, 0x1F, 1)
        Bytes rec(opts.exit->stage.begin(), opts.exit->stage.end());
        if (rec.size() < 8) rec.resize(8, 0);
        const u8 tail[5] = {opts.exit->start, opts.exit->room, 0xF0, 0x1F, 1};
        rec.insert(rec.end(), tail, tail + 5);
        auto added = add_scls(out, rec);
        if (!added) return std::unexpected(added.error());
        out = std::move(added->first);
        if (added->second)
            add(rep, fmt("R%02d added room SCLS[0] -> %s start %d room %d", rn, opts.exit->stage.c_str(),
                         int(opts.exit->start), int(opts.exit->room)));
    }
    return out;
}

}  // namespace hdf::stage
