// Module J: formats/j3d.py (sections, read_strtab, bti_header via tex::bti_fields), formats/hd2gc.py
// (convert_j3d, convert_bti; the texture functions are module T's) and pipeline/convert.py (convert_arc), plus
// the whole-archive helper of plan 3.2 (convert + bake + write, then memfit.arc_stats).
#include "j3d_internal.hpp"

#include "hdf/tex.hpp"

#include <cstdio>

namespace hdf::j3d {

namespace detail {

// j3d.sections(buf)
std::vector<Section> sections_impl(View d) {
    // magic = buf[:8]; size, nsec = struct.unpack_from(">II", buf, 8)
    if (!has(d, 8, 8)) bad("J3D header cut");
    const u32 nsec = u32_at(d, 12);
    std::vector<Section> out;
    u64 o = 0x20;
    for (u32 k = 0; k < nsec; ++k) {
        if (o + 8 > d.size()) break;
        Section s;
        std::memcpy(s.tag.data(), d.data() + o, 4);
        s.offset = o;
        s.size = u32_at(d, o + 4);
        out.push_back(s);
        if (s.size == 0) break;
        o += s.size;
    }
    return out;
}

const Section& section_of(const std::vector<Section>& secs, const char* tag) {
    for (std::size_t i = secs.size(); i-- > 0;)
        if (is_tag(secs[i], tag)) return secs[i];
    bad(std::string("no ") + tag + " section");
}

namespace {

// j3d.read_strtab(buf, o): names as raw bytes (Python decodes shift_jis with "replace"; every name in the inputs is
// ASCII, VERIFIED wave 2 J, so the report lines are the same).
std::vector<std::string> read_strtab(View d, u64 o) {
    const u16 n = u16_at(d, o);
    std::vector<std::string> names;
    names.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        const u64 eo = o + 4 + u64{i} * 4;
        if (!has(d, eo, 4)) bad("string table cut");  // struct.unpack_from(">HH", ...)
        const u16 off = u16_at(d, eo + 2);
        const u64 start = o + off;
        // buf.index(b"\0", start): ValueError when there is no NUL at or after start
        if (start > d.size()) bad("string table: no NUL");
        const u8* p = static_cast<const u8*>(std::memchr(d.data() + start, 0, static_cast<std::size_t>(d.size() - start)));
        if (!p) bad("string table: no NUL");
        names.emplace_back(reinterpret_cast<const char*>(d.data() + start), static_cast<std::size_t>(p - (d.data() + start)));
    }
    return names;
}

// j3d.GX_FMT.get(fmt, fmt)
std::string gx_fmt_name(u32 f) {
    switch (f) {
        case 0: return "I4";
        case 1: return "I8";
        case 2: return "IA4";
        case 3: return "IA8";
        case 4: return "RGB565";
        case 5: return "RGB5A3";
        case 6: return "RGBA8";
        case 8: return "C4";
        case 9: return "C8";
        case 10: return "C14X2";
        case 14: return "CMPR";
        default: return std::to_string(f);
    }
}

// the report line of hd2gc.convert_texture
std::string texture_line(std::string_view label, const tex::TexConv& c, const tex::GtxSurface& s) {
    char b[256];
    std::snprintf(b, sizeof b, ": %s %ux%u mips=%u <- GTX %s %ux%u level %d (%u levels)", gx_fmt_name(c.format).c_str(),
                  c.width, c.height, c.levels, tex::gx2_format_name(s.format).c_str(), s.width, s.height, c.level0,
                  c.levels);
    return std::string(label) + b;
}

std::vector<tex::GtxSurface> surfaces_of(std::optional<View> gtx) {
    // g = hdpack.gtx_surfaces(gtx_buf) if gtx_buf else {"surfaces": []}
    if (!gtx || gtx->empty()) return {};
    auto s = tex::gtx_surfaces(*gtx);
    if (!s) bad("gtx: " + s.error().detail);
    return std::move(*s);
}

struct Tex1Entry {
    View raw;  // d[ho:ho + 0x20]
    u8 format = 0;
    u32 width = 0, height = 0, mips = 0;
    Bytes pixels;
};

// the TEX1 branch of hd2gc.convert_j3d: header 0x20, entries, pixel blobs (32-aligned), string table
Bytes rebuild_tex1(View d, const Section& s, const std::vector<tex::GtxSurface>& g, std::string_view label,
                   std::vector<std::string>& report, std::stop_token stop) {
    const u64 o = s.offset;
    const u16 n = u16_at(d, o + 8);
    const u32 hdr_off = u32_at(d, o + 0xC);
    const u32 str_off = u32_at(d, o + 0x10);
    std::vector<std::string> names;
    if (str_off) {
        names = read_strtab(d, o + str_off);
    } else {
        for (u32 i = 0; i < n; ++i) names.push_back("#" + std::to_string(i));
    }
    const View strtab = str_off ? slice(d, o + str_off, o + s.size) : View{};
    std::vector<Tex1Entry> hdrs;
    hdrs.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        if (stop.stop_requested()) throw Stopped{};
        const u64 ho = o + hdr_off + u64{i} * 0x20;
        auto h = tex::bti_fields(d, static_cast<std::size_t>(ho));  // j3d.bti_header(d, ho)
        if (!h) bad("TEX1 entry cut");
        if (i >= names.size()) bad("TEX1 name index out of range");  // names[i]
        const std::string lab = std::string(label) + " TEX1[" + std::to_string(i) + "] " + names[i];
        Tex1Entry e;
        e.raw = slice(d, ho, ho + 0x20);
        if (i < g.size()) {
            auto c = tex::convert_texture(*h, g[i], 1);
            if (!c) bad("texture: " + c.error().detail);
            report.push_back(texture_line(lab, *c, g[i]));
            e.format = c->format;
            e.width = c->width;
            e.height = c->height;
            e.mips = c->levels;
            e.pixels = std::move(c->pixels);
        } else {
            report.push_back(lab + ": NO SOURCE (left without pixels)");
            e.format = h->format;
            e.width = h->width;
            e.height = h->height;
            e.mips = 1;
        }
        hdrs.push_back(std::move(e));
    }
    Bytes body(0x20 + std::size_t{n} * 0x20, 0);
    std::vector<u64> blob_offs;
    blob_offs.reserve(n);
    for (const Tex1Entry& e : hdrs) {
        while (body.size() % 32) body.push_back(0);
        blob_offs.push_back(body.size());
        body.insert(body.end(), e.pixels.begin(), e.pixels.end());
    }
    while (body.size() % 4) body.push_back(0);
    const u64 new_str_off = body.size();
    body.insert(body.end(), strtab.begin(), strtab.end());
    while (body.size() % 32) body.push_back(0);
    std::memcpy(body.data(), "TEX1", 4);
    if (body.size() > 0xFFFFFFFFu || new_str_off > 0xFFFFFFFFu) bad("TEX1 too large");  // struct '>I'
    // struct.pack_into(">IHHII", body, 4, len(body), n, 0xFFFF, 0x20, new_str_off)
    io::put32(body, 4, static_cast<u32>(body.size()));
    io::put16(body, 8, n);
    io::put16(body, 0xA, 0xFFFF);
    io::put32(body, 0xC, 0x20);
    io::put32(body, 0x10, static_cast<u32>(new_str_off));
    for (u32 i = 0; i < n; ++i) {
        const Tex1Entry& e = hdrs[i];
        const u64 eo = 0x20 + u64{i} * 0x20;
        const u64 rel = e.pixels.empty() ? 0 : blob_offs[i] - eo;
        if (rel > 0xFFFFFFFFu) bad("TEX1 image offset too large");
        // patch_header: struct ">HH" (w, h <= 0xFFFF) and bytearray items b[0x18] = nmips, b[0x17] = (nmips - 1) * 8 (<= 255)
        if (e.width > 0xFFFF || e.height > 0xFFFF || e.mips > 32) bad("TEX1 header field out of range");
        const Bytes ph = tex::patch_header(e.raw, e.format, e.width, e.height, e.mips, static_cast<u32>(rel));
        slice_assign(body, eo, eo + 0x20, ph);
    }
    return body;
}

}  // namespace

