// gzip inflate, zlib deflate level 9 and CRC-32 on the vendored zlib 1.3.2 (module C1).
//
// gzip_inflate = Python's gzip.decompress as hdpack.read_any uses it: one or more members in a row, zero bytes
// allowed after a member, anything else after a member must be another member; CRC-32 and ISIZE are checked
// by zlib (windowBits 15 + 16). Output grows in 1 MB steps and the stop token is checked before each step
// (plan 2.3: at least every 1 MB of inflate output).
// deflate9 = zlib.compress(data, 9) = compress2(level 9): deflateInit(9), all input at once, Z_FINISH.
#include "io_internal.hpp"

#include <zlib.h>

#include <algorithm>
#include <climits>

namespace hdf::io {

namespace {

constexpr std::size_t kStep = std::size_t(1) << 20;  // output step and stop-check interval
constexpr std::size_t kMaxChunk = UINT_MAX;          // zlib's avail_in / avail_out are uInt

u32 le32(View d, std::size_t o) {
    return static_cast<u32>(d[o]) | (static_cast<u32>(d[o + 1]) << 8) | (static_cast<u32>(d[o + 2]) << 16) |
           (static_cast<u32>(d[o + 3]) << 24);
}

bool gzip_magic(View d, std::size_t o) { return d.size() >= o + 2 && d[o] == 0x1f && d[o + 1] == 0x8b; }

struct Inflater {
    z_stream zs{};
    bool open = false;
    explicit Inflater(int window_bits) { open = inflateInit2(&zs, window_bits) == Z_OK; }
    ~Inflater() {
        if (open) inflateEnd(&zs);
    }
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
};

std::string zmsg(const z_stream& zs, int rc) {
    if (zs.msg) return zs.msg;
    return "zlib error " + std::to_string(rc);
}

// One zlib/gzip inflate over `in`, starting at `pos`. Appends to `out`. On return `pos` is the first input
// byte after the stream. `max_out` caps the total output.
Result<void> inflate_stream(Inflater& z, View in, std::size_t& pos, Bytes& out, std::size_t max_out,
                            const std::stop_token& stop) {
    z.zs.next_in = const_cast<Bytef*>(in.data() + pos);
    z.zs.avail_in = static_cast<uInt>(std::min(in.size() - pos, kMaxChunk));
    std::size_t fed_end = pos + z.zs.avail_in;  // input given to zlib so far ends here
    for (;;) {
        if (stop.stop_requested()) return fail("stopped");
        if (z.zs.avail_in == 0 && fed_end < in.size()) {
            z.zs.next_in = const_cast<Bytef*>(in.data() + fed_end);
            z.zs.avail_in = static_cast<uInt>(std::min(in.size() - fed_end, kMaxChunk));
            fed_end += z.zs.avail_in;
        }
        const std::size_t old = out.size();
        if (old >= max_out) return fail("fmt.io", "inflate: output larger than " + std::to_string(max_out) + " bytes");
        const std::size_t step = std::min(kStep, max_out - old);
        out.resize(old + step);
        z.zs.next_out = out.data() + old;
        z.zs.avail_out = static_cast<uInt>(step);
        const int rc = inflate(&z.zs, Z_NO_FLUSH);
        out.resize(old + step - z.zs.avail_out);
        if (rc == Z_STREAM_END) {
            pos = fed_end - z.zs.avail_in;
            return {};
        }
        if (rc == Z_OK) continue;
        if (rc == Z_BUF_ERROR) {
            // no progress possible: either more output room is needed (loop again) or the input ran out
            if (z.zs.avail_in == 0 && fed_end >= in.size())
                return fail("fmt.io", "compressed data ended before the end-of-stream marker");
            continue;
        }
        return fail("fmt.io", "inflate: " + zmsg(z.zs, rc));
    }
}

}  // namespace

Result<Bytes> zlib_inflate(View in, std::size_t max_out, std::stop_token stop) {
    Inflater z(15);
    if (!z.open) return fail("io.zlib", "inflateInit2 failed");
    Bytes out;
    std::size_t pos = 0;
    if (auto r = inflate_stream(z, in, pos, out, max_out, stop); !r) return std::unexpected(r.error());
    return out;
}

Result<Bytes> gzip_inflate(View in, std::stop_token stop) {
    if (!gzip_magic(in, 0)) return fail("fmt.io", "not a gzip file");
    Inflater z(15 + 16);
    if (!z.open) return fail("io.zlib", "inflateInit2 failed");
    Bytes out;
    // Size hint: ISIZE of a single-member file (the last 4 bytes). Only used when deflate could produce it
    // (at most 1032:1), so a bad hint can't cause a huge allocation.
    if (in.size() >= 18) {
        const u64 isize = le32(in, in.size() - 4);
        if (isize <= static_cast<u64>(in.size()) * 1032 && isize <= kMaxInflate) out.reserve(isize);
    }
    std::size_t pos = 0;
    for (;;) {
        if (auto r = inflate_stream(z, in, pos, out, kMaxInflate, stop); !r) return std::unexpected(r.error());
        while (pos < in.size() && in[pos] == 0) ++pos;  // gzip files can be padded with zeroes
        if (pos >= in.size()) break;
        if (!gzip_magic(in, pos)) return fail("fmt.io", "gzip: data after the last member is not a gzip member");
        if (inflateReset(&z.zs) != Z_OK) return fail("io.zlib", "inflateReset failed");
    }
    return out;
}

// hdpack.read_any (on bytes): gunzip when the data starts with 1f 8b, else the bytes unchanged.
Result<Bytes> read_any(View in, std::stop_token stop) {
    if (gzip_magic(in, 0)) return gzip_inflate(in, stop);
    return Bytes(in.begin(), in.end());
}

Bytes deflate9(View in) {
    z_stream zs{};
    if (deflateInit(&zs, 9) != Z_OK) return {};
    Bytes out;
    const uLong bound_in = static_cast<uLong>(std::min<std::size_t>(in.size(), ULONG_MAX));
    out.resize(static_cast<std::size_t>(deflateBound(&zs, bound_in)));
    std::size_t in_pos = 0, out_pos = 0;
    zs.avail_in = 0;
    zs.avail_out = 0;
    int rc = Z_OK;
    do {
        if (zs.avail_out == 0) {
            if (out_pos == out.size()) out.resize(out.size() + kStep);
            zs.next_out = out.data() + out_pos;
            zs.avail_out = static_cast<uInt>(std::min(out.size() - out_pos, kMaxChunk));
        }
        if (zs.avail_in == 0) {
            zs.next_in = const_cast<Bytef*>(in.data() + in_pos);
            zs.avail_in = static_cast<uInt>(std::min(in.size() - in_pos, kMaxChunk));
            in_pos += zs.avail_in;
        }
        const uInt before = zs.avail_out;
        rc = deflate(&zs, in_pos < in.size() ? Z_NO_FLUSH : Z_FINISH);
        out_pos += before - zs.avail_out;
    } while (rc == Z_OK);  // as compress2
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return {};
    out.resize(out_pos);
    return out;
}

u32 crc32(View in, u32 crc) {
    return static_cast<u32>(::crc32_z(static_cast<uLong>(crc), in.data(), in.size()));
}

}  // namespace hdf::io
