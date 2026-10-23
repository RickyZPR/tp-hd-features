// Module D: the XML side of detection (Cemu settings.xml, title_list_cache.xml, perm_setting.xml; Wii U
// meta.xml), title ids and UTF-8 paths. Python spec: tools/dist/hdf_inputs.py _meta_title, the regexes of
// detect_hd and _mlc_dirs, TPHD_TITLES. The Python regexes don't decode entities; Cemu writes '&' as "&amp;",
// so this port decodes the five named entities and numeric references (plan 3.3 D "entity-encoded paths").
// Correction #1(b): the text is untrusted. Every loop is bounded by the input size and every index is checked.
#include "hdf/detect.hpp"

#include "detect_internal.hpp"

#include <algorithm>
#include <array>

namespace hdf::detect {

namespace internal {

namespace {

constexpr std::size_t kMaxDepth = 256;          // element nesting tracked by the parsers
constexpr std::size_t kMaxTitles = 100000;      // title_list_cache.xml entries kept
constexpr std::size_t kMaxGamePaths = 1000;     // settings.xml GamePaths entries kept
constexpr std::size_t kMaxText = 64 * 1024;     // bytes kept per element text (paths are at most ~32 K)
constexpr std::size_t kEntityWindow = 12;       // xml_unescape: the ';' of an entity is at most this far after '&'

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool is_hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

void put_utf8(std::string& out, u32 c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

// Digits only, at most 9 of them (no overflow); nothing otherwise.
std::optional<u32> parse_u32(std::string_view s) {
    if (s.empty() || s.size() > 9) return std::nullopt;
    u32 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<u32>(c - '0');
    }
    return v;
}

bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

void append_text(std::string& dst, const std::string& text) {
    if (dst.size() < kMaxText) dst.append(text, 0, std::min(text.size(), kMaxText - dst.size()));
}

}  // namespace

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

XmlScanner::XmlScanner(std::string_view xml) : s_(xml) {
    if (starts_with(s_, "\xEF\xBB\xBF")) i_ = 3;
}

bool XmlScanner::next(XmlToken& t) {
    while (i_ < s_.size()) {
        if (s_[i_] != '<') {
            std::size_t j = s_.find('<', i_);
            if (j == std::string_view::npos) j = s_.size();
            t.type = XmlToken::Text;
            t.name = {};
            t.attrs = {};
            t.self_closing = false;
            t.text = xml_unescape(s_.substr(i_, j - i_));
            i_ = j;
            return true;
        }
        const std::string_view rest = s_.substr(i_);
        if (starts_with(rest, "<!--")) {
            const std::size_t j = s_.find("-->", i_ + 4);
            if (j == std::string_view::npos) return false;
            i_ = j + 3;
            continue;
        }
        if (starts_with(rest, "<![CDATA[")) {
            const std::size_t j = s_.find("]]>", i_ + 9);
            if (j == std::string_view::npos) return false;
            t.type = XmlToken::Text;
            t.name = {};
            t.attrs = {};
            t.self_closing = false;
            t.text.assign(s_.substr(i_ + 9, j - (i_ + 9)));
            i_ = j + 3;
            return true;
        }
        if (starts_with(rest, "<?")) {
            const std::size_t j = s_.find("?>", i_ + 2);
            if (j == std::string_view::npos) return false;
            i_ = j + 2;
            continue;
        }
        if (starts_with(rest, "<!")) {  // <!DOCTYPE ...> (no internal subset in these files)
            const std::size_t j = s_.find('>', i_ + 2);
            if (j == std::string_view::npos) return false;
            i_ = j + 1;
            continue;
        }
        const bool end_tag = starts_with(rest, "</");
        std::size_t k = i_ + (end_tag ? 2 : 1);
        const std::size_t name_start = k;
        while (k < s_.size() && !is_space(s_[k]) && s_[k] != '>' && s_[k] != '/' && s_[k] != '<') ++k;
        if (k == name_start || k >= s_.size() || s_[k] == '<') return false;
        const std::string_view name = s_.substr(name_start, k - name_start);
        // the closing '>' outside quoted attribute values
        char quote = 0;
        std::size_t close = std::string_view::npos;
        for (std::size_t m = k; m < s_.size(); ++m) {
            const char c = s_[m];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                close = m;
                break;
            } else if (c == '<') {
                return false;
            }
        }
        if (close == std::string_view::npos) return false;
        std::size_t attr_end = close;
        bool self_closing = false;
        if (!end_tag) {
            std::size_t b = close;
            while (b > k && is_space(s_[b - 1])) --b;
            if (b > k && s_[b - 1] == '/') {
                self_closing = true;
                attr_end = b - 1;
            }
        }
        t.type = end_tag ? XmlToken::End : XmlToken::Start;
        t.name = name;
        t.attrs = end_tag ? std::string_view() : s_.substr(k, attr_end - k);
        t.self_closing = self_closing;
        t.text.clear();
        i_ = close + 1;
        return true;
    }
    return false;
}

std::optional<std::string> xml_attr(std::string_view attrs, std::string_view name) {
    std::size_t i = 0;
    while (i < attrs.size()) {
        while (i < attrs.size() && is_space(attrs[i])) ++i;
        const std::size_t n0 = i;
        while (i < attrs.size() && !is_space(attrs[i]) && attrs[i] != '=') ++i;
        const std::string_view an = attrs.substr(n0, i - n0);
        while (i < attrs.size() && is_space(attrs[i])) ++i;
        if (i >= attrs.size() || attrs[i] != '=') {
            if (an.empty()) return std::nullopt;  // nothing more to read
            continue;                             // an attribute without a value: skip it
        }
        ++i;
        while (i < attrs.size() && is_space(attrs[i])) ++i;
        if (i >= attrs.size() || (attrs[i] != '"' && attrs[i] != '\'')) return std::nullopt;
        const char q = attrs[i++];
        const std::size_t v0 = i;
        const std::size_t v1 = attrs.find(q, i);
        if (v1 == std::string_view::npos) return std::nullopt;
        i = v1 + 1;
        if (an == name) return xml_unescape(attrs.substr(v0, v1 - v0));
    }
    return std::nullopt;
}

std::string xml_first_text(std::string_view xml, std::string_view name) {
    XmlScanner sc(xml);
    XmlToken t;
    bool in = false;
    std::string text;
    while (sc.next(t)) {
        if (!in) {
            if (t.type == XmlToken::Start && t.name == name) {
                if (t.self_closing) return {};
                in = true;
            }
        } else if (t.type == XmlToken::Text) {
            append_text(text, t.text);
        } else if (t.type == XmlToken::End && t.name == name) {
            return trim(text);
        } else if (t.type == XmlToken::Start) {
            return {};  // mixed content: not a plain text element
        }
    }
    return {};
}

std::optional<std::filesystem::path> path_from_utf8(std::string_view s) {
    if (s.empty()) return std::nullopt;
#ifdef _WIN32
    std::wstring w;
    w.reserve(s.size());
#else
    std::size_t units = 0;
#endif
    std::size_t i = 0;
    while (i < s.size()) {
        const u8 b0 = static_cast<u8>(s[i]);
        u32 c = 0;
        std::size_t n = 0;
        if (b0 < 0x80) {
            c = b0;
            n = 1;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            c = b0 & 0x1F;
            n = 2;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            c = b0 & 0x0F;
            n = 3;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            c = b0 & 0x07;
            n = 4;
        } else {
            return std::nullopt;
        }
        if (n > s.size() - i) return std::nullopt;
        for (std::size_t k = 1; k < n; ++k) {
            const u8 b = static_cast<u8>(s[i + k]);
            if ((b & 0xC0) != 0x80) return std::nullopt;
            c = (c << 6) | (b & 0x3F);
        }
        if ((n == 3 && c < 0x800) || (n == 4 && (c < 0x10000 || c > 0x10FFFF))) return std::nullopt;  // overlong
        if (c >= 0xD800 && c <= 0xDFFF) return std::nullopt;
        if (c < 0x20 || c == 0x7F) return std::nullopt;
#ifdef _WIN32
        if (c >= 0x10000) {
            w.push_back(static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10)));
            w.push_back(static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            w.push_back(static_cast<wchar_t>(c));
        }
        if (w.size() > 32767) return std::nullopt;
#else
        units += c >= 0x10000 ? 2 : 1;
        if (units > 32767) return std::nullopt;
#endif
        i += n;
    }
#ifdef _WIN32
    return std::filesystem::path(std::move(w));
#else
    return std::filesystem::path(std::string(s));
#endif
}

