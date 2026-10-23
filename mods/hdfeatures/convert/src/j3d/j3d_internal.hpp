// Module J (j3d): helpers shared by src/j3d/*.cpp. Not a public header.
//
// Bounds rule (Correction #1): the Python spec raises (struct.error, IndexError, KeyError, ValueError,
// OverflowError) where a read or write leaves the buffer, a table lookup misses or a number does not fit. The
// port throws detail::Bad at exactly those places, and every public function of src/j3d catches it and returns
// Error{"fmt.j3d", ...}. Bad is never thrown by anything else and never leaves src/j3d. Python slices
// (b[lo:hi]) are reproduced with slice(), which clamps like Python; slice assignment with slice_assign(), which
// can grow or shrink the buffer like Python's bytearray.
#pragma once

#include "hdf/io.hpp"
#include "hdf/j3d.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::j3d::detail {

struct Bad {
    std::string what;
};
[[noreturn]] inline void bad(std::string what) { throw Bad{std::move(what)}; }

// the stop token was set inside a long loop (convert_members turns it into Error{"stopped"})
struct Stopped {};

inline Error to_error(const Bad& b) { return Error{"fmt.j3d", b.what}; }

// o + n <= size, without overflow
inline bool has(View d, u64 o, u64 n) { return o <= d.size() && n <= d.size() - o; }

inline u8 u8_at(View d, u64 o) {
    if (!has(d, o, 1)) bad("read past the end");
    return d[static_cast<std::size_t>(o)];
}
inline u16 u16_at(View d, u64 o) {
    if (!has(d, o, 2)) bad("read past the end");
    return io::be16(d, static_cast<std::size_t>(o));
}
inline u32 u32_at(View d, u64 o) {
    if (!has(d, o, 4)) bad("read past the end");
    return io::be32(d, static_cast<std::size_t>(o));
}
inline float f32_at(View d, u64 o) {
    const u32 bits = u32_at(d, o);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// struct.pack_into(..., b, o, v): raises when the field does not fit
inline void put_u8(Bytes& b, u64 o, u8 v) {
    if (!has(b, o, 1)) bad("write past the end");
    b[static_cast<std::size_t>(o)] = v;
}
inline void put_u16(Bytes& b, u64 o, u16 v) {
    if (!has(b, o, 2)) bad("write past the end");
    io::put16(b, static_cast<std::size_t>(o), v);
}
inline void put_u32(Bytes& b, u64 o, u32 v) {
    if (!has(b, o, 4)) bad("write past the end");
    io::put32(b, static_cast<std::size_t>(o), v);
}
// struct.pack('>f', x): PyFloat_Pack4 raises OverflowError when a finite double rounds to an infinite float
inline float to_f32(double x) {
    const float y = static_cast<float>(x);
    if (std::isinf(y) && !std::isinf(x)) bad("float too large to pack with f format");
    return y;
}
inline void put_f32(Bytes& b, u64 o, double x) {
    const float y = to_f32(x);
    u32 bits;
    std::memcpy(&bits, &y, 4);
    put_u32(b, o, bits);
}

// Python b[lo:hi] for lo, hi >= 0
inline View slice(View d, u64 lo, u64 hi) {
    if (lo > d.size()) lo = d.size();
    if (hi > d.size()) hi = d.size();
    if (hi < lo) hi = lo;
    return d.subspan(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi - lo));
}

// Python bytearray b[lo:hi] = data for lo, hi >= 0: the clamped slice is replaced (the buffer may grow).
inline void slice_assign(Bytes& b, u64 lo, u64 hi, View data) {
    if (lo > b.size()) lo = b.size();
    if (hi > b.size()) hi = b.size();
    if (hi < lo) hi = lo;
    const std::size_t old_n = static_cast<std::size_t>(hi - lo);
    if (old_n == data.size()) {
        if (!data.empty()) std::memmove(b.data() + lo, data.data(), data.size());
        return;
    }
    const Bytes copy(data.begin(), data.end());  // data may point into b
    const auto first = b.begin() + static_cast<std::ptrdiff_t>(lo);
    b.erase(first, first + static_cast<std::ptrdiff_t>(old_n));
    b.insert(b.begin() + static_cast<std::ptrdiff_t>(lo), copy.begin(), copy.end());
}

inline View sv_bytes(std::string_view s) { return View(reinterpret_cast<const u8*>(s.data()), s.size()); }

inline std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}
inline bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// j3d.sections(buf) -> (magic, size, [(tag, offset, size)]): raises when the 16-byte header is short.
std::vector<Section> sections_impl(View d);
inline bool is_tag(const Section& s, const char* tag) { return std::memcmp(s.tag.data(), tag, 4) == 0; }
// {t: (o, s) for t, o, s in secs}[tag]: the last section with that tag; KeyError (Bad) when there is none.
const Section& section_of(const std::vector<Section>& secs, const char* tag);

// The models and BTIs of convert.convert_arc, with the report lines (stats are counted from them).
Bytes convert_j3d_impl(View d, std::optional<View> gtx, std::string_view label, std::vector<std::string>& report,
                       std::stop_token stop);
Bytes convert_bti_impl(View d, std::optional<View> gtx, std::string_view label, std::vector<std::string>& report);
Mat4Fix mat4fix_impl(View d);
std::optional<u32> effective_stride_impl(View b, u64 o);
Baked bake_impl(View d, const std::array<float, 3>& t, int angle);
DlBytes pc_dl_impl(View d);
// memfit.model_stats: zeros when sections() raises, dl = 0 when pc_dl_bytes raises; the count loop may still raise.
ModelStats model_stats_impl(View d);

}  // namespace hdf::j3d::detail
