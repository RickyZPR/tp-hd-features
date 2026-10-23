// Module U (ui) internals: the HD archive + pack pair (stampbook.Source), the SCRNblo2 readers
// (stampbook.Blo for the TEX1 references, hdtext.parse_blo for layout.json) and the text decoders they need.
// Not a public header: only src/ui/* and tests/ui_test.cpp include it.
#pragma once

#include "hdf/core.hpp"
#include "hdf/io.hpp"
#include "hdf/tex.hpp"

#include <array>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdf::ui::detail {

// Error{"fmt.ui", what}
std::unexpected<Error> bad(std::string what);
// Error{"stopped"} when a stop was requested.
inline bool stopped(const std::stop_token& stop) { return stop.stop_requested(); }
std::unexpected<Error> stop_error();

std::string ascii_lower(std::string_view s);
bool ends_with(std::string_view s, std::string_view suffix);

// Python bytes.decode('latin-1') as UTF-8.
std::string latin1_to_utf8(View b);
// Python bytes.decode('cp1252', 'replace') as UTF-8 (0x81 0x8D 0x8F 0x90 0x9D -> U+FFFD).
std::string cp1252_to_utf8(View b);
// Python bytes.decode('shift_jis') (strict) as UTF-8, or nullopt where Python raises UnicodeDecodeError.
// JIS X 0208 pairs come from the Windows cp932 table, minus the cp932-only rows (lead 0x87, 0xED, 0xEE, 0xF0+)
// and with the 6 code points where Python's shift_jis and cp932 differ (VERIFIED against Python on all pairs,
// tests/ui_test.cpp). Without Windows only ASCII and half-width katakana decode (INFERRED unused elsewhere).
std::optional<std::string> shift_jis_to_utf8(View b);
// One decoded pair (lead, trail) as a code point, or 0 if Python's shift_jis rejects it (tests).
u32 shift_jis_pair(u8 lead, u8 trail);

// ---------------------------------------------------------------------------------------------- stampbook.Source
// One HD archive + its TMPK pack (both read only). The pack is read on first use (Python reads it in the
// constructor; the bytes are the same, only the peak memory differs).
class HdArchive {
public:
    // Source(hd, folder, arc_name): folder like "res/LayoutRevo". The pack is <arc_name minus .arc>.pack.gz in the
    // same folder, if the source has it.
    static Result<std::unique_ptr<HdArchive>> open(const Source& hd, std::string_view folder,
                                                   std::string_view arc_name, std::stop_token stop);

    const io::Rarc& arc() const { return arc_; }
    const std::string& rel() const { return rel_; }
    bool has_pack() const { return !pack_path_.empty(); }

    // Source.member(path): Rarc.get(path), and its path must equal `path` (ignoring case). nullptr = KeyError.
    const io::RarcMember* member(std::string_view path) const;
    // Source.gtx(member_path): the first pack entry (dict of lower-case names: a repeated name keeps its first
    // position and takes the last entry) whose name ends with '/' + member_path.lower() + '.gtx'.
    Result<std::optional<View>> gtx(std::string_view member_path, std::stop_token stop) const;
    // Source.texture(member_path): the 32-byte HD stub + surface 0 level 0 of its GTX (tex::bti_job).
    Result<tex::BtiJob> texture(std::string_view member_path, std::stop_token stop) const;
    // cosconfirm._timg(src, name): '<root>/timg/<name.lower()>' if member() finds it.
    std::optional<std::string> timg(std::string_view name) const;
    // Release the pack bytes (they are read again if needed).
    void drop_pack() const;

private:
    HdArchive(const Source& hd) : hd_(hd) {}
    Result<void> load_pack(std::stop_token stop) const;

    const Source& hd_;
    std::string rel_, pack_path_;
    io::Rarc arc_;
    mutable std::optional<io::Tmpk> pack_;
    mutable std::vector<std::pair<std::string, std::size_t>> gtx_;  // lower-case name -> entry index
};