bool is_hex16(std::string_view s) {
    if (s.size() != 16) return false;
    for (char c : s)
        if (!is_hex_digit(c)) return false;
    return true;
}

std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

std::optional<std::pair<std::string, u32>> top_title(std::string_view top) {
    // hdf_inputs.open_hd: re.match(r'([0-9a-fA-F]{16})_v(\d+)$', top)
    if (top.size() < 19 || top[16] != '_' || top[17] != 'v') return std::nullopt;
    if (!is_hex16(top.substr(0, 16))) return std::nullopt;
    const std::string_view digits = top.substr(18);
    std::optional<u32> v = parse_u32(digits);
    if (!v) {
        // more than 9 digits: still a title folder, the version is just not known
        for (char c : digits)
            if (c < '0' || c > '9') return std::nullopt;
        v = 0;
    }
    return std::pair<std::string, u32>{upper_ascii(top.substr(0, 16)), *v};
}

bool is_tphd_product(std::string_view product) {
    return product.size() >= 9 && upper_ascii(product.substr(0, 9)) == "WUP-P-AZA";
}

std::string tphd_title_of_product(std::string_view product) {
    if (!is_tphd_product(product) || product.size() < 10) return {};
    switch (product[9]) {  // region letter (runtime 8 table)
        case 'E': case 'e': return "000500001019E500";
        case 'P': case 'p': return "000500001019E600";
        case 'J': case 'j': return "000500001019C800";
        default: return {};
    }
}

}  // namespace internal

