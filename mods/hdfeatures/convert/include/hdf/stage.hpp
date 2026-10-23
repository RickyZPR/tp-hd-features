// hdf_convert: stage data (module ST "stage"; src/stage/*).
//
// Ports: formats/dzx.py (parse subset, Chunk), pipeline/stagedata.py, cave/losbin.py, cave/cave_patch.py
// (patch_stage, patch_room, add_scls, drop_records), pipeline/memfit.py (plan, mem0_for, block_bytes_pc,
// estimate, stage_mem_tables), pipeline/cavebuild.py (hd_tag_records, hd_tags_bin, floor_chain),
// pipeline/stampbuild.py (hd_chest_records, hd_chests_bin). Facts (particle ids, chests, tag flow/rooms) come
// from facts.hpp (Correction #11). ModelStats is in core.hpp (produced by j3d).
//
// Every read is bounds-checked (Correction #1): bad data gives Error{"fmt.stage"}, never a crash. Where Python
// would also fail (struct.error, IndexError, KeyError, ValueError) this port fails; where Python's slice rules
// give a value (a short slice, a slice assignment that grows the buffer) this port gives the same value.
// Chunk counts are read signed as Python does ('>i'): a negative count loops 0 times.
#pragma once

#include "hdf/core.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::stage {

// ---------------------------------------------------------------------------------------------- DZx
struct Chunk {          // cave_patch.chunks: [tag, count, offset, header_offset]
    std::string tag;    // 4 chars
    u32 count = 0;
    u32 offset = 0;     // first record
    u32 header_offset = 0;  // this chunk's 12-byte entry in the chunk table
};
Result<std::vector<Chunk>> chunks(View dzx);                     // stagedata.chunks / cave_patch.chunks
// stagedata.chunk_map(b).get(tag): the LAST chunk with that tag (Python dict), nullptr if none. (ST addition)
const Chunk* find_chunk(std::span<const Chunk> cs, std::string_view tag);

struct Record {         // stagedata.records: ACTR/ACTn/TGOB/PLYR/TRES/TREn (0x20), SCOB/SCOn/TGSC/TGDR/Door/Doon (0x24)
    std::string tag;    // chunk tag
    int layer = -1;     // stagedata.layer_of(tag)
    u32 index = 0;      // 'i': index in its chunk
    u32 offset = 0;     // 'off': byte offset of the record
    std::string name;   // up to 8 chars (latin-1)
    u32 params = 0;     // 'prm'
    std::array<float, 3> pos{};
    std::array<i16, 3> ang{};
    u16 set_id = 0;     // 'setid'
};
Result<std::vector<Record>> records(View dzx);

// stagedata.stag = dzx.dec_stag of the stage's STAG chunk (ST addition; cavebuild.build reads msg_group).
// Field names avoid near/far (windows.h macros). Error "fmt.stage" when there is no STAG chunk (Python: None).
struct Stag {
    float near_plane = 0, far_plane = 0;  // 'near', 'far'
    u8 cam_type = 0;                      // 'camType'
    u8 f09 = 0;
    u16 f0a = 0;                          // particleNo (bits 3-10) and more
    u32 f0c = 0, f10 = 0;
    i16 gap_level = 0, range_up = 0, range_down = 0;
    u8 msg_group = 0;                     // 'msgGroup'
    u16 stage_title_no = 0;               // 'stageTitleNo'
    std::vector<u8> particle_no;          // 'particleNo': buf[o + 0x2C:o + 0x3C] (16 bytes, fewer at the end)
};
Result<Stag> stag(View dzs);

// ---------------------------------------------------------------------------------------------- los.bin
struct LosEntry {       // losbin.load: 0x20 bytes each, big-endian
    u32 index = 0;
    u32 room = 0;
    i32 next = 0, prev = 0, floor = 0;
    float x = 0, y = 0, z = 0;
    i16 unk1c = 0, angle_y = 0;
};
struct LosTable {
    u64 size = 0;
    u32 count = 0, unk4 = 0, unk8 = 0;
    std::vector<LosEntry> entries;
    Bytes tail;
    const LosEntry* by_room(u32 room) const;   // {e['room']: e for e in L}: the last entry of that room
};
Result<LosTable> read_los(View los_bin);
// cavebuild.floor_chain: the main chain from the floor-1 room along the next links. (ST addition)
Result<std::vector<LosEntry>> floor_chain(const LosTable& los);

