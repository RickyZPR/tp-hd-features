// Module T: GX2 untile and GX2 -> RGBA8. Port of formats/gtx2img.py (thickness, pixel_index, pipe_wo_rot,
// bank_wo_rot, addr_linear, addr_micro, addr_macro, deswizzle, _roundup, level_info, deswizzle_level, _rgb565,
// decode_bc1, to_rgba). The address functions follow AMD's R600 addrlib (via gtx2img.py) for the Wii U config:
// 2 pipes, 4 banks, 256-byte pipe interleave. src/tex/addrlib_ref.inc keeps the CC0 tphd AddrLib as a test
// cross-check (hdf-convert only).
#include "hdf/tex.hpp"

#include <algorithm>
#include <cstring>

namespace hdf::tex {

namespace {

constexpr u64 kNumPipes = 2, kNumBanks = 4;
constexpr u64 kPipeInterleaveBits = 8;  // 256 bytes
constexpr u64 kPipeBits = 1, kBankBits = 2;

// gtx2img.thickness
u32 thickness(u32 tm) {
    switch (tm) {
        case 3: case 7: case 11: case 13: case 15: return 4;
        case 16: case 17: return 8;
        default: return 1;
    }
}

// gtx2img.pixel_index(x, y, bpp, tm, z=0)
u64 pixel_index(u64 x, u64 y, u32 bpp, u32 tm) {
    u64 b0, b1, b2, b3, b4, b5;
    if (bpp == 8) {
        b0 = x & 1; b1 = (x >> 1) & 1; b2 = (x >> 2) & 1; b3 = (y >> 1) & 1; b4 = y & 1; b5 = (y >> 2) & 1;
    } else if (bpp == 16) {
        b0 = x & 1; b1 = (x >> 1) & 1; b2 = (x >> 2) & 1; b3 = y & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
    } else if (bpp == 32 || bpp == 96) {
        b0 = x & 1; b1 = (x >> 1) & 1; b2 = y & 1; b3 = (x >> 2) & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
    } else if (bpp == 64) {
        b0 = x & 1; b1 = y & 1; b2 = (x >> 1) & 1; b3 = (x >> 2) & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
    } else if (bpp == 128) {
        b0 = y & 1; b1 = x & 1; b2 = (x >> 1) & 1; b3 = (x >> 2) & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
    } else {
        b0 = x & 1; b1 = (x >> 1) & 1; b2 = y & 1; b3 = (x >> 2) & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
    }
    const u64 v = b0 | (b1 << 1) | (b2 << 2) | (b3 << 3) | (b4 << 4) | (b5 << 5);
    // z = 0: the thick-tile z bits are 0 (Python: v |= (z & 1) << 6 ...)
    (void)tm;
    return v;
}

// gtx2img.pipe_wo_rot / bank_wo_rot
u64 pipe_wo_rot(u64 x, u64 y) { return ((y >> 3) ^ (x >> 3)) & 1; }
u64 bank_wo_rot(u64 x, u64 y) {
    const u64 b0 = ((y / (16 * kNumPipes)) ^ (x >> 3)) & 1;
    return b0 | 2 * (((y / (8 * kNumPipes)) ^ (x >> 4)) & 1);
}

// gtx2img.addr_linear
u64 addr_linear(u64 x, u64 y, u32 bpp, u64 pitch) { return (y * pitch + x) * bpp / 8; }

// gtx2img.addr_micro
u64 addr_micro(u64 x, u64 y, u32 bpp, u64 pitch, u32 tm) {
    const u64 mtt = thickness(tm);
    const u64 mt_bytes = (64 * mtt * bpp + 7) / 8;
    const u64 mt_off = mt_bytes * ((x >> 3) + (y >> 3) * (pitch >> 3));
    return (bpp * pixel_index(x, y, bpp, tm)) / 8 + mt_off;
}

// gtx2img.addr_macro (the height argument is unused there too). Bank-swapped modes are refused by the caller.
u64 addr_macro(u64 x, u64 y, u32 bpp, u64 pitch, u32 tm, u64 pipe_swz, u64 bank_swz) {
    const u64 mtt = thickness(tm);
    const u64 elem = (bpp * pixel_index(x, y, bpp, tm)) >> 3;
    u64 pipe = pipe_wo_rot(x, y);
    u64 bank = bank_wo_rot(x, y);
    u64 bank_pipe = pipe + kNumPipes * bank;
    const u64 swz = pipe_swz + kNumPipes * bank_swz;
    bank_pipe ^= swz;  // slice 0, sample 0
    bank_pipe %= kNumPipes * kNumBanks;
    pipe = bank_pipe % kNumPipes;
    bank = bank_pipe / kNumPipes;
    u64 mtp = 8 * kNumBanks, mth = 8 * kNumPipes;
    if (tm == 5 || tm == 9) {
        mtp >>= 1;
        mth *= 2;
    } else if (tm == 6 || tm == 10) {
        mtp >>= 2;
        mth *= 4;
    }
    const u64 per_row = pitch / mtp;
    const u64 mt_bytes = (mtt * bpp * mth * mtp + 7) / 8;
    const u64 mt_off = ((x / mtp) + per_row * (y / mth)) * mt_bytes;
    constexpr u64 group_mask = (1u << kPipeInterleaveBits) - 1;
    constexpr u64 nsw = kBankBits + kPipeBits;
    const u64 total = elem + (mt_off >> nsw);
    const u64 hi = (total & ~group_mask) << nsw;
    const u64 lo = total & group_mask;
    return (bank << (kPipeBits + kPipeInterleaveBits)) | (pipe << kPipeInterleaveBits) | lo | hi;
}

// gtx2img._roundup
u64 roundup(u64 v, u64 a) { return (v + a - 1) / a * a; }

// gtx2img._rgb565
inline void rgb565(u32 c, u8* p) {
    const u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    p[0] = static_cast<u8>((r << 3) | (r >> 2));
    p[1] = static_cast<u8>((g << 2) | (g >> 4));
    p[2] = static_cast<u8>((b << 3) | (b >> 2));
}

}  // namespace

// gtx2img.FMT_BPP
u32 gx2_bpp(u32 format) {
    switch (format & 0xFFFF) {
        case 0x01: case 0x02: return 8;
        case 0x07: case 0x08: case 0x0A: case 0x0B: return 16;
        case 0x1A: case 0x41A: return 32;
        case 0x31: case 0x431: case 0x34: case 0x234: return 64;
        case 0x32: case 0x432: case 0x33: case 0x433: case 0x35: case 0x235: return 128;
        default: return 0;
    }
}

// gtx2img.BCN
bool gx2_is_bcn(u32 format) {
    switch (format & 0xFFFF) {
        case 0x31: case 0x431: case 0x32: case 0x432: case 0x33: case 0x433: case 0x34: case 0x234: case 0x35:
        case 0x235: return true;
        default: return false;
    }
}

// gtx2img.deswizzle(s, data)
Result<Bytes> deswizzle(u32 format, u32 width, u32 height, u32 pitch, u32 tm, u32 swizzle, View data) {
    const u32 bpp = gx2_bpp(format);
    if (bpp == 0) return fail("fmt.tex", "GX2 format not supported");
    const bool bc = gx2_is_bcn(format);
    const u64 w = bc ? (u64{width} + 3) / 4 : width;
    const u64 h = bc ? (u64{height} + 3) / 4 : height;
    const u64 swz = (swizzle >> 8) & 7;
    const u64 pipe_swz = swz & 1, bank_swz = (swz >> 1) & 3;
    const u64 bpe = bpp / 8;
    if (w * h > (u64{1} << 28)) return fail("fmt.tex", "surface too large");
    if (tm >= 4 && (tm == 8 || tm == 9 || tm == 10 || tm == 11 || tm == 14 || tm == 15) && w * h > 0)
        return fail("fmt.tex", "bank-swapped tile modes not implemented");
    Bytes out(static_cast<std::size_t>(w * h * bpe), 0);
    bool exact = true;  // out keeps its size (no Python slice shrink)
    for (u64 y = 0; y < h; ++y) {
        for (u64 x = 0; x < w; ++x) {
            u64 a;
            if (tm == 0 || tm == 1)
                a = addr_linear(x, y, bpp, pitch);
            else if (tm == 2 || tm == 3)
                a = addr_micro(x, y, bpp, pitch, tm);
            else
                a = addr_macro(x, y, bpp, pitch, tm, pipe_swz, bank_swz);
            const u64 d = (y * w + x) * bpe;
            if (exact && a <= data.size() && bpe <= data.size() - a) {
                std::memcpy(out.data() + d, data.data() + a, static_cast<std::size_t>(bpe));
                continue;
            }
            // Python: out[d:d + bpe] = data[a:a + bpe] with a short (or empty) right side resizes out.
            exact = false;
            const u64 k = a < data.size() ? std::min<u64>(bpe, data.size() - a) : 0;
            const u64 lo = std::min<u64>(d, out.size()), hi = std::min<u64>(d + bpe, out.size());
            if (k == hi - lo) {
                if (k) std::memcpy(out.data() + lo, data.data() + a, static_cast<std::size_t>(k));
            } else {
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(lo), out.begin() + static_cast<std::ptrdiff_t>(hi));
                if (k)
                    out.insert(out.begin() + static_cast<std::ptrdiff_t>(lo), data.begin() + static_cast<std::ptrdiff_t>(a),
                               data.begin() + static_cast<std::ptrdiff_t>(a + k));
            }
        }
    }
    return out;
}

