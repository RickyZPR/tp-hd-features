// Module T: the stamp item-get icons. Port of gen_stamp_table.py make_icon_rgba, _to565, _from565, bc1_encode,
// bti_cmpr (build_icons' per-icon part). make_icon_rgba is double math: the operation order of the Python is
// kept exactly, and int(round(x)) is io::py_round_int (half to even).
#include "hdf/tex.hpp"

#include "hdf/io.hpp"

#include <algorithm>
#include <cmath>

namespace hdf::tex {

namespace {

constexpr u8 kPaper[3] = {0xF4, 0xEC, 0xD6};  // gen_stamp_table.PAPER
constexpr u8 kEdge[3] = {0x7A, 0x5A, 0x30};   // gen_stamp_table.EDGE
constexpr int kSize = 48, kMargin = 4;

// gen_stamp_table._to565 / _from565
u32 to565(const u8* p) { return ((u32{p[0]} >> 3) << 11) | ((u32{p[1]} >> 2) << 5) | (u32{p[2]} >> 3); }
void from565(u32 c, int* p) {
    const int r = static_cast<int>((c >> 11) & 31), g = static_cast<int>((c >> 5) & 63), b = static_cast<int>(c & 31);
    p[0] = (r << 3) | (r >> 2);
    p[1] = (g << 2) | (g >> 4);
    p[2] = (b << 3) | (b >> 2);
}

// Python int(x) of a float: truncation toward zero
i64 py_int(double x) { return static_cast<i64>(std::trunc(x)); }

}  // namespace

// gen_stamp_table.make_icon_rgba(w, h, rgba, size=48, margin=4)
Bytes make_icon_rgba(View rgba, u32 w32, u32 h32) {
    const i64 w = w32, h = h32;
    if (static_cast<u64>(rgba.size()) < static_cast<u64>(w) * static_cast<u64>(h) * 4 || w == 0 || h == 0) return {};
    const int size = kSize, margin = kMargin;
    const int inner = size - 2 * margin;
    const double scale = static_cast<double>(std::max(w, h)) / static_cast<double>(inner);
    Bytes out(static_cast<std::size_t>(size) * size * 4, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool border = x == 0 || y == 0 || x == size - 1 || y == size - 1;
            int r = border ? kEdge[0] : kPaper[0];
            int g = border ? kEdge[1] : kPaper[1];
            int b = border ? kEdge[2] : kPaper[2];
            const int ix = x - margin, iy = y - margin;
            const double ox = (static_cast<double>(inner) - static_cast<double>(w) / scale) / 2.0;
            const double oy = (static_cast<double>(inner) - static_cast<double>(h) / scale) / 2.0;
            const double sx0 = (static_cast<double>(ix) - ox) * scale, sy0 = (static_cast<double>(iy) - oy) * scale;
            const double sx1 = sx0 + scale, sy1 = sy0 + scale;
            if (!border && 0 <= ix && ix < inner && 0 <= iy && iy < inner && sx1 > 0 && sy1 > 0 &&
                sx0 < static_cast<double>(w) && sy0 < static_cast<double>(h)) {
                double acc[4] = {0.0, 0.0, 0.0, 0.0};
                double tot = 0.0;
                i64 yy = std::max<i64>(0, py_int(sy0));
                while (yy < std::min<i64>(h, py_int(sy1) + 1)) {
                    const double wy = std::min(sy1, static_cast<double>(yy + 1)) - std::max(sy0, static_cast<double>(yy));
                    i64 xx = std::max<i64>(0, py_int(sx0));
                    while (xx < std::min<i64>(w, py_int(sx1) + 1)) {
                        const double wx = std::min(sx1, static_cast<double>(xx + 1)) - std::max(sx0, static_cast<double>(xx));
                        const double wgt = wx * wy;
                        if (wgt > 0) {
                            const std::size_t o = static_cast<std::size_t>((yy * w + xx) * 4);
                            const double a = rgba[o + 3] / 255.0;
                            acc[0] += static_cast<double>(rgba[o]) * a * wgt;
                            acc[1] += static_cast<double>(rgba[o + 1]) * a * wgt;
                            acc[2] += static_cast<double>(rgba[o + 2]) * a * wgt;
                            acc[3] += a * wgt;
                            tot += wgt;
                        }
                        xx += 1;
                    }
                    yy += 1;
                }
                if (tot > 0 && acc[3] > 0) {
                    const double a = acc[3] / tot;
                    const double cr = acc[0] / acc[3], cg = acc[1] / acc[3], cb = acc[2] / acc[3];
                    r = static_cast<int>(io::py_round_int(cr * a + static_cast<double>(r) * (1 - a)));
                    g = static_cast<int>(io::py_round_int(cg * a + static_cast<double>(g) * (1 - a)));
                    b = static_cast<int>(io::py_round_int(cb * a + static_cast<double>(b) * (1 - a)));
                }
            }
            u8* d = out.data() + (static_cast<std::size_t>(y) * size + x) * 4;
            d[0] = static_cast<u8>(r);  // bytes((r, g, b, 255)): values stay in 0..255 (convex blend)
            d[1] = static_cast<u8>(g);
            d[2] = static_cast<u8>(b);
            d[3] = 255;
        }
    }
    return out;
}