// ---------------------------------------------------------------------------------------------- patches
// cavebuild.build opts: the one configuration the pipeline uses (defaults = production values).
struct PatchOptions {
    bool no_rtbl = false;
    bool no_meco = false;
    std::string cam = "Hall";              // RCAM BonusDungeon -> Hall
    std::optional<u8> particle;            // STAG particleNo (None)
    bool gates = true, kbota = true, swc = true, tagev = true, fseirei = true;
    std::vector<u16> drop_tagms;           // HD-only Tag_ms flows (computed: [facts::kCaveTagFlow] on GZ2E01)
    struct Exit { std::string stage = "F_SP124"; u8 start = 6; u8 room = 0; };
    std::optional<Exit> exit = Exit{};     // cavebuild.EXIT (GC D_SB01's own exit)
    bool tbox_unique = true;
    bool fairy_bypass = true;
    std::optional<u8> reward_item;         // None
    bool banks_gc = false;
};

// cave_patch.patch_room(rn, b, opts, rep). report (optional) gets the Python report lines.
Result<Bytes> patch_room(int room, View hd_room_dzr, const PatchOptions& opts = {},
                         std::vector<std::string>* report = nullptr);
// cave_patch.patch_stage with opts.mem: MEM0 is planned from the built rooms (memfit.plan over MEC0 after the
// rewrite), exactly as cavebuild.build does (first a pass without mem, then the final pass).
Result<Bytes> patch_stage(View hd_stage_dzs, std::span<const ModelStats> rooms, const LosTable& los,
                          const PatchOptions& opts = {}, std::vector<std::string>* report = nullptr);
// cave_patch.patch_stage(b, L, dict(opts, mem=mem), rep) with mem given: nullopt = None (MEM0 kept), else one
// value per MEM0 block. (ST addition: the no-mem pass of cavebuild.build, and tests.)
Result<Bytes> patch_stage_mem(View hd_stage_dzs, const LosTable& los, const PatchOptions& opts,
                              const std::optional<std::vector<u32>>& mem, std::vector<std::string>* report = nullptr);
// cave_patch.add_scls: insert a 1-entry SCLS chunk (12 more header bytes, every chunk offset + 12, the record at
// the 4-aligned end). first = the new bytes, second = added (false and unchanged when there is an SCLS). (ST addition)
Result<std::pair<Bytes, bool>> add_scls(View dzr, View scls_record);
// cave_patch.drop_records: delete the actor/SCOB records for which should_drop(tag, name, params, angles) is
// true; later records move up, the count drops, the freed tail is zero-filled. A negative count is written back
// as 0, as Python does. (ST addition)
using DropFn = std::function<bool(std::string_view tag, std::string_view name, u32 params,
                                  const std::array<i16, 3>& angles)>;
Result<Bytes> drop_records(int room, View dzr, const DropFn& should_drop, std::vector<std::string>* report = nullptr);

// ---------------------------------------------------------------------------------------------- memfit
u64 estimate(const ModelStats& s);                               // memfit.estimate
u32 mem0_for(u64 need);                                          // memfit.mem0_for: int(need * 1.1 + 0.999)
u64 block_bytes_pc(u32 mem0);                                    // memfit.block_bytes_pc
struct MemTables { std::vector<u32> mem0; std::vector<std::pair<u8, u8>> mec; };  // MEC0: room -> block
Result<MemTables> stage_mem_tables(View dzs);                    // memfit.stage_mem_tables
// memfit.plan: rooms (with .room set) + MEC0 room->block -> MEM0 per block (nblocks = old MEM0 count).
std::vector<u32> plan_mem0(std::span<const ModelStats> rooms, std::span<const std::pair<u8, u8>> mec,
                           std::size_t nblocks);

// ---------------------------------------------------------------------------------------------- res/ binaries
// Both read their HD inputs through `hd` (a CheckedSource in production).
Result<Bytes> hd_tags_bin(const Source& hd);                     // 'HDFT' -> res/cave/hd_tags.bin (88 B)
Result<Bytes> hd_chests_bin(const Source& hd);                   // 'HDFC' -> res/stamps/stamps/hd_chests.bin (260 B)

}  // namespace hdf::stage
