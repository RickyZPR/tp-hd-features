// Module CV (cave): helpers shared by src/cave/*.cpp only.
#pragma once

#include "hdf/cave.hpp"
#include "hdf/io.hpp"

#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::cave::detail {

inline std::unexpected<Error> bad(std::string detail) { return fail("fmt.cave", std::move(detail)); }

// printf into a std::string (report and log lines).
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string fmt(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string s;
    if (n > 0) {
        s.resize(static_cast<std::size_t>(n) + 1);
        std::vsnprintf(s.data(), s.size(), f, ap2);
        s.resize(static_cast<std::size_t>(n));
    }
    va_end(ap2);
    return s;
}

// Python '%.<prec>f' % x: correctly rounded (std::to_chars, as Python's float formatting).
inline std::string py_fixed(double x, int prec) {
    char b[400];
    const auto r = std::to_chars(b, b + sizeof b, x, std::chars_format::fixed, prec);
    if (r.ec != std::errc{}) return "?";
    return std::string(b, r.ptr);
}

// repr() of a Python list of ints: "[1, 2, 3]", "[]".
template <class T>
std::string py_int_list(const std::vector<T>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += std::to_string(v[i]);
    }
    return s + "]";
}

// bytes.decode('latin-1') then UTF-8 (how a Python str of a texture name lands in a log line).
inline std::string latin1_to_utf8(std::string_view s) {
    std::string out;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

inline std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
inline bool ends_with(std::string_view s, std::string_view suf) {
    return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

// j3d.GX_FMT.get(fmt, fmt)
inline std::string gx_fmt_name(u32 f) {
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
        default: return std::to_string(f);
    }
}

}  // namespace hdf::cave::detail
