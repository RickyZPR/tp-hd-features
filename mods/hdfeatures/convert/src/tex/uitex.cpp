// Module T: the UI GX codec (stamp book, pre-entry screen). Port of the encoder half of pipeline/stampbook.py:
// E3/E4/E5/E6, _nearest, tex_bytes, _tile, _untile, encode, decode, max_err, tex_error, choose_format,
// bti_fields, make_bti, and Source.texture after its lookup (bti_job). The encoder rounds to the nearest value
// under Aurora's ExpandTo8 bit replication; the decoder mirrors Aurora (I4/I8 put the intensity in alpha too).
#include "hdf/tex.hpp"

#include "hdf/io.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>

namespace hdf::tex {

namespace {

constexpr int kMaxFlatErr = 3;  // stampbook.MAX_FLAT_ERR

// Aurora ExpandTo8<n> tables: stampbook.E3/E4/E5/E6
struct Tables {
    std::array<u8, 8> e3{};
    std::array<u8, 16> e4{};
    std::array<u8, 32> e5{};
    std::array<u8, 64> e6{};
    std::array<u8, 256> n3{}, n4{}, n5{}, n6{};
    Tables() {
        for (u32 q = 0; q < 8; ++q) e3[q] = static_cast<u8>((q << 5) | (q << 2) | (q >> 1));
        for (u32 q = 0; q < 16; ++q) e4[q] = static_cast<u8>((q << 4) | q);
        for (u32 q = 0; q < 32; ++q) e5[q] = static_cast<u8>((q << 3) | (q >> 2));
        for (u32 q = 0; q < 64; ++q) e6[q] = static_cast<u8>((q << 2) | (q >> 4));
        nearest(e3.data(), 8, n3);
        nearest(e4.data(), 16, n4);
        nearest(e5.data(), 32, n5);
        nearest(e6.data(), 64, n6);
    }
    // stampbook._nearest: min(range(len(table)), key=lambda q: (abs(table[q] - v), q))
    static void nearest(const u8* t, int n, std::array<u8, 256>& out) {
        for (int v = 0; v < 256; ++v) {
            int best = 0;
            for (int q = 1; q < n; ++q)
                if (std::abs(t[q] - v) < std::abs(t[best] - v)) best = q;
            out[static_cast<std::size_t>(v)] = static_cast<u8>(best);
        }
    }
};

const Tables& T() {
    static const Tables t;
    return t;
}

// stampbook.TILE / BPP
bool tile_bpp(Gx f, u32& bw, u32& bh, u32& bpp) {
    switch (f) {
        case Gx::I4: bw = 8; bh = 8; bpp = 4; return true;
        case Gx::I8: bw = 8; bh = 4; bpp = 8; return true;
        case Gx::IA4: bw = 8; bh = 4; bpp = 8; return true;
        case Gx::IA8: bw = 4; bh = 4; bpp = 16; return true;
        case Gx::RGB565: bw = 4; bh = 4; bpp = 16; return true;
        case Gx::RGB5A3: bw = 4; bh = 4; bpp = 16; return true;
        case Gx::RGBA8: bw = 4; bh = 4; bpp = 32; return true;
        case Gx::CMPR: bw = 8; bh = 8; bpp = 4; return true;
        default: return false;
    }
}

// stampbook.FMT_NAME
const char* fmt_name(u32 f) {
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
        default: return nullptr;
    }
}

// stampbook._tile(units, n, w, h, bw, bh): row-major n-byte units -> GX tile order, zero outside the image
Bytes tile(const u8* units, u32 n, u32 w, u32 h, u32 bw, u32 bh) {
    const u32 tw = (w + bw - 1) / bw, th = (h + bh - 1) / bh;
    Bytes out(std::size_t{tw} * th * bw * bh * n, 0);
    std::size_t o = 0;
    const std::size_t step = std::size_t{bw} * n;
    for (u32 ty = 0; ty < th; ++ty)
        for (u32 tx = 0; tx < tw; ++tx)
            for (u32 r = 0; r < bh; ++r) {
                const u32 y = ty * bh + r;
                for (u32 k = 0; k < bw; ++k) {
                    const u32 x = tx * bw + k;
                    if (x < w && y < h) std::memcpy(out.data() + o + std::size_t{k} * n, units + (std::size_t{y} * w + x) * n, n);
                }
                o += step;
            }
    return out;
}

// stampbook._untile(data, n, w, h, bw, bh) -> row-major units (w x h)
Bytes untile_gx(View data, u32 n, u32 w, u32 h, u32 bw, u32 bh) {
    const u32 tw = (w + bw - 1) / bw, th = (h + bh - 1) / bh;
    Bytes out(std::size_t{w} * h * n, 0);
    std::size_t o = 0;
    const std::size_t step = std::size_t{bw} * n;
    for (u32 ty = 0; ty < th; ++ty)
        for (u32 tx = 0; tx < tw; ++tx)
            for (u32 r = 0; r < bh; ++r) {
                const u32 y = ty * bh + r;
                // rows[y][tx*step : +step] = data[o : o + step] (short data leaves zeros)
                for (u32 k = 0; k < bw; ++k) {
                    const u32 x = tx * bw + k;
                    for (u32 b = 0; b < n; ++b) {
                        const std::size_t src = o + std::size_t{k} * n + b;
                        if (x < w && y < h && src < data.size()) out[(std::size_t{y} * w + x) * n + b] = data[src];
                    }
                }
                o += step;
            }
    return out;
}

}  // namespace

