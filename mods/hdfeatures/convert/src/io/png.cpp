// PNG writing (pipeline/common.png_bytes), the pixel hash (tools/dist/hdf_builder.png_pixel_hash) and a small
// reader for tests and the compare diagnostic (pipeline/common.png_read). Module C1.
//
// png_bytes: signature, IHDR (8-bit RGBA, no interlace), ONE IDAT = zlib.compress(raw, 9) where raw puts filter
// byte 0 before every row, IEND. zlib 1.3.2 at level 9 gives the oracle's bytes (libs 2, VERIFIED there).
#include "io_internal.hpp"

#include "hdf/source.hpp"  // Sha256 (C2, Windows CNG)

#include <algorithm>
#include <cstring>

namespace hdf::io {

namespace {

constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

void append_be32(Bytes& out, u32 v) {
    out.push_back(static_cast<u8>(v >> 24));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

// common.png_bytes.chunk: len, type, data, crc32(type + data)
void put_chunk(Bytes& out, const char (&type)[5], View data) {
    append_be32(out, static_cast<u32>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    append_be32(out, crc32(View(out.data() + start, out.size() - start)));
}

std::array<u8, 13> ihdr(u32 w, u32 h) {
    return {static_cast<u8>(w >> 24), static_cast<u8>(w >> 16), static_cast<u8>(w >> 8), static_cast<u8>(w),
            static_cast<u8>(h >> 24), static_cast<u8>(h >> 16), static_cast<u8>(h >> 8), static_cast<u8>(h),
            8, 6, 0, 0, 0};
}

// fix-wave1 L3: w * h fits in u64 (both < 2^32), but w * h * 4 may not (w = h = 2^31 gives 0). Compare pixel
// counts instead, so no size can wrap round to the buffer's size.
bool rgba_size_ok(u32 w, u32 h, View rgba) {
    return rgba.size() % 4 == 0 && static_cast<u64>(w) * h == rgba.size() / 4;
}

}  // namespace

Bytes png_encode_rgba(u32 w, u32 h, View rgba) {
    if (!rgba_size_ok(w, h, rgba)) return {};  // Python raises ValueError('rgba size ...')
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    Bytes raw;
    raw.reserve(static_cast<std::size_t>(h) * (stride + 1));
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        const u8* row = rgba.data() + static_cast<std::size_t>(y) * stride;
        raw.insert(raw.end(), row, row + stride);
    }
    const Bytes idat = deflate9(raw);
    if (idat.empty()) return {};
    Bytes out(std::begin(kSignature), std::end(kSignature));
    out.reserve(8 + 25 + 12 + idat.size() + 12);
    const auto hdr = ihdr(w, h);
    put_chunk(out, "IHDR", hdr);
    put_chunk(out, "IDAT", idat);
    put_chunk(out, "IEND", View{});
    return out;
}

// hdf_builder.png_pixel_hash on the pixels: sha256(IHDR data + the raw scanlines, each with filter byte 0).
std::string png_pixel_hash(u32 w, u32 h, View rgba) {
    if (!rgba_size_ok(w, h, rgba)) return {};
    Sha256 s;
    const auto hdr = ihdr(w, h);
    s.update(hdr);
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    const u8 zero = 0;
    for (u32 y = 0; y < h; ++y) {
        s.update(View(&zero, 1));
        s.update(rgba.subspan(static_cast<std::size_t>(y) * stride, stride));
    }
    return s.finish_hex();
}

// hdf_builder.png_pixel_hash(data): IHDR data + zlib.decompress(all IDAT data). "" when the PNG can't be read.
std::string png_pixel_hash(View png) {
    std::size_t o = 8;
    Bytes idat;
    View hdr;
    while (o + 8 <= png.size()) {
        const std::size_t n = be32(png, o);
        const View t = png.subspan(o + 4, 4);
        const std::size_t d0 = std::min(png.size(), o + 8);
        const std::size_t d1 = std::min(png.size(), o + 8 + n);  // Python slices clamp
        const View d = png.subspan(d0, d1 - d0);
        if (std::memcmp(t.data(), "IHDR", 4) == 0) {
            hdr = d;
        } else if (std::memcmp(t.data(), "IDAT", 4) == 0) {
            idat.insert(idat.end(), d.begin(), d.end());
        } else if (std::memcmp(t.data(), "IEND", 4) == 0) {
            break;
        }
        o += 12 + n;
    }
    auto raw = zlib_inflate(idat, kMaxInflate);
    if (!raw) return {};
    Sha256 s;
    s.update(hdr);
    s.update(*raw);
    return s.finish_hex();
}

// common.png_read: 8-bit RGBA, non-interlaced, filters 0-4, every chunk CRC checked. Tests / compare only.
Result<std::pair<std::array<u32, 2>, Bytes>> png_decode(View data) {
    if (data.size() < 8 || std::memcmp(data.data(), kSignature, 8) != 0) return fail("fmt.io", "not a PNG");
    std::size_t o = 8;
    Bytes idat;
    bool have_ihdr = false;
    u32 w = 0, h = 0;
    while (o < data.size()) {
        if (o + 8 > data.size()) return fail("fmt.io", "PNG: truncated chunk header");
        const std::size_t n = be32(data, o);
        if (o + 12 + static_cast<u64>(n) > data.size()) return fail("fmt.io", "PNG: truncated chunk");
        const View t = data.subspan(o + 4, 4);
        const View d = data.subspan(o + 8, n);
        const u32 crc = be32(data, o + 8 + n);
        if (crc32(data.subspan(o + 4, 4 + n)) != crc)
            return fail("fmt.io", "PNG: bad CRC in " + std::string(reinterpret_cast<const char*>(t.data()), 4));
        if (std::memcmp(t.data(), "IHDR", 4) == 0) {
            if (n != 13) return fail("fmt.io", "PNG: IHDR size");
            w = be32(d, 0);
            h = be32(d, 4);
            if (d[8] != 8 || d[9] != 6 || d[12] != 0)
                return fail("fmt.io", "unsupported PNG (bit depth " + std::to_string(d[8]) + ", colour type " +
                                          std::to_string(d[9]) + ", interlace " + std::to_string(d[12]) + ")");
            have_ihdr = true;
        } else if (std::memcmp(t.data(), "IDAT", 4) == 0) {
            idat.insert(idat.end(), d.begin(), d.end());
        } else if (std::memcmp(t.data(), "IEND", 4) == 0) {
            break;
        }
        o += 12 + n;
    }
    if (!have_ihdr) return fail("fmt.io", "PNG: no IHDR");
    const u64 stride = static_cast<u64>(w) * 4;  // < 2^34
    // fix-wave1 L3: checked by division, so h * (stride + 1) can't wrap round to a small number
    if (h != 0 && stride + 1 > kMaxInflate / h) return fail("fmt.io", "PNG: image too large");
    const u64 want = static_cast<u64>(h) * (stride + 1);
    auto raw = zlib_inflate(idat, static_cast<std::size_t>(want) + 1);
    if (!raw) return std::unexpected(raw.error());
    if (raw->size() != want)
        return fail("fmt.io", "IDAT size " + std::to_string(raw->size()) + " != " + std::to_string(want));
    Bytes out(static_cast<std::size_t>(h * stride));
    Bytes prev(static_cast<std::size_t>(stride), 0);
    Bytes line(static_cast<std::size_t>(stride));
    for (u64 y = 0; y < h; ++y) {
        const u8* src = raw->data() + y * (stride + 1);
        const u8 ft = src[0];
        std::memcpy(line.data(), src + 1, static_cast<std::size_t>(stride));
        for (std::size_t x = 0; x < stride; ++x) {
            const int a = x >= 4 ? line[x - 4] : 0;
            const int b = prev[x];
            const int c = x >= 4 ? prev[x - 4] : 0;
            if (ft == 1) {
                line[x] = static_cast<u8>(line[x] + a);
            } else if (ft == 2) {
                line[x] = static_cast<u8>(line[x] + b);
            } else if (ft == 3) {
                line[x] = static_cast<u8>(line[x] + ((a + b) >> 1));
            } else if (ft == 4) {
                const int p = a + b - c;
                const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
                const int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                line[x] = static_cast<u8>(line[x] + pr);
            }  // 0 and unknown filter types: unchanged (as png_read)
        }
        std::memcpy(out.data() + y * stride, line.data(), static_cast<std::size_t>(stride));
        prev.swap(line);
    }
    return std::pair<std::array<u32, 2>, Bytes>{{w, h}, std::move(out)};
}

}  // namespace hdf::io