// ---------------------------------------------------------------------------------------------- stampbook.Blo
// stampbook.Blo / cosconfirm.Blo: SCRNblo2 blocks, the TEX1/FNT1 references, the materials and the pane tree with
// J2DPane::makeMatrix placement (_place). Only `tex` reaches an output (cosconfirm's archive member list); the
// rest feeds the manifests and previews, which are on the drop list.
struct ResRef {
    u8 kind = 0;          // J2DResReference type (2 = TIMG lookup in the archive)
    std::string name;     // latin-1 as UTF-8
    std::string raw;      // the name bytes as they are
};

struct BloMaterial {
    std::vector<std::string> textures;              // TEX1 names (raw bytes)
    u32 tevstages = 0;
    std::array<std::optional<std::array<i16, 4>>, 4> tevcolor{};
    std::optional<std::array<u8, 4>> matcolor;
};

struct BloPane {
    std::string type;     // "SCREEN", "PAN2", "PIC2", "TBX2", "WIN2"
    std::string name, user;
    bool visible = true;
    double w = 0, h = 0, sx = 1, sy = 1, rz = 0, x = 0, y = 0;
    std::array<double, 2> anchor{0, 0};
    u16 aid = 0xFFFF;     // cosconfirm.Blo: J2DPane animation index (PAN2 +0xA)
    std::optional<u16> material;
    std::array<std::array<double, 3>, 2> m{};  // _place: absolute 2x3 matrix
    std::vector<std::unique_ptr<BloPane>> children;
};

struct Blo {
    std::string magic;
    std::vector<std::string> blocks;
    std::vector<ResRef> tex, fnt;
    std::vector<BloMaterial> mats;
    std::vector<std::pair<std::string, u32>> counts;  // PAN2/PIC2/TBX2/WIN2 in first-seen order
    std::unique_ptr<BloPane> root;
};
// _resrefs(d, o)
Result<std::vector<ResRef>> blo_resrefs(View d, std::size_t o);
Result<Blo> blo_read(View d);
// _place(p, parent)
void blo_place(BloPane& p, const std::array<std::array<double, 3>, 2>& parent);

// ---------------------------------------------------------------------------------------------- hdtext.parse_blo
// _res_ref(b, blk): TEX1/FNT1 names, path and .bti/.bfn stripped for types 2-4 (latin-1 as UTF-8).
Result<std::vector<std::string>> res_ref(View b, std::size_t blk);
// _name_tab(b, o)
Result<std::vector<std::string>> name_tab(View b, std::size_t o);
struct Mat1Entry {
    std::string name;
    std::vector<std::string> textures;
    std::optional<std::string> mat_color;  // '#rrggbbaa'
    std::string black, white;
};
// _mat1(b, blk, tex_names)
Result<std::vector<Mat1Entry>> mat1(View b, std::size_t blk, const std::vector<std::string>& tex_names);
// parse_blo(b) -> {width, height, textures_used, panes} as the JSON value hdtext writes into layout.json.
struct ParsedBlo {
    io::Json json;                           // the dict, in Python's key order
    std::vector<std::string> textures_used;  // sorted set (code point order)
    u32 panes = 0;
};
Result<ParsedBlo> parse_blo(View b);

// hdtext._tmpk_find(tm, name): first entry whose lower-case name ends with '/' + name.lower().
std::optional<View> tmpk_find(const io::Tmpk& tm, std::string_view name);
// stampbuild._gtx_png / hdtext._gtx_rgba: surface 0, level 0 -> (w, h, RGBA).
struct Rgba {
    u32 w = 0, h = 0;
    Bytes rgba;
};
Result<Rgba> gtx_rgba(View gtx_member);
// stampbuild._tga_rgba(t): uncompressed 32-bit true-colour TGA -> RGBA (the stamp fallback; never used on v81).
Result<Rgba> tga_rgba(View t);

}  // namespace hdf::ui::detail
