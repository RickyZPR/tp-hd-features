// hdf_convert: the Cave of Shadows overlay files (module CV "cave"; src/cave/*).
//
// Ports: pipeline/cavebuild.py (build, _list_stage -> the fixed list), pipeline/particles.py (parse, build,
// tex_block, _convert_hd_texture, build_scene; needed_ids -> facts::kCaveParticleIds), the Obj_Stamp part of
// pipeline/stampbuild.build, the object archives (@bg0084, @bg0085 GC layout; O_gD_puL4, Obj_Stamp HD layout).
// The particle budget is the GZ2E01 constant inputs::kPsceneBudget (7.3).
//
// Wave 2 (module CV) additions, all additive: kObjStamp, kStampBmdIndex, kOverlayCount, kBuildSteps,
// kPsceneLargestName; ParticleStats, ObjStampInfo, BuildInfo and the build / build_particles / build_obj_stamp
// overloads that fill them (the Python report, patch log and log lines, for G's log and the oracle test);
// build_scene (particles.build_scene with an explicit id set, for tests); the JPAC2-10 reader and writer
// (particles.parse, build, tex_block) and gc_flow_ids (cavebuild.gc_flow_ids).
// Every read is bounds-checked (Correction #1): where the Python raises, the C++ returns Error{"fmt.cave"}
// (or the error of the module it calls); where Python's slice rules give a shorter value, the port gives the same.
// The not-ported parts of cavebuild.build are the res/ files (floors.json and colossal_wallet.png are on the drop
// list; hd_tags.bin is stage::hd_tags_bin) and the particle source scan (the oracle is made without --src).
#pragma once

#include "hdf/core.hpp"
#include "hdf/j3d.hpp"
#include "hdf/tex.hpp"

#include <array>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdf::cave {

inline constexpr std::string_view kStage = "D_SB11";
inline constexpr int kRoomCount = 50;                                         // R00..R49
inline constexpr std::array<std::string_view, 2> kObjStage = {"@bg0084", "@bg0085"};  // GC retail layout
inline constexpr std::array<std::string_view, 1> kObjItem = {"O_gD_puL4"};            // HD entry layout kept
inline constexpr std::string_view kParticleFile = "Pscene206.jpc";
// (CV additions)
inline constexpr std::string_view kObjStamp = "Obj_Stamp";                    // HD entry layout kept (stampbuild)
inline constexpr u32 kStampBmdIndex = 4;                                      // stampbuild.HD_STAMP_BMD_INDEX
inline constexpr std::size_t kOverlayCount = 56;                              // 51 stage + 4 objects + 1 particle
inline constexpr u32 kBuildSteps = 56;                                        // build(): p.done += 1 per file
inline constexpr std::string_view kPsceneLargestName = "Pscene020.jpc";       // particles.gc_budget on GZ2E01

// The 56 overlay files, rel as in the package ("overlay/res/Stage/D_SB11/STG_00.arc", R00..R49,
// "overlay/res/Object/{@bg0084,@bg0085,O_gD_puL4,Obj_Stamp}.arc", "overlay/res/Particle/Pscene206.jpc").
// hd/gc: CheckedSources. Order as Python writes them: R00..R49, STG_00, Pscene206.jpc, @bg0084, @bg0085,
// O_gD_puL4, Obj_Stamp. Progress: p.done += 1 per file (kBuildSteps in all); p.total and p.stage are G's.
// The stop token is checked between files and inside the j3d conversion; a stop gives Error{"stopped"}.
Result<std::vector<std::pair<std::string, Bytes>>> build(const Source& hd, const Source& gc, Progress& p,
                                                         std::stop_token stop);

// particles.build_scene(gc200, hd206, hd_gtx, need = facts, budget): Pscene206.jpc (265,600 B).
// hd_gtx_gz: tex/Particle/Pscene206.jpc.gtx.gz as stored.
Result<Bytes> build_particles(View gc_pscene200, View hd_pscene206, View hd_gtx_gz, u64 budget,
                              std::vector<std::string>* report = nullptr);

// The Obj_Stamp part of stampbuild.build: overlay/res/Object/Obj_Stamp.arc (stamp.bmd stays entry 4).
Result<Bytes> build_obj_stamp(const Source& hd, std::stop_token stop);

// ---------------------------------------------------------------------------------------------- (CV additions)
// particles.build_scene `stats` (+ need: len(needed_ids), halved_index: the HD texture index of each halved name).
struct ParticleStats {
    u32 need = 0;                         // len(need) (facts.CAVE_PARTICLE_IDS & the HD 206 ids)
    u32 resources = 0, from_gc = 0, from_hd = 0;
    u32 textures = 0, tex_gc = 0, tex_hd = 0;
    std::vector<std::string> halved;      // sorted(half): texture names (latin-1 bytes)
    std::vector<u32> halved_index;        // their HD texture indices, sorted
    u64 size = 0, budget = 0;
    u32 shared_header_differs = 0, hd_total = 0, gc_total = 0;
};

