// Module TX: HD text -> GC text (hdtext._ascii, convert_text, preview, _attr_diff, _strip_revo, build_message).
#include "text_internal.hpp"

#include <algorithm>
#include <cstdio>

namespace hdf::text::detail {

namespace {

// hdtext._ascii(c) for c = 0x80..0xFF, made with Python 3's unicodedata (NFKD) by
//   [hdtext._ascii(c) for c in range(0x80, 0x100)]   (sha256 of '\n'.join(table) = 9f83d410e5371ed9...1332a6)
// The C++ library has no Unicode database, so the table is precomputed. It is this project's own derived data:
// a mapping of the CP1252 code page, no game data.
constexpr std::array<std::string_view, 128> kAscii = {
    "?", "?", "?", "?", "?", "...", "?", "?",        // 0x80
    "?", "?", "S", "?", "?", "?", "Z", "?",          // 0x88
    "?", "'", "'", "\"", "\"", "?", "-", "-",        // 0x90
    " ", "TM", "s", "?", "?", "?", "z", "Y",         // 0x98
    " ", "?", "?", "?", "?", "?", "?", "?",          // 0xA0
    " ", "(c)", "a", "?", "?", "?", "(R)", " ",      // 0xA8
    "?", "?", "2", "3", " ", "?", "?", "?",          // 0xB0
    " ", "1", "o", "?", "14", "12", "34", "?",       // 0xB8
    "A", "A", "A", "A", "A", "A", "?", "C",          // 0xC0
    "E", "E", "E", "E", "I", "I", "I", "I",          // 0xC8
    "?", "N", "O", "O", "O", "O", "O", "x",          // 0xD0
    "?", "U", "U", "U", "U", "Y", "?", "?",          // 0xD8
    "a", "a", "a", "a", "a", "a", "?", "c",          // 0xE0
    "e", "e", "e", "e", "i", "i", "i", "i",          // 0xE8
    "?", "n", "o", "o", "o", "o", "o", "?",          // 0xF0
    "?", "u", "u", "u", "u", "y", "?", "y",          // 0xF8
};

// CP1252 0x80-0x9F (as src/stamps/hdtext.hpp:245, but Python's decode('cp1252', 'replace') gives U+FFFD for the
// 5 undefined bytes 0x81 0x8D 0x8F 0x90 0x9D, where the mod's display table uses '?').
constexpr std::array<char16_t, 32> kCp1252High = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

void put_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

std::string hex(View b) {
    static constexpr char k[] = "0123456789abcdef";
    std::string s;
    for (u8 x : b) {
        s += k[x >> 4];
        s += k[x & 15];
    }
    return s;
}

// Python repr() of an ASCII str (only for the conversion notes, which no kept output contains).
std::string py_repr(std::string_view s) {
    const bool dq = s.find('\'') != std::string_view::npos && s.find('"') == std::string_view::npos;
    const char q = dq ? '"' : '\'';
    std::string out(1, q);
    for (char c : s) {
        if (c == '\\' || c == q) out += '\\';
        out += c;
    }
    out += q;
    return out;
}

Toks lit(std::string_view s) {  // hdtext._lit
    Toks out;
    for (char c : s) out.push_back(Tok::ch(static_cast<u8>(c)));
    return out;
}

// hdtext.TAG7
struct Tag7 {
    u16 id;
    Toks rep;
    const char* note;
};
const std::vector<Tag7>& tag7() {
    static const std::vector<Tag7> t = {
        {1, {Tok::t(0, 19)}, "HD {7,1} (L stick) -> GC {0,19} Control Stick"},
        {2, {Tok::t(0, 12)}, "HD {7,2} (R stick) -> GC {0,12} C Stick"},
        {4, {Tok::t(0, 12)}, "HD {7,4} (R stick view) -> GC {0,12} C Stick"},
        {5, {Tok::t(0, 19)}, "HD {7,5} (gyro aim) -> GC {0,19} Control Stick"},
        {6, {Tok::t(0, 13)}, "HD {7,6} (ZL) -> GC {0,13} L"},
        {7, {Tok::t(0, 14)}, "HD {7,7} (ZR) -> GC {0,14} R"},
        {8, {Tok::t(3, 12)}, "HD {7,8} (map button) -> GC {3,12} (GC zel_00 #136 map prompt)"},
        {9, lit("START"), "HD {7,9} (+ button) -> \"START\" (GC wording)"},
        {13, lit("the NFC touchpoint"), "HD {7,13} (GamePad NFC touchpoint glyph) -> \"the NFC touchpoint\""},
    };
    return t;
}

// hdtext._ITEM3 / _ITEM2
const Toks& item3() {
    static const Toks t = {Tok::t(0, 16), Tok::ch(44), Tok::ch(32), Tok::t(0, 15), Tok::ch(44), Tok::ch(32),
                           Tok::ch(111), Tok::ch(114), Tok::ch(32), Tok::t(0, 14)};
    return t;
}
const Toks& item2() {
    static const Toks t = {Tok::t(0, 16), Tok::ch(32), Tok::ch(111), Tok::ch(114), Tok::ch(32), Tok::t(0, 15)};
    return t;
}

const char* btn(u16 i) {  // hdtext.BTN
    switch (i) {
    case 10: return "A";
    case 11: return "B";
    case 12: return "C Stick";
    case 13: return "L";
    case 14: return "R";
    case 15: return "X";
    case 16: return "Y";
    case 17: return "Z";
    case 18: return "+Control Pad";
    case 19: return "Control Stick";
    case 20: return "<";
    case 21: return ">";
    case 22: return "^";
    case 23: return "v";
    case 24: return "Control Stick Up";
    case 25: return "Control Stick Down";
    case 26: return "Control Stick Left";
    case 27: return "Control Stick Right";
    case 28: return "Control Stick Up/Down";
    case 29: return "Control Stick Left/Right";
    case 35: return "red target";
    case 36: return "yellow target";
    case 42: return "white target";
    case 57: return "heart";
    default: return nullptr;
    }
}

const char* btn3(u16 i) {  // hdtext.BTN3
    switch (i) {
    case 1: return "A";
    case 2: return "B";
    case 6: return "D-Pad Up";
    case 7: return "D-Pad Down";
    case 9: return "D-Pad Up";
    case 12: return "D-Pad Right";
    case 20: return "Z";
    default: return nullptr;
    }
}

constexpr u16 kLabelsGc = 822;  // LABELS_GC

// hdtext._attr_diff reduced to "same_attrs": no difference in a used INF1 attribute (UNUSED_ATTRS item_no 0x0C,
// lines_per_page 0x12 and pad 0x13 don't count). Offset 6 is the u16 event label.
bool same_used_attrs(const std::array<u8, 20>& hd_e, const std::array<u8, 20>& gc_e) {
    if (hd_e[6] != gc_e[6] || hd_e[7] != gc_e[7]) return false;
    for (int off : {8, 9, 10, 11, 13, 14, 15, 16, 17})
        if (hd_e[static_cast<std::size_t>(off)] != gc_e[static_cast<std::size_t>(off)]) return false;
    return true;
}

}  // namespace

std::string_view ascii_fallback(u8 c) { return c < 0x80 ? std::string_view{} : kAscii[c - 0x80u]; }

char32_t cp1252(u8 c) {
    if (c >= 0x80 && c < 0xA0) return kCp1252High[c - 0x80u];
    return c;
}

// hdtext.convert_text
Result<Converted> convert_text(View text, const std::set<u8>& gc_chars) {
    Converted r;
    auto toks = tokens(text);
    if (!toks) return std::unexpected(toks.error());
    Toks out;
    char buf[96];
    for (Tok& t : *toks) {
        if (t.tag && t.c == 3 && t.id == 0) {
            r.notes.push_back("strip {3,0} Wii message-id override (" + hex(t.args) + ")");
            continue;
        }
        if (t.tag && t.c == 7) {
            const Tag7* rep = nullptr;
            for (const Tag7& x : tag7())
                if (x.id == t.id) rep = &x;
            if (!rep) {
                std::snprintf(buf, sizeof buf, "remove unknown HD {7,%d}", t.id);
                r.notes.push_back(buf);
                continue;
            }
            r.notes.push_back(rep->note);
            out.insert(out.end(), rep->rep.begin(), rep->rep.end());
            continue;
        }
        out.push_back(std::move(t));
    }
    Toks res;
    const Toks& i3 = item3();
    std::size_t i = 0;
    while (i < out.size()) {
        if (out.size() - i >= i3.size() && std::equal(i3.begin(), i3.end(), out.begin() + static_cast<std::ptrdiff_t>(i))) {
            res.insert(res.end(), item2().begin(), item2().end());
            r.notes.push_back("HD \"{0,16}, {0,15}, or {0,14}\" (Y/X/R) -> GC \"{0,16} or {0,15}\"");
            i += i3.size();
            continue;
        }
        res.push_back(out[i]);
        ++i;
    }
    Toks fin;
    for (Tok& t : res) {
        if (!t.tag && t.c >= 0x80 && !gc_chars.contains(t.c)) {
            const std::string_view s = ascii_fallback(t.c);
            std::snprintf(buf, sizeof buf, "char 0x%02x -> ", t.c);
            r.notes.push_back(buf + py_repr(s));
            Toks l = lit(s);
            fin.insert(fin.end(), l.begin(), l.end());
        } else if (t.tag && (!gc_tag_known(t.c, t.id) ||
                             (t.c == 255 && t.id == 0 && t.args.size() != 1 && t.args.size() != 4 && t.args.size() != 8))) {
            std::snprintf(buf, sizeof buf, "remove tag {%d,%d} unknown to v2.0.3", t.c, t.id);
            r.notes.push_back(buf);
        } else {
            fin.push_back(std::move(t));
        }
    }
    r.text = encode(fin);
    return r;
}

// hdtext.preview: plain UTF-8, HD's own line breaks, tags as [A], {player}, ...
Result<std::string> preview(View gc_text) {
    auto toks = tokens(gc_text);
    if (!toks) return std::unexpected(toks.error());
    std::string out;
    for (const Tok& t : *toks) {
        if (!t.tag) {
            put_utf8(out, cp1252(t.c));
            continue;
        }
        const u8 g = t.c;
        const u16 i = t.id;
        if (g == 0 && i == 0) {
            out += "{player}";
        } else if (g == 0 && i == 34) {
            out += "{horse}";
        } else if (g == 0 && btn(i)) {
            out += std::string("[") + btn(i) + "]";
        } else if (g == 3 && btn3(i)) {
            out += std::string("[") + btn3(i) + "]";
        } else if (g == 6 && (i == 10 || i == 11)) {
            out += i == 10 ? "* " : "  ";
        }
    }
    return out;
}

// hdtext._strip_revo
Result<Bytes> strip_revo(View text) {
    auto toks = tokens(text);
    if (!toks) return std::unexpected(toks.error());
    Toks keep;
    for (Tok& t : *toks)
        if (!(t.tag && t.c == 3 && t.id == 0)) keep.push_back(std::move(t));
    return encode(keep);
}

std::string_view action_name(Action a) {
    switch (a) {
    case Action::Native: return "native";
    case Action::Override: return "override";
    case Action::Register: return "register";
    }
    return "register";
}

std::string Message::key() const { return file + ":" + std::to_string(id); }

// hdtext.build_message
Result<std::optional<Message>> build_message(const Bmg& hd, const Bmg& gc, std::string_view fname, u16 mid,
                                             std::string_view category, std::string_view use, u8 group,
                                             const Ctx& ctx) {
    const Entry* he = hd.get(mid);
    if (!he || he->text.size() <= 1) return std::optional<Message>{};
    auto conv = convert_text(he->text, ctx.gc_chars);
    if (!conv) return std::unexpected(conv.error());
    Message m;
    m.notes = std::move(conv->notes);
    Bytes text = std::move(conv->text);
    auto pv = preview(text);  // text_hd_lines: HD's own line breaks
    if (!pv) return std::unexpected(pv.error());
    m.preview = std::move(*pv);
    const u8 box = he->entry[9];
    const Entry* ge0 = gc.get(mid);
    bool same_as_gc = false;
    if (ge0 && ge0->text.size() > 1) {
        auto s = strip_revo(ge0->text);
        if (!s) return std::unexpected(s.error());
        same_as_gc = *s == text;
    }
    if (ctx.fm && wrap_box_kind(box) && !same_as_gc) {  // GC's own text already fits
        auto rw = rewrap(text, ctx.limits.get(box), *ctx.fm);
        if (!rw) return std::unexpected(rw.error());
        text = std::move(rw->text);
        if (rw->note) m.notes.push_back(*rw->note);
    }
    std::array<u8, 20> entry = he->entry;
    for (int k = 0; k < 6; ++k) entry[static_cast<std::size_t>(k)] = 0;
    const u16 label = static_cast<u16>((entry[6] << 8) | entry[7]);
    if (label >= kLabelsGc) {
        m.notes.push_back("INF1 event_label " + std::to_string(label) + " >= 822 -> 0");
        entry[6] = entry[7] = 0;
    }
    const Entry* ge = gc.get(mid);
    bool same_text = false, same_attrs = false;
    if (ge) {
        Bytes gtext{0};
        if (ge->text.size() > 1) {
            auto s = strip_revo(ge->text);
            if (!s) return std::unexpected(s.error());
            gtext = std::move(*s);
        }
        same_text = gtext == text;
        same_attrs = same_used_attrs(entry, ge->entry);
        m.gc_index = ge->index;
    }
    if (ge && same_text && same_attrs) {
        m.action = Action::Native;
    } else if (ge && (same_attrs || use == "item-get" || use == "item-text" || use == "shop")) {
        m.action = Action::Override;  // item-get/text, shop: the game looks the id up itself: override only
    } else {
        m.action = Action::Register;
    }
    m.file = std::string(fname);
    m.file_no = fname == "zel_00" ? 0 : fname == "zel_04" ? 4 : 5;  // FILE_NO
    m.group = group;
    m.id = mid;
    m.hd_index = he->index;
    m.entry = entry;
    m.text = std::move(text);
    m.use = std::string(use);
    m.category = std::string(category);
    return std::optional<Message>(std::move(m));
}

}  // namespace hdf::text::detail