// gen_stamp_table.bc1_encode(rgba, w, h): small opaque-only BC1 encoder, linear little-endian blocks
Bytes bc1_encode(View rgba, u32 w, u32 h) {
    if (rgba.size() < std::size_t{w} * h * 4 || w % 4 || h % 4) return {};  // Python reads past rows otherwise
    Bytes out;
    out.reserve(std::size_t{w} * h / 2);
    for (u32 by = 0; by < h; by += 4) {
        for (u32 bx = 0; bx < w; bx += 4) {
            const u8* px[16];
            int lum[16];
            for (u32 y = 0; y < 4; ++y)
                for (u32 x = 0; x < 4; ++x) {
                    const u8* p = rgba.data() + ((std::size_t{by} + y) * w + (bx + x)) * 4;
                    px[y * 4 + x] = p;
                    lum[y * 4 + x] = p[0] * 77 + p[1] * 150 + p[2] * 29;
                }
            int imax = 0, imin = 0;  // lum.index(max(lum)), lum.index(min(lum)): the first one
            for (int i = 1; i < 16; ++i) {
                if (lum[i] > lum[imax]) imax = i;
                if (lum[i] < lum[imin]) imin = i;
            }
            u32 c0 = to565(px[imax]), c1 = to565(px[imin]);
            if (c0 < c1) std::swap(c0, c1);
            auto put = [&](u32 a, u32 b, u32 bits) {  // struct.pack("<HHI", ...)
                out.push_back(static_cast<u8>(a));
                out.push_back(static_cast<u8>(a >> 8));
                out.push_back(static_cast<u8>(b));
                out.push_back(static_cast<u8>(b >> 8));
                for (int k = 0; k < 4; ++k) out.push_back(static_cast<u8>(bits >> (8 * k)));
            };
            if (c0 == c1) {
                put(c0, c1, 0);
                continue;
            }
            int pal[4][3];
            from565(c0, pal[0]);
            from565(c1, pal[1]);
            for (int k = 0; k < 3; ++k) {
                pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
                pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
            }
            u32 bits = 0;
            for (int i = 0; i < 16; ++i) {
                int best = 0, best_d = -1;  // min(range(4), key=...): the first minimal
                for (int k = 0; k < 4; ++k) {
                    int d = 0;
                    for (int j = 0; j < 3; ++j) {
                        const int e = px[i][j] - pal[k][j];
                        d += e * e;
                    }
                    if (best_d < 0 || d < best_d) {
                        best_d = d;
                        best = k;
                    }
                }
                bits |= static_cast<u32>(best) << (2 * i);
            }
            put(c0, c1, bits);
        }
    }
    return out;
}

// gen_stamp_table.bti_cmpr(rgba, size)
Bytes bti_cmpr(View rgba, u32 w, u32 h) {
    const Bytes bc1 = bc1_encode(rgba, w, h);
    if (bc1.empty() && w && h) return {};
    const Bytes img = bc1_to_cmpr(bc1, w, h);
    Bytes b(0x20, 0);
    b[0] = 14;  // CMPR, no alpha
    io::put16(b, 2, static_cast<u16>(w));
    io::put16(b, 4, static_cast<u16>(h));
    // wrap S/T clamp, no palette, mipmap/edgeLOD/bias/aniso 0
    b[0x14] = 1;  // min filter linear
    b[0x15] = 1;  // mag filter linear
    b[0x18] = 1;  // 1 image
    io::put32(b, 0x1C, 0x20);
    b.insert(b.end(), img.begin(), img.end());
    return b;
}

Bytes stamp_icon_bti(View stamp_rgba, u32 w, u32 h) {
    const Bytes icon = make_icon_rgba(stamp_rgba, w, h);
    if (icon.empty()) return {};
    return bti_cmpr(icon, kSize, kSize);
}

}  // namespace hdf::tex
