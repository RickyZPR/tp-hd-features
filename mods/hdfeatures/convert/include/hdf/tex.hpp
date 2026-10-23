// hdf_convert: GX2 surfaces, untile, GX texture codecs, BTI headers (module T "tex"; src/tex/*).
//
// Ports: formats/hdpack.py (gtx_surfaces, gmx_sections), formats/gtx2img.py (addr_*, deswizzle_level,
// level_info, to_rgba, _rgb565), formats/gxtex.py (bc1_to_cmpr, encode, _tiles, _px, _lum, box_downscale),
// pipeline/stampbook.py encoder half (_nearest, tex_bytes, _tile, encode, decode, max_err, tex_error,
// choose_format, bti_fields, make_bti), gen_stamp_table.py (make_icon_rgba, _to565, _from565, bc1_encode,
// bti_cmpr). Scope (libs 6): tile modes 2 and 4, pipe/bank swizzle 0, formats RGBA8 BC1 R8 R4G4 R8G8 R5G6B5;
// anything else returns Error{"fmt.tex"}. The two GX encoder families differ on purpose: keep both.
//
// Wave 2 (module T) additions, all additive: GtxSurface::gtx/image_off/mip_off/has_image/has_mip/image_block/
// mip_block, gx2_bpp, gx2_is_bcn, gx2_format_name, deswizzle, decode_model, the hd2gc texture part
// (pick_level, convert_texture, patch_header, gx_tex_size, convert_bti; module J calls these) and bti_job
// (stampbook.Source.texture after the member/pack lookup; module U calls it). LevelInfo::offset is absolute in
// GtxSurface::gtx, as gtx2img.level_info's data_off is in the .gtx buffer.
#pragma once

#include "hdf/core.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::tex {

// GX2 surface formats used by TP HD (hdpack.GX2_FMT subset).
inline constexpr u32 kGx2R8 = 0x01, kGx2R4G4 = 0x02, kGx2R8G8 = 0x07, kGx2R5G6B5 = 0x08, kGx2Rgba8 = 0x1A,
                     kGx2Rgba8Srgb = 0x41A, kGx2Bc1 = 0x31, kGx2Bc1Srgb = 0x431;

// One GX2 surface of a Gfx2 (.gtx) member. All views point into the member's bytes: keep them alive.
struct GtxSurface {
    u32 dim = 0, width = 0, height = 0, depth = 0, mips = 0, format = 0, aa = 0, use = 0, tile_mode = 0,
        swizzle = 0, alignment = 0, pitch = 0;
    u32 image_size = 0, mip_size = 0;        // GX2Surface imageSize / mipSize (hdpack 'imageSize'/'mipSize')
    std::array<u8, 4> comp_sel{0, 1, 2, 3};  // GX2Texture.compSel at +0x84 (R,G,B,A sources: 0-3 RGBA, 4 = 0, 5 = 1)
    std::array<u32, 13> mip_offsets{};
    View image, mip_data;                    // the image / mip blocks (block size, clipped to the member)
    // added in wave 2 (T):
    View gtx;                                // the whole Gfx2 member (gtx2img slices levels from it)
    u64 image_off = 0, mip_off = 0;          // hdpack 'img_off'/'mip_off': block data offsets in gtx
    bool has_image = false, has_mip = false; // False = Python None (no such block)
    u32 image_block = 0, mip_block = 0;      // hdpack 'img'/'mip': block data sizes as the block headers say
    bool has_comp_sel = false;               // the surface block was >= 0x88 bytes (else compSel "" = identity)
};

struct Gtx {
    u32 major = 0, minor = 0, gpu = 0;
    std::vector<GtxSurface> surfaces;
    u32 blocks = 0;
};

Result<Gtx> gtx_read(View gtx_member);                          // hdpack.gtx_surfaces (whole result)
Result<std::vector<GtxSurface>> gtx_surfaces(View pack_member);  // plan 3.3: the surfaces only

struct GmxSection {
    std::string tag;
    u32 offset = 0, size = 0;
};
Result<std::vector<GmxSection>> gmx_sections(View gmx);         // hdpack.gmx_sections

u32 gx2_bpp(u32 format);                                         // gtx2img.FMT_BPP[fmt & 0xFFFF], 0 if unknown
bool gx2_is_bcn(u32 format);                                     // fmt & 0xFFFF in gtx2img.BCN
std::string gx2_format_name(u32 format);                         // hdpack.GX2_FMT.get(fmt, "0x%x" % fmt)

struct LevelInfo {                                               // gtx2img.level_info
    u32 width = 0, height = 0, tile_mode = 0, pitch = 0;
    u64 offset = 0, size = 0;                                    // absolute in GtxSurface::gtx (data_off/size)
};
Result<LevelInfo> level_info(const GtxSurface& s, int level);
// gtx2img.deswizzle: format/w/h in pixels (BCn: elements are 4x4 blocks), pitch in elements. Python slice
// semantics are kept for addresses past the end of `data` (they never happen on TP HD, see the T report).
Result<Bytes> deswizzle(u32 format, u32 width, u32 height, u32 pitch, u32 tile_mode, u32 swizzle, View data);
Result<Bytes> untile(const GtxSurface& s, int level);            // gtx2img.deswizzle_level: linear elements
Result<Bytes> to_rgba(const GtxSurface& s, int level, View linear);  // RGBA8 + compSel applied

// GX texture formats (GXTexFmt).
enum class Gx : u8 { I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA8 = 6, C4 = 8, C8 = 9,
                     C14X2 = 10, CMPR = 14 };