struct ObjStampInfo {                     // stampbuild.build res['obj_stamp'] + its log line
    std::optional<u32> bmd_index_hd, bmd_index;  // rarcx.entry_index(..., 'stamp.bmd'); nullopt = None
    j3d::ConvertStats conv;
    std::vector<std::string> report;
    std::string log;                      // 'stamps: Obj_Stamp.arc %d bytes, stamp.bmd entry index %s (HD %s), ...'
};

struct BuildTotals {                      // cavebuild.build `tot`
    u32 models = 0, mat_fixed = 0, textures = 0, no_source = 0, bti = 0, baked = 0;
};

// cavebuild.build's result dict, the parts module CV makes (for G's log and the oracle test).
struct BuildInfo {
    u8 msg_group = 0;                     // res['msg_group'] (STAG of the HD stage.dzs)
    std::vector<u16> gc_flows;            // sorted(gc_flow_ids(gc, msg_group))
    std::vector<u16> tagms_flows;         // res['tagms_flows']
    std::vector<u16> drop_tagms;          // res['drop_tagms'] (HD-only Tag_ms flows)
    std::vector<std::pair<std::string, j3d::ConvertStats>> conv;  // res['conv'], dict order (archive file names)
    BuildTotals totals;                   // res['totals']
    std::vector<u32> mem_old, mem_new;    // res['mem']['old'], ['new']
    ParticleStats particles;              // res['particles']['stats']
    std::vector<std::string> report;      // res['report']
    std::vector<std::string> patch_log;   // res['patch_log']
    std::vector<std::string> log;         // the log(...) lines up to the object archives, without the "[ t s]" prefix
    ObjStampInfo obj_stamp;
    // wall seconds per part: inputs (D_SB11 reads, SHA-256, pack inflate), convert, bake, patch (room.dzr, stage.dzs),
    // write (rarc_write + arc_stats), particles (inputs included), objects (inputs included, Obj_Stamp too)
    std::vector<std::pair<std::string, double>> seconds;
};

// build() that also fills `info` (nullptr: same as build() above).
Result<std::vector<std::pair<std::string, Bytes>>> build(const Source& hd, const Source& gc, Progress& p,
                                                         std::stop_token stop, BuildInfo* info);
// build_particles() that also fills `stats`; report gets particles.needed_ids' line and build_scene's lines.
Result<Bytes> build_particles(View gc_pscene200, View hd_pscene206, View hd_gtx_gz, u64 budget,
                              std::vector<std::string>* report, ParticleStats* stats, std::stop_token stop = {});
// particles.build_scene(gc200, hd206, hd_gtx, need, budget) with an explicit id set. hd_gtx: the inflated Gfx2
// (hdpack.read_any of the .gtx.gz). report: build_scene's lines (hd2gc.convert_texture lines of every blocks()
// call, the halving lines, the summary). stats.need is left 0.
Result<Bytes> build_scene(View gc200, View hd206, View hd_gtx, std::span<const u16> need, u64 budget,
                          std::vector<std::string>* report = nullptr, ParticleStats* stats = nullptr,
                          std::stop_token stop = {});
Result<Bytes> build_obj_stamp(const Source& hd, std::stop_token stop, ObjStampInfo* info);

// particles.parse: a JPAC2-10 file. Resource raws and texture raws are Python slices (clipped at the end).
struct JpcResource {
    u16 id = 0;
    Bytes raw;                            // d[st:o]: header + blocks
    std::optional<u32> tdb_off;           // offset of the last TDB1 block's data in raw, nullopt = None
    std::vector<u16> tdb;                 // texture indices ([] without TDB1 or with ntdb 0)
};
struct JpcTexture {
    std::string name;                     // d[o + 0xC:o + 0x20] up to the first NUL (latin-1 bytes)
    Bytes raw;                            // d[o:o + size]
    tex::BtiFields h;                     // j3d.bti_header(d, o + 0x20)
};
struct Jpc {
    std::vector<JpcResource> res;
    std::vector<JpcTexture> tex;
    u64 end = 0;                          // the offset after the last texture block
};
Result<Jpc> parse_jpc(View d);
// particles.build(resources, textures): header, resources, zero pad to a 32-aligned texture table, textures.
Result<Bytes> build_jpc(std::span<const Bytes> resources, std::span<const Bytes> textures);
// particles.tex_block(name_hdr, timg, pixels): name_hdr[:0x20] + timg + pixels (zero pad to 32), size at +4.
Result<Bytes> tex_block(View name_hdr, View timg, View pixels);

// cavebuild.gc_flow_ids: the flow ids (FLI1 entries >> 16) of the first .bmg member of a bmgres archive, sorted.
Result<std::vector<u16>> gc_flow_ids(View bmgres_arc);
// 'bmgres.arc' if group == 0 else 'bmgres%d.arc' % group, under res/Msgus/.
std::string bmgres_path(int group);

}  // namespace hdf::cave
