// Module U: the Cave of Shadows pre-entry / record screens (pipeline/cosconfirm.py build).
//
// res/cave/confirm/cosconfirm_gc.arc  RARC root 'cosconfirm', nodes ROOT/TIMG/SCRN:
//     scrn/<the 4 LAYOUTS>.blo and scrn/zelda_file_select2.bck from HD Layout/saveres.arc as they are;
//     timg/<every TEX1 texture of the 4 BLOs> (sorted by lower-case name) as full GC BTIs (make_bti), from saveres
//     or else, in TEX_FALLBACK order, main2DR / clctresR / stampResR (the first archive with timg/<name>);
//     timg/cos_heart_00..03.bti from saveres tt_heart_00..03; sorted by (timg first, name).
// confirm.json (sources, also_in, drawn textures, messages, frames, panes) is on the drop list: the also_in
// textures, _hd_messages, Bck and drawn_textures only feed it, so they are not run.
#include "hdf/ui.hpp"

#include "hdf/io.hpp"
#include "hdf/tex.hpp"
#include "ui/ui_internal.hpp"

#include <algorithm>
#include <memory>
#include <set>

namespace hdf::ui {

namespace {

using detail::bad;
using detail::HdArchive;

constexpr const char* kArcName = "res/cave/confirm/cosconfirm_gc.arc";
constexpr const char* kArcRoot = "cosconfirm";
// LAYOUTS (FS2, INFO, ICONS, RECORD) and ANIMS (BCK)
constexpr const char* kLayouts[] = {"zelda_file_select2.blo", "zelda_file_select_info_amiibo.blo",
                                    "zelda_file_select_icon_parts_revo.blo", "zelda_file_select_los_record.blo"};
constexpr const char* kBck = "zelda_file_select2.bck";
// TEX_FALLBACK
constexpr const char* kFallback[] = {"main2DR.arc", "clctresR.arc", "stampResR.arc"};
// CODE_TEXTURES (archive name, HD saveres member)
constexpr std::pair<const char*, const char*> kCode[] = {{"cos_heart_00.bti", "tt_heart_00.bti"},
                                                         {"cos_heart_01.bti", "tt_heart_01.bti"},
                                                         {"cos_heart_02.bti", "tt_heart_02.bti"},
                                                         {"cos_heart_03.bti", "tt_heart_03.bti"}};

Result<Bytes> made_bti(const HdArchive& s, std::string_view member, std::stop_token stop) {
    auto t = s.texture(member, stop);
    if (!t) return std::unexpected(t.error());
    auto m = tex::make_bti(*t);
    if (!m) return std::unexpected(m.error());
    // the pack stays loaded: the next texture is likely in the same archive
    return std::move(m->bti);
}

}  // namespace

Result<UiOut> build_confirm(const Source& hd, std::stop_token stop) {
    if (detail::stopped(stop)) return detail::stop_error();
    UiOut out;
    auto sv = HdArchive::open(hd, "res/Layout", "saveres.arc", stop);
    if (!sv) return std::unexpected(sv.error());
    std::vector<std::unique_ptr<HdArchive>> falls;  // the archives only; each pack is read when first needed
    for (const char* a : kFallback) {
        auto s = HdArchive::open(hd, "res/LayoutRevo", a, stop);
        if (!s) return std::unexpected(s.error());
        falls.push_back(std::move(*s));
    }
    std::vector<std::pair<std::string, Bytes>> entries;
    std::set<std::string> refs;  // TEX1 names, lower-cased like JKR's name lookup (a dict, then sorted())
    for (const char* name : kLayouts) {
        const io::RarcMember* m = (*sv)->member(std::string("saveres/scrn/") + name);
        if (m == nullptr) return bad(std::string("saveres: no member ") + name);  // KeyError
        auto d = (*sv)->arc().data(*m);
        if (!d) return std::unexpected(d.error());
        auto blo = detail::blo_read(*d);  // Blo(d): only .tex is used here
        if (!blo) return std::unexpected(blo.error());
        for (const detail::ResRef& r : blo->tex) {
            for (char c : r.raw)  // rarc_write encodes member names as Shift-JIS: only ASCII round-trips
                if (static_cast<unsigned char>(c) >= 0x80) return bad("non-ASCII TEX1 name");
            refs.insert(detail::ascii_lower(r.raw));
        }
        entries.emplace_back(std::string("scrn/") + name, std::move(*d));
    }
    {
        const io::RarcMember* m = (*sv)->member(std::string("saveres/scrn/") + kBck);
        if (m == nullptr) return bad(std::string("saveres: no member ") + kBck);
        auto d = (*sv)->arc().data(*m);
        if (!d) return std::unexpected(d.error());
        entries.emplace_back(std::string("scrn/") + kBck, std::move(*d));
    }
    for (const std::string& tn : refs) {  // sorted(refs.items()): byte order = code point order (ASCII)
        if (detail::stopped(stop)) return detail::stop_error();
        const HdArchive* s0 = nullptr;
        std::optional<std::string> path;
        for (const HdArchive* s : {sv->get(), falls[0].get(), falls[1].get(), falls[2].get()}) {
            if (auto p = s->timg(tn)) {
                s0 = s;
                path = std::move(p);
                break;
            }
        }
        if (s0 == nullptr) return bad("no HD source for TEX1 texture " + tn);  // KeyError
        auto d = made_bti(*s0, *path, stop);
        if (!d) return std::unexpected(d.error());
        entries.emplace_back("timg/" + tn, std::move(*d));
    }
    for (const auto& [an, hn] : kCode) {
        if (detail::stopped(stop)) return detail::stop_error();
        auto p = (*sv)->timg(hn);
        if (!p) return bad(std::string("saveres: no ") + hn);  // sv.texture(None) fails
        auto d = made_bti(**sv, *p, stop);
        if (!d) return std::unexpected(d.error());
        entries.emplace_back(std::string("timg/") + an, std::move(*d));
    }
    falls.clear();
    sv->reset();
    // timg first, then scrn (node order ROOT, TIMG, SCRN), by name
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        const int ga = a.first.starts_with("timg/") ? 0 : 1, gb = b.first.starts_with("timg/") ? 0 : 1;
        if (ga != gb) return ga < gb;
        return a.first < b.first;
    });
    out.files.emplace_back(kArcName, io::rarc_build(kArcRoot, entries));
    return out;
}

}  // namespace hdf::ui