u64 tex_bytes(Gx f, u32 w, u32 h);                               // stampbook.tex_bytes (0 for C4/C8/C14X2)
Bytes bc1_to_cmpr(View bc1_linear, u32 w, u32 h);                // gxtex.bc1_to_cmpr (lossless reorder)
Bytes encode_model(Gx f, View rgba, u32 w, u32 h);               // gxtex.encode (truncating; hd2gc, particles);
                                                                 // empty for CMPR/C4/C8/C14X2 (Python raises)
Bytes decode_model(Gx f, View data, u32 w, u32 h);               // gxtex.decode (incl. cmpr_to_rgba); tests
Bytes box_downscale(View rgba, u32 w, u32 h, u32 nw, u32 nh);    // gxtex.box_downscale (empty on bad sizes)
Bytes encode_ui(Gx f, View rgba, u32 w, u32 h);                  // stampbook.encode (Aurora ExpandTo8 nearest)
Bytes decode_ui(Gx f, View data, u32 w, u32 h);                  // stampbook.decode -> RGBA8
struct TexError { int visible = 0, transparent = 0; };
TexError tex_error(View back, View src);                         // stampbook.tex_error
struct FormatChoice { Gx format = Gx::RGBA8; std::string why; };
FormatChoice choose_ui_format(View rgba, u8 hd_format);          // stampbook.choose_format (hd_format = BTI fmt)

// BTI header (ResTIMG, 0x20 bytes) as stampbook.bti_fields reads it.
struct BtiFields {
    u8 format = 0, alpha = 0;
    u16 width = 0, height = 0;
    u8 wrap_s = 0, wrap_t = 0, palette_enabled = 0, palette_format = 0;
    u16 palette_count = 0;
    u32 palette_offset = 0;
    u8 mipmap_enabled = 0, edge_lod = 0, bias_clamp = 0, max_aniso = 0, min_filter = 0, mag_filter = 0;
    i8 min_lod = 0, max_lod = 0;
    u8 mip_count = 0, unk19 = 0;
    i16 lod_bias = 0;
    u32 image_offset = 0;
};
Result<BtiFields> bti_fields(View d, std::size_t offset = 0);

// stampbook.make_bti input `t`: the HD BTI stub (header) + the RGBA to store.
struct BtiJob {
    std::array<u8, 0x20> stub{};  // the HD header bytes
    BtiFields hdr;                // bti_fields(stub)
    Bytes rgba;
    u32 w = 0, h = 0;
    u32 gtx_format = 0;           // the GX2 format the RGBA came from (report only)
};
struct BtiMade {
    Bytes bti;                    // full GC BTI: 0x20 header + texels
    Gx format = Gx::RGBA8;
    std::string reason;
    TexError error;
    Bytes decoded;                // decode_ui(format, texels): what the game will show
};
Result<BtiMade> make_bti(const BtiJob& t);                        // stampbook.make_bti
// stampbook.Source.texture after the lookup: stub = the HD member (>= 0x20 bytes), gtx = its .gtx pack member.
// Surface 0, level 0, to_rgba. (Module U does the member and pack lookup.)
Result<BtiJob> bti_job(View stub, View gtx_member);

// gen_stamp_table: icon RGBA on the paper card, small BC1 encoder, BC1 -> CMPR, BTI header.
Bytes make_icon_rgba(View stamp_rgba, u32 w, u32 h);             // gen_stamp_table.make_icon_rgba (48 x 48)
Bytes bc1_encode(View rgba, u32 w, u32 h);                       // gen_stamp_table.bc1_encode
// gen_stamp_table.bti_cmpr(rgba, size): `cmpr` is the icon RGBA (w x h, w = h = size), not CMPR texels; the
// result is the 0x20 header + bc1_to_cmpr(bc1_encode(rgba)).
Bytes bti_cmpr(View cmpr, u32 w, u32 h);
Bytes stamp_icon_bti(View stamp_rgba, u32 w, u32 h);             // make_icon_rgba + bc1_encode + bti_cmpr

// ---------------------------------------------------------------------------------------------- hd2gc part
// (added in wave 2, T) formats/hd2gc.py texture functions, shared by j3d (TEX1, BTI members) and cave (particles).
int pick_level(const GtxSurface& s, u32 tw, u32 th);             // hd2gc.pick_level
struct TexConv {                                                 // hd2gc.convert_texture result
    u8 format = 0;                                               // out_fmt (GX)
    u32 width = 0, height = 0;                                   // w0, h0
    u32 levels = 0;                                              // len(levels)
    int level0 = 0;                                              // L0 (for the report line)
    Bytes pixels;                                                // b"".join(levels)
};
// hdr: the GC-scale header (j3d.bti_header: format, width, height, mip_count are used).
Result<TexConv> convert_texture(const BtiFields& hdr, const GtxSurface& surf, u32 scale = 1);
// hd2gc.patch_header(raw_hdr, fmt, w, h, nmips, img_off): raw_hdr = the 0x20 header bytes.
Bytes patch_header(View raw_hdr, u8 fmt, u32 w, u32 h, u32 nmips, u32 img_off);
u64 gx_tex_size(u8 fmt, u32 w, u32 h, u32 mips = 1);             // j3d.tex_size
// hd2gc.convert_bti(d, gtx_buf, scale, ...): a header-only HD BTI + its .gtx member -> full BTI; d unchanged
// when it already has pixels or there is no GTX.
Result<Bytes> convert_bti(View d, std::optional<View> gtx_member, u32 scale = 1);

}  // namespace hdf::tex
