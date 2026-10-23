// Python number rules (module C1, libs 3): round(x, n), int(round(x)), repr(float), // and %.
//
// py_round copies CPython's double_round: the value is rounded to n decimals on its EXACT decimal expansion,
// ties to even (dtoa mode 3), and the decimal string is read back with a correctly rounded strtod. Here the
// exact expansion comes from std::to_chars(fixed, 1074) (exact for every double) and the read-back from
// std::from_chars. py_float_repr uses the shortest round-trip digits of std::to_chars and lays them out like
// Python's float_repr_style 'short' (fixed when -4 <= exponent < 16).
#include "hdf/io.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace hdf::io {

namespace {

// CPython floatobject.c: NDIGITS_MAX = (int)((DBL_MANT_DIG - DBL_MIN_EXP) * 0.30103) = 323,
// NDIGITS_MIN = -(int)((DBL_MAX_EXP + 1) * 0.30103) = -308.
constexpr int kNdigitsMax = 323;
constexpr int kNdigitsMin = -308;

}  // namespace

double py_round(double x, int ndigits) {
    if (!std::isfinite(x)) return x;
    if (ndigits > kNdigitsMax) return x;
    if (ndigits < kNdigitsMin) return 0.0 * x;
    // exact decimal expansion of |x|: at most 309 integer digits + '.' + 1074 fraction digits
    char buf[1500];
    const double ax = std::fabs(x);
    const auto r = std::to_chars(buf, buf + sizeof(buf), ax, std::chars_format::fixed, 1074);
    if (r.ec != std::errc()) return x;
    const std::string_view s(buf, static_cast<std::size_t>(r.ptr - buf));
    const std::size_t dot = s.find('.');
    std::string digits(s.substr(0, dot));
    digits += s.substr(dot + 1);
    const long long int_len = static_cast<long long>(dot);
    // keep `keep` leading digits (may be <= 0: the result is 0 or one unit of 10^-ndigits)
    const long long keep = int_len + ndigits;
    std::string kept;
    bool round_up = false;
    if (keep < 0) {
        round_up = false;  // value < half a unit
    } else {
        kept = digits.substr(0, static_cast<std::size_t>(keep));
        const char next = static_cast<std::size_t>(keep) < digits.size() ? digits[static_cast<std::size_t>(keep)] : '0';
        bool rest_zero = true;
        for (std::size_t i = static_cast<std::size_t>(keep) + 1; i < digits.size(); ++i)
            if (digits[i] != '0') {
                rest_zero = false;
                break;
            }
        if (next > '5') {
            round_up = true;
        } else if (next == '5') {
            if (!rest_zero) {
                round_up = true;
            } else {  // exact tie: to even
                const char last = kept.empty() ? '0' : kept.back();
                round_up = ((last - '0') % 2) == 1;
            }
        }
    }
    if (round_up) {
        // add one unit in the last kept place
        int i = static_cast<int>(kept.size()) - 1;
        while (i >= 0 && kept[static_cast<std::size_t>(i)] == '9') kept[static_cast<std::size_t>(i--)] = '0';
        if (i >= 0) {
            ++kept[static_cast<std::size_t>(i)];
        } else {
            kept.insert(kept.begin(), '1');
        }
    }
    if (kept.empty()) kept = "0";
    // value = kept * 10^(-ndigits)
    std::string text = (x < 0 || (x == 0 && std::signbit(x))) ? "-" : "";
    text += kept;
    text += "e";
    text += std::to_string(-ndigits);
    double out = 0.0;
    const auto p = std::from_chars(text.data(), text.data() + text.size(), out);
    if (p.ec != std::errc()) {
        // overflow on read-back: CPython raises OverflowError; keep the sign and give inf
        return std::copysign(std::numeric_limits<double>::infinity(), x);
    }
    return out;
}

// int(round(x)): half to even. Python raises for nan/inf and gives a big int beyond i64; here 0 / clamped.
// fix-wave1 L9: the result does not depend on the thread's rounding mode (MXCSR). floor() is exact; x - floor(x)
// is exact (Sterbenz) except for -0.5 < x < 0, where 1 + x lies in (0.5, 1) and any rounding of it still gives
// 0 below; r + 1 is exact below 2^53, and |x| >= 2^52 is already an integer. Same results as std::nearbyint
// under the default mode (round half to even).
i64 py_round_int(double x) {
    if (!std::isfinite(x)) return 0;
    double r = std::floor(x);
    const double frac = x - r;  // in [0, 1)
    if (frac > 0.5 || (frac == 0.5 && std::fmod(r, 2.0) != 0.0)) r += 1.0;
    if (r >= 9223372036854775807.0) return std::numeric_limits<i64>::max();
    if (r <= -9223372036854775808.0) return std::numeric_limits<i64>::min();
    return static_cast<i64>(r);
}

std::string py_float_repr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x < 0 ? "-inf" : "inf";
    if (x == 0) return std::signbit(x) ? "-0.0" : "0.0";
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), x, std::chars_format::scientific);
    std::string_view s(buf, static_cast<std::size_t>(r.ptr - buf));
    std::string out;
    if (s.front() == '-') {
        out = "-";
        s.remove_prefix(1);
    }
    const std::size_t e = s.find('e');
    std::string digits;
    for (char c : s.substr(0, e))
        if (c != '.') digits += c;
    const int exp10 = std::stoi(std::string(s.substr(e + 1)));  // value = d.ddd * 10^exp10
    const int decpt = exp10 + 1;                                 // digits before the decimal point
    const int nd = static_cast<int>(digits.size());
    if (exp10 >= -4 && exp10 < 16) {
        if (decpt <= 0) {
            out += "0.";
            out.append(static_cast<std::size_t>(-decpt), '0');
            out += digits;
        } else if (decpt >= nd) {
            out += digits;
            out.append(static_cast<std::size_t>(decpt - nd), '0');
            out += ".0";
        } else {
            out += digits.substr(0, static_cast<std::size_t>(decpt));
            out += '.';
            out += digits.substr(static_cast<std::size_t>(decpt));
        }
    } else {
        out += digits[0];
        if (nd > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += 'e';
        out += exp10 < 0 ? '-' : '+';
        const std::string ed = std::to_string(exp10 < 0 ? -exp10 : exp10);
        if (ed.size() < 2) out += '0';
        out += ed;
    }
    return out;
}

i64 floor_div(i64 a, i64 b) {
    if (b == 0) return 0;  // Python raises ZeroDivisionError; callers never divide by zero
    if (a == std::numeric_limits<i64>::min() && b == -1) return a;  // would overflow i64
    i64 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

i64 floor_mod(i64 a, i64 b) {
    if (b == 0) return 0;
    if (b == -1) return 0;
    i64 r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

}  // namespace hdf::io
