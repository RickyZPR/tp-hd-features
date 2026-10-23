// `hdf-convert test tex`: the oracle test of module T (plan 3.3).
//
//   hdf-convert test tex --oracle <hd_features-oracle-2.0.0.dusk> --hd <TP HD title/content folder or .wua>
//                        [--golden <tex.tsv>] [--verbose]
//
//   (a) detile: every GX2 surface of the 62 packed HD inputs (61 .pack.gz + Pscene206.jpc.gtx.gz), every mip
//       level: surface fields, level geometry, SHA-256 of untile (gtx2img.deswizzle_level) and of to_rgba equal
//       the golden list; and untile equals the CC0 tphd AddrLib on the same bytes (src/tex/addrlib_ref.inc, built
//       into hdf-convert by tests/tex_test.cpp).
//       The tphd mip geometry (computeSurfaceInfo) is compared too, for information only (Python is the spec).
//   (b) model textures: hd2gc.convert_texture for every TEX1 entry of the 55 converted models (631) = the
//       oracle's TEX1 header and texels; (c) the 5 stage BTI members (hd2gc.convert_bti) = the oracle members;
//   (d) the 27 HD particle textures of Pscene206.jpc (7 at half size) = the oracle's TEX1 blocks;
//   (e) the 126 UI-archive BTIs (74 stamp book + 52 pre-entry) and the 4 loose BTIs (stampbook.make_bti) = the
//       oracle; (f) the 50 stamp icons (gen_stamp_table) from the oracle's stampNN.png = the oracle's stampNN.bti.
//
// The golden list (tools/dev/golden_tex.py, default <exe folder>/golden/tex.tsv) maps every job to its HD source
// and its oracle location by index (no names from inside the files). Its rows are counted by kind and must equal
// its "# counts" line and the fixed totals above (wave 4, T L3: a cut list fails).
// Exit 0 pass, 1 fail, 2 bad arguments or an
// oracle that is not the pinned one, 3 skipped (no --hd or no golden list).
#include "../cli.hpp"

#include "../../src/tex/addrlib_ref.hpp"
#include "hdf/io.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"
#include "hdf/tex.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using hdf::Bytes;
using hdf::u16;
using hdf::u32;
using hdf::u64;
using hdf::u8;
using hdf::View;
namespace io = hdf::io;
namespace tex = hdf::tex;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double secs(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

using Row = std::vector<std::string>;

long long num(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

std::string hex8(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x", (v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    return b;
}

// first differing offset of two byte strings, or -1
long long first_diff(View a, View b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return static_cast<long long>(i);
    return a.size() == b.size() ? -1 : static_cast<long long>(n);
}

// "= the oracle, golden hash differs" or "differs from the oracle at byte N"
std::string diff_text(long long at) {
    if (at == -1) return "= the oracle, but the golden list differs";
    if (at == -2) return "failed";
    return "differs from the oracle at byte " + std::to_string(at);
}

struct Counter {
    const char* what;
    u32 ok = 0, bad = 0, shown = 0;
    bool verbose = false;
    void pass() { ++ok; }
    template <class... A>
    void failf(const char* fmt, A... a) {
        ++bad;
        if (shown < 12 || verbose) {
            std::printf("  %s FAIL: ", what);
            std::printf(fmt, a...);
            std::printf("\n");
        }
        ++shown;
    }
    void line(double t) const {
        std::printf("(%s) %u/%u equal%s (%.2f s)\n", what, ok, ok + bad, bad ? " -- FAIL" : "", t);
    }
};

// ---------------------------------------------------------------------------------------------- inputs (cached)
class Inputs {
public:
    Inputs(const hdf::Source& hd, const hdf::ZipSource& oracle) : hd_(hd), oracle_(oracle) {}

    // an HD file, inflated if it is gzip (hdpack.read_any)
    hdf::Result<const Bytes*> hd_any(const std::string& path) {
        auto it = any_.find(path);
        if (it != any_.end()) return &it->second;
        auto raw = hd_.read(path);
        if (!raw) return std::unexpected(raw.error());
        auto d = io::read_any(*raw);
        if (!d) return std::unexpected(d.error());
        return &any_.emplace(path, std::move(*d)).first->second;
    }
    hdf::Result<const io::Rarc*> hd_arc(const std::string& path) { return arc(path, true); }
    hdf::Result<const io::Rarc*> oracle_arc(const std::string& path) { return arc(path, false); }
    hdf::Result<const io::Tmpk*> pack(const std::string& path) {
        auto it = packs_.find(path);
        if (it != packs_.end()) return &it->second;
        auto raw = hd_.read(path);
        if (!raw) return std::unexpected(raw.error());
        auto t = io::tmpk_from_gz(*raw);
        if (!t) return std::unexpected(t.error());
        return &packs_.emplace(path, std::move(*t)).first->second;
    }
    hdf::Result<const Bytes*> oracle_file(const std::string& path) {
        auto it = ofiles_.find(path);
        if (it != ofiles_.end()) return &it->second;
        auto d = oracle_.read(path);
        if (!d) return std::unexpected(d.error());
        return &ofiles_.emplace(path, std::move(*d)).first->second;
    }
    void drop_hd() {
        any_.clear();
        packs_.clear();
    }

private:
    hdf::Result<const io::Rarc*> arc(const std::string& path, bool hd) {
        auto& cache = hd ? hd_arcs_ : oracle_arcs_;
        auto it = cache.find(path);
        if (it != cache.end()) return &it->second;
        auto raw = hd ? hd_.read(path) : oracle_.read(path);
        if (!raw) return std::unexpected(raw.error());
        auto a = io::rarc_read(*raw);
        if (!a) return std::unexpected(a.error());
        return &cache.emplace(path, std::move(*a)).first->second;
    }

    const hdf::Source& hd_;
    const hdf::ZipSource& oracle_;
    std::map<std::string, Bytes> any_, ofiles_;
    std::map<std::string, io::Tmpk> packs_;
    std::map<std::string, io::Rarc> hd_arcs_, oracle_arcs_;
};

hdf::Result<Bytes> member_data(const io::Rarc& a, long long idx) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= a.members.size()) return hdf::fail("test.tex", "member index");
    return a.data(a.members[static_cast<std::size_t>(idx)]);
}