// gtx2img.level_info(s, L)
Result<LevelInfo> level_info(const GtxSurface& s, int level) {
    const u32 bpp = gx2_bpp(s.format);
    if (bpp == 0) return fail("fmt.tex", "GX2 format not supported");
    if (level < 0 || level > 31) return fail("fmt.tex", "bad mip level");
    const bool bc = gx2_is_bcn(s.format);
    LevelInfo li;
    li.width = std::max<u32>(1, s.width >> level);
    li.height = std::max<u32>(1, s.height >> level);
    const u64 ew = bc ? (u64{li.width} + 3) / 4 : li.width;
    const u64 eh = bc ? (u64{li.height} + 3) / 4 : li.height;
    u32 tm = s.tile_mode;
    if (level == 0) {
        if (!s.has_image) return fail("fmt.tex", "surface has no image block");  // Python: None + slice -> TypeError
        li.pitch = s.pitch;
        li.tile_mode = tm;
        li.offset = s.image_off;
        li.size = s.image_block;
        return li;
    }
    const u32 sw = (s.swizzle >> 16) & 0xFF;
    if (tm >= 4 && static_cast<u32>(level) >= sw) tm = 2;
    u64 pitch, hh;
    if (tm >= 4) {
        pitch = roundup(ew, std::max<u64>(32, 32 * (256 / bpp / 8)));
        hh = roundup(eh, 16);
    } else if (tm == 2 || tm == 3) {
        pitch = roundup(ew, std::max<u64>(8, 256 / bpp));
        hh = roundup(eh, 8);
    } else {
        pitch = ew;
        hh = eh;
    }
    if (!s.has_mip) return fail("fmt.tex", "surface has no mip block");
    if (level - 1 >= 13) return fail("fmt.tex", "mip level past mipOffsets");  // Python IndexError
    li.offset = s.mip_off + (level == 1 ? 0 : s.mip_offsets[static_cast<std::size_t>(level - 1)]);
    li.pitch = static_cast<u32>(pitch);
    li.tile_mode = tm;
    li.size = pitch * hh * bpp / 8;
    return li;
}