using namespace internal;

// ---------------------------------------------------------------------------------------------- public parsers
std::string xml_unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c != '&') {
            out.push_back(c);
            ++i;
            continue;
        }
        // Fix pass F1: look for ';' only in the next kEntityWindow bytes. Searching the whole rest of the text for
        // every '&' was quadratic: a crafted 4 MiB settings.xml of '&' took 66 s, with no stop check. The result is
        // the same as before, which also kept an '&' as written when its ';' was more than 12 bytes away. The
        // longest entity known here ("&#x10FFFF;", "&#1114111;") ends 10 bytes after the '&'.
        const std::size_t w = s.substr(i + 1, kEntityWindow).find(';');
        if (w == std::string_view::npos) {
            out.push_back('&');
            ++i;
            continue;
        }
        const std::size_t semi = i + 1 + w;
        const std::string_view ent = s.substr(i + 1, semi - i - 1);
        bool done = true;
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent.size() >= 2 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const std::string_view digits = ent.substr(hex ? 2 : 1);
            u32 v = 0;
            bool ok = !digits.empty() && digits.size() <= (hex ? 6u : 7u);
            for (char d : digits) {
                if (!ok) break;
                if (hex ? !is_hex_digit(d) : (d < '0' || d > '9')) ok = false;
                else v = v * (hex ? 16u : 10u) + static_cast<u32>(hex ? hex_value(d) : d - '0');
            }
            if (ok && v != 0 && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) put_utf8(out, v);
            else done = false;
        } else {
            done = false;
        }
        if (done) {
            i = semi + 1;
        } else {
            out.push_back('&');  // not an entity this parser knows: kept as written
            ++i;
        }
    }
    return out;
}

std::vector<TitleListEntry> parse_title_list(std::string_view xml) {
    // hdf_inputs.detect_hd: <title titleId="(16 hex)"[^>]*>(.*?)</title>, then <format>, <path> (and Cemu's
    // <sub_path> for a .wua) inside it.
    std::vector<TitleListEntry> out;
    XmlScanner sc(xml);
    XmlToken t;
    bool in_title = false;
    TitleListEntry cur;
    std::string field_name, field;
    bool in_field = false;
    std::size_t depth = 0, title_depth = 0;
    while (sc.next(t)) {
        if (t.type == XmlToken::Start) {
            if (!in_title && t.name == "title") {
                cur = TitleListEntry{};
                cur.title_id = trim(xml_attr(t.attrs, "titleId").value_or(""));
                cur.version = parse_u32(trim(xml_attr(t.attrs, "version").value_or(""))).value_or(0);
                if (t.self_closing) {
                    if (out.size() < kMaxTitles) out.push_back(cur);
                    continue;
                }
                in_title = true;
                title_depth = depth;
            } else if (in_title && !in_field && depth == title_depth + 1 &&
                       (t.name == "format" || t.name == "path" || t.name == "sub_path")) {
                field_name.assign(t.name);
                field.clear();
                if (!t.self_closing) in_field = true;
            }
            if (!t.self_closing && ++depth > kMaxDepth) break;
        } else if (t.type == XmlToken::Text) {
            if (in_field) append_text(field, t.text);
        } else {  // End
            if (depth > 0) --depth;
            if (in_field && t.name == field_name) {
                in_field = false;
                const std::string v = trim(field);
                if (field_name == "format") cur.format = static_cast<int>(parse_u32(v).value_or(0));
                else if (field_name == "path") cur.path = v;
                else cur.sub_path = v;
            } else if (in_title && t.name == "title" && depth == title_depth) {
                in_title = false;
                in_field = false;
                if (out.size() < kMaxTitles) out.push_back(cur);
            }
        }
    }
    return out;
}