hdf::Result<View> pack_member(const io::Tmpk& t, long long idx) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= t.entries.size()) return hdf::fail("test.tex", "pack index");
    return t.data(t.entries[static_cast<std::size_t>(idx)]);
}

// j3d.sections: the offset of the first TEX1 section, or 0
std::size_t tex1_offset(View d) {
    if (d.size() < 0x20) return 0;
    const u32 nsec = io::be32(d, 12);
    std::size_t o = 0x20;
    for (u32 k = 0; k < nsec; ++k) {
        if (o + 8 > d.size()) break;
        const u32 ssz = io::be32(d, o + 4);
        if (d[o] == 'T' && d[o + 1] == 'E' && d[o + 2] == 'X' && d[o + 3] == '1') return o;
        if (ssz == 0) break;
        o += ssz;
    }
    return 0;
}

// particles.parse: the TEX1 blocks of a JPAC2-10 file
std::vector<View> jpc_textures(View d) {
    std::vector<View> out;
    if (d.size() < 0x10) return out;
    const u32 ntex = io::be16(d, 10), texoff = io::be32(d, 12);
    std::size_t o = texoff;
    for (u32 i = 0; i < ntex; ++i) {
        if (o + 8 > d.size()) break;
        const u32 sz = io::be32(d, o + 4);
        if (sz < 8 || o + sz > d.size()) break;
        out.push_back(d.subspan(o, sz));
        o += sz;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- (a) detile
bool part_detile(Inputs& in, const std::vector<Row>& rows, bool verbose, u32& n_surf) {
    const auto t0 = Clock::now();
    Counter surf{"a: surfaces"}, lvl{"a: untile"}, rgba{"a: to_rgba"}, ref{"a: tphd AddrLib"};
    surf.verbose = lvl.verbose = rgba.verbose = ref.verbose = verbose;
    u32 geo_same = 0, geo_total = 0, geo_shown = 0;
    u64 elements = 0;
    double t_untile = 0;
    // rows grouped by (path, tmpk idx) in file order
    std::string cur_path;
    long long cur_idx = -2;
    std::vector<tex::GtxSurface> surfaces;
    Bytes owned;  // a bare Gfx2 file
    View member;
    bool member_ok = false;
    for (const Row& r : rows) {
        if (r[0] != "surf" && r[0] != "lvl" && r[0] != "lvlerr") continue;
        const std::string& path = r[1];
        const long long idx = num(r[2]);
        if (path != cur_path || idx != cur_idx) {
            if (path != cur_path) in.drop_hd();
            cur_path = path;
            cur_idx = idx;
            member_ok = false;
            surfaces.clear();
            if (idx < 0) {
                auto d = in.hd_any(path);
                if (!d) {
                    surf.failf("%s: %s", path.c_str(), d.error().detail.c_str());
                    continue;
                }
                member = **d;
            } else {
                auto t = in.pack(path);
                if (!t) {
                    surf.failf("%s: %s", path.c_str(), t.error().detail.c_str());
                    continue;
                }
                auto m = pack_member(**t, idx);
                if (!m) {
                    surf.failf("%s #%lld: no such pack member", path.c_str(), idx);
                    continue;
                }
                member = *m;
            }
            auto s = tex::gtx_surfaces(member);
            if (!s) {
                surf.failf("%s #%lld: %s", path.c_str(), idx, s.error().detail.c_str());
                continue;
            }
            surfaces = std::move(*s);
            member_ok = true;
        }
        if (!member_ok) continue;
        const long long si = num(r[3]);
        if (si < 0 || static_cast<std::size_t>(si) >= surfaces.size()) {
            surf.failf("%s #%lld surface %lld missing", path.c_str(), idx, si);
            continue;
        }
        const tex::GtxSurface& s = surfaces[static_cast<std::size_t>(si)];
        if (r[0] == "surf") {
            ++n_surf;
            const std::string comp = s.has_comp_sel ? hex8((u32{s.comp_sel[0]} << 24) | (u32{s.comp_sel[1]} << 16) |
                                                           (u32{s.comp_sel[2]} << 8) | s.comp_sel[3])
                                                    : "-";
            const bool same = num(r[4]) == s.format && num(r[5]) == s.width && num(r[6]) == s.height &&
                              num(r[7]) == s.mips && num(r[8]) == s.tile_mode && num(r[9]) == s.swizzle &&
                              num(r[10]) == s.pitch && r[11] == comp && num(r[12]) == s.image_block &&
                              num(r[13]) == s.mip_block;
            if (same)
                surf.pass();
            else
                surf.failf("%s #%lld surface %lld: fields differ", path.c_str(), idx, si);
            continue;
        }
        const int L = static_cast<int>(num(r[4]));
        if (r[0] == "lvlerr") {
            auto li = tex::untile(s, L);
            if (!li)
                lvl.pass();
            else
                lvl.failf("%s #%lld s%lld L%d: Python raised %s, C++ did not", path.c_str(), idx, si, L, r[5].c_str());
            continue;
        }
        auto li = tex::level_info(s, L);
        const auto tu = Clock::now();
        auto lin = tex::untile(s, L);
        t_untile += secs(tu);
        if (!li || !lin) {
            lvl.failf("%s #%lld s%lld L%d: %s", path.c_str(), idx, si, L, (!li ? li.error() : lin.error()).detail.c_str());
            continue;
        }
        const bool geo = num(r[5]) == li->width && num(r[6]) == li->height && num(r[7]) == li->pitch &&
                         num(r[8]) == li->tile_mode;
        if (geo && num(r[9]) == static_cast<long long>(lin->size()) && r[10] == hdf::sha256_hex(*lin))
            lvl.pass();
        else
            lvl.failf("%s #%lld s%lld L%d: untile differs (geometry %s)", path.c_str(), idx, si, L, geo ? "same" : "DIFFERS");
        const u32 bpp = tex::gx2_bpp(s.format);
        const bool bc = tex::gx2_is_bcn(s.format);
        elements += lin->size() / (bpp / 8);
        auto rg = tex::to_rgba(s, L, *lin);
        if (rg && num(r[11]) == static_cast<long long>(rg->size()) && r[12] == hdf::sha256_hex(*rg))
            rgba.pass();
        else
            rgba.failf("%s #%lld s%lld L%d: to_rgba differs", path.c_str(), idx, si, L);
        // cross-check: tphd AddrLib on the same bytes and the same geometry
        const View g = s.gtx;
        const u64 b = std::min<u64>(li->offset, g.size()), e = std::min<u64>(li->offset + li->size, g.size());
        namespace ar = hdf::tex::addrlib_ref;
        ar::SurfaceDesc desc{li->width, li->height, li->pitch, bpp, static_cast<ar::TileMode>(li->tile_mode),
                             li->tile_mode < 4 ? 0u : s.swizzle, bc, false};
        const std::vector<u8> rl = ar::deswizzle(desc, g.subspan(static_cast<std::size_t>(b), static_cast<std::size_t>(e - b)));
        if (View(rl.data(), rl.size()).size() == lin->size() && std::equal(rl.begin(), rl.end(), lin->begin()))
            ref.pass();
        else
            ref.failf("%s #%lld s%lld L%d: tphd AddrLib differs at %lld", path.c_str(), idx, si, L,
                      first_diff(View(rl.data(), rl.size()), *lin));
        // tphd mip geometry (information only)
        ar::SurfaceInfoIn sin{s.width, s.height, bpp, static_cast<u32>(L), static_cast<ar::TileMode>(s.tile_mode), bc};
        ar::SurfaceInfoOut sout{};
        ar::computeSurfaceInfo(sin, sout);
        ++geo_total;
        if (sout.pitch == li->pitch && static_cast<u32>(sout.tileMode) == li->tile_mode) {
            ++geo_same;
        } else if (verbose && geo_shown++ < 20) {
            std::printf("  a: info: %s #%lld s%lld L%d: gtx2img pitch %u mode %u, tphd pitch %u mode %u\n", path.c_str(),
                        idx, si, L, li->pitch, li->tile_mode, sout.pitch, static_cast<u32>(sout.tileMode));
        }
    }
    in.drop_hd();
    const double t = secs(t0);
    surf.line(t);
    lvl.line(t);
    rgba.line(t);
    ref.line(t);
    std::printf("(a) %u surfaces, %llu elements untiled in %.2f s; tphd computeSurfaceInfo pitch+mode agree on %u/%u "
                "levels (information only)\n", n_surf, static_cast<unsigned long long>(elements), t_untile, geo_same,
                geo_total);
    return surf.bad == 0 && lvl.bad == 0 && rgba.bad == 0 && ref.bad == 0 && surf.ok > 0 && lvl.ok > 0;
}

// ---------------------------------------------------------------------------------------------- (b) + (c) models
bool part_models(Inputs& in, const std::vector<Row>& rows, bool verbose) {
    const auto t0 = Clock::now();
    Counter model{"b: model textures"}, bti{"c: stage BTIs"};
    model.verbose = bti.verbose = verbose;
    std::map<std::pair<std::string, long long>, std::vector<tex::GtxSurface>> gcache;
    std::string last_arc;
    for (const Row& r : rows) {
        if (r[0] != "model" && r[0] != "bti") continue;
        const bool is_model = r[0] == "model";
        Counter& c = is_model ? model : bti;
        const std::string& arc_path = r[1];
        if (arc_path != last_arc) {  // rows come archive by archive: keep one pack in memory
            gcache.clear();
            in.drop_hd();
            last_arc = arc_path;
        }
        const long long mi = num(r[2]);
        const std::size_t k = is_model ? 1 : 0;  // the model row has the tex index at [3]
        const std::string& pack_path = r[3 + k];
        const long long gi = num(r[4 + k]);
        const std::string& opath = r[5 + k];
        const long long omi = num(r[6 + k]);
        auto a = in.hd_arc(arc_path);
        auto oa = in.oracle_arc(opath);
        if (!a || !oa) {
            c.failf("%s: archive: %s", arc_path.c_str(), (!a ? a.error() : oa.error()).detail.c_str());
            continue;
        }
        auto d = member_data(**a, mi);
        auto od = member_data(**oa, omi);
        if (!d || !od) {
            c.failf("%s #%lld: member", arc_path.c_str(), mi);
            continue;
        }
        std::optional<View> gtx;
        if (pack_path != "-" && gi >= 0) {
            auto t = in.pack(pack_path);
            if (!t) {
                c.failf("%s: %s", pack_path.c_str(), t.error().detail.c_str());
                continue;
            }
            auto m = pack_member(**t, gi);
            if (!m) {
                c.failf("%s #%lld: pack member", pack_path.c_str(), gi);
                continue;
            }
            gtx = *m;
        }
        if (!is_model) {
            auto out = tex::convert_bti(*d, gtx);
            if (out && *out == *od && num(r[7]) == static_cast<long long>(out->size()) && r[8] == hdf::sha256_hex(*out))
                c.pass();
            else
                c.failf("%s #%lld: convert_bti %s", arc_path.c_str(), mi, diff_text(out ? first_diff(*out, *od) : -2).c_str());
            continue;
        }
        const long long ti = num(r[3]);
        auto key = std::make_pair(pack_path, gi);
        auto git = gcache.find(key);
        if (git == gcache.end()) {
            std::vector<tex::GtxSurface> ss;
            if (gtx) {
                auto s = tex::gtx_surfaces(*gtx);
                if (s) ss = std::move(*s);
            }
            git = gcache.emplace(key, std::move(ss)).first;
        }
        const std::size_t o = tex1_offset(*d), oo = tex1_offset(*od);
        if (!o || !oo || ti < 0 || static_cast<std::size_t>(ti) >= git->second.size()) {
            c.failf("%s #%lld TEX1[%lld]: no TEX1 or surface", arc_path.c_str(), mi, ti);
            continue;
        }
        const std::size_t ho = o + io::be32(*d, o + 0xC) + static_cast<std::size_t>(ti) * 0x20;
        const std::size_t eo = oo + io::be32(*od, oo + 0xC) + static_cast<std::size_t>(ti) * 0x20;
        auto hdr = tex::bti_fields(*d, ho);
        if (!hdr || eo + 0x20 > od->size()) {
            c.failf("%s #%lld TEX1[%lld]: header", arc_path.c_str(), mi, ti);
            continue;
        }
        auto cv = tex::convert_texture(*hdr, git->second[static_cast<std::size_t>(ti)], 1);
        if (!cv) {
            c.failf("%s #%lld TEX1[%lld]: %s", arc_path.c_str(), mi, ti, cv.error().detail.c_str());
            continue;
        }
        const u32 rel = io::be32(*od, eo + 0x1C);
        const Bytes want = tex::patch_header(View(*d).subspan(ho, 0x20), cv->format, cv->width, cv->height, cv->levels,
                                             cv->pixels.empty() ? 0 : rel);
        const View oh = View(*od).subspan(eo, 0x20);
        const bool hdr_same = std::equal(want.begin(), want.end(), oh.begin());
        const bool pix_in = eo + rel + cv->pixels.size() <= od->size();
        const bool pix_same =
            pix_in && std::equal(cv->pixels.begin(), cv->pixels.end(), od->begin() + static_cast<std::ptrdiff_t>(eo + rel));
        const bool gold = num(r[8]) == cv->format && num(r[9]) == cv->width && num(r[10]) == cv->height &&
                          num(r[11]) == cv->levels && num(r[12]) == static_cast<long long>(cv->pixels.size()) &&
                          r[13] == hdf::sha256_hex(cv->pixels);
        if (hdr_same && pix_same && gold)
            c.pass();
        else
            c.failf("%s #%lld TEX1[%lld]: header %s, texels %s, golden %s", arc_path.c_str(), mi, ti,
                    hdr_same ? "same" : "DIFFERS", pix_same ? "same" : "DIFFER", gold ? "same" : "DIFFERS");
    }
    const double t = secs(t0);
    model.line(t);
    bti.line(t);
    return model.bad == 0 && bti.bad == 0 && model.ok > 0 && bti.ok > 0;
}

// ---------------------------------------------------------------------------------------------- (d) particles
bool part_particles(Inputs& in, const std::vector<Row>& rows, bool verbose) {
    const auto t0 = Clock::now();
    Counter c{"d: particle textures"};
    c.verbose = verbose;
    auto hd206 = in.hd_any("res/Particle/Pscene206.jpc");
    auto gtx = in.hd_any("tex/Particle/Pscene206.jpc.gtx.gz");
    auto od = in.oracle_file("overlay/res/Particle/Pscene206.jpc");
    if (!hd206 || !gtx || !od) {
        c.failf("inputs missing");
        c.line(secs(t0));
        return false;
    }
    const auto H = jpc_textures(**hd206), O = jpc_textures(**od);
    auto surfaces = tex::gtx_surfaces(**gtx);
    if (!surfaces) {
        c.failf("Pscene206 GTX: %s", surfaces.error().detail.c_str());
        c.line(secs(t0));
        return false;
    }
    u32 halves = 0;
    for (const Row& r : rows) {
        if (r[0] != "ptex") continue;
        const long long i = num(r[1]), j = num(r[3]);
        const bool half = num(r[2]) != 0;
        if (i < 0 || static_cast<std::size_t>(i) >= H.size() || static_cast<std::size_t>(i) >= surfaces->size() || j < 0 ||
            static_cast<std::size_t>(j) >= O.size()) {
            c.failf("texture %lld / block %lld out of range", i, j);
            continue;
        }
        const View raw = H[static_cast<std::size_t>(i)];
        auto h = tex::bti_fields(raw, 0x20);
        if (!h) {
            c.failf("texture %lld: header", i);
            continue;
        }
        tex::BtiFields hh = *h;
        if (half) {  // particles._convert_hd_texture
            hh.width = static_cast<u16>(std::max<u32>(8, hh.width / 2u));
            hh.height = static_cast<u16>(std::max<u32>(8, hh.height / 2u));
            ++halves;
        }
        auto cv = tex::convert_texture(hh, (*surfaces)[static_cast<std::size_t>(i)], 1);
        if (!cv) {
            c.failf("texture %lld: %s", i, cv.error().detail.c_str());
            continue;
        }
        // particles.tex_block(name_hdr, timg, pixels)
        Bytes blk(raw.begin(), raw.begin() + 0x20);
        const Bytes timg = tex::patch_header(raw.subspan(0x20, 0x20), cv->format, cv->width, cv->height, cv->levels, 0x20);
        blk.insert(blk.end(), timg.begin(), timg.end());
        blk.insert(blk.end(), cv->pixels.begin(), cv->pixels.end());
        blk.resize(blk.size() + (32 - cv->pixels.size() % 32) % 32, 0);
        io::put32(blk, 4, static_cast<u32>(blk.size()));
        const View ob = O[static_cast<std::size_t>(j)];
        if (View(blk).size() == ob.size() && std::equal(blk.begin(), blk.end(), ob.begin()) &&
            num(r[4]) == static_cast<long long>(blk.size()) && r[5] == hdf::sha256_hex(blk))
            c.pass();
        else
            c.failf("texture %lld -> block %lld: %s", i, j, diff_text(first_diff(blk, ob)).c_str());
    }
    c.line(secs(t0));
    std::printf("(d) %u of them at half size\n", halves);
    return c.bad == 0 && c.ok > 0;
}

// ---------------------------------------------------------------------------------------------- (e) UI BTIs
bool part_ui(Inputs& in, const std::vector<Row>& rows, bool verbose) {
    const auto t0 = Clock::now();
    Counter book{"e: UI archive BTIs"}, loose{"e: loose BTIs"};
    book.verbose = loose.verbose = verbose;
    std::map<std::string, u32> fmts;
    for (const Row& r : rows) {
        if (r[0] != "ui") continue;
        const long long omi = num(r[6]);
        Counter& c = omi < 0 ? loose : book;
        auto a = in.hd_arc(r[1]);
        auto t = in.pack(r[3]);
        if (!a || !t) {
            c.failf("%s: inputs", r[1].c_str());
            continue;
        }
        auto stub = member_data(**a, num(r[2]));
        auto g = pack_member(**t, num(r[4]));
        if (!stub || !g) {
            c.failf("%s #%s: member", r[1].c_str(), r[2].c_str());
            continue;
        }
        auto job = tex::bti_job(*stub, *g);
        if (!job) {
            c.failf("%s #%s: %s", r[1].c_str(), r[2].c_str(), job.error().detail.c_str());
            continue;
        }
        auto made = tex::make_bti(*job);
        if (!made) {
            c.failf("%s #%s: %s", r[1].c_str(), r[2].c_str(), made.error().detail.c_str());
            continue;
        }
        Bytes want;
        if (omi < 0) {
            auto f = in.oracle_file(r[5]);
            if (f) want = **f;
        } else {
            auto oa = in.oracle_arc(r[5]);
            if (oa) {
                auto m = member_data(**oa, omi);
                if (m) want = std::move(*m);
            }
        }
        static const char* names[] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8"};
        const u8 fi = static_cast<u8>(made->format);
        const std::string fname = fi <= 6 ? names[fi] : "?";
        ++fmts[fname];
        const bool rgba_same = r[8] == hdf::sha256_hex(job->rgba);
        if (made->bti == want && r[7] == fname && rgba_same && num(r[9]) == static_cast<long long>(made->bti.size()) &&
            r[10] == hdf::sha256_hex(made->bti))
            c.pass();
        else
            c.failf("%s #%s: format %s (Python %s), rgba %s, BTI %s", r[1].c_str(), r[2].c_str(), fname.c_str(),
                    r[7].c_str(), rgba_same ? "same" : "DIFFERS", diff_text(first_diff(made->bti, want)).c_str());
    }
    const double t = secs(t0);
    book.line(t);
    loose.line(t);
    std::printf("(e) formats:");
    for (const auto& [k, v] : fmts) std::printf(" %s %u", k.c_str(), v);
    std::printf("\n");
    return book.bad == 0 && loose.bad == 0 && book.ok > 0 && loose.ok > 0;
}

// ---------------------------------------------------------------------------------------------- (f) icons
bool part_icons(Inputs& in, const std::vector<Row>& rows, bool verbose) {
    const auto t0 = Clock::now();
    Counter c{"f: stamp icons"};
    c.verbose = verbose;
    for (const Row& r : rows) {
        if (r[0] != "icon") continue;
        char png_path[64], bti_path[64];
        const int i = static_cast<int>(num(r[1]));
        std::snprintf(png_path, sizeof png_path, "res/stamps/stamps/stamp%02d.png", i);
        std::snprintf(bti_path, sizeof bti_path, "res/stamps/stamp_icons/stamp%02d.bti", i);
        auto png = in.oracle_file(png_path);
        auto want = in.oracle_file(bti_path);
        if (!png || !want) {
            c.failf("icon %d: oracle files", i);
            continue;
        }
        auto dec = io::png_decode(**png);
        if (!dec) {
            c.failf("icon %d: PNG: %s", i, dec.error().detail.c_str());
            continue;
        }
        const auto [wh, rgba] = std::move(*dec);
        const Bytes icon = tex::make_icon_rgba(rgba, wh[0], wh[1]);
        const Bytes bc1 = tex::bc1_encode(icon, 48, 48);
        const Bytes out = tex::bti_cmpr(icon, 48, 48);
        const Bytes one = tex::stamp_icon_bti(rgba, wh[0], wh[1]);
        const bool icon_same = r[2] == hdf::sha256_hex(icon), bc1_same = r[3] == hdf::sha256_hex(bc1);
        if (out == **want && one == out && icon_same && bc1_same && r[5] == hdf::sha256_hex(out))
            c.pass();
        else
            c.failf("icon %d: icon RGBA %s, BC1 %s, BTI %s", i, icon_same ? "same" : "DIFFERS", bc1_same ? "same" : "DIFFERS",
                    diff_text(first_diff(out, **want)).c_str());
    }
    c.line(secs(t0));
    return c.bad == 0 && c.ok > 0;
}

// ---------------------------------------------------------------------------------------------- job counts
// wave2_T_verify L3: a cut golden list must not pass. The rows are counted by kind and compared with the list's
// own "# counts" line and with the fixed totals of the pinned inputs (golden_tex.py on USA v81 + GZ2E01).
struct FixedCount {
    const char* name;
    long long want;
};
constexpr FixedCount kTexCounts[] = {
    {"surf", 1492}, {"lvl", 6200},     {"lvlerr", 0},    {"model", 631},    {"models", 55},
    {"no_source", 0}, {"bti", 5},      {"ptex", 27},     {"ptex_half", 7},  {"ui_book", 74},
    {"ui_confirm", 52}, {"ui_loose", 4}, {"icon", 50},
};

std::map<std::string, long long> count_rows(const std::vector<Row>& rows) {
    std::map<std::string, long long> n;
    for (const FixedCount& k : kTexCounts) n[k.name] = 0;
    std::set<std::string> models;
    for (const Row& r : rows) {
        const std::string& kind = r[0];
        if (kind == "model") {
            ++n["model"];
            models.insert(r[1] + "\t" + r[2]);  // golden_tex.py counts['models']: one per (archive, member)
        } else if (kind == "ptex") {
            ++n["ptex"];
            n["ptex_half"] += num(r[2]);  // ('ptex', i, int(half), j, len, sha)
        } else if (kind == "ui") {
            if (num(r[6]) < 0) ++n["ui_loose"];
            else if (r[5].rfind("res/stamps/", 0) == 0) ++n["ui_book"];
            else ++n["ui_confirm"];
        } else {
            ++n[kind];  // surf, lvl, lvlerr, bti, icon
        }
    }
    n["models"] = static_cast<long long>(models.size());
    return n;  // no_source has no rows: only the "# counts" line and the fixed total speak about it
}

// The "# counts k=v ..." line (golden_tex.py).
std::map<std::string, long long> parse_counts(const std::string& line) {
    std::map<std::string, long long> out;
    std::size_t s = std::string("# counts").size();
    while (s < line.size()) {
        while (s < line.size() && line[s] == ' ') ++s;
        const std::size_t e = std::min(line.find(' ', s), line.size());
        const std::string kv = line.substr(s, e - s);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos && eq > 0) out[kv.substr(0, eq)] = num(kv.substr(eq + 1));
        s = e;
    }
    return out;
}

bool check_counts(const std::vector<Row>& rows, const std::optional<std::map<std::string, long long>>& listed) {
    if (!listed) {
        std::printf("(counts) FAIL: the golden list has no \"# counts\" line\n");
        return false;
    }
    const std::map<std::string, long long> have = count_rows(rows);
    u32 bad = 0;
    for (const FixedCount& k : kTexCounts) {
        const auto l = listed->find(k.name);
        const long long rows_n = have.at(k.name);
        const bool from_rows = std::string_view(k.name) != "no_source";
        const bool ok = l != listed->end() && l->second == k.want && (!from_rows || rows_n == k.want);
        if (!ok) {
            ++bad;
            std::printf("(counts) FAIL: %s: %s rows, \"# counts\" says %s, expected %lld\n", k.name,
                        from_rows ? std::to_string(rows_n).c_str() : "-",
                        l == listed->end() ? "nothing" : std::to_string(l->second).c_str(), k.want);
        }
    }
    std::printf("(counts) %u/%zu job counts equal to the fixed totals%s\n", u32(std::size(kTexCounts)) - bad,
                std::size(kTexCounts), bad ? " -- FAIL" : "");
    return bad == 0;
}

struct Golden {
    std::vector<Row> rows;
    std::optional<std::map<std::string, long long>> counts;
};

hdf::Result<Golden> read_golden(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return hdf::fail("io.read", "can't open " + hdf::cli::to_utf8(p));
    Golden g;
    std::vector<Row>& rows = g.rows;
    std::string line;
    bool header = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# hdf golden tex 1", 0) == 0) header = true;
        if (line.rfind("# counts ", 0) == 0) g.counts = parse_counts(line);
        if (line.empty() || line[0] == '#') continue;
        Row r;
        std::size_t s = 0;
        while (true) {
            const std::size_t t = line.find('\t', s);
            r.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
            if (t == std::string::npos) break;
            s = t + 1;
        }
        static const std::map<std::string, std::size_t> width = {{"surf", 14}, {"lvl", 13}, {"lvlerr", 6}, {"model", 14},
                                                                 {"bti", 9},   {"ptex", 6},  {"ui", 11},    {"icon", 6}};
        auto w = width.find(r[0]);
        if (w == width.end() || r.size() != w->second) return hdf::fail("fmt.golden", "bad line: " + line.substr(0, 40));
        rows.push_back(std::move(r));
    }
    if (!header) return hdf::fail("fmt.golden", "not a tex golden list (tools/dev/golden_tex.py)");
    return g;
}

