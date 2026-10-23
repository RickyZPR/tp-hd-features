// hdf_convert: J3D models and archive conversion (module J "j3d"; src/j3d/*).
//
// Ports: formats/j3d.py, formats/hd2gc.py (convert_j3d, convert_bti, convert_archive, pick_level,
// patch_header, convert_texture), critic/mat4fix.py, critic/matstride.py (parts used), cave/bake_los.py,
// formats/j3d_dlcheck.py (pc_dl_bytes), pipeline/convert.py, memfit.model_stats/arc_stats.
// Bake angles: only -0x8000, -0x4000, 0, 0x4000, with the exact (cos, sin) table of pipeline 5, signed zero
// included: -0x8000 -> (-1.0, -0.0). Any other angle returns Error{"fmt.j3d"}.
//
// Wave 2 (module J) additions, all additive: Section + sections (j3d.sections), the convert_members and
// convert_arc overloads with the archive's file name (report labels), DlBytes + pc_dl_detail (the whole
// j3d_dlcheck.pc_dl_bytes dict), mat_effective_stride (matstride.effective_stride).
// Every function here is bounds-checked: where the Python raises, the C++ returns Error{"fmt.j3d"} (model_stats,
// which has no error path, returns zeros as memfit.model_stats does for a model it cannot parse).
// The texture part of hd2gc (pick_level, convert_texture, patch_header) lives in tex.hpp (module T).
#pragma once

#include "hdf/core.hpp"
#include "hdf/io.hpp"

#include <array>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::j3d {

struct ConvertStats {    // convert.convert_arc `st`
    u32 models = 0, mat_fixed = 0, textures = 0, no_source = 0, bti = 0, mdl3 = 0;
};

// convert.convert_arc: every .bmd/.bdl/.bti member converted (TEX1 from the pack's .gtx, MAT4->MAT3 + repack);
// nothing baked, nothing written. The caller adds its own replacements (bake, room.dzr) and writes the archive.
struct Converted {
    io::Rarc arc;                     // the HD archive (Yaz0 decoded)
    io::Replace replace;              // member path -> converted bytes
    ConvertStats stats;
    std::vector<std::string> report;  // the Python report lines
};
// hd_arc: the .arc as stored (Yaz0); pack: the matching .pack.gz already read (nullptr: no pack, e.g. R24).
// The stats are counted from the report lines, as convert.convert_arc does. Report labels start with ":" here;
// the overload below takes the archive's file name, as convert.convert_arc uses os.path.basename(arc_path).
Result<Converted> convert_members(View hd_arc, const io::Tmpk* pack, std::stop_token stop = {});
Result<Converted> convert_members(View hd_arc, const io::Tmpk* pack, std::string_view arc_name,
                                  std::stop_token stop = {});

struct Bake {
    std::array<float, 3> t{};   // los.bin x, y, z
    int angle = 0;              // los.bin angleY
};

struct ArcResult {
    Bytes arc;
    std::vector<ModelStats> stats;  // one ModelStats for the whole archive (memfit.arc_stats), room unset
};
// Whole-archive helper for the object archives and Obj_Stamp: convert, optional bake of every model,
// write in GC retail layout or keep the HD entry layout (same_layout, rarcx.rebuild_same_layout).
// pack_gz: the .pack.gz as stored (gzip), nullopt when the archive has none.
Result<ArcResult> convert_arc(View hd_arc_yaz0, std::optional<View> pack_gz, std::optional<Bake> bake,
                              bool same_layout, std::stop_token stop = {});
Result<ArcResult> convert_arc(View hd_arc_yaz0, std::optional<View> pack_gz, std::optional<Bake> bake,
                              bool same_layout, std::string_view arc_name, std::stop_token stop = {});

// hd2gc.convert_j3d (scale 1, no donors): MAT4 tag -> MAT3 and TEX1 rebuilt with pixels from the .gtx member's
// surfaces (TEX1 entry i <-> surface i). gtx: the .gtx member (nullopt or empty: every texture "NO SOURCE").
Result<Bytes> convert_j3d(View bmd, std::optional<View> gtx, std::vector<std::string>* report = nullptr);
// hd2gc.convert_bti (scale 1): a header-only HD BTI + its .gtx member -> full BTI; unchanged when it already has
// pixels or there is no GTX.
Result<Bytes> convert_bti(View bti, std::optional<View> gtx, std::vector<std::string>* report = nullptr);

struct Mat4Fix {
    Bytes data;
    int fixed = 0;   // MAT blocks repacked 0x14E -> 0x14C
};
Result<Mat4Fix> mat4fix(View bmd);                                // critic/mat4fix.fix_j3d
// matstride.effective_stride(buf, mat_block_offset): 0x14C / 0x14E by content, nullopt = None (inconsistent).
Result<std::optional<u32>> mat_effective_stride(View bmd, u64 mat_block_offset);

struct Baked {
    Bytes data;
    std::array<double, 3> min{}, max{};  // bbox after the bake (report)
    u32 vertices = 0;
};
Result<Baked> bake_los(View bmd, const std::array<float, 3>& t, int angle);  // bake_los.bake_model

// j3d_dlcheck.pc_dl_bytes(d): the whole result dict.
struct DlBytes {
    u32 shapes = 0;      // distinct shapes with at least one packet
    u64 packets = 0, raw = 0, optimized = 0, copy = 0;
};
Result<DlBytes> pc_dl_detail(View bmd);
Result<u64> pc_dl_bytes(View bmd);                                // j3d_dlcheck.pc_dl_bytes(d)['copy']
ModelStats model_stats(View bmd);                                 // memfit.model_stats (zeros on parse errors)
Result<ModelStats> arc_stats(View arc);                           // memfit.arc_stats

// j3d.sections(buf): the section table of a J3D file (tag, absolute offset, size). Stops at the buffer end or at
// a section of size 0 (which is listed). Error when the 16-byte header is short.
struct Section {
    std::array<char, 4> tag{};
    u64 offset = 0;
    u32 size = 0;
};
Result<std::vector<Section>> sections(View j3d);

}  // namespace hdf::j3d