CemuSettings parse_settings(std::string_view xml) {
    // hdf_inputs._mlc_dirs: <mlc_path>([^<]+)</mlc_path>; GamePaths: the <Entry> children of <GamePaths>
    // (runtime 8; RecentLaunchFiles and GraphicPack also hold <Entry> elements, so only these are taken).
    CemuSettings out;
    XmlScanner sc(xml);
    XmlToken t;
    std::vector<std::string_view> stack;
    std::string text;
    bool have_mlc = false;
    enum { None, Mlc, Entry } capture = None;
    std::size_t capture_depth = 0;
    while (sc.next(t)) {
        if (t.type == XmlToken::Start) {
            if (capture == None && !t.self_closing) {
                if (!have_mlc && t.name == "mlc_path" && stack.size() == 1) {
                    capture = Mlc;
                } else if (t.name == "Entry" && !stack.empty() && stack.back() == "GamePaths") {
                    capture = Entry;
                }
                if (capture != None) {
                    text.clear();
                    capture_depth = stack.size();
                }
            }
            if (!t.self_closing) {
                if (stack.size() >= kMaxDepth) break;
                stack.push_back(t.name);
            }
        } else if (t.type == XmlToken::Text) {
            if (capture != None) append_text(text, t.text);
        } else {
            if (!stack.empty() && stack.back() == t.name) stack.pop_back();
            else break;  // not well-formed: keep what was read so far
            if (capture != None && stack.size() == capture_depth) {
                std::string v = trim(text);
                if (capture == Mlc) {
                    out.mlc_path = std::move(v);
                    have_mlc = true;
                } else if (!v.empty() && out.game_paths.size() < kMaxGamePaths) {
                    out.game_paths.push_back(std::move(v));
                }
                capture = None;
            }
        }
    }
    return out;
}

MetaXml parse_meta(std::string_view xml) {
    // hdf_inputs._meta_title: <title_id[^>]*>\s*([0-9A-Fa-f]{16})\s*<, <title_version[^>]*>\s*(\d+)\s*<;
    // product_code as plan 2.5 / runtime 8 ("WUP-P-AZA").
    MetaXml out;
    XmlScanner sc(xml);
    XmlToken t;
    std::size_t depth = 0;
    std::string_view field;
    std::string text;
    bool have_id = false, have_code = false, have_version = false;
    while (sc.next(t)) {
        if (t.type == XmlToken::Start) {
            if (!t.self_closing) {
                if (depth == 1 && (t.name == "title_id" || t.name == "product_code" || t.name == "title_version")) {
                    field = t.name;
                    text.clear();
                }
                if (++depth > kMaxDepth) break;
            }
        } else if (t.type == XmlToken::Text) {
            if (!field.empty()) append_text(text, t.text);
        } else {
            if (depth > 0) --depth;
            if (!field.empty() && depth == 1 && t.name == field) {
                const std::string v = trim(text);
                if (field == "title_id" && !have_id) {
                    if (is_hex16(v)) out.title_id = upper_ascii(v);
                    have_id = true;
                } else if (field == "product_code" && !have_code) {
                    out.product_code = v;
                    have_code = true;
                } else if (field == "title_version" && !have_version) {
                    out.title_version = parse_u32(v);
                    have_version = true;
                }
                field = {};
            }
        }
    }
    return out;
}

bool is_tphd_title(std::string_view title_id) {
    // hdf_inputs: tid[8:].upper() in TPHD_TITLES ('1019E500' USA, '1019C800' JPN per runtime 8, '1019E600' EUR).
    if (!is_hex16(title_id)) return false;
    const std::string low = upper_ascii(title_id.substr(8));
    return low == "1019E500" || low == "1019E600" || low == "1019C800";
}

}  // namespace hdf::detect
