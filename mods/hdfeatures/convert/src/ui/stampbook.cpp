// Module U: the HD stamp book (pipeline/stampbook.py build).
//
// res/stamps/stampbook/stampResR_gc.arc  RARC root 'stampresr': HD LayoutRevo/stampResR.arc with every BTI made a
//     full GC BTI from its GTX surface (make_bti), the .tga members dropped, the BLOs as they are, plus
//     timg/tr_a_btn_00.bti and timg/tr_b_btn_00.bti from main2DR; timg first, then scrn (stable sort).
// res/stamps/stampbook/{stampicon,stampbutton,stampbuticon,filesel_stampicon}.bti  (LOOSE)
// book.json (and the BUTTONS_TWIN hash, the previews and the validation) only feed the dropped manifest.
#include "hdf/ui.hpp"

#include "hdf/io.hpp"
#include "hdf/tex.hpp"
#include "ui/ui_internal.hpp"

#include <algorithm>
#include <memory>

namespace hdf::ui {

namespace {

using detail::bad;
using detail::HdArchive;

constexpr const char* kArcName = "stampResR_gc.arc";
constexpr const char* kArcRoot = "stampresr";
constexpr const char* kOutDir = "res/stamps/stampbook/";

struct Loose {
    const char* file;
    const char* folder;
    const char* arc;
    const char* member;
};
// stampbook.LOOSE (out file, HD folder, archive, member)
constexpr Loose kLoose[] = {{"stampicon.bti", "res/LayoutRevo", "clctresR.arc", "clctresr/timg/stampicon.bti"},
                            {"stampbutton.bti", "res/Object", "fileSel.arc", "filesel/timg/stampbutton.bti"},
                            {"stampbuticon.bti", "res/Object", "fileSel.arc", "filesel/timg/stampbuticon.bti"},
                            {"filesel_stampicon.bti", "res/Object", "fileSel.arc", "filesel/timg/stampicon.bti"}};
// stampbook.EXTRA_GLYPHS
constexpr const char* kGlyphs[] = {"main2dr/timg/tr_a_btn_00.bti", "main2dr/timg/tr_b_btn_00.bti"};

// make_bti(src.texture(member)) -> the GC BTI bytes
Result<Bytes> made_bti(const HdArchive& s, std::string_view member, std::stop_token stop) {
    auto t = s.texture(member, stop);
    if (!t) return std::unexpected(t.error());
    auto m = tex::make_bti(*t);
    if (!m) return std::unexpected(m.error());
    return std::move(m->bti);
}

}  // namespace

Result<UiOut> build_stampbook(const Source& hd, std::stop_token stop) {
    if (detail::stopped(stop)) return detail::stop_error();
    UiOut out;
    std::vector<std::pair<std::string, Bytes>> entries;
    {
        auto src = HdArchive::open(hd, "res/LayoutRevo", "stampResR.arc", stop);
        if (!src) return std::unexpected(src.error());
        const io::Rarc& arc = (*src)->arc();
        for (const io::RarcMember& m : arc.members) {
            if (detail::stopped(stop)) return detail::stop_error();
            const auto slash = m.path.find('/');
            if (slash == std::string::npos) return bad("member outside a folder");  // split('/', 1)[1]
            const std::string low = detail::ascii_lower(std::string_view(m.path).substr(slash + 1));
            if (detail::ends_with(low, ".tga")) continue;  // res['dropped']
            if (detail::ends_with(low, ".bti")) {
                auto d = made_bti(**src, m.path, stop);
                if (!d) return std::unexpected(d.error());
                entries.emplace_back(low, std::move(*d));
            } else {  // .blo and anything else: as it is
                auto d = arc.data(m);
                if (!d) return std::unexpected(d.error());
                entries.emplace_back(low, std::move(*d));
            }
        }
    }
    {
        auto s2 = HdArchive::open(hd, "res/LayoutRevo", "main2DR.arc", stop);
        if (!s2) return std::unexpected(s2.error());
        for (const char* member : kGlyphs) {
            if (detail::stopped(stop)) return detail::stop_error();
            auto d = made_bti(**s2, member, stop);
            if (!d) return std::unexpected(d.error());
            // member.split('/')[-1][:-4].lower()
            std::string_view mv(member);
            const std::string name = detail::ascii_lower(mv.substr(mv.rfind('/') + 1, mv.size() - mv.rfind('/') - 5));
            entries.emplace_back("timg/" + name + ".bti", std::move(*d));
        }
    }
    // timg first, then scrn: same node order as HD's stampResR (ROOT, TIMG, SCRN)
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return (a.first.starts_with("timg/") ? 0 : 1) < (b.first.starts_with("timg/") ? 0 : 1);
    });
    out.files.emplace_back(std::string(kOutDir) + kArcName, io::rarc_build(kArcRoot, entries));
    entries.clear();
    entries.shrink_to_fit();
    // loose BTIs (Source.get caches by folder + archive: fileSel.arc serves three)
    std::unique_ptr<HdArchive> cur;
    std::string cur_key;
    for (const Loose& l : kLoose) {
        if (detail::stopped(stop)) return detail::stop_error();
        const std::string key = std::string(l.folder) + "/" + detail::ascii_lower(l.arc);
        if (key != cur_key) {
            cur.reset();
            auto s2 = HdArchive::open(hd, l.folder, l.arc, stop);
            if (!s2) return std::unexpected(s2.error());
            cur = std::move(*s2);
            cur_key = key;
        }
        auto d = made_bti(*cur, l.member, stop);
        if (!d) return std::unexpected(d.error());
        out.files.emplace_back(std::string(kOutDir) + l.file, std::move(*d));
    }
    return out;
}

}  // namespace hdf::ui