// hd2gc.convert_j3d(d, gtx_buf, 1, {}, None, report, label)
Bytes convert_j3d_impl(View d, std::optional<View> gtx, std::string_view label, std::vector<std::string>& report,
                       std::stop_token stop) {
    const std::vector<Section> secs = sections_impl(d);
    const View head = slice(d, 0, 0x20);
    Bytes out(head.begin(), head.end());
    const std::vector<tex::GtxSurface> g = surfaces_of(gtx);
    for (const Section& s : secs) {
        const View raw = slice(d, s.offset, s.offset + s.size);
        Bytes sec(raw.begin(), raw.end());
        if (is_tag(s, "MAT4")) {
            slice_assign(sec, 0, 4, sv_bytes("MAT3"));
            report.push_back(std::string(label) + ": MAT4 -> MAT3 (header offsets kept; 31st offset ignored by loader)");
        } else if (is_tag(s, "MDL3")) {
            report.push_back(std::string(label) + ": WARNING MDL3 present (precompiled material DLs untouched)");
        } else if (is_tag(s, "TEX1")) {
            sec = rebuild_tex1(d, s, g, label, report, stop);
        }
        out.insert(out.end(), sec.begin(), sec.end());
    }
    if (out.size() > 0xFFFFFFFFu) bad("model too large");
    put_u32(out, 8, static_cast<u32>(out.size()));
    return out;
}