int run(const hdf::cli::OracleArgs& a) {
    const auto t0 = Clock::now();
    if (!a.oracle) {
        std::fprintf(stderr, "hdf-convert test tex: --oracle <hd_features-oracle-2.0.0.dusk> is required\n");
        return hdf::cli::kExitUsage;
    }
    auto pin = hdf::cli::file_sha256(*a.oracle);
    if (!pin) return hdf::cli::report("test tex", pin.error());
    if (pin->second != hdf::outputs::kOracleSha256 || pin->first != hdf::outputs::kOracleSize) {
        std::fprintf(stderr, "hdf-convert test tex: the oracle's SHA-256 is %s, not the pinned one: refused\n",
                     pin->second.c_str());
        return hdf::cli::kExitUsage;
    }
    fs::path golden_path;
    if (auto gp = a.args ? a.args->path("golden") : std::nullopt) {
        golden_path = *gp;
    } else {
#ifdef _WIN32
        wchar_t exe[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        golden_path = fs::path(std::wstring(exe, n)).parent_path() / "golden" / "tex.tsv";
#endif
    }
    if (!a.hd || golden_path.empty() || !fs::exists(golden_path)) {
        std::printf("test tex: skipped: needs --hd and the golden list (%s); make it with\n"
                    "    python -I -B tools/dev/golden_tex.py --hd <..> --gc <..> --oracle <..> --out <build dir>/golden/tex.tsv\n",
                    golden_path.empty() ? "--golden" : hdf::cli::to_utf8(golden_path).c_str());
        return hdf::cli::kExitTodo;
    }
    auto golden = read_golden(golden_path);
    if (!golden) return hdf::cli::report("test tex", golden.error());
    const std::vector<Row>* rows = &golden->rows;
    auto hd = hdf::cli::open_hd(*a.hd);
    if (!hd) return hdf::cli::report("test tex", hd.error());
    auto oracle = hdf::ZipSource::open(*a.oracle);
    if (!oracle) return hdf::cli::report("test tex", oracle.error());
    std::printf("oracle: %s (SHA-256 pinned); golden list: %zu lines\n", hdf::cli::to_utf8(*a.oracle).c_str(), rows->size());

    const bool rc = check_counts(*rows, golden->counts);
    Inputs in(**hd, **oracle);
    u32 n_surf = 0;
    const bool ra = part_detile(in, *rows, a.verbose, n_surf);
    const bool rb = part_models(in, *rows, a.verbose);
    const bool rd = part_particles(in, *rows, a.verbose);
    const bool re = part_ui(in, *rows, a.verbose);
    const bool rf = part_icons(in, *rows, a.verbose);
    const bool pass = rc && ra && rb && rd && re && rf;
    std::printf("test tex: job counts %s, (a) detile %s, (b)+(c) models and stage BTIs %s, (d) particles %s, (e) UI %s, "
                "(f) icons %s\n",
                rc ? "pass" : "FAIL", ra ? "pass" : "FAIL", rb ? "pass" : "FAIL", rd ? "pass" : "FAIL",
                re ? "pass" : "FAIL", rf ? "pass" : "FAIL");
    std::printf("time: test tex: %s\n", hdf::cli::time_line(secs(t0)).c_str());
    return pass ? hdf::cli::kExitOk : hdf::cli::kExitFail;
}

}  // namespace

HDF_ORACLE_TEST("tex", "detile + to_rgba of every GX2 surface level vs the golden list and the tphd AddrLib; texel bytes of the 631 model textures, 5 stage BTIs, 27 particle textures, 126 UI-archive BTIs + 4 loose BTIs and 50 icons vs the oracle", &run);
