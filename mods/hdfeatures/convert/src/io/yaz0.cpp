// Yaz0 decoding (module C1). Port of tools/formats/yaz0.py (decompress_yaz0, is_yaz0, maybe_decompress).
// Yay0 is not ported: no Yay0 data exists in the inputs (pipeline 3). maybe_yaz0 refuses Yay0 instead of
// passing it through, so a Yay0 blob can never be used as if it were plain data.
//
// Every read is bounds-checked (Correction #1): a bad stream gives Error{"fmt.io"}, never a crash.
#include "hdf/io.hpp"

#include <cstring>

namespace hdf::io {

namespace {

bool has_magic(View in, const char (&m)[5]) {
    return in.size() >= 16 && std::memcmp(in.data(), m, 4) == 0;
}

}  // namespace

// yaz0.is_yaz0: len(data) >= 16 and data[:4] == b"Yaz0"
bool is_yaz0(View in) { return has_magic(in, "Yaz0"); }

// yaz0.decompress_yaz0
Result<Bytes> yaz0_decode(View src) {
    if (!is_yaz0(src)) return fail("fmt.io", "not Yaz0 data");
    const std::size_t out_size = be32(src, 4);
    const std::size_t slen = src.size();
    // A Yaz0 stream can't expand more than 2184 bytes per 25 input bytes (8 long back-references of 273 bytes
    // under one code byte). A larger declared size can't be produced by this input, so refuse it before the
    // allocation (Python would allocate it and then fail with "truncated input").
    if (out_size > (slen - 16) * 88 + 64) return fail("fmt.io", "Yaz0: declared size larger than the input allows");
    Bytes dst(out_size);
    std::size_t sp = 16, dp = 0;
    while (dp < out_size) {
        if (sp >= slen) return fail("fmt.io", "Yaz0: truncated input at dst " + std::to_string(dp));
        const u8 code = src[sp++];
        for (int bit = 0; bit < 8; ++bit) {
            if (dp >= out_size) break;
            if (code & (0x80 >> bit)) {
                if (sp >= slen) return fail("fmt.io", "Yaz0: truncated input (literal)");
                dst[dp++] = src[sp++];
                continue;
            }
            if (sp + 1 >= slen) return fail("fmt.io", "Yaz0: truncated input (back-reference)");
            const u8 b1 = src[sp], b2 = src[sp + 1];
            sp += 2;
            const std::size_t dist = ((static_cast<std::size_t>(b1 & 0x0F) << 8) | b2) + 1;
            const unsigned n = b1 >> 4;
            std::size_t length;
            if (n == 0) {
                if (sp >= slen) return fail("fmt.io", "Yaz0: truncated input (length byte)");
                length = static_cast<std::size_t>(src[sp++]) + 0x12;
            } else {
                length = n + 2;
            }
            if (dist > dp) return fail("fmt.io", "Yaz0: back-reference before start");
            const std::size_t cp = dp - dist;
            if (length > out_size - dp) length = out_size - dp;
            if (dist >= length) {
                std::memcpy(dst.data() + dp, dst.data() + cp, length);
            } else {
                for (std::size_t i = 0; i < length; ++i) dst[dp + i] = dst[cp + i];  // overlapping run
            }
            dp += length;
        }
    }
    return dst;
}

// yaz0.maybe_decompress: decoded bytes for Yaz0, an unchanged copy otherwise. Yay0 is refused (not ported).
Result<Bytes> maybe_yaz0(View in) {
    if (is_yaz0(in)) return yaz0_decode(in);
    if (has_magic(in, "Yay0")) return fail("fmt.io", "Yay0 data is not supported");
    return Bytes(in.begin(), in.end());
}

}  // namespace hdf::io