// hd2gc.convert_bti(d, gtx_buf, 1, report, label)
Bytes convert_bti_impl(View d, std::optional<View> gtx, std::string_view label, std::vector<std::string>& report) {
    auto h = tex::bti_fields(d, 0);
    if (!h) bad("BTI header cut");
    const u64 exp = tex::gx_tex_size(h->format, h->width, h->height, h->mip_count);
    if (h->image_offset && d.size() >= u64{h->image_offset} + exp) return Bytes(d.begin(), d.end());  // has pixels
    if (!gtx || gtx->empty()) {
        report.push_back(std::string(label) + ": header-only BTI and no GTX -> left as is");
        return Bytes(d.begin(), d.end());
    }
    auto surfs = tex::gtx_surfaces(*gtx);
    if (!surfs) bad("gtx: " + surfs.error().detail);
    if (surfs->empty()) bad("gtx has no surface");  // ["surfaces"][0]
    const tex::GtxSurface& surf = (*surfs)[0];
    auto c = tex::convert_texture(*h, surf, 1);
    if (!c) bad("texture: " + c.error().detail);
    report.push_back(texture_line(label, *c, surf));
    if (c->width > 0xFFFF || c->height > 0xFFFF || c->levels > 32) bad("BTI header field out of range");  // as above
    Bytes out = tex::patch_header(slice(d, 0, 0x20), c->format, c->width, c->height, c->levels, 0x20);
    out.insert(out.end(), c->pixels.begin(), c->pixels.end());
    return out;
}

}  // namespace detail

using namespace detail;

Result<std::vector<Section>> sections(View j3d) {
    try {
        return sections_impl(j3d);
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    }
}

Result<Bytes> convert_j3d(View bmd, std::optional<View> gtx, std::vector<std::string>* report) {
    std::vector<std::string> rep;
    try {
        Bytes out = convert_j3d_impl(bmd, gtx, "", rep, {});
        if (report) report->insert(report->end(), rep.begin(), rep.end());
        return out;
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    }
}

Result<Bytes> convert_bti(View bti, std::optional<View> gtx, std::vector<std::string>* report) {
    std::vector<std::string> rep;
    try {
        Bytes out = convert_bti_impl(bti, gtx, "", rep);
        if (report) report->insert(report->end(), rep.begin(), rep.end());
        return out;
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    }
}

