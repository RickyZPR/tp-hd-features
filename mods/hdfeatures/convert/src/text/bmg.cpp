// Module TX: the MESGbmg1 reader and the text tokens (hdtext.Bmg, load_bmg, tokens, encode, show, gc_tag_known,
// valid_encoded_text). Layout as tools/formats/bmg.py's docstring and Dusklight svc/flow.cpp parse_resource.
//
// The inputs reach this code only after their full SHA-256 matched (Correction #1), but every offset is still
// bounds-checked: where Python would raise (struct.error, IndexError) this returns Error{"fmt.text"}.
#include "text_internal.hpp"

#include <cstdio>
#include <cstring>

namespace hdf::text::detail {

namespace {

std::unexpected<Error> bad(const std::string& name, const std::string& what) {
    return fail("fmt.text", name + ": " + what);
}

}  // namespace

const Entry* Bmg::get(u16 id) const {
    const auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &entries[it->second];
}

std::optional<u16> Bmg::flow(u32 id) const {
    const auto it = flow_index.find(id);
    if (it == flow_index.end()) return std::nullopt;
    return it->second;
}

// hdtext.Bmg.__init__
Result<Bmg> parse_bmg(View buf, std::string name) {
    if (buf.size() < 8 || std::memcmp(buf.data(), "MESGbmg1", 8) != 0) return bad(name, "not MESGbmg1");
    if (buf.size() < 0x20) return bad(name, "header truncated");
    Bmg b;
    b.name = std::move(name);
    b.encoding = buf[0x10];
    struct Sec {
        std::string tag;
        std::size_t off = 0, size = 0;
    };
    std::vector<Sec> secs;  // dict: a later tag overwrites
    auto find = [&](std::string_view tag) -> const Sec* {
        for (const Sec& s : secs)
            if (s.tag == tag) return &s;
        return nullptr;
    };
    std::size_t o = 0x20;
    const u32 nsec = io::be32(buf, 0xC);
    for (u32 k = 0; k < nsec; ++k) {
        if (o + 8 > buf.size()) return bad(b.name, "bad section");
        const std::string tag(reinterpret_cast<const char*>(buf.data() + o), 4);
        std::size_t sz = io::be32(buf, o + 4);
        if (sz < 8) return bad(b.name, "bad section " + tag);
        sz = std::min(sz, buf.size() - o);  // HD's last section size counts padding past the file end
        bool replaced = false;
        for (Sec& s : secs)
            if (s.tag == tag) {
                s.off = o;
                s.size = sz;
                replaced = true;
            }
        if (!replaced) secs.push_back(Sec{tag, o, sz});
        o += sz;
    }
    const Sec* inf = find("INF1");
    const Sec* dat = find("DAT1");
    if (!inf || !dat) return bad(b.name, "no INF1/DAT1");
    if (inf->off + 12 > buf.size()) return bad(b.name, "INF1 truncated");
    const u32 cnt = io::be16(buf, inf->off + 8);
    const u32 esz = io::be16(buf, inf->off + 10);
    if (esz != 20 || 0x10 + static_cast<std::size_t>(cnt) * esz > inf->size)
        return bad(b.name, "INF1 entry size " + std::to_string(esz));
    const std::size_t dat0 = dat->off + 8, dend = dat->off + dat->size;
    if (dend > buf.size() || dat0 > dend) return bad(b.name, "DAT1 truncated");
    b.entries.reserve(cnt);
    for (u32 i = 0; i < cnt; ++i) {
        const std::size_t eo = inf->off + 0x10 + static_cast<std::size_t>(i) * esz;
        Entry e;
        e.index = i;
        std::memcpy(e.entry.data(), buf.data() + eo, 20);
        e.id = io::be16(buf, eo + 4);
        const std::size_t p = dat0 + io::be32(buf, eo);
        std::size_t q = p;
        while (q < dend && buf[q] != 0) {
            if (buf[q] == 0x1A) {
                if (q + 1 >= buf.size()) return bad(b.name, "text runs past the end");
                q += buf[q + 1] >= 2 ? buf[q + 1] : 1;
            } else {
                q += 1;
            }
        }
        // buf[p:min(q, dend - 1) + 1] with Python's slice clamping
        const std::size_t end = std::min(std::min(q, dend - 1) + 1, buf.size());
        if (p < end) e.text.assign(buf.begin() + static_cast<std::ptrdiff_t>(p), buf.begin() + static_cast<std::ptrdiff_t>(end));
        b.entries.push_back(std::move(e));
    }
    for (const Entry& e : b.entries) b.by_id.emplace(e.id, e.index);  // setdefault
    if (const Sec* flw = find("FLW1")) {
        const std::size_t fo = flw->off;
        if (fo + 12 > buf.size()) return bad(b.name, "FLW1 truncated");
        const u32 nn = io::be16(buf, fo + 8), ne = io::be16(buf, fo + 10);
        const std::size_t eb = fo + 0x10 + static_cast<std::size_t>(nn) * 8;
        if (eb + static_cast<std::size_t>(ne) * 2 > buf.size()) return bad(b.name, "FLW1 truncated");
        b.nodes.resize(nn);
        for (u32 i = 0; i < nn; ++i) std::memcpy(b.nodes[i].data(), buf.data() + fo + 0x10 + i * 8u, 8);
        b.edges.resize(ne);
        for (u32 i = 0; i < ne; ++i) b.edges[i] = io::be16(buf, eb + 2u * i);
    }
    if (const Sec* fli = find("FLI1")) {
        const std::size_t lo = fli->off;
        if (lo + 10 > buf.size()) return bad(b.name, "FLI1 truncated");
        const u32 n = io::be16(buf, lo + 8);
        if (n > 0 && lo + 0x10 + (static_cast<std::size_t>(n) - 1) * 8 + 6 > buf.size()) return bad(b.name, "FLI1 truncated");
        for (u32 i = 0; i < n; ++i) {
            const u32 v = io::be32(buf, lo + 0x10 + i * 8u);
            const u16 idx = io::be16(buf, lo + 0x14 + i * 8u);
            if (b.flow_index.emplace(v >> 16, idx).second) b.flows.emplace_back(v >> 16, idx);  // setdefault
        }
    }
    return b;
}

// hdtext.load_bmg (the archive's bytes instead of its path)
Result<Bmg> load_bmg(View arc, std::string_view member, std::string name) {
    auto a = io::rarc_read(arc);
    if (!a) return std::unexpected(a.error());
    const std::string key = std::string(member) + ".bmg";
    const io::RarcMember* m = a->get(key);
    if (!m) return bad(name, "no " + key);
    auto d = a->data(*m);
    if (!d) return std::unexpected(d.error());
    return parse_bmg(*d, std::move(name));
}

// hdtext.tokens
Result<Toks> tokens(View text) {
    Toks out;
    std::size_t q = 0;
    while (q < text.size() && text[q] != 0) {
        const u8 c = text[q];
        if (c == 0x1A) {
            const std::size_t ln = q + 1 < text.size() ? text[q + 1] : 0;
            if (ln < 5 || q + ln > text.size()) return fail("fmt.text", "truncated control tag at " + std::to_string(q));
            out.push_back(Tok::t(text[q + 2], io::be16(text, q + 3),
                                 Bytes(text.begin() + static_cast<std::ptrdiff_t>(q + 5),
                                       text.begin() + static_cast<std::ptrdiff_t>(q + ln))));
            q += ln;
        } else {
            out.push_back(Tok::ch(c));
            q += 1;
        }
    }
    return out;
}

// hdtext.encode
Bytes encode(const Toks& toks) {
    Bytes b;
    for (const Tok& t : toks) {
        if (!t.tag) {
            b.push_back(t.c);
        } else {
            b.push_back(0x1A);
            b.push_back(static_cast<u8>(5 + t.args.size()));
            b.push_back(t.c);
            b.push_back(static_cast<u8>(t.id >> 8));
            b.push_back(static_cast<u8>(t.id));
            b.insert(b.end(), t.args.begin(), t.args.end());
        }
    }
    b.push_back(0);
    return b;
}

// hdtext.show
std::string show(View text) {
    auto toks = tokens(text);
    if (!toks) return "<" + toks.error().detail + ">";
    std::string out;
    char buf[32];
    for (const Tok& t : *toks) {
        if (!t.tag) {
            if (t.c == 10) {
                out += '|';
            } else if (t.c >= 32 && t.c < 127) {
                out += static_cast<char>(t.c);
            } else {
                std::snprintf(buf, sizeof buf, "<%02x>", t.c);
                out += buf;
            }
        } else {
            std::snprintf(buf, sizeof buf, "{%d,%d", t.c, t.id);
            out += buf;
            if (!t.args.empty()) {
                out += ':';
                for (u8 x : t.args) {
                    std::snprintf(buf, sizeof buf, "%02x", x);
                    out += buf;
                }
            }
            out += '}';
        }
    }
    return out;
}

// hdtext.gc_tag_known: control tags v2.0.3's d_msg_class.cpp processors handle (include/d/d_msg_class.h).
bool gc_tag_known(u8 g, u16 i) {
    if (g == 1 || g == 2) return true;
    if (g == 0) return i <= 63;
    if (g == 3) return i <= 20;
    if (g == 4) return i <= 13;
    if (g == 5) return i == 0 || i == 3 || (i >= 7 && i <= 14);
    if (g == 6) return i <= 11;
    if (g == 255) return i <= 2;
    return false;
}

// hdtext.valid_encoded_text (svc/flow.cpp valid_encoded_text)
bool valid_encoded_text(View t) {
    if (t.empty()) return false;
    std::size_t o = 0;
    while (o < t.size()) {
        if (t[o] == 0) return o + 1 == t.size();
        if (t[o] == 0x1A) {
            if (o + 2 > t.size() || t[o + 1] < 5 || t[o + 1] > t.size() - o) return false;
            const std::size_t n = t[o + 1] - 5u;
            if (t[o + 2] == 255 && io::be16(t, o + 3) == 0 && n != 1 && n != 4 && n != 8) return false;
            o += t[o + 1];
        } else {
            o += 1;
        }
    }
    return false;
}

}  // namespace hdf::text::detail
