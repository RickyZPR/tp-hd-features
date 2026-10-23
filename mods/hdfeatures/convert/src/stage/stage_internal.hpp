// Module ST (stage): helpers shared by src/stage/*.cpp. Not a public header.
//
// Bounds rule (Correction #1): every fixed-size read goes through has()/be*_at(), which never read outside the
// buffer. Python slices (b[o:o + n]) are reproduced with slice(), which clamps like Python, and Python slice
// assignment with slice_assign(), which can grow the buffer like Python.
#pragma once

#include "hdf/io.hpp"
#include "hdf/stage.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace hdf::stage::detail {

inline Error bad(std::string what) { return Error{"fmt.stage", std::move(what)}; }

// o + n <= size, without overflow
inline bool has(View d, u64 o, u64 n) { return o <= d.size() && n <= d.size() - o; }

inline u16 be16_at(View d, u64 o) { return io::be16(d, static_cast<std::size_t>(o)); }
inline u32 be32_at(View d, u64 o) { return io::be32(d, static_cast<std::size_t>(o)); }
inline i16 bes16_at(View d, u64 o) { return static_cast<i16>(be16_at(d, o)); }
inline float bef32_at(View d, u64 o) {
    const u32 bits = be32_at(d, o);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
inline void put16s(Bytes& b, u64 o, i16 v) { io::put16(b, static_cast<std::size_t>(o), static_cast<u16>(v)); }

// Python b[lo:hi] for lo, hi >= 0
inline View slice(View d, u64 lo, u64 hi) {
    if (lo > d.size()) lo = d.size();
    if (hi > d.size()) hi = d.size();
    if (hi < lo) hi = lo;
    return d.subspan(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi - lo));
}

// Python b[lo:hi] = data for lo, hi >= 0 (bytearray): the clamped slice is replaced, so the buffer grows or
// shrinks when the lengths differ.
inline void slice_assign(Bytes& b, u64 lo, u64 hi, View data) {
    if (lo > b.size()) lo = b.size();
    if (hi > b.size()) hi = b.size();
    if (hi < lo) hi = lo;
    const auto first = b.begin() + static_cast<std::ptrdiff_t>(lo);
    const std::size_t old_n = static_cast<std::size_t>(hi - lo);
    if (old_n == data.size()) {
        std::memmove(b.data() + lo, data.data(), data.size());
        return;
    }
    const Bytes copy(data.begin(), data.end());  // data may point into b
    b.erase(first, first + static_cast<std::ptrdiff_t>(old_n));
    b.insert(b.begin() + static_cast<std::ptrdiff_t>(lo), copy.begin(), copy.end());
}

// Python b[lo:hi] = data with any int lo, hi (negative = from the end, as Python): drop_records writes
// b[off:off + cnt * stride] with a negative count.
inline void slice_assign_py(Bytes& b, i64 lo, i64 hi, View data) {
    const i64 n = static_cast<i64>(b.size());
    auto norm = [n](i64 i) {
        if (i < 0) {
            i += n;
            return i < 0 ? i64(0) : i;
        }
        return i > n ? n : i;
    };
    lo = norm(lo);
    hi = norm(hi);
    if (hi < lo) hi = lo;
    slice_assign(b, static_cast<u64>(lo), static_cast<u64>(hi), data);
}

// b[o:o + n].split(b'\0')[0] as raw bytes (latin-1 decode keeps the bytes)
inline std::string cstr(View d, u64 o, u64 n) {
    const View s = slice(d, o, o + n);
    std::size_t k = 0;
    while (k < s.size() && s[k] != 0) ++k;
    return std::string(reinterpret_cast<const char*>(s.data()), k);
}

inline bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// Python's '%' formatting for the report lines (printf conversions are the same for %d %x %02x %08x %-2d %s).
inline std::string fmt(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    char buf[512];
    const int n = std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < 0) return {};
    if (static_cast<std::size_t>(n) < sizeof buf) return std::string(buf, static_cast<std::size_t>(n));
    std::string s(static_cast<std::size_t>(n) + 1, '\0');
    va_start(ap, f);
    std::vsnprintf(s.data(), s.size(), f, ap);
    va_end(ap);
    s.resize(static_cast<std::size_t>(n));
    return s;
}

// The signed record count Python reads ('>4siI'); loops run max(count, 0) times.
inline i64 count_of(const Chunk& c) { return static_cast<i64>(static_cast<i32>(c.count)); }

inline constexpr std::string_view kLayer = "0123456789abcde";  // cave_patch.LAYER / stagedata.LAYER

// cave_patch.is_actor_tag / is_scob_tag (tags are always 4 characters here)
inline bool is_actor_tag(std::string_view t) {
    if (t == "ACTR" || t == "TGOB" || t == "PLYR" || t == "TRES") return true;
    return t.size() >= 4 && (t.substr(0, 3) == "ACT" || t.substr(0, 3) == "TRE") && kLayer.find(t[3]) != std::string_view::npos;
}
inline bool is_scob_tag(std::string_view t) {
    if (t == "SCOB" || t == "TGSC" || t == "TGDR" || t == "Door") return true;
    return t.size() >= 4 && (t.substr(0, 3) == "SCO" || t.substr(0, 3) == "Doo") && kLayer.find(t[3]) != std::string_view::npos;
}

// stagedata.layer_of
inline int layer_of(std::string_view t) {
    if (t.size() == 4) {
        const std::size_t k = kLayer.find(t[3]);
        const std::string_view p = t.substr(0, 3);
        if (k != std::string_view::npos && (p == "ACT" || p == "TRE" || p == "SCO" || p == "Doo"))
            return static_cast<int>(k);
    }
    return -1;
}

// Python's repr of a list of '%02x' strings: ['c1', '80'] / []
inline std::string hex_list(View v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += fmt("'%02x'", unsigned(v[i]));
    }
    return s + "]";
}

// memfit.cpp: memfit.mem0_for / memfit.plan without the u32 limit of the public versions (patch_stage refuses a
// MEM0 above 0xFFFFFFFF, as Python's struct.pack_into('>I') does).
u64 mem0_for_wide(u64 need);
std::vector<u64> plan_wide(std::span<const ModelStats> rooms, std::span<const std::pair<u8, u8>> mec,
                           std::size_t nblocks);

}  // namespace hdf::stage::detail
