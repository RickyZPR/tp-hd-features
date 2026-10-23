// Module TX: line fitting (hdtext.FontMetrics, gc_line_limits, _lines, _visible, _vis_bytes, _words, _join_words,
// _wrap_para, _title_end, _sentence_end, _split_sentences, _wrap_chunk, _pack, rewrap).
//
// Floats: FontMetrics.char is (w * 23) / float(cell_w) + 1 in doubles, width() a plain left-to-right double sum
// (hdtext.py:568, Python 3.12's compensated sum() is NOT used) rounded with py_round_int, _wrap_para adds
// (cw + sp) + ww in that order. All integer partial sums are exact in doubles, so starting at 0.0 equals
// Python's int 0 start.
#include "text_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hdf::text::detail {

namespace {

std::unexpected<Error> bad(const std::string& what) { return fail("fmt.text", "rodan_b_24_22.bfn: " + what); }

using Para = std::vector<Line>;  // a paragraph: lines

bool ends_with_stop(const Bytes& b) {  // bytes[-1:] in (b'!', b'.', b'?')
    return !b.empty() && (b.back() == '!' || b.back() == '.' || b.back() == '?');
}

bool contains(const Bytes& hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [](u8 a, char b) { return a == static_cast<u8>(b); }) != hay.end();
}

// hdtext._wrap_para
Para wrap_para(const Para& lines, int limit, const FontMetrics& fm) {
    std::vector<Line> words;
    for (const Line& l : lines)
        for (Line& w : words_of(l)) words.push_back(std::move(w));
    Para out;
    Line cur;
    double cw = 0;
    const double sp = fm.char_w(32);
    for (const Line& w : words) {
        const int ww = fm.width(w);
        if (!cur.empty() && cw + sp + ww > limit) {
            out.push_back(std::move(cur));
            cur.clear();
            cw = 0;
        }
        if (!cur.empty()) {
            cur.push_back(Tok::ch(32));
            cw += sp;
        }
        cur.insert(cur.end(), w.begin(), w.end());
        cw += ww;
    }
    out.push_back(std::move(cur));
    return out;
}

