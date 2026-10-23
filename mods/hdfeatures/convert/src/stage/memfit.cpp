// Module ST: room memory-block (STAG MEM0) sizing for the converted D_SB11 rooms.
//
// Ports tools/pipeline/memfit.py (estimate, mem0_for, block_bytes_pc, plan, stage_mem_tables). model_stats and
// arc_stats belong to module J (they fill hdf::ModelStats); calibrate is a developer check, not ported.
// The constants and the reasoning behind them are in memfit.py's docstring.
#include "stage_internal.hpp"

#include <algorithm>
#include <limits>

namespace hdf::stage {

using namespace detail;

namespace {

constexpr u64 kArcOvh = 0x4000;   // ARC_OVH
constexpr u64 kMdl = 0x2000;      // MDL
constexpr u64 kMat = 0x1800;      // MAT
constexpr u64 kShp = 0x800;       // SHP
constexpr u64 kTex = 0x100;       // TEX
constexpr u64 kJnt = 0x200;       // JNT
constexpr u64 kAnm = 0x1000;      // ANM
constexpr u64 kVcdVat = 0x180;    // VCDVAT
constexpr double kMargin = 0.10;  // MARGIN
constexpr i64 kPcFactor = 2;      // PC_FACTOR

}  // namespace

// memfit.estimate(s)
u64 estimate(const ModelStats& s) {
    return io::align(s.arc, 32) + kArcOvh + kMdl * s.models + kMat * s.mat + kShp * s.shp + kTex * s.tex +
           kJnt * s.jnt + kAnm * s.anm + s.dl + kVcdVat * s.shp;
}

namespace detail {

// memfit.mem0_for(need) without the u32 limit (patch_stage refuses a MEM0 above 0xFFFFFFFF, as struct.pack does)
u64 mem0_for_wide(u64 need) {
    // want = int(need * (1.0 + MARGIN) + 0.999): float math in Python's order, then truncation
    const double w = static_cast<double>(need) * (1.0 + kMargin) + 0.999;
    const i64 want = w >= 9.0e18 ? std::numeric_limits<i64>::max() / 4 : static_cast<i64>(w);
    // m = (want + PC_FACTOR - 1) // PC_FACTOR - 0x380
    const i64 m = io::floor_div(want + kPcFactor - 1, kPcFactor) - 0x380;
    return io::align(static_cast<u64>(std::max<i64>(m, 0x20)), 0x20);  // common.align(max(m, 0x20), 0x20)
}

// memfit.plan(room_stats, room_block, nblocks)[0] without the u32 limit
std::vector<u64> plan_wide(std::span<const ModelStats> rooms, std::span<const std::pair<u8, u8>> mec,
                           std::size_t nblocks) {
    // need = {r: estimate(s) for r, s in room_stats.items()}: one value per room, the last one wins
    std::vector<std::pair<int, u64>> need;
    for (const ModelStats& s : rooms) {
        const u64 n = estimate(s);
        auto it = std::find_if(need.begin(), need.end(), [&](const auto& p) { return p.first == s.room; });
        if (it == need.end()) {
            need.emplace_back(s.room, n);
        } else {
            it->second = n;
        }
    }
    auto block_of = [&](int r) -> std::optional<u8> {  // room_block.get(r)
        std::optional<u8> hit;
        for (const auto& [room, blk] : mec)
            if (static_cast<int>(room) == r) hit = blk;
        return hit;
    };
    std::vector<u64> mem0;
    for (std::size_t b = 0; b < nblocks; ++b) {
        u64 mx = 0x8000;  // max([need[r] for r in rooms] + [0x8000])
        for (const auto& [r, n] : need) {
            const std::optional<u8> blk = block_of(r);
            if (blk && *blk == b) mx = std::max(mx, n);
        }
        mem0.push_back(mem0_for_wide(mx));
    }
    return mem0;
}

}  // namespace detail

u32 mem0_for(u64 need) {
    const u64 v = detail::mem0_for_wide(need);
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(v);
}

// memfit.block_bytes_pc(mem0)
u64 block_bytes_pc(u32 mem0) { return (u64(mem0) + 0x380) * static_cast<u64>(kPcFactor); }

// memfit.stage_mem_tables(dzs)
Result<MemTables> stage_mem_tables(View dzs) {
    auto cs = chunks(dzs);  // ch[tag] = (cnt, off): the last chunk of a tag wins
    if (!cs) return std::unexpected(cs.error());
    MemTables t;
    if (const Chunk* c = find_chunk(*cs, "MEM0")) {
        for (i64 i = 0; i < count_of(*c); ++i) {
            const u64 o = u64(c->offset) + 4 * u64(i);
            if (!has(dzs, o, 4)) return std::unexpected(bad("MEM0 is outside the data"));
            t.mem0.push_back(be32_at(dzs, o));
        }
    }
    if (const Chunk* c = find_chunk(*cs, "MEC0")) {
        for (i64 i = 0; i < count_of(*c); ++i) {  // {dzs[off + 2 * i]: dzs[off + 2 * i + 1] ...}
            const u64 o = u64(c->offset) + 2 * u64(i);
            if (!has(dzs, o, 2)) return std::unexpected(bad("MEC0 is outside the data"));
            const u8 room = dzs[o], blk = dzs[o + 1];
            auto it = std::find_if(t.mec.begin(), t.mec.end(), [&](const auto& p) { return p.first == room; });
            if (it == t.mec.end()) {
                t.mec.emplace_back(room, blk);
            } else {
                it->second = blk;  // dict: first position, last value
            }
        }
    }
    return t;
}

std::vector<u32> plan_mem0(std::span<const ModelStats> rooms, std::span<const std::pair<u8, u8>> mec,
                           std::size_t nblocks) {
    std::vector<u32> out;
    for (u64 v : detail::plan_wide(rooms, mec, nblocks)) out.push_back(v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(v));
    return out;
}

}  // namespace hdf::stage
