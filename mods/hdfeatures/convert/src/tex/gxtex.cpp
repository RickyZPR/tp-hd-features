// Module T: GameCube GX texture helpers of the model family. Port of formats/gxtex.py (bc1_to_cmpr, _565,
// cmpr_to_rgba, _tiles, _px, _lum, encode, decode, box_downscale). The encoder truncates (x >> 3 etc.) and turns
// colour into intensity with _lum: it is the hd2gc/particles family, NOT stampbook's nearest-value encoder.
#include "hdf/tex.hpp"

#include <algorithm>
#include <cstring>

namespace hdf::tex {

namespace {

// gxtex.TILE
bool tile_of(Gx f, u32& bw, u32& bh) {
    switch (f) {
        case Gx::I4: bw = 8; bh = 8; return true;
        case Gx::I8: bw = 8; bh = 4; return true;
        case Gx::IA4: bw = 8; bh = 4; return true;
        case Gx::IA8: case Gx::RGB565: case Gx::RGB5A3: case Gx::RGBA8: bw = 4; bh = 4; return true;
        case Gx::CMPR: bw = 8; bh = 8; return true;
        default: return false;
    }
}

// gxtex._REV2: reverse the four 2-bit fields of a byte
constexpr u8 rev2(u32 b) {
    return static_cast<u8>(((b & 3) << 6) | (((b >> 2) & 3) << 4) | (((b >> 4) & 3) << 2) | ((b >> 6) & 3));
}

// gxtex._565
inline void c565(u32 c, u8* p) {
    const u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    p[0] = static_cast<u8>((r << 3) | (r >> 2));
    p[1] = static_cast<u8>((g << 2) | (g >> 4));
    p[2] = static_cast<u8>((b << 3) | (b >> 2));
}

// gxtex._lum
inline u32 lum(const u8* p) { return (p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8; }

// p[0] if p[0] == p[1] == p[2] else _lum(p)
inline u32 inten(const u8* p) { return (p[0] == p[1] && p[1] == p[2]) ? p[0] : lum(p); }

// gxtex.cmpr_to_rgba(data, w, h)
Bytes cmpr_to_rgba(View data, u32 w, u32 h) {
    Bytes out(std::size_t{w} * h * 4, 0);
    const u32 tw = (w + 7) / 8, th = (h + 7) / 8;
    std::size_t o = 0;
    for (u32 ty = 0; ty < th; ++ty)
        for (u32 tx = 0; tx < tw; ++tx)
            for (u32 sy = 0; sy < 2; ++sy)
                for (u32 sx = 0; sx < 2; ++sx) {
                    if (o + 8 > data.size()) return {};  // struct.error
                    const u32 c0 = (data[o] << 8) | data[o + 1], c1 = (data[o + 2] << 8) | data[o + 3];
                    u8 pal[4][4];
                    c565(c0, pal[0]);
                    c565(c1, pal[1]);
                    pal[0][3] = pal[1][3] = 255;
                    if (c0 > c1) {
                        for (int k = 0; k < 3; ++k) {
                            pal[2][k] = static_cast<u8>((2 * pal[0][k] + pal[1][k]) / 3);
                            pal[3][k] = static_cast<u8>((pal[0][k] + 2 * pal[1][k]) / 3);
                        }
                        pal[2][3] = pal[3][3] = 255;
                    } else {
                        for (int k = 0; k < 3; ++k) pal[2][k] = static_cast<u8>((pal[0][k] + pal[1][k]) / 2);
                        pal[2][3] = 255;
                        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
                    }
                    for (u32 py = 0; py < 4; ++py) {
                        const u32 row = data[o + 4 + py];
                        for (u32 px = 0; px < 4; ++px) {
                            const u32 x = tx * 8 + sx * 4 + px, y = ty * 8 + sy * 4 + py;
                            if (x < w && y < h) {
                                const u32 idx = (row >> (6 - 2 * px)) & 3;
                                std::memcpy(out.data() + (std::size_t{y} * w + x) * 4, pal[idx], 4);
                            }
                        }
                    }
                    o += 8;
                }
    return out;
}

}  // namespace

// gxtex.bc1_to_cmpr(lin, w, h)
Bytes bc1_to_cmpr(View lin, u32 w, u32 h) {
    const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    const u32 tw = (bw + 1) / 2, th = (bh + 1) / 2;
    Bytes out(std::size_t{tw} * th * 32, 0);
    std::size_t o = 0;
    for (u32 ty = 0; ty < th; ++ty)
        for (u32 tx = 0; tx < tw; ++tx)
            for (u32 sy = 0; sy < 2; ++sy)
                for (u32 sx = 0; sx < 2; ++sx) {
                    const u32 bx = tx * 2 + sx, by = ty * 2 + sy;
                    if (bx < bw && by < bh) {
                        const std::size_t s = (std::size_t{by} * bw + bx) * 8;
                        if (s + 8 > lin.size()) return {};  // struct.error / IndexError
                        out[o] = lin[s + 1];                // <HH -> >HH
                        out[o + 1] = lin[s];
                        out[o + 2] = lin[s + 3];
                        out[o + 3] = lin[s + 2];
                        for (u32 r = 0; r < 4; ++r) out[o + 4 + r] = rev2(lin[s + 4 + r]);
                    }
                    o += 8;
                }
    return out;
}

// gxtex.encode(fmt, rgba, w, h)
Bytes encode_model(Gx f, View rgba, u32 w, u32 h) {
    u32 bw = 0, bh = 0;
    if (!tile_of(f, bw, bh) || f == Gx::CMPR) return {};  // NotImplementedError (CMPR: use bc1_to_cmpr)
    if (rgba.size() < std::size_t{w} * h * 4) return {};  // IndexError in _px
    Bytes out;
    const u32 ntx = (w + bw - 1) / bw, nty = (h + bh - 1) / bh;
    out.reserve(std::size_t{ntx} * nty * bw * bh * 4);
    static constexpr u8 zero[4] = {0, 0, 0, 0};
    auto px = [&](u32 x, u32 y) -> const u8* {  // gxtex._px
        if (x >= w || y >= h) return zero;
        return rgba.data() + (std::size_t{y} * w + x) * 4;
    };
    std::vector<u8> nib;
    for (u32 ty = 0; ty < nty; ++ty) {
        for (u32 tx = 0; tx < ntx; ++tx) {
            const u32 x0 = tx * bw, y0 = ty * bh;
            if (f == Gx::RGBA8) {
                u8 ar[32], gb[32];
                int k = 0;
                for (u32 y = 0; y < bh; ++y)
                    for (u32 x = 0; x < bw; ++x) {
                        const u8* p = px(x0 + x, y0 + y);
                        ar[k] = p[3];
                        ar[k + 1] = p[0];
                        gb[k] = p[1];
                        gb[k + 1] = p[2];
                        k += 2;
                    }
                out.insert(out.end(), ar, ar + 32);
                out.insert(out.end(), gb, gb + 32);
                continue;
            }
            nib.clear();
            for (u32 y = 0; y < bh; ++y) {
                for (u32 x = 0; x < bw; ++x) {
                    const u8* p = px(x0 + x, y0 + y);
                    switch (f) {
                        case Gx::I8: out.push_back(static_cast<u8>(inten(p))); break;
                        case Gx::I4: nib.push_back(static_cast<u8>(inten(p) >> 4)); break;
                        case Gx::IA4: out.push_back(static_cast<u8>(((p[3] >> 4) << 4) | (inten(p) >> 4))); break;
                        case Gx::IA8:
                            out.push_back(p[3]);
                            out.push_back(static_cast<u8>(inten(p)));
                            break;
                        case Gx::RGB565: {
                            const u32 v = ((p[0] >> 3u) << 11) | ((p[1] >> 2u) << 5) | (p[2] >> 3u);
                            out.push_back(static_cast<u8>(v >> 8));
                            out.push_back(static_cast<u8>(v));
                            break;
                        }
                        case Gx::RGB5A3: {
                            u32 v;
                            if (p[3] >= 0xE0)
                                v = 0x8000u | ((p[0] >> 3u) << 10) | ((p[1] >> 3u) << 5) | (p[2] >> 3u);
                            else
                                v = ((p[3] >> 5u) << 12) | ((p[0] >> 4u) << 8) | ((p[1] >> 4u) << 4) | (p[2] >> 4u);
                            out.push_back(static_cast<u8>(v >> 8));
                            out.push_back(static_cast<u8>(v));
                            break;
                        }
                        default: return {};
                    }
                }
            }
            if (f == Gx::I4)
                for (std::size_t i = 0; i + 1 < nib.size(); i += 2) out.push_back(static_cast<u8>((nib[i] << 4) | nib[i + 1]));
        }
    }
    return out;
}

// gxtex.decode(fmt, data, w, h)
Bytes decode_model(Gx f, View data, u32 w, u32 h) {
    if (f == Gx::CMPR) return cmpr_to_rgba(data, w, h);
    u32 bw = 0, bh = 0;
    if (!tile_of(f, bw, bh)) return {};
    Bytes out(std::size_t{w} * h * 4, 0);
    std::size_t o = 0;
    const u32 ntx = (w + bw - 1) / bw, nty = (h + bh - 1) / bh;
    for (u32 ty = 0; ty < nty; ++ty) {
        for (u32 tx = 0; tx < ntx; ++tx) {
            const u32 x0 = tx * bw, y0 = ty * bh;
            if (f == Gx::RGBA8) {
                if (o + 64 > data.size()) return {};
                for (u32 i = 0; i < 16; ++i) {
                    const u32 x = x0 + i % 4, y = y0 + i / 4;
                    const u8 a = data[o + 2 * i], r = data[o + 2 * i + 1];
                    const u8 g = data[o + 32 + 2 * i], b = data[o + 33 + 2 * i];
                    if (x < w && y < h) {
                        u8* d = out.data() + (std::size_t{y} * w + x) * 4;
                        d[0] = r;
                        d[1] = g;
                        d[2] = b;
                        d[3] = a;
                    }
                }
                o += 64;
                continue;
            }
            const std::size_t step = f == Gx::I4 ? bw * bh / 2 : (f == Gx::I8 || f == Gx::IA4) ? bw * bh : bw * bh * 2;
            if (o + step > data.size()) return {};
            for (u32 i = 0; i < bw * bh; ++i) {
                const u32 x = x0 + i % bw, y = y0 + i / bw;
                u8 p[4];
                switch (f) {
                    case Gx::I4: {
                        u32 v = data[o + i / 2];
                        v = (i % 2 == 0) ? (v >> 4) : (v & 15);
                        v *= 17;
                        p[0] = p[1] = p[2] = static_cast<u8>(v);
                        p[3] = 255;
                        break;
                    }
                    case Gx::I8:
                        p[0] = p[1] = p[2] = data[o + i];
                        p[3] = 255;
                        break;
                    case Gx::IA4: {
                        const u32 v = data[o + i];
                        p[0] = p[1] = p[2] = static_cast<u8>((v & 15) * 17);
                        p[3] = static_cast<u8>((v >> 4) * 17);
                        break;
                    }
                    case Gx::IA8:
                        p[3] = data[o + 2 * i];
                        p[0] = p[1] = p[2] = data[o + 2 * i + 1];
                        break;
                    case Gx::RGB565:
                        c565((data[o + 2 * i] << 8) | data[o + 2 * i + 1], p);
                        p[3] = 255;
                        break;
                    case Gx::RGB5A3: {
                        const u32 v = (data[o + 2 * i] << 8) | data[o + 2 * i + 1];
                        if (v & 0x8000) {
                            p[0] = static_cast<u8>(((v >> 10) & 31) * 255 / 31);
                            p[1] = static_cast<u8>(((v >> 5) & 31) * 255 / 31);
                            p[2] = static_cast<u8>((v & 31) * 255 / 31);
                            p[3] = 255;
                        } else {
                            p[0] = static_cast<u8>(((v >> 8) & 15) * 17);
                            p[1] = static_cast<u8>(((v >> 4) & 15) * 17);
                            p[2] = static_cast<u8>((v & 15) * 17);
                            p[3] = static_cast<u8>(((v >> 12) & 7) * 255 / 7);
                        }
                        break;
                    }
                    default: return {};
                }
                if (x < w && y < h) std::memcpy(out.data() + (std::size_t{y} * w + x) * 4, p, 4);
            }
            o += step;
        }
    }
    return out;
}

// gxtex.box_downscale(rgba, w, h, nw, nh)
Bytes box_downscale(View rgba, u32 w, u32 h, u32 nw, u32 nh) {
    if (nw == 0 || nh == 0) return {};  // ZeroDivisionError
    const u32 fx = std::max<u32>(1, w / nw), fy = std::max<u32>(1, h / nh);
    Bytes out(std::size_t{nw} * nh * 4, 0);
    const u64 n = u64{fx} * fy;
    for (u32 y = 0; y < nh; ++y) {
        for (u32 x = 0; x < nw; ++x) {
            u64 acc[4] = {0, 0, 0, 0};
            for (u32 yy = 0; yy < fy; ++yy) {
                const u64 row = ((u64{y} * fy + yy) * w + u64{x} * fx) * 4;
                for (u32 xx = 0; xx < fx; ++xx) {
                    const u64 o = row + u64{xx} * 4;
                    if (o + 4 > rgba.size()) return {};  // IndexError
                    for (int c = 0; c < 4; ++c) acc[c] += rgba[o + c];
                }
            }
            u8* d = out.data() + (std::size_t{y} * nw + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<u8>(acc[c] / n);
        }
    }
    return out;
}

}  // namespace hdf::tex
