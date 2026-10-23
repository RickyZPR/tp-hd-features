// Module J: formats/j3d_dlcheck.py (pc_dl_bytes with _vtx_formats, _elem_size, _packets, _tri_indices,
// _optimized_size) and pipeline/memfit.py (model_stats, arc_stats): the numbers memfit.estimate needs.
//
// pc_dl_bytes: bytes the PC build copies per model. Every J3DShapeDraw copies its packet display list after
// aurora's optimizer: NOPs dropped; consecutive expandable draws with the same VAT merge into one batch (<= 0xFFFF
// vertices); a batch of only GX_TRIANGLES becomes one 3-byte header + vertices, any other batch a 10-byte
// DRAW_INDEXED header + 2 bytes per index + vertices; other draws are copied verbatim.
// 'copy' = sum over packets of align32(max(raw, optimized)).
#include "j3d_internal.hpp"

#include <algorithm>
#include <set>

namespace hdf::j3d {

namespace detail {

namespace {

constexpr u8 kQuads = 0x80, kTriangles = 0x90, kTriangleStrip = 0x98, kTriangleFan = 0xA0;

// j3d_dlcheck._vtx_formats(d, vo): attribute -> (ct, comp); a repeated attribute keeps the last row
using VtxFmts = std::vector<std::pair<u32, std::pair<u32, u32>>>;
VtxFmts vtx_formats(View d, u64 vo) {
    const u32 fmt_off = u32_at(d, vo + 8);
    VtxFmts fmts;
    for (u64 o = vo + fmt_off;; o += 0x10) {
        if (!has(d, o, 13)) bad("VTX1 format list cut");  // struct.unpack_from(">IIIB", d, o)
        const u32 a = u32_at(d, o);
        if (a == 0xFF) break;
        const std::pair<u32, u32> v{u32_at(d, o + 4), u32_at(d, o + 8)};
        auto it = std::find_if(fmts.begin(), fmts.end(), [&](const auto& x) { return x.first == a; });
        if (it != fmts.end()) it->second = v;
        else fmts.emplace_back(a, v);
    }
    return fmts;
}

// j3d_dlcheck._elem_size(fmts, a)
u32 elem_size(const VtxFmts& fmts, u32 a) {
    auto it = std::find_if(fmts.begin(), fmts.end(), [&](const auto& x) { return x.first == a; });
    if (it == fmts.end()) bad("fmts[" + std::to_string(a) + "]");  // KeyError
    const u32 ct = it->second.first, comp = it->second.second;
    if (a == 11 || a == 12) {
        switch (comp) {
            case 0: return 2;
            case 1: return 3;
            case 2: return 4;
            case 3: return 2;
            case 4: return 3;
            case 5: return 4;
            default: bad("colour comp " + std::to_string(comp));
        }
    }
    u32 n;
    if (a == 9) n = ct == 0 ? 2 : 3;
    else if (a == 10) n = 3;
    else if (a == 25) n = 9;
    else n = ct == 0 ? 1 : 2;
    u32 sz;
    switch (comp) {  // COMP_SZ
        case 0:
        case 1: sz = 1; break;
        case 2:
        case 3: sz = 2; break;
        case 4: sz = 4; break;
        default: bad("COMP_SZ[" + std::to_string(comp) + "]");
    }
    return n * sz;
}

struct Packet {
    u32 shape = 0;
    u64 stride = 0;
    u64 p = 0;    // absolute display-list offset
    u32 dsz = 0;  // display-list size
};

// j3d_dlcheck._packets(d)
std::vector<Packet> packets(View d) {
    const std::vector<Section> secs = sections_impl(d);
    const VtxFmts fmts = vtx_formats(d, section_of(secs, "VTX1").offset);
    const u64 so = section_of(secs, "SHP1").offset;
    const u16 nshp = u16_at(d, so + 8);
    if (!has(d, so + 0xC, 8 * 4)) bad("SHP1 offsets cut");
    const u32 init_off = u32_at(d, so + 0xC), desc_off = u32_at(d, so + 0x18), dl_off = u32_at(d, so + 0x20),
              drawinit_off = u32_at(d, so + 0x28);
    std::vector<Packet> out;
    for (u32 i = 0; i < nshp; ++i) {
        const u64 si = so + init_off + u64{i} * 0x28;
        if (!has(d, si, 10)) bad("SHP1 shape cut");  // ">BBHHHH"
        const u16 ngrp = u16_at(d, si + 2), desc_idx = u16_at(d, si + 4), drawinit_i = u16_at(d, si + 8);
        u64 stride = 0;  // sum(sz for a, t, sz in desc if t in (1, 2, 3))
        for (u64 q = so + desc_off + desc_idx;; q += 8) {
            if (!has(d, q, 8)) bad("SHP1 vertex descriptor cut");
            const u32 a = u32_at(d, q), t = u32_at(d, q + 4);
            if (a == 0xFF) break;
            // (1 if a <= 8 else _elem_size(fmts, a)) if t == 1 else (1 if t == 2 else 2)
            const u64 sz = t == 1 ? (a <= 8 ? 1 : elem_size(fmts, a)) : (t == 2 ? 1 : 2);
            if (t == 1 || t == 2 || t == 3) stride += sz;
        }
        for (u32 g = 0; g < ngrp; ++g) {
            const u64 gi = so + drawinit_off + (u64{drawinit_i} + g) * 8;
            if (!has(d, gi, 8)) bad("SHP1 draw table cut");
            out.push_back(Packet{i, stride, so + dl_off + u32_at(d, gi + 4), u32_at(d, gi)});
        }
    }
    return out;
}

// j3d_dlcheck._tri_indices(prim, n): index count after expand_triangles, or nullopt when not expandable
std::optional<u64> tri_indices(u8 prim, u64 n) {
    if (prim == kTriangles) return (n >= 3 && n % 3 == 0) ? std::optional<u64>(n) : std::nullopt;
    if (prim == kTriangleStrip || prim == kTriangleFan) return n >= 3 ? std::optional<u64>((n - 2) * 3) : std::nullopt;
    if (prim == kQuads) return (n >= 4 && n % 4 == 0) ? std::optional<u64>((n / 4) * 6) : std::nullopt;
    return std::nullopt;
}

// j3d_dlcheck._optimized_size(d, p, end, stride): nullopt = None (the raw list is copied)
std::optional<u64> optimized_size(View d, u64 p, u64 end, u64 stride) {
    u64 out = 0;
    u64 bv = 0, bi = 0;  // batch vertices / indices
    bool ball = true;    // batch only GX_TRIANGLES
    int bfmt = -1;       // None
    auto flush = [&] {
        if (bv) out += (ball ? 3 : 10 + 2 * bi) + bv * stride;
        bv = bi = 0;
        ball = true;
        bfmt = -1;
    };
    while (p < end) {
        const u8 op = u8_at(d, p);
        if (op == 0) {  // GX_NOP: dropped, does not split a batch
            ++p;
            continue;
        }
        const u8 prim = op & 0xF8;
        if (prim < 0x80 || prim > 0xB8) return std::nullopt;  // unknown command
        const u64 n = u16_at(d, p + 1);
        const int fmt = op & 7;
        const u64 size = 3 + n * stride;
        if (p + size > end) return std::nullopt;
        const std::optional<u64> tri = tri_indices(prim, n);
        if (!tri) {
            flush();
            out += size;
        } else {
            if (bv && (bfmt != fmt || bv + n > 0xFFFF)) flush();
            if (!bv) bfmt = fmt;
            if (prim != kTriangles) ball = false;
            bv += n;
            bi += *tri;
        }
        p += size;
    }
    flush();
    return out;
}

}  // namespace

// j3d_dlcheck.pc_dl_bytes(d)
DlBytes pc_dl_impl(View d) {
    DlBytes r;
    std::set<u32> shapes;
    for (const Packet& k : packets(d)) {
        shapes.insert(k.shape);
        ++r.packets;
        const std::optional<u64> o = optimized_size(d, k.p, k.p + k.dsz, k.stride);
        r.raw += k.dsz;
        r.optimized += o ? *o : k.dsz;
        r.copy += (std::max<u64>(k.dsz, o ? *o : 0) + 0x1F) & ~u64{0x1F};
    }
    r.shapes = static_cast<u32>(shapes.size());
    return r;
}

// memfit.model_stats(d)
ModelStats model_stats_impl(View d) {
    ModelStats st;
    std::vector<Section> secs;
    try {
        secs = sections_impl(d);
    } catch (const Bad&) {
        return st;  // except Exception: return st
    }
    try {
        st.dl = pc_dl_impl(d).copy;
    } catch (const Bad&) {
        st.dl = 0;
    }
    for (const Section& s : secs) {  // not inside a try in Python: a short section header raises
        if (is_tag(s, "MAT3") || is_tag(s, "MAT4") || is_tag(s, "MAT2")) st.mat += u16_at(d, s.offset + 8);
        else if (is_tag(s, "SHP1")) st.shp += u16_at(d, s.offset + 8);
        else if (is_tag(s, "JNT1")) st.jnt += u16_at(d, s.offset + 8);
        else if (is_tag(s, "TEX1")) st.tex += u16_at(d, s.offset + 8);
    }
    return st;
}

}  // namespace detail

using namespace detail;

Result<DlBytes> pc_dl_detail(View bmd) {
    try {
        return pc_dl_impl(bmd);
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    }
}

Result<u64> pc_dl_bytes(View bmd) {
    auto r = pc_dl_detail(bmd);
    if (!r) return std::unexpected(r.error());
    return r->copy;
}

ModelStats model_stats(View bmd) {
    try {
        return model_stats_impl(bmd);
    } catch (const Bad&) {
        return {};  // the count loop raised (Python raises there too); this API has no error path
    }
}

// memfit.arc_stats(buf)
Result<ModelStats> arc_stats(View arc) {
    auto a = io::rarc_read(arc);  // rarc.Rarc(buf, 'x'): Yaz0 decoded
    if (!a) return std::unexpected(a.error());
    ModelStats s;
    s.arc = a->buf.size();
    try {
        for (const io::RarcMember& m : a->members) {
            ++s.files;
            const std::string n = lower(m.name);
            if (ends_with(n, ".bmd") || ends_with(n, ".bdl")) {
                auto d = a->data(m);
                if (!d) return std::unexpected(d.error());
                const ModelStats ms = model_stats_impl(*d);
                ++s.models;
                s.mat += ms.mat;
                s.shp += ms.shp;
                s.jnt += ms.jnt;
                s.tex += ms.tex;
                s.dl += ms.dl;
            } else {
                for (const char* suf : {".btk", ".brk", ".bck", ".btp", ".bpk", ".bva", ".bla", ".bxa"})
                    if (ends_with(n, suf)) {
                        ++s.anm;
                        break;
                    }
            }
        }
    } catch (const Bad& b) {
        return std::unexpected(to_error(b));
    }
    return s;
}

}  // namespace hdf::j3d