// stampbook.tex_bytes(fmt, w, h)
u64 tex_bytes(Gx f, u32 w, u32 h) {
    u32 bw = 0, bh = 0, bpp = 0;
    if (!tile_bpp(f, bw, bh, bpp)) return 0;
    return u64{(w + bw - 1) / bw} * ((h + bh - 1) / bh) * bw * bh * bpp / 8;
}

// stampbook.encode(fmt, rgba, w, h)
Bytes encode_ui(Gx f, View rgba, u32 w, u32 h) {
    const std::size_t n = std::size_t{w} * h;
    if (rgba.size() != n * 4) return {};
    const Tables& t = T();
    const u8* p = rgba.data();
    switch (f) {
        case Gx::RGBA8: {
            Bytes ar(2 * n), gb(2 * n);
            for (std::size_t i = 0; i < n; ++i) {
                ar[2 * i] = p[4 * i + 3];
                ar[2 * i + 1] = p[4 * i];
                gb[2 * i] = p[4 * i + 1];
                gb[2 * i + 1] = p[4 * i + 2];
            }
            const Bytes t_ar = tile(ar.data(), 2, w, h, 4, 4), t_gb = tile(gb.data(), 2, w, h, 4, 4);
            Bytes out;
            out.reserve(t_ar.size() * 2);
            for (std::size_t i = 0; i < t_ar.size(); i += 32) {
                out.insert(out.end(), t_ar.begin() + static_cast<std::ptrdiff_t>(i), t_ar.begin() + static_cast<std::ptrdiff_t>(i + 32));
                out.insert(out.end(), t_gb.begin() + static_cast<std::ptrdiff_t>(i), t_gb.begin() + static_cast<std::ptrdiff_t>(i + 32));
            }
            return out;
        }
        case Gx::IA8: {
            Bytes ia(2 * n);
            for (std::size_t i = 0; i < n; ++i) {
                ia[2 * i] = p[4 * i + 3];
                ia[2 * i + 1] = p[4 * i];
            }
            return tile(ia.data(), 2, w, h, 4, 4);
        }
        case Gx::IA4: {
            Bytes u(n);
            for (std::size_t i = 0; i < n; ++i) u[i] = static_cast<u8>((t.n4[p[4 * i + 3]] << 4) | t.n4[p[4 * i]]);
            return tile(u.data(), 1, w, h, 8, 4);
        }
        case Gx::I8: {
            Bytes u(n);
            for (std::size_t i = 0; i < n; ++i) u[i] = p[4 * i];
            return tile(u.data(), 1, w, h, 8, 4);
        }
        case Gx::I4: {
            Bytes u(n);
            for (std::size_t i = 0; i < n; ++i) u[i] = t.n4[p[4 * i]];
            const Bytes tt = tile(u.data(), 1, w, h, 8, 8);
            Bytes out(tt.size() / 2);
            for (std::size_t i = 0; i + 1 < tt.size(); i += 2) out[i / 2] = static_cast<u8>((tt[i] << 4) | tt[i + 1]);
            return out;
        }
        case Gx::RGB565: {
            Bytes u(2 * n);
            for (std::size_t i = 0; i < n; ++i) {
                const u32 v = (u32{t.n5[p[4 * i]]} << 11) | (u32{t.n6[p[4 * i + 1]]} << 5) | t.n5[p[4 * i + 2]];
                u[2 * i] = static_cast<u8>(v >> 8);
                u[2 * i + 1] = static_cast<u8>(v);
            }
            return tile(u.data(), 2, w, h, 4, 4);
        }
        case Gx::RGB5A3: {
            Bytes u(2 * n);
            for (std::size_t i = 0; i < n; ++i) {
                const u8 al = p[4 * i + 3];
                const u32 q = t.n3[al];
                u32 v;
                if (q == 7 && std::abs(255 - al) <= std::abs(t.e3[q] - al))
                    v = 0x8000u | (u32{t.n5[p[4 * i]]} << 10) | (u32{t.n5[p[4 * i + 1]]} << 5) | t.n5[p[4 * i + 2]];
                else
                    v = (q << 12) | (u32{t.n4[p[4 * i]]} << 8) | (u32{t.n4[p[4 * i + 1]]} << 4) | t.n4[p[4 * i + 2]];
                u[2 * i] = static_cast<u8>(v >> 8);
                u[2 * i + 1] = static_cast<u8>(v);
            }
            return tile(u.data(), 2, w, h, 4, 4);
        }
        default: return {};  // NotImplementedError
    }
}

