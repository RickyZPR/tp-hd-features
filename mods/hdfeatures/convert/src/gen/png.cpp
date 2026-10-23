// Module G: common.png_bytes with stop checks. The bytes are those of io::png_encode_rgba (signature, IHDR 8-bit
// RGBA, ONE IDAT = zlib.compress(raw, 9) where raw puts filter byte 0 before every row, IEND), but the deflate
// takes its input in 64 KiB steps with a stop check between them. io's encoder makes the whole IDAT in one
// compress2 call: 236 ms for res/cave/amiibo/popup.png (1024 x 449) on this PC, longer than the 50 ms between
// stop checks that plan 2.3 asks for.
//
// zlib's deflate never decides on a block or a match with less than MIN_LOOKAHEAD bytes ahead unless it is asked
// to flush (deflate_slow returns need_more for Z_NO_FLUSH), so splitting the input with Z_NO_FLUSH and finishing
// with Z_FINISH gives the same stream as compress2 (deflateInit level 9, windowBits 15, memLevel 8, default
// strategy, the same as compress2). Checked byte for byte against io::png_encode_rgba by the unit test
// gen_png_matches_io, and on the real data by every run's output check (the 58 PNGs against the oracle hashes).
#include "gen_internal.hpp"

#include "hdf/io.hpp"

#include <zlib.h>

#include <algorithm>
#include <climits>

namespace hdf::gen {

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
    append_be32(out, io::crc32(View(out.data() + start, out.size() - start)));
}

struct Deflater {
    z_stream zs{};
    bool open = false;
    ~Deflater() {
        if (open) deflateEnd(&zs);
    }
};

}  // namespace

Result<Bytes> png_encode(u32 w, u32 h, View rgba, std::stop_token stop) {
    if (rgba.size() % 4 != 0 || static_cast<u64>(w) * h != rgba.size() / 4)  // as io (fix-wave1 L3)
        return fail("out.png", "RGBA size differs from the image size");
    if (stop.stop_requested()) return fail("stopped");
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    Bytes raw;
    raw.reserve(static_cast<std::size_t>(h) * (stride + 1));
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        const u8* row = rgba.data() + static_cast<std::size_t>(y) * stride;
        raw.insert(raw.end(), row, row + stride);
    }
    if (raw.size() > UINT_MAX / 2) return fail("out.png", "image too large");

    Deflater d;
    if (deflateInit(&d.zs, 9) != Z_OK) return fail("out.png", "deflateInit failed");
    d.open = true;
    Bytes idat(static_cast<std::size_t>(deflateBound(&d.zs, static_cast<uLong>(raw.size()))));
    d.zs.next_out = idat.data();
    d.zs.avail_out = static_cast<uInt>(idat.size());
    constexpr std::size_t kStep = std::size_t(64) << 10;  // about 8 ms of level-9 work on this PC
    std::size_t pos = 0;
    for (;;) {
        if (stop.stop_requested()) return fail("stopped");
        const std::size_t n = std::min(kStep, raw.size() - pos);
        d.zs.next_in = raw.data() + pos;
        d.zs.avail_in = static_cast<uInt>(n);
        pos += n;
        const bool last = pos == raw.size();
        const int rc = deflate(&d.zs, last ? Z_FINISH : Z_NO_FLUSH);
        if (last) {
            if (rc != Z_STREAM_END) return fail("out.png", "deflate did not finish");
            break;
        }
        if (rc != Z_OK || d.zs.avail_in != 0) return fail("out.png", "deflate failed");
    }
    idat.resize(idat.size() - d.zs.avail_out);

    const u8 hdr[13] = {static_cast<u8>(w >> 24), static_cast<u8>(w >> 16), static_cast<u8>(w >> 8), static_cast<u8>(w),
                        static_cast<u8>(h >> 24), static_cast<u8>(h >> 16), static_cast<u8>(h >> 8), static_cast<u8>(h),
                        8, 6, 0, 0, 0};
    Bytes out(std::begin(kSignature), std::end(kSignature));
    out.reserve(8 + 25 + 12 + idat.size() + 12);
    put_chunk(out, "IHDR", View(hdr, sizeof hdr));
    put_chunk(out, "IDAT", idat);
    put_chunk(out, "IEND", View{});
    return out;
}

}  // namespace hdf::gen