// hdtext._split_sentences: one paragraph -> one pseudo-paragraph (a single line) per sentence
std::vector<Para> split_sentences(const Para& lines) {
    std::vector<Para> out;
    std::vector<Line> cur;
    std::vector<Line> words;
    for (const Line& l : lines)
        for (Line& w : words_of(l)) words.push_back(std::move(w));
    for (std::size_t i = 0; i < words.size(); ++i) {
        cur.push_back(words[i]);
        if (ends_with_stop(vis_bytes(words[i])) && i + 1 < words.size()) {
            out.push_back(Para{join_words(cur)});
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(Para{join_words(cur)});
    return out;
}

// hdtext._wrap_chunk: one 4-line page -> paragraphs ([[]] = a kept blank line)
std::vector<Para> wrap_chunk(const std::vector<Line>& chunk, int limit, const FontMetrics& fm,
                             bool (*hard)(const Line&), bool sentences) {
    std::size_t k = chunk.size();
    while (k > 0 && !visible(chunk[k - 1]) && chunk[k - 1].empty()) --k;
    std::vector<std::optional<Para>> paras;  // nullopt = None
    Para cur;
    for (std::size_t j = 0; j < k; ++j) {
        const Line& l = chunk[j];
        if (!visible(l)) {
            if (!cur.empty()) {
                paras.emplace_back(std::move(cur));
                cur.clear();
            }
            if (l.empty())
                paras.emplace_back(std::nullopt);
            else
                paras.emplace_back(Para{l});
            continue;
        }
        cur.push_back(l);
        if (hard(l)) {
            paras.emplace_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) paras.emplace_back(std::move(cur));
    std::vector<Para> out;
    for (const auto& p : paras) {
        if (!p) {
            out.push_back(Para{Line{}});
        } else if (p->size() == 1 && !visible((*p)[0])) {
            out.push_back(Para{(*p)[0]});
        } else if (sentences) {
            for (const Para& s : split_sentences(*p)) out.push_back(wrap_para(s, limit, fm));
        } else {
            out.push_back(wrap_para(*p, limit, fm));
        }
    }
    return out;
}

// hdtext._pack: lay paragraphs out on 4-line pages
std::vector<Line> pack(const std::vector<Para>& paras) {
    std::vector<Line> out;
    for (const Para& p : paras) {
        const std::size_t used = out.size() % kLineMax;
        if (p.size() == 1 && p[0].empty()) {  // p == [[]]
            if (used) out.emplace_back();
            continue;
        }
        if (used && used + p.size() > kLineMax && p.size() <= kLineMax)
            for (std::size_t n = 0; n < kLineMax - used; ++n) out.emplace_back();
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

int max_width(const std::vector<Line>& lines, const FontMetrics& fm) {
    int m = 0;
    bool first = true;
    for (const Line& l : lines) {
        const int w = fm.width(l);
        if (first || w > m) m = w;
        first = false;
    }
    return m;
}

}  // namespace

bool wrap_box_kind(u8 box) {
    return std::find(kWrapBoxKinds.begin(), kWrapBoxKinds.end(), box) != kWrapBoxKinds.end();
}

// ---------------------------------------------------------------------------------------------- FontMetrics
// FontMetrics.__init__: BFN 'FONTbfn1', header 0x20, blocks INF1 / WID1 / MAP1 / GLY1.
Result<FontMetrics> font_metrics(View f) {
    if (f.size() < 8 || std::memcmp(f.data(), "FONTbfn1", 8) != 0) return bad("not a BFN");
    FontMetrics fm;
    bool have_cell_w = false;
    std::size_t o = 0x20;
    while (o + 8 <= f.size()) {
        const u32 sz = io::be32(f, o + 4);
        if (sz < 8) break;
        const std::string_view tag(reinterpret_cast<const char*>(f.data() + o), 4);
        if (tag == "INF1") {
            if (o + 0x10 > f.size()) return bad("INF1 truncated");
            fm.cell = io::be16(f, o + 0xE);
        } else if (tag == "GLY1" && !have_cell_w) {
            if (o + 0xE > f.size()) return bad("GLY1 truncated");
            fm.cell_w = io::be16(f, o + 0xC);  // JUTResFont::getCellWidth (first glyph block)
            have_cell_w = true;
        } else if (tag == "WID1") {
            if (o + 12 > f.size()) return bad("WID1 truncated");
            const int s = io::be16(f, o + 8), e = io::be16(f, o + 10);
            for (int i = 0; i < e - s + 1; ++i) {
                const std::size_t at = o + 13 + 2 * static_cast<std::size_t>(i);
                if (at >= f.size()) return bad("WID1 truncated");
                fm.wid[s + i] = f[at];
            }
        } else if (tag == "MAP1") {
            if (o + 16 > f.size()) return bad("MAP1 truncated");
            FontMetrics::Map m;
            m.type = io::be16(f, o + 8);
            m.first = io::be16(f, o + 10);
            m.last = io::be16(f, o + 12);
            const u32 n = io::be16(f, o + 14);
            if (o + 16 + 2 * static_cast<std::size_t>(n) > f.size()) return bad("MAP1 truncated");
            for (u32 i = 0; i < n; ++i) m.table.push_back(io::be16(f, o + 16 + 2 * i));
            fm.maps.push_back(std::move(m));
        }
        o += sz;
    }
    if (!have_cell_w || fm.cell_w == 0) fm.cell_w = fm.cell;  // self.cell_w or self.cell
    if (fm.cell_w == 0) return bad("cell width 0");
    for (int c = 0; c < 256; ++c) fm.chars[static_cast<std::size_t>(c)] = fm.char_w(c);
    fm.name = kNameChars * fm.char_w('n');
    return fm;
}

// FontMetrics.char: one character's advance in screen px.
double FontMetrics::char_w(int c) const {
    int w = cell;
    for (const Map& m : maps) {
        if (m.first <= c && c <= m.last) {
            std::optional<int> g;
            if (m.type == 0)
                g = c - m.first;
            else if (m.type == 2 && static_cast<std::size_t>(c - m.first) < m.table.size())
                g = m.table[static_cast<std::size_t>(c - m.first)];
            if (g) {
                const auto it = wid.find(*g);
                if (it != wid.end()) {
                    w = it->second;
                    break;
                }
            }
        }
    }
    return static_cast<double>(w * kFontSizeX) / static_cast<double>(cell_w) + kCharSpace;
}

// FontMetrics.tok
double FontMetrics::tok(const Tok& t) const {
    if (!t.tag) return chars[t.c];
    const u8 g = t.c;
    const u16 i = t.id;
    if (g == 0 && (i == 0 || i == 34)) return name;
    if ((g == 0 && ((10 <= i && i <= 29) || i == 35 || i == 36 || i == 42 || i == 46 || i == 47 || i == 48 || i == 57)) ||
        (g == 3 && 1 <= i && i <= 20) || (g == 6 && 2 <= i && i <= 11))
        return kFontSizeX + kCharSpace;  // outfont icon / glyph (do_outfont / do_space)
    return 0;
}

// FontMetrics.width: a plain left-to-right sum, then int(round(w)).
int FontMetrics::width(const std::vector<Tok>& toks) const {
    double w = 0;
    for (const Tok& t : toks) w += tok(t);
    return static_cast<int>(io::py_round_int(w));
}

int LineLimits::get(int box) const {
    const auto it = limit.find(box);
    if (it != limit.end()) return it->second;
    const auto t = limit.find(0);
    return t != limit.end() ? t->second : 450;
}

// hdtext.gc_line_limits: box kind -> 99th percentile of the widest line per GC message of that kind.
LineLimits gc_line_limits(const std::vector<const Bmg*>& gc_bmgs, const FontMetrics& fm) {
    std::map<int, std::vector<int>> per;
    for (const Bmg* b : gc_bmgs) {
        for (const Entry& e : b->entries) {
            if (e.text.size() <= 1) continue;
            auto toks = tokens(e.text);
            if (!toks) continue;  // except ValueError
            const int w = max_width(lines_of(*toks), fm);
            per[e.entry[9]].push_back(w);
        }
    }
    LineLimits out;
    for (auto& [k, v] : per) {
        std::stable_sort(v.begin(), v.end());
        const std::size_t n = v.size();
        const std::size_t at = std::min(n - 1, static_cast<std::size_t>(static_cast<double>(n) * 0.99));
        out.limit[k] = v[at];
        out.samples[k] = static_cast<int>(n);
    }
    const auto t0 = out.limit.find(0);
    const int talk = t0 != out.limit.end() ? t0->second : 450;  // out.get(0, 450)
    for (u8 k : kWrapBoxKinds) {
        const auto it = per.find(k);
        if (it == per.end() || it->second.size() < 50) {
            const auto cur = out.limit.find(k);
            out.limit[k] = std::min(cur != out.limit.end() ? cur->second : talk, talk);  // too few GC samples
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- lines, words
std::vector<Line> lines_of(const Toks& toks) {
    std::vector<Line> lines(1);
    for (const Tok& t : toks) {
        if (t.is_c(10))
            lines.emplace_back();
        else
            lines.back().push_back(t);
    }
    return lines;
}

bool visible(const Line& line) {
    for (const Tok& t : line)
        if (!t.tag && t.c != 32) return true;
    for (const Tok& t : line)
        if (t.tag && t.c == 0 && (t.id == 0 || t.id == 34)) return true;
    return false;
}

Bytes vis_bytes(const Line& word) {
    Bytes b;
    for (const Tok& t : word)
        if (!t.tag) b.push_back(t.c);
    return b;
}

// hdtext._words: split at spaces; tags stay glued to their word; a number stays glued to a following "Rupee(s)".
std::vector<Line> words_of(const Line& line) {
    std::vector<Line> words;
    Line cur;
    for (const Tok& t : line) {
        if (t.is_c(32)) {
            if (!cur.empty()) {
                words.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(t);
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    std::vector<Line> out;
    for (Line& w : words) {
        Bytes prev;
        if (!out.empty()) prev = vis_bytes(out.back());
        bool digits = !prev.empty();
        for (u8 ch : prev)
            if (!((ch >= '0' && ch <= '9') || ch == ',')) digits = false;
        const Bytes wv = vis_bytes(w);
        const bool rupee = wv.size() >= 5 && std::memcmp(wv.data(), "Rupee", 5) == 0;
        if (!out.empty() && digits && rupee) {  // "50 Rupees" never split
            out.back().push_back(Tok::ch(32));
            out.back().insert(out.back().end(), w.begin(), w.end());
        } else {
            out.push_back(std::move(w));
        }
    }
    return out;
}

Line join_words(const std::vector<Line>& words) {
    Line line;
    for (const Line& w : words) {
        if (!line.empty()) line.push_back(Tok::ch(32));
        line.insert(line.end(), w.begin(), w.end());
    }
    return line;
}

// hdtext._title_end: item-get title ({0,1}...{0,2}) or shop header (a {255,0:06} Rupee price).
bool title_end(const Line& line) {
    const Tok* last = nullptr;
    for (const Tok& t : line)
        if (!(t.tag && t.c == 255)) last = &t;
    if (last && last->tag && last->c == 0 && last->id == 2) return true;  // ...{0,2} INSTANT off
    const Tok price = Tok::t(255, 0, Bytes{0x06});
    return std::find(line.begin(), line.end(), price) != line.end() && contains(vis_bytes(line), "Rupee");
}

bool sentence_end(const Line& line) {
    Bytes vis = vis_bytes(line);
    while (!vis.empty() && vis.back() == ' ') vis.pop_back();  // rstrip(b' ')
    return title_end(line) || ends_with_stop(vis);
}

// hdtext.rewrap -> (text, note or None). Only pages with a line wider than `limit` are touched.
Result<Rewrapped> rewrap(View text, int limit, const FontMetrics& fm) {
    auto toks = tokens(text);
    if (!toks) return std::unexpected(toks.error());
    const std::vector<Line> lines = lines_of(*toks);
    const int widest = max_width(lines, fm);
    char buf[200];
    if (widest <= limit) return Rewrapped{Bytes(text.begin(), text.end()), std::nullopt};
    for (const Tok& t : *toks) {
        if (t.tag && t.c == 0 && (t.id == 8 || t.id == 9)) {
            std::snprintf(buf, sizeof buf, "selection message wider than GC (%d > %d px): left as is", widest, limit);
            return Rewrapped{Bytes(text.begin(), text.end()), std::string(buf)};
        }
    }
    std::vector<std::vector<Line>> chunks;
    for (std::size_t i = 0; i < lines.size(); i += kLineMax)
        chunks.emplace_back(lines.begin() + static_cast<std::ptrdiff_t>(i),
                            lines.begin() + static_cast<std::ptrdiff_t>(std::min(lines.size(), i + kLineMax)));
    std::vector<Line> out;
    for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
        const std::vector<Line>& ch = chunks[ci];
        std::vector<Line> nw;
        if (max_width(ch, fm) <= limit) {
            nw = ch;
        } else {
            nw = pack(wrap_chunk(ch, limit, fm, &sentence_end, false));
            if (nw.size() > kLineMax) {
                std::vector<Line> alt = pack(wrap_chunk(ch, limit, fm, &title_end, false));
                nw = alt.size() <= kLineMax ? std::move(alt) : pack(wrap_chunk(ch, limit, fm, &sentence_end, true));
            }
        }
        out.insert(out.end(), nw.begin(), nw.end());
        if (ci + 1 < chunks.size())
            while (out.size() % kLineMax) out.emplace_back();
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    if (out.empty()) return fail("fmt.text", "rewrap: no lines left");  // Python: max() of an empty sequence
    Toks res;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i) res.push_back(Tok::ch(10));
        res.insert(res.end(), out[i].begin(), out[i].end());
    }
    Rewrapped r;
    r.text = encode(res);
    const std::size_t pages = (out.size() + kLineMax - 1) / kLineMax;
    std::snprintf(buf, sizeof buf,
                  "lines re-wrapped to the GC window width (widest %d -> %d px, limit %d; %zu -> %zu lines, %zu page%s)",
                  widest, max_width(out, fm), limit, lines.size(), out.size(), pages, pages == 1 ? "" : "s");
    r.note = std::string(buf);
    return r;
}

}  // namespace hdf::text::detail