// convert.convert_arc(arc_path, pack_path) -> (a, repl, st, report)
Result<Converted> convert_members(View hd_arc, const io::Tmpk* pack, std::string_view arc_name, std::stop_token stop) {
    auto a = io::rarc_read(hd_arc);
    if (!a) return std::unexpected(a.error());
    Converted out;
    out.arc = std::move(*a);
    try {
        for (const io::RarcMember& m : out.arc.members) {
            const std::string ln = lower(m.name);
            const bool bti = ends_with(ln, ".bti");
            if (!bti && !ends_with(ln, ".bmd") && !ends_with(ln, ".bdl")) continue;
            if (stop.stop_requested()) return fail("stopped", "j3d");
            std::optional<View> gtx;
            if (pack) {
                const std::string want = "/" + lower(m.path) + ".gtx";
                for (const io::TmpkEntry& e : pack->entries)
                    if (ends_with(lower(e.name), want)) {
                        gtx = pack->data(e);
                        break;
                    }
            }
            auto d = out.arc.data(m);
            if (!d) return std::unexpected(d.error());
            const std::string lab = std::string(arc_name) + ":" + m.path;
            const std::size_t n0 = out.report.size();
            Bytes nd;
            if (bti) {
                nd = convert_bti_impl(*d, gtx, lab, out.report);
                ++out.stats.bti;
            } else {
                nd = convert_j3d_impl(*d, gtx, lab, out.report, stop);
                Mat4Fix fx = mat4fix_impl(nd);
                nd = std::move(fx.data);
                ++out.stats.models;
                out.stats.mat_fixed += static_cast<u32>(fx.fixed);
                if (fx.fixed) out.report.push_back(lab + ": MAT 0x14E -> 0x14C repack (" + std::to_string(fx.fixed) + " block)");
            }
            for (std::size_t k = n0; k < out.report.size(); ++k) {
                const std::string& line = out.report[k];
                if (line.find("NO SOURCE") != std::string::npos || line.find("left as is") != std::string::npos)
                    ++out.stats.no_source;
                else if (line.find("MDL3") != std::string::npos)
                    ++out.stats.mdl3;
                else if (line.find("<- GTX") != std::string::npos)
                    ++out.stats.textures;
            }
            out.replace.insert_or_assign(m.path, std::move(nd));  // repl[m.path] = nd (last wins)
        }
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    } catch (const Stopped&) {
        return fail("stopped", "j3d");
    }
    return out;
}

Result<Converted> convert_members(View hd_arc, const io::Tmpk* pack, std::stop_token stop) {
    return convert_members(hd_arc, pack, std::string_view{}, stop);
}

Result<ArcResult> convert_arc(View hd_arc_yaz0, std::optional<View> pack_gz, std::optional<Bake> bake,
                              bool same_layout, std::string_view arc_name, std::stop_token stop) {
    std::optional<io::Tmpk> tm;
    if (pack_gz) {
        auto t = io::tmpk_from_gz(*pack_gz, stop);  // hdpack.Tmpk(hdpack.read_any(pack_path))
        if (!t) return std::unexpected(t.error());
        tm = std::move(*t);
    }
    auto c = convert_members(hd_arc_yaz0, tm ? &*tm : nullptr, arc_name, stop);
    if (!c) return std::unexpected(c.error());
    if (bake) {
        // cavebuild.build: every converted .bmd/.bdl baked with the room's los entry
        for (auto& [path, bytes] : c->replace) {
            const std::string lp = lower(path);
            if (!ends_with(lp, ".bmd") && !ends_with(lp, ".bdl")) continue;
            if (stop.stop_requested()) return fail("stopped", "j3d");
            auto b = bake_los(bytes, bake->t, bake->angle);
            if (!b) return std::unexpected(b.error());
            bytes = std::move(b->data);
        }
    }
    auto built = same_layout ? io::rarc_rebuild_same_layout(c->arc, c->replace) : io::rarc_write(c->arc, c->replace);
    if (!built) return std::unexpected(built.error());
    auto st = arc_stats(*built);
    if (!st) return std::unexpected(st.error());
    ArcResult r;
    r.arc = std::move(*built);
    r.stats.push_back(*st);
    return r;
}

Result<ArcResult> convert_arc(View hd_arc_yaz0, std::optional<View> pack_gz, std::optional<Bake> bake,
                              bool same_layout, std::stop_token stop) {
    return convert_arc(hd_arc_yaz0, pack_gz, bake, same_layout, std::string_view{}, stop);
}

}  // namespace hdf::j3d
