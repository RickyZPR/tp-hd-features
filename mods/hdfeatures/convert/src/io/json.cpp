// JSON writing with Python's json.dumps rules (module C1).
//
// Objects keep insertion order (Python dicts). Indent: newline + indent*level spaces before every item and
// before the closing bracket; empty containers are "[]" / "{}". Strings: ensure_ascii escapes everything
// outside ' '..'~' as \uXXXX (lower-case hex, UTF-16 surrogate pairs above U+FFFF), plus \" \\ \b \f \n \r \t;
// without ensure_ascii only those short escapes and \u00XX for other control characters are written, and the
// rest is copied as UTF-8. Floats use repr (py_float_repr); NaN/Infinity as json.dumps writes them.
#include "hdf/io.hpp"

#include <cmath>
#include <string>

namespace hdf::io {

namespace {

constexpr char kHex[] = "0123456789abcdef";

void put_u(std::string& out, u32 v) {
    out += "\\u";
    out += kHex[(v >> 12) & 0xF];
    out += kHex[(v >> 8) & 0xF];
    out += kHex[(v >> 4) & 0xF];
    out += kHex[v & 0xF];
}

// Next code point of a UTF-8 string (WTF-8 surrogates accepted). Invalid bytes give U+FFFD (1 byte).
u32 next_cp(std::string_view s, std::size_t& i) {
    const u8 c0 = static_cast<u8>(s[i]);
    auto cont = [&](std::size_t k) { return i + k < s.size() && (static_cast<u8>(s[i + k]) & 0xC0) == 0x80; };
    if (c0 < 0x80) {
        ++i;
        return c0;
    }
    if (c0 >= 0xC2 && c0 <= 0xDF && cont(1)) {
        const u32 cp = ((c0 & 0x1Fu) << 6) | (static_cast<u8>(s[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if (c0 >= 0xE0 && c0 <= 0xEF && cont(1) && cont(2)) {
        const u32 cp = ((c0 & 0x0Fu) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 6) |
                       (static_cast<u8>(s[i + 2]) & 0x3Fu);
        if (cp >= 0x800) {
            i += 3;
            return cp;
        }
    }
    if (c0 >= 0xF0 && c0 <= 0xF4 && cont(1) && cont(2) && cont(3)) {
        const u32 cp = ((c0 & 0x07u) << 18) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 12) |
                       ((static_cast<u8>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<u8>(s[i + 3]) & 0x3Fu);
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            i += 4;
            return cp;
        }
    }
    ++i;
    return 0xFFFD;
}

bool short_escape(std::string& out, u32 c) {
    switch (c) {
    case '"': out += "\\\""; return true;
    case '\\': out += "\\\\"; return true;
    case '\b': out += "\\b"; return true;
    case '\f': out += "\\f"; return true;
    case '\n': out += "\\n"; return true;
    case '\r': out += "\\r"; return true;
    case '\t': out += "\\t"; return true;
    default: return false;
    }
}

void put_string(std::string& out, std::string_view s, bool ensure_ascii) {
    out += '"';
    if (ensure_ascii) {
        std::size_t i = 0;
        while (i < s.size()) {
            const u32 c = next_cp(s, i);
            if (short_escape(out, c)) continue;
            if (c >= 0x20 && c <= 0x7E) {
                out += static_cast<char>(c);
            } else if (c >= 0x10000) {
                const u32 v = c - 0x10000;
                put_u(out, 0xD800 | (v >> 10));
                put_u(out, 0xDC00 | (v & 0x3FF));
            } else {
                put_u(out, c);
            }
        }
    } else {
        for (const char ch : s) {
            const u8 c = static_cast<u8>(ch);
            if (short_escape(out, c)) continue;
            if (c < 0x20) {
                put_u(out, c);
            } else {
                out += ch;  // ASCII and UTF-8 bytes as they are
            }
        }
    }
    out += '"';
}

void put_float(std::string& out, double d) {
    // json.encoder floatstr (allow_nan=True)
    if (std::isnan(d)) {
        out += "NaN";
    } else if (std::isinf(d)) {
        out += d < 0 ? "-Infinity" : "Infinity";
    } else {
        out += py_float_repr(d);
    }
}

void newline(std::string& out, const JsonStyle& st, int level) {
    out += '\n';
    out.append(static_cast<std::size_t>(st.indent) * static_cast<std::size_t>(level), ' ');
}

void dump(std::string& out, const Json& v, const JsonStyle& st, int level) {
    switch (v.v.index()) {
    case 0: out += "null"; return;
    case 1: out += std::get<bool>(v.v) ? "true" : "false"; return;
    case 2: out += std::to_string(std::get<i64>(v.v)); return;
    case 3: put_float(out, std::get<double>(v.v)); return;
    case 4: put_string(out, std::get<std::string>(v.v), st.ensure_ascii); return;
    case 5: {
        const JsonArray& a = std::get<JsonArray>(v.v);
        if (a.empty()) {
            out += "[]";
            return;
        }
        out += '[';
        bool first = true;
        for (const Json& item : a) {
            if (!first) out += st.item_sep;
            first = false;
            if (st.indent >= 0) newline(out, st, level + 1);
            dump(out, item, st, level + 1);
        }
        if (st.indent >= 0) newline(out, st, level);
        out += ']';
        return;
    }
    case 6: {
        const JsonObject& o = std::get<JsonObject>(v.v);
        if (o.empty()) {
            out += "{}";
            return;
        }
        out += '{';
        bool first = true;
        for (const auto& [key, item] : o) {
            if (!first) out += st.item_sep;
            first = false;
            if (st.indent >= 0) newline(out, st, level + 1);
            put_string(out, key, st.ensure_ascii);
            out += st.key_sep;
            dump(out, item, st, level + 1);
        }
        if (st.indent >= 0) newline(out, st, level);
        out += '}';
        return;
    }
    default: return;
    }
}

}  // namespace

std::string json_dumps(const Json& v, const JsonStyle& style) {
    std::string out;
    dump(out, v, style, 0);
    return out;
}

std::string json_lines(std::span<const Json> items, const JsonStyle& style) {
    std::string out = "[\n";
    bool first = true;
    for (const Json& x : items) {
        if (!first) out += ",\n";
        first = false;
        dump(out, x, style, 0);
    }
    out += "\n]\n";
    return out;
}

}  // namespace hdf::io
