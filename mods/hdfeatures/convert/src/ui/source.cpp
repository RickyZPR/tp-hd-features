// Module U: stampbook.Source (one HD archive + its TMPK pack), the shared GTX -> RGBA step and the text decoders.
#include "ui/ui_internal.hpp"

#include <algorithm>
#include <map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hdf::ui::detail {

std::unexpected<Error> bad(std::string what) { return fail("fmt.ui", std::move(what)); }
std::unexpected<Error> stop_error() { return fail("stopped", "ui"); }

std::string ascii_lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

namespace {

void put_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Python's cp1252 codec, bytes 0x80-0x9F (0 = undefined -> U+FFFD with errors='replace').
constexpr u16 kCp1252High[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                 0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

}  // namespace

std::string latin1_to_utf8(View b) {
    std::string out;
    out.reserve(b.size());
    for (u8 c : b) put_utf8(out, c);
    return out;
}

std::string cp1252_to_utf8(View b) {
    std::string out;
    out.reserve(b.size());
    for (u8 c : b) {
        if (c >= 0x80 && c < 0xA0) {
            const u16 cp = kCp1252High[c - 0x80];
            put_utf8(out, cp ? cp : 0xFFFD);
        } else {
            put_utf8(out, c);
        }
    }
    return out;
}

u32 shift_jis_pair(u8 lead, u8 trail) {
    // Python's shift_jis is JIS X 0208 only: leads 0x81-0x9F and 0xE0-0xEF; cp932's NEC row 13 (0x87) and the
    // NEC-selected IBM rows (0xED, 0xEE) are refused, as is anything from 0xF0 (user area, IBM extensions).
    if (!((lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xEF))) return 0;
    if (lead == 0x87 || lead == 0xED || lead == 0xEE) return 0;
    if (trail < 0x40 || trail > 0xFC || trail == 0x7F) return 0;
    // The 6 pairs both codecs decode, but differently (Python shift_jis value first).
    const u16 code = static_cast<u16>((lead << 8) | trail);
    switch (code) {
    case 0x8160: return 0x301C;  // WAVE DASH (cp932: U+FF5E)
    case 0x8161: return 0x2016;  // DOUBLE VERTICAL LINE (cp932: U+2225)
    case 0x817C: return 0x2212;  // MINUS SIGN (cp932: U+FF0D)
    case 0x8191: return 0x00A2;  // CENT SIGN (cp932: U+FFE0)
    case 0x8192: return 0x00A3;  // POUND SIGN (cp932: U+FFE1)
    case 0x81CA: return 0x00AC;  // NOT SIGN (cp932: U+FFE2)
    default: break;
    }
#ifdef _WIN32
    const char in[2] = {static_cast<char>(lead), static_cast<char>(trail)};
    wchar_t out[4] = {};
    const int n = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, in, 2, out, 4);
    if (n != 1) return 0;
    const u32 cp = static_cast<u16>(out[0]);
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    return cp;
#else
    return 0;
#endif
}

