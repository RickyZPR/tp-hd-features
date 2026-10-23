// Module U: the stamp images (pipeline/stampbuild.py build, part 2) and their message-box icons
// (gen_stamp_table.build_icons).
//
// res/stamps/stamps/stampNN.png    HD LayoutRevo/stampResR.pack.gz timg/stampN.bti.gtx (fallback: timg/stampN.tga
//                                  from stampResR.arc; never needed on TP HD v81: all 50 have a GTX, VERIFIED)
// res/stamps/stamps/stamp_frame.png, stamp_empty.png (and the dropped stamp_background.png): FRAMES
// res/stamps/stamp_icons/stampNN.bti  48 x 48 CMPR from the stamp's RGBA (build_icons decodes the PNG it just
//                                  wrote; the PNG holds the same RGBA, so the bytes are the same)
// Obj_Stamp.arc (part 1) belongs to module CV; stamps.json and _stamp_rows only feed the dropped manifest;
// hd_chests.bin belongs to module ST.
#include "hdf/ui.hpp"

#include "hdf/io.hpp"
#include "hdf/tex.hpp"
#include "ui/ui_internal.hpp"

#include <cstdio>

namespace hdf::ui {

namespace {

using detail::bad;

// stampbuild.FRAMES (dict order): out file -> HD texture
struct Frame {
    const char* file;
    const char* src;
    bool kept;
};
constexpr Frame kFrames[] = {{"stamp_frame.png", "box", true},
                             {"stamp_empty.png", "icon_empty", true},
                             {"stamp_background.png", "paper", false}};  // drop list

// stampbuild._gtx_png(tm, name): tm.get(name + '.bti.gtx') (hdpack.Tmpk.get: exact, '/' suffix, basename)
Result<std::optional<detail::Rgba>> gtx_png(const io::Tmpk& tm, const std::string& name) {
    auto g = tm.get(name + ".bti.gtx");
    if (!g) return std::optional<detail::Rgba>{};
    auto r = detail::gtx_rgba(*g);
    if (!r) return std::unexpected(r.error());
    return std::optional<detail::Rgba>(std::move(*r));
}

}  // namespace

namespace detail {

// stampbuild._tga_rgba(t): uncompressed 32-bit true-colour TGA -> RGBA (bottom-up unless descriptor bit 5)
Result<Rgba> tga_rgba(View t) {
    if (t.size() < 18) return bad("TGA header cut");
    const u32 idl = t[0], cmt = t[1], it = t[2];
    const u32 w = static_cast<u32>(t[12] | (t[13] << 8)), h = static_cast<u32>(t[14] | (t[15] << 8));
    const u32 bpp = t[16], desc = t[17];
    if (it != 2 || bpp != 32 || cmt) return bad("unsupported TGA type");
    const std::size_t px0 = 18 + std::size_t{idl}, n = std::size_t{w} * h * 4;
    if (px0 > t.size() || t.size() - px0 < n) return bad("TGA pixels cut");  // row[o + k] IndexError
    Rgba out{w, h, Bytes(n)};
    for (u32 y = 0; y < h; ++y) {
        const u32 sy = (desc & 0x20) ? y : (h - 1 - y);
        const std::size_t row = px0 + std::size_t{sy} * w * 4;
        for (u32 x = 0; x < w; ++x) {
            const std::size_t o = row + std::size_t{x} * 4, d = (std::size_t{y} * w + x) * 4;
            out.rgba[d] = t[o + 2];
            out.rgba[d + 1] = t[o + 1];
            out.rgba[d + 2] = t[o];
            out.rgba[d + 3] = t[o + 3];
        }
    }
    return out;
}

}  // namespace detail

Result<UiOut> build_stamps(const Source& hd, std::stop_token stop) { return build_stamps(hd, stop, Options{}); }

Result<UiOut> build_stamps(const Source& hd, std::stop_token stop, const Options& opt) {
    if (detail::stopped(stop)) return detail::stop_error();
    UiOut out;
    // tm = hdpack.Tmpk(hdpack.read_any(LayoutRevo/stampResR.pack.gz)); arc = LayoutRevo/stampResR.arc
    auto raw = hd.read("res/LayoutRevo/stampResR.pack.gz", stop);
    if (!raw) return std::unexpected(raw.error());
    auto tm = io::tmpk_from_gz(*raw, stop);
    if (!tm) return std::unexpected(tm.error());
    raw->clear();
    raw->shrink_to_fit();
    std::optional<io::Rarc> arc;  // read when a TGA fallback is needed (Python reads it up front)
    for (int i = 0; i < 50; ++i) {
        if (detail::stopped(stop)) return detail::stop_error();
        char name[32];
        std::snprintf(name, sizeof name, "stamp%d", i);
        auto r = gtx_png(*tm, name);
        if (!r) return std::unexpected(r.error());
        if (!*r) {
            if (!arc) {
                auto a = hd.read("res/LayoutRevo/stampResR.arc", stop);
                if (!a) return std::unexpected(a.error());
                auto ra = io::rarc_read(*a);
                if (!ra) return std::unexpected(ra.error());
                arc = std::move(*ra);
            }
            std::snprintf(name, sizeof name, "timg/stamp%d.tga", i);
            const io::RarcMember* m = arc->get(name);
            if (m == nullptr) return bad(std::string("no ") + name);  // None.data() AttributeError
            auto d = arc->data(*m);
            if (!d) return std::unexpected(d.error());
            auto t = detail::tga_rgba(*d);
            if (!t) return std::unexpected(t.error());
            *r = std::move(*t);
        }
        detail::Rgba& img = **r;
        // gen_stamp_table.build_icons: make_icon_rgba + bti_cmpr(icon, 48), assert len(bti) <= 0xC00
        Bytes icon = tex::stamp_icon_bti(img.rgba, img.w, img.h);
        if (icon.empty() || icon.size() > 0xC00) return bad("stamp icon " + std::to_string(i));
        char rel[64];
        std::snprintf(rel, sizeof rel, "res/stamps/stamp_icons/stamp%02d.bti", i);
        out.files.emplace_back(rel, std::move(icon));
        std::snprintf(rel, sizeof rel, "res/stamps/stamps/stamp%02d.png", i);
        out.pngs.push_back(Png{rel, img.w, img.h, std::move(img.rgba)});
    }
    for (const Frame& f : kFrames) {
        if (!f.kept && !opt.dropped) continue;
        if (detail::stopped(stop)) return detail::stop_error();
        auto r = gtx_png(*tm, f.src);
        if (!r) return std::unexpected(r.error());
        if (!*r) continue;  // a missing frame is skipped
        Png p{std::string("res/stamps/stamps/") + f.file, (*r)->w, (*r)->h, std::move((*r)->rgba)};
        (f.kept ? out.pngs : out.dropped).push_back(std::move(p));
    }
    return out;
}

}  // namespace hdf::ui