// gtx2img.deswizzle_level(s, gtx, L) -> the linear bytes (w, h are level_info's)
Result<Bytes> untile(const GtxSurface& s, int level) {
    auto li = level_info(s, level);
    if (!li) return std::unexpected(li.error());
    // gtx[data_off:data_off + data_size] (a Python slice clips at the end)
    const View g = s.gtx;
    const u64 b = std::min<u64>(li->offset, g.size());
    const u64 e = std::min<u64>(li->offset + li->size, g.size());
    const View data = g.subspan(static_cast<std::size_t>(b), static_cast<std::size_t>(e - b));
    const u32 swizzle = li->tile_mode < 4 ? 0 : s.swizzle;
    return deswizzle(s.format, li->width, li->height, li->pitch, li->tile_mode, swizzle, data);
}

// gtx2img.to_rgba(s2, lin) with s2['w'], s2['h'] = the level's size
Result<Bytes> to_rgba(const GtxSurface& s, int level, View lin) {
    if (level < 0 || level > 31) return fail("fmt.tex", "bad mip level");
    const u32 fmt = s.format & 0xFFFF;
    const u64 w = std::max<u32>(1, s.width >> level), h = std::max<u32>(1, s.height >> level);
    const u64 n = w * h;
    if (n > (u64{1} << 28)) return fail("fmt.tex", "surface too large");
    Bytes raw;
    if (fmt == 0x31 || fmt == 0x431) {
        // decode_bc1(lin, w, h)
        const u64 bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (lin.size() < bw * bh * 8) return fail("fmt.tex", "BC1 data too short");  // struct.error
        raw.assign(static_cast<std::size_t>(n * 4), 0);
        for (u64 by = 0; by < bh; ++by) {
            for (u64 bx = 0; bx < bw; ++bx) {
                const u64 o = (by * bw + bx) * 8;
                const u32 c0 = lin[o] | (lin[o + 1] << 8), c1 = lin[o + 2] | (lin[o + 3] << 8);
                const u32 bits = static_cast<u32>(lin[o + 4]) | (static_cast<u32>(lin[o + 5]) << 8) |
                                 (static_cast<u32>(lin[o + 6]) << 16) | (static_cast<u32>(lin[o + 7]) << 24);
                u8 pal[4][4];
                rgb565(c0, pal[0]);
                rgb565(c1, pal[1]);
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
                for (u64 py = 0; py < 4; ++py) {
                    const u64 yy = by * 4 + py;
                    if (yy >= h) break;
                    for (u64 px = 0; px < 4; ++px) {
                        const u64 xx = bx * 4 + px;
                        if (xx >= w) continue;
                        const u32 idx = (bits >> (2 * (py * 4 + px))) & 3;
                        std::memcpy(raw.data() + (yy * w + xx) * 4, pal[idx], 4);
                    }
                }
            }
        }
    } else if (fmt == 0x01) {
        if (lin.size() < n) return fail("fmt.tex", "R8 data too short");
        raw.assign(static_cast<std::size_t>(n * 4), 0);
        for (u64 i = 0; i < n; ++i) {
            raw[i * 4] = lin[i];
            raw[i * 4 + 3] = 255;
        }
    } else if (fmt == 0x02) {  // R4G4: R in bits 0-3, G in 4-7
        if (lin.size() < n) return fail("fmt.tex", "R4G4 data too short");
        raw.assign(static_cast<std::size_t>(n * 4), 0);
        for (u64 i = 0; i < n; ++i) {
            const u32 v = lin[i];
            raw[i * 4] = static_cast<u8>((v & 15) * 17);
            raw[i * 4 + 1] = static_cast<u8>((v >> 4) * 17);
            raw[i * 4 + 3] = 255;
        }
    } else if (fmt == 0x07) {
        if (lin.size() < 2 * n) return fail("fmt.tex", "R8G8 data too short");
        raw.assign(static_cast<std::size_t>(n * 4), 0);
        for (u64 i = 0; i < n; ++i) {
            raw[i * 4] = lin[2 * i];
            raw[i * 4 + 1] = lin[2 * i + 1];
            raw[i * 4 + 3] = 255;
        }
    } else if (fmt == 0x08) {
        if (lin.size() < 2 * n) return fail("fmt.tex", "R5G6B5 data too short");
        raw.assign(static_cast<std::size_t>(n * 4), 0);
        for (u64 i = 0; i < n; ++i) {
            rgb565(lin[2 * i] | (lin[2 * i + 1] << 8), raw.data() + i * 4);
            raw[i * 4 + 3] = 255;
        }
    } else if (fmt == 0x1A || fmt == 0x41A) {
        raw.assign(lin.begin(), lin.end());  // bytearray(lin): any length
    } else {
        return fail("fmt.tex", "decode of this GX2 format is not supported");
    }
    const auto& comp = s.comp_sel;
    if (comp[0] == 0 && comp[1] == 1 && comp[2] == 2 && comp[3] == 3) return raw;
    if (raw.size() < n * 4) return fail("fmt.tex", "RGBA8 data too short");  // px[sel] IndexError
    Bytes out(static_cast<std::size_t>(n * 4));
    for (u64 i = 0; i < n; ++i) {
        const u8* px = raw.data() + i * 4;
        for (int c = 0; c < 4; ++c) {
            const u8 sel = comp[static_cast<std::size_t>(c)];
            out[i * 4 + c] = sel < 4 ? px[sel] : (sel == 4 ? 0 : 255);
        }
    }
    return out;
}

}  // namespace hdf::tex
