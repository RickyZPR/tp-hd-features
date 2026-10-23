// Module T: the texture part of formats/hd2gc.py (pick_level, convert_texture, patch_header, convert_bti) and
// j3d.tex_size. Module J (TEX1 rebuild, BTI members) and module CV (Pscene206 textures) call these.
#include "hdf/tex.hpp"

#include "hdf/io.hpp"

#include <algorithm>

namespace hdf::tex {

namespace {

// j3d.BPP / j3d.BLOCK (unknown formats: 16 bpp, 4x4)
void j3d_block(u8 fmt, u32& bw, u32& bh, u32& bpp) {
    switch (fmt) {
        case 0: bw = 8; bh = 8; bpp = 4; return;
        case 1: bw = 8; bh = 4; bpp = 8; return;
        case 2: bw = 8; bh = 4; bpp = 8; return;
        case 3: bw = 4; bh = 4; bpp = 16; return;
        case 4: bw = 4; bh = 4; bpp = 16; return;
        case 5: bw = 4; bh = 4; bpp = 16; return;
        case 6: bw = 4; bh = 4; bpp = 32; return;
        case 8: bw = 8; bh = 8; bpp = 4; return;
        case 9: bw = 8; bh = 4; bpp = 8; return;
        case 10: bw = 4; bh = 4; bpp = 16; return;
        case 14: bw = 8; bh = 8; bpp = 4; return;
        default: bw = 4; bh = 4; bpp = 16; return;
    }
}

}  // namespace

// j3d.tex_size(fmt, w, h, mips=1)
u64 gx_tex_size(u8 fmt, u32 w, u32 h, u32 mips) {
    u32 bw, bh, bpp;
    j3d_block(fmt, bw, bh, bpp);
    u64 total = 0;
    for (u32 i = 0; i < std::max<u32>(mips, 1); ++i) {
        const u64 ww = i < 32 ? std::max<u32>(w >> i, 1) : 1, hh = i < 32 ? std::max<u32>(h >> i, 1) : 1;
        total += ((ww + bw - 1) / bw) * ((hh + bh - 1) / bh) * (u64{bw} * bh * bpp / 8);
    }
    return total;
}

// hd2gc.pick_level(s, tw, th): the smallest GTX level whose size is >= the target
int pick_level(const GtxSurface& s, u32 tw, u32 th) {
    int best = 0;
    const u32 n = std::max<u32>(1, s.mips);
    for (u32 L = 0; L < n && L < 32; ++L) {
        const u32 w = std::max<u32>(1, s.width >> L), h = std::max<u32>(1, s.height >> L);
        if (w >= tw && h >= th) best = static_cast<int>(L);
    }
    return best;
}

// hd2gc.convert_texture(hdr, gtx_buf, surf, scale, report, label)
Result<TexConv> convert_texture(const BtiFields& hdr, const GtxSurface& surf, u32 scale) {
    const u8 fmt = hdr.format;
    u64 tw = u64{hdr.width} * scale, th = u64{hdr.height} * scale;
    tw = std::min<u64>({tw, 1024, surf.width});
    th = std::min<u64>({th, 1024, surf.height});
    TexConv r;
    r.level0 = pick_level(surf, static_cast<u32>(tw), static_cast<u32>(th));
    r.width = std::max<u32>(1, surf.width >> r.level0);
    r.height = std::max<u32>(1, surf.height >> r.level0);
    const u32 want_mips = std::max<u32>(1, hdr.mip_count);
    u8 out_fmt = (fmt <= 6 || fmt == 14) ? fmt : 5;  // palette formats -> RGB5A3
    const u32 gx2 = surf.format & 0xFFFF;
    const u32 have = std::max<u32>(1, surf.mips);
    for (u32 k = 0; k < want_mips; ++k) {
        const u32 L = static_cast<u32>(r.level0) + k;
        if (L >= have) break;
        auto li = level_info(surf, static_cast<int>(L));
        if (!li) return std::unexpected(li.error());
        auto lin = untile(surf, static_cast<int>(L));
        if (!lin) return std::unexpected(lin.error());
        const u32 w = li->width, h = li->height;
        Bytes level;
        if (out_fmt == 14 && (gx2 == 0x31 || gx2 == 0x431)) {
            level = bc1_to_cmpr(*lin, w, h);
            if (level.empty() && w && h) return fail("fmt.tex", "BC1 data too short");
        } else {
            auto rgba = to_rgba(surf, static_cast<int>(L), *lin);
            if (!rgba) return std::unexpected(rgba.error());
            if (out_fmt == 14) out_fmt = 5;  // non-BC1 source for a CMPR slot: RGB5A3
            level = encode_model(static_cast<Gx>(out_fmt), *rgba, w, h);
            if (level.empty()) return fail("fmt.tex", "GX encode failed");
        }
        r.pixels.insert(r.pixels.end(), level.begin(), level.end());
        ++r.levels;
        if (w <= 1 && h <= 1) break;
    }
    r.format = out_fmt;
    return r;
}

// hd2gc.patch_header(raw_hdr, fmt, w, h, nmips, img_off)
Bytes patch_header(View raw_hdr, u8 fmt, u32 w, u32 h, u32 nmips, u32 img_off) {
    Bytes b(raw_hdr.begin(), raw_hdr.end());
    if (b.size() < 0x20) b.resize(0x20, 0);
    b[0] = fmt;
    io::put16(b, 2, static_cast<u16>(w));
    io::put16(b, 4, static_cast<u16>(h));
    if (fmt != 8 && fmt != 9 && fmt != 10) {
        b[8] = 0;
        b[9] = 0;
        io::put16(b, 0xA, 0);  // no palette
        io::put32(b, 0xC, 0);
    }
    b[0x18] = static_cast<u8>(nmips);
    if (nmips <= 1) {
        b[0x10] = 0;                   // mipmap disabled
        if (b[0x14] > 1) b[0x14] = 1;  // min filter used mips -> GX_LINEAR
        b[0x17] = 0;                   // max LOD
    } else {
        b[0x10] = 1;
        b[0x17] = static_cast<u8>((nmips - 1) * 8);  // maxLOD in 1/8 units
    }
    io::put32(b, 0x1C, img_off);
    return b;
}

// hd2gc.convert_bti(d, gtx_buf, scale, report, label)
Result<Bytes> convert_bti(View d, std::optional<View> gtx, u32 /*scale: hd2gc passes 1 to convert_texture*/) {
    auto h = bti_fields(d, 0);
    if (!h) return std::unexpected(h.error());
    const u64 exp = gx_tex_size(h->format, h->width, h->height, h->mip_count);
    if (h->image_offset && d.size() >= u64{h->image_offset} + exp) return Bytes(d.begin(), d.end());  // has pixels
    if (!gtx || gtx->empty()) return Bytes(d.begin(), d.end());  // header-only BTI and no GTX -> left as is
    auto surfs = gtx_surfaces(*gtx);
    if (!surfs) return std::unexpected(surfs.error());
    if (surfs->empty()) return fail("fmt.tex", "GTX has no surface");
    auto c = convert_texture(*h, (*surfs)[0], 1);
    if (!c) return std::unexpected(c.error());
    Bytes out = patch_header(d.subspan(0, 0x20), c->format, c->width, c->height, c->levels, 0x20);
    out.insert(out.end(), c->pixels.begin(), c->pixels.end());
    return out;
}

}  // namespace hdf::tex