std::optional<std::string> shift_jis_to_utf8(View b) {
    std::string out;
    out.reserve(b.size() * 2);
    std::size_t i = 0;
    while (i < b.size()) {
        const u8 c = b[i];
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
        } else if (c >= 0xA1 && c <= 0xDF) {
            put_utf8(out, 0xFF61u + (c - 0xA1u));  // half-width katakana
            ++i;
        } else {
            if (i + 1 >= b.size()) return std::nullopt;  // lead byte at the end
            const u32 cp = shift_jis_pair(c, b[i + 1]);
            if (cp == 0) return std::nullopt;
            put_utf8(out, cp);
            i += 2;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- GTX -> RGBA
// stampbuild._gtx_png / hdtext._gtx_rgba: hdpack.gtx_surfaces(g)['surfaces'][0], deswizzle_level(s, g, 0), to_rgba
Result<Rgba> gtx_rgba(View g) {
    auto surfs = tex::gtx_surfaces(g);
    if (!surfs) return std::unexpected(surfs.error());
    if (surfs->empty()) return bad("GTX has no surface");  // IndexError
    const tex::GtxSurface& s = (*surfs)[0];
    auto li = tex::level_info(s, 0);
    if (!li) return std::unexpected(li.error());
    auto lin = tex::untile(s, 0);
    if (!lin) return std::unexpected(lin.error());
    auto rgba = tex::to_rgba(s, 0, *lin);
    if (!rgba) return std::unexpected(rgba.error());
    return Rgba{li->width, li->height, std::move(*rgba)};
}

std::optional<View> tmpk_find(const io::Tmpk& tm, std::string_view name) {
    const std::string low = "/" + ascii_lower(name);
    for (const io::TmpkEntry& e : tm.entries)
        if (ends_with(ascii_lower(e.name), low)) return tm.data(e);
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------- HdArchive
Result<std::unique_ptr<HdArchive>> HdArchive::open(const Source& hd, std::string_view folder,
                                                    std::string_view arc_name, std::stop_token stop) {
    std::unique_ptr<HdArchive> a(new HdArchive(hd));
    const std::string dir(folder);
    a->rel_ = dir + "/" + std::string(arc_name);
    auto raw = hd.read(a->rel_, stop);
    if (!raw) return std::unexpected(raw.error());  // IOError('HD %s/%s missing')
    auto arc = io::rarc_read(*raw);
    if (!arc) return std::unexpected(arc.error());
    a->arc_ = std::move(*arc);
    if (arc_name.size() >= 4) {
        const std::string pack = dir + "/" + std::string(arc_name.substr(0, arc_name.size() - 4)) + ".pack.gz";
        if (hd.size(pack)) a->pack_path_ = pack;
    }
    return a;
}

Result<void> HdArchive::load_pack(std::stop_token stop) const {
    if (pack_ || pack_path_.empty()) return {};
    auto raw = hd_.read(pack_path_, stop);
    if (!raw) return std::unexpected(raw.error());
    auto t = io::tmpk_from_gz(*raw, stop);
    if (!t) return std::unexpected(t.error());
    pack_ = std::move(*t);
    // self._gtx[e['name'].lower()] = e, in entry order (dict: first position, last value)
    gtx_.clear();
    std::map<std::string, std::size_t> pos;
    for (std::size_t i = 0; i < pack_->entries.size(); ++i) {
        std::string k = ascii_lower(pack_->entries[i].name);
        auto it = pos.find(k);
        if (it != pos.end()) {
            gtx_[it->second].second = i;
        } else {
            pos.emplace(k, gtx_.size());
            gtx_.emplace_back(std::move(k), i);
        }
    }
    return {};
}

void HdArchive::drop_pack() const {
    pack_.reset();
    gtx_.clear();
}

const io::RarcMember* HdArchive::member(std::string_view path) const {
    const io::RarcMember* m = arc_.get(path);
    if (m == nullptr || ascii_lower(m->path) != ascii_lower(path)) return nullptr;
    return m;
}

Result<std::optional<View>> HdArchive::gtx(std::string_view member_path, std::stop_token stop) const {
    if (pack_path_.empty()) return std::optional<View>{};  // self._gtx is empty
    if (auto r = load_pack(stop); !r) return std::unexpected(r.error());
    const std::string suffix = "/" + ascii_lower(member_path) + ".gtx";
    for (const auto& [k, idx] : gtx_)
        if (ends_with(k, suffix)) return std::optional<View>(pack_->data(pack_->entries[idx]));
    return std::optional<View>{};
}

Result<tex::BtiJob> HdArchive::texture(std::string_view member_path, std::stop_token stop) const {
    const io::RarcMember* m = member(member_path);
    if (m == nullptr) return bad(rel_ + ": no member " + std::string(member_path));  // KeyError
    auto stub = arc_.data(*m);
    if (!stub) return std::unexpected(stub.error());
    auto g = gtx(member_path, stop);
    if (!g) return std::unexpected(g.error());
    if (!*g) return bad(rel_ + ": no GTX for " + std::string(member_path));  // KeyError
    return tex::bti_job(*stub, **g);
}

std::optional<std::string> HdArchive::timg(std::string_view name) const {
    const std::string p = std::string(arc_.root_name()) + "/timg/" + ascii_lower(name);
    const io::RarcMember* m = member(p);
    if (m == nullptr) return std::nullopt;
    return m->path;
}

}  // namespace hdf::ui::detail