// stampbook.decode(fmt, data, w, h)
Bytes decode_ui(Gx f, View data, u32 w, u32 h) {
    const std::size_t n = std::size_t{w} * h;
    const Tables& t = T();
    Bytes out(n * 4, 0);
    switch (f) {
        case Gx::RGBA8: {
            // tiles of 64: AR halves then GB halves
            Bytes ar_t, gb_t;
            for (std::size_t i = 0; i < data.size(); i += 64) {
                const std::size_t e = std::min(data.size(), i + 64);
                const std::size_t m = std::min(e, i + 32);
                ar_t.insert(ar_t.end(), data.begin() + static_cast<std::ptrdiff_t>(i), data.begin() + static_cast<std::ptrdiff_t>(m));
                gb_t.insert(gb_t.end(), data.begin() + static_cast<std::ptrdiff_t>(m), data.begin() + static_cast<std::ptrdiff_t>(e));
            }
            const Bytes ar = untile_gx(ar_t, 2, w, h, 4, 4), gb = untile_gx(gb_t, 2, w, h, 4, 4);
            for (std::size_t i = 0; i < n; ++i) {
                out[4 * i] = ar[2 * i + 1];
                out[4 * i + 1] = gb[2 * i];
                out[4 * i + 2] = gb[2 * i + 1];
                out[4 * i + 3] = ar[2 * i];
            }
            return out;
        }
        case Gx::IA8: {
            const Bytes ia = untile_gx(data, 2, w, h, 4, 4);
            for (std::size_t i = 0; i < n; ++i) {
                out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = ia[2 * i + 1];
                out[4 * i + 3] = ia[2 * i];
            }
            return out;
        }
        case Gx::IA4: {
            const Bytes v = untile_gx(data, 1, w, h, 8, 4);
            for (std::size_t i = 0; i < n; ++i) {
                out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = t.e4[v[i] & 15];
                out[4 * i + 3] = t.e4[v[i] >> 4];
            }
            return out;
        }
        case Gx::I8:
        case Gx::I4: {
            Bytes v;
            if (f == Gx::I4) {
                Bytes nib(data.size() * 2);
                for (std::size_t i = 0; i < data.size(); ++i) {
                    nib[2 * i] = t.e4[(data[i] >> 4) & 15];
                    nib[2 * i + 1] = t.e4[data[i] & 15];
                }
                v = untile_gx(nib, 1, w, h, 8, 8);
            } else {
                v = untile_gx(data, 1, w, h, 8, 4);
            }
            for (std::size_t i = 0; i < n; ++i) out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = out[4 * i + 3] = v[i];
            return out;
        }
        case Gx::RGB565:
        case Gx::RGB5A3: {
            const Bytes u = untile_gx(data, 2, w, h, 4, 4);
            for (std::size_t i = 0; i < n; ++i) {
                const u32 v = (u32{u[2 * i]} << 8) | u[2 * i + 1];
                u8* px = out.data() + 4 * i;
                if (f == Gx::RGB565) {
                    px[0] = t.e5[v >> 11];
                    px[1] = t.e6[(v >> 5) & 63];
                    px[2] = t.e5[v & 31];
                    px[3] = 255;
                } else if (v & 0x8000) {
                    px[0] = t.e5[(v >> 10) & 31];
                    px[1] = t.e5[(v >> 5) & 31];
                    px[2] = t.e5[v & 31];
                    px[3] = 255;
                } else {
                    px[0] = t.e4[(v >> 8) & 15];
                    px[1] = t.e4[(v >> 4) & 15];
                    px[2] = t.e4[v & 15];
                    px[3] = t.e3[(v >> 12) & 7];
                }
            }
            return out;
        }
        default: return {};  // NotImplementedError
    }
}

// stampbook.tex_error(back, src) -> (visible, transparent)
TexError tex_error(View back, View src) {
    TexError r;
    for (std::size_t i = 0; i + 3 < src.size() && i + 3 < back.size(); i += 4) {
        int e = 0;
        for (int k = 0; k < 4; ++k) e = std::max(e, std::abs(int{back[i + k]} - int{src[i + k]}));
        if (src[i + 3])
            r.visible = std::max(r.visible, e);
        else
            r.transparent = std::max(r.transparent, e);
    }
    return r;
}

// stampbook.choose_format(rgba, hd_fmt) -> (GX format, reason)
FormatChoice choose_ui_format(View rgba, u8 hd_fmt) {
    const Tables& t = T();
    const std::size_t n = rgba.size() / 4;
    const u8* p = rgba.data();
    bool grey = true, a_eq_r = true;
    for (std::size_t i = 0; i < n && grey; ++i)
        if (p[4 * i] != p[4 * i + 1] || p[4 * i + 1] != p[4 * i + 2]) grey = false;
    if (grey) {
        bool all17 = true;  // every value in set(r) | set(a) is a multiple of 17
        for (std::size_t i = 0; i < n; ++i) {
            if (p[4 * i + 3] != p[4 * i]) a_eq_r = false;
            if (p[4 * i] % 17 != 0 || p[4 * i + 3] % 17 != 0) all17 = false;
        }
        if (a_eq_r)
            return all17 ? FormatChoice{Gx::I4, "greyscale, alpha = intensity, 4-bit exact"}
                         : FormatChoice{Gx::I8, "greyscale, alpha = intensity"};
        if (all17) return {Gx::IA4, "greyscale, 4-bit exact"};
        return {Gx::IA8, "greyscale: lossless"};
    }
    bool alpha01 = true, alpha255 = true;
    std::set<std::tuple<u8, u8, u8>> vis;
    bool r5 = true, b5 = true, g6 = true;
    std::array<bool, 256> in5{}, in6{};
    for (u8 v : t.e5) in5[v] = true;
    for (u8 v : t.e6) in6[v] = true;
    for (std::size_t i = 0; i < n; ++i) {
        const u8 al = p[4 * i + 3];
        if (al != 0 && al != 255) alpha01 = false;
        if (al != 255) alpha255 = false;
        if (al && vis.size() < 2) vis.insert({p[4 * i], p[4 * i + 1], p[4 * i + 2]});
        if (!in5[p[4 * i]]) r5 = false;
        if (!in6[p[4 * i + 1]]) g6 = false;
        if (!in5[p[4 * i + 2]]) b5 = false;
    }
    if (alpha01 && vis.size() == 1) {
        const auto [x, y, z] = *vis.begin();
        char col[16];
        std::snprintf(col, sizeof col, "#%02X%02X%02X", x, y, z);
        const char* hn = fmt_name(hd_fmt);
        const std::string hds = hn ? std::string(hn) : std::to_string(hd_fmt);
        return {Gx::RGB5A3, "flat ink " + std::string(col) + ", alpha 0/255: one colour, no gradient (HD header " + hds + ")"};
    }
    // alphas == {255}: n > 0 and every alpha is 255
    if (n > 0 && alpha255 && r5 && b5 && g6) return {Gx::RGB565, "opaque and 565-exact"};
    return {Gx::RGBA8, "continuous tone / soft alpha: lossless"};
}

// stampbook.bti_fields(d, o)
Result<BtiFields> bti_fields(View d, std::size_t o) {
    if (o > d.size() || d.size() - o < 0x20) return fail("fmt.tex", "BTI header cut");
    BtiFields f;
    const u8* b = d.data() + o;
    f.format = b[0];
    f.alpha = b[1];
    f.width = io::be16(d, o + 2);
    f.height = io::be16(d, o + 4);
    f.wrap_s = b[6];
    f.wrap_t = b[7];
    f.palette_enabled = b[8];
    f.palette_format = b[9];
    f.palette_count = io::be16(d, o + 0xA);
    f.palette_offset = io::be32(d, o + 0xC);
    f.mipmap_enabled = b[0x10];
    f.edge_lod = b[0x11];
    f.bias_clamp = b[0x12];
    f.max_aniso = b[0x13];
    f.min_filter = b[0x14];
    f.mag_filter = b[0x15];
    f.min_lod = static_cast<i8>(b[0x16]);
    f.max_lod = static_cast<i8>(b[0x17]);
    f.mip_count = b[0x18];
    f.unk19 = b[0x19];
    f.lod_bias = static_cast<i16>(io::be16(d, o + 0x1A));
    f.image_offset = io::be32(d, o + 0x1C);
    return f;
}

// stampbook.make_bti(t) -> (bytes, info, back)
Result<BtiMade> make_bti(const BtiJob& t) {
    const std::size_t n = std::size_t{t.w} * t.h;
    if (t.rgba.size() != n * 4) return fail("fmt.tex", "make_bti: RGBA size");
    BtiMade m;
    FormatChoice c = choose_ui_format(t.rgba, t.hdr.format);
    Bytes pix = encode_ui(c.format, t.rgba, t.w, t.h);
    Bytes back = decode_ui(c.format, pix, t.w, t.h);
    TexError e = tex_error(back, t.rgba);
    if (c.format == Gx::RGB5A3) {
        bool alpha_same = true;
        for (std::size_t i = 0; i < n && alpha_same; ++i)
            if (back[4 * i + 3] != t.rgba[4 * i + 3]) alpha_same = false;
        if (e.visible > kMaxFlatErr || e.transparent > 4 * kMaxFlatErr || !alpha_same) {
            c.why = "flat ink but RGB5A3 error " + std::to_string(e.visible) + "/" + std::to_string(e.transparent) +
                    " too high: lossless";
            c.format = Gx::RGBA8;
            pix = encode_ui(c.format, t.rgba, t.w, t.h);
            back = decode_ui(c.format, pix, t.w, t.h);
            e = tex_error(back, t.rgba);
        }
    }
    if (pix.size() != tex_bytes(c.format, t.w, t.h)) return fail("fmt.tex", "make_bti: encoder size");
    u8 alpha;
    if (t.hdr.format == 8 || t.hdr.format == 9 || t.hdr.format == 10) {  // HD palette header
        bool only255 = true, only01 = true;
        for (std::size_t i = 0; i < n; ++i) {
            const u8 a = t.rgba[4 * i + 3];
            if (a != 255) only255 = false;
            if (a != 0 && a != 255) only01 = false;
        }
        // al == {255} needs a non-empty image; set() <= {0, 255} holds for an empty one
        alpha = (n > 0 && only255) ? 0 : (only01 ? 1 : 2);
    } else {
        alpha = t.hdr.alpha;
    }
    Bytes b(t.stub.begin(), t.stub.end());
    b[0] = static_cast<u8>(c.format);
    b[1] = alpha;
    io::put16(b, 2, static_cast<u16>(t.w));
    io::put16(b, 4, static_cast<u16>(t.h));
    b[8] = b[9] = 0;  // no palette
    io::put16(b, 0xA, 0);
    io::put32(b, 0xC, 0);
    b[0x10] = 0;      // one image, mipmapping off
    if (b[0x14] > 1) b[0x14] = 1;
    b[0x17] = 0;
    b[0x18] = 1;
    io::put32(b, 0x1C, 0x20);
    b.insert(b.end(), pix.begin(), pix.end());
    m.bti = std::move(b);
    m.format = c.format;
    m.reason = std::move(c.why);
    m.error = e;
    m.decoded = std::move(back);
    return m;
}

// stampbook.Source.texture(member_path) after the lookup
Result<BtiJob> bti_job(View stub, View gtx_member) {
    if (stub.size() < 0x20) return fail("fmt.tex", "BTI stub cut");
    auto hdr = bti_fields(stub, 0);
    if (!hdr) return std::unexpected(hdr.error());
    auto surfs = gtx_surfaces(gtx_member);
    if (!surfs) return std::unexpected(surfs.error());
    if (surfs->empty()) return fail("fmt.tex", "GTX has no surface");  // surfs[0] IndexError
    const GtxSurface& s = (*surfs)[0];
    auto li = level_info(s, 0);
    if (!li) return std::unexpected(li.error());
    auto lin = untile(s, 0);
    if (!lin) return std::unexpected(lin.error());
    auto rgba = to_rgba(s, 0, *lin);
    if (!rgba) return std::unexpected(rgba.error());
    BtiJob j;
    std::copy(stub.begin(), stub.begin() + 0x20, j.stub.begin());
    j.hdr = *hdr;
    j.w = li->width;
    j.h = li->height;
    j.rgba = std::move(*rgba);
    j.gtx_format = s.format;
    return j;
}

}  // namespace hdf::tex
