// Module U: SCRNblo2 readers.
//   stampbook.Blo (_resrefs, _tag8, _hexc, Blo.__init__, Blo._mat1, Blo._pane, _place) + cosconfirm.Blo._pane's
//   animation index: only the TEX1 references reach an output (cosconfirm's member list).
//   hdtext.parse_blo (_res_ref, _name_tab, _mat1): the dicts of cave/amiibo/layout.json.
// Every read is bounds-checked: where Python raises (IndexError, struct.error, ValueError) these return
// Error{"fmt.ui"}; where Python slices (which clip at the end) these clip too.
#include "ui/ui_internal.hpp"

#include "hdf/source.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>

namespace hdf::ui::detail {

namespace {

using io::Json;
using io::JsonArray;
using io::JsonObject;

// Bounds-checked big-endian reads; a read past the end sets ok = false and returns 0 (the caller then fails).
struct Rd {
    View b;
    bool ok = true;
    bool has(std::size_t o, std::size_t n) {
        if (o > b.size() || b.size() - o < n) ok = false;
        return ok;
    }
    u8 u8_(std::size_t o) { return has(o, 1) ? b[o] : u8{0}; }
    u16 u16_(std::size_t o) { return has(o, 2) ? io::be16(b, o) : u16{0}; }
    i16 i16_(std::size_t o) { return static_cast<i16>(u16_(o)); }
    u32 u32_(std::size_t o) { return has(o, 4) ? io::be32(b, o) : u32{0}; }
    double f32_(std::size_t o) { return static_cast<double>(std::bit_cast<float>(u32_(o))); }
    // b[o:o + n] (clips at the end; empty when o is past it)
    View slice(std::size_t o, std::size_t n) const {
        if (o >= b.size()) return {};
        return b.subspan(o, std::min(n, b.size() - o));
    }
};

std::string hex_of(View v) { return to_hex(v); }

// '#' + b[o:o + 4].hex()
std::string color_hex(const Rd& r, std::size_t o) { return "#" + hex_of(r.slice(o, 4)); }

// stampbook._hexc(v)
std::array<u8, 4> hexc(u32 v) {
    return {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
}

// stampbook._tag8(b): b.strip(b'\0').decode('latin1')
std::string tag8(View v) {
    std::size_t a = 0, e = v.size();
    while (a < e && v[a] == 0) ++a;
    while (e > a && v[e - 1] == 0) --e;
    return latin1_to_utf8(v.subspan(a, e - a));
}

std::string tag4(View d, std::size_t o) { return latin1_to_utf8(d.subspan(o, 4)); }

}  // namespace

// ---------------------------------------------------------------------------------------------- stampbook.Blo
// _resrefs(d, o)
Result<std::vector<ResRef>> blo_resrefs(View d, std::size_t o) {
    Rd r{d};
    const std::size_t lo = o + r.u32_(o + 12);
    const u16 n = r.u16_(lo);
    if (!r.ok) return bad("BLO resource list cut");
    std::vector<ResRef> out;
    for (u32 i = 0; i < n; ++i) {
        const std::size_t eo = lo + r.u16_(lo + 2 + 2 * i);
        const u8 kind = r.u8_(eo);
        const u8 len = r.u8_(eo + 1);
        if (!r.ok) return bad("BLO resource reference cut");
        const View name = r.slice(eo + 2, len);
        out.push_back(ResRef{kind, latin1_to_utf8(name), std::string(name.begin(), name.end())});
    }
    return out;
}

namespace {

// Blo._mat1(o)
Result<void> blo_mat1(Blo& blo, View d, std::size_t o) {
    Rd r{d};
    const u16 n = r.u16_(o + 8);
    u32 offs[23];
    for (int k = 0; k < 23; ++k) offs[k] = r.u32_(o + 0x0C + 4 * k);  // struct.unpack_from('>23I', ...)
    if (!r.ok) return bad("BLO MAT1 header cut");
    const u32 o_init = offs[0], o_id = offs[1], o_matcol = offs[5], o_texno = offs[11], o_tsn = offs[16],
              o_tevcol = offs[14];
    for (u32 i = 0; i < n; ++i) {
        const std::size_t ib = o + o_init + std::size_t{r.u16_(o + o_id + 2 * i)} * 0xE8;
        auto h = [&](std::size_t k) { return r.u16_(ib + k); };
        BloMaterial m;
        for (int k = 0; k < 8; ++k) {
            const u16 t = h(0x38 + 2 * k);
            if (!r.ok) return bad("BLO material cut");
            if (t == 0xFFFF) continue;
            const u16 ti = r.u16_(o + o_texno + 2 * std::size_t{t});
            if (!r.ok) return bad("BLO material texture cut");
            if (ti >= blo.tex.size()) return bad("BLO material texture index");  // self.tex[...] IndexError
            if (!blo.tex[ti].raw.empty()) m.textures.push_back(blo.tex[ti].raw);  // [t for t in texs if t]
        }
        for (int k = 0; k < 4; ++k) {
            const u16 x = h(0x92 + 2 * k);
            if (!r.ok) return bad("BLO material cut");
            if (x == 0xFFFF) continue;
            std::array<i16, 4> c{};
            for (int j = 0; j < 4; ++j) c[j] = r.i16_(o + o_tevcol + 8 * std::size_t{x} + 2 * j);
            m.tevcolor[k] = c;
        }
        const u16 mc = h(8);
        const u8 ts = r.u8_(ib + 4);
        if (!r.ok) return bad("BLO material cut");
        m.tevstages = ts != 0xFF ? r.u8_(o + o_tsn + ts) : 0;
        if (mc != 0xFFFF) m.matcolor = hexc(r.u32_(o + o_matcol + 4 * std::size_t{mc}));
        if (!r.ok) return bad("BLO material cut");
        blo.mats.push_back(std::move(m));
    }
    return {};
}

// Blo._pane(o, t) + cosconfirm.Blo._pane (aid)
Result<std::unique_ptr<BloPane>> blo_pane(View d, std::size_t o, const std::string& t) {
    Rd r{d};
    const std::size_t q = t == "PAN2" ? o : o + 8;
    const u32 size = r.u32_(q + 4);
    const u8 vis = r.u8_(q + 0xC), bp = r.u8_(q + 0xD);
    double f[9];
    for (int k = 0; k < 9; ++k) f[k] = r.f32_(q + 0x20 + 4 * k);  // w, h, sx, sy, rx, ry, rz, tx, ty
    if (!r.ok) return bad("BLO pane cut");
    auto p = std::make_unique<BloPane>();
    p->type = t;
    p->name = tag8(r.slice(q + 0x10, 8));
    p->user = tag8(r.slice(q + 0x18, 8));
    p->visible = vis != 0;
    p->w = f[0];
    p->h = f[1];
    p->sx = f[2];
    p->sy = f[3];
    p->rz = f[6];
    p->x = f[7];
    p->y = f[8];
    if (bp < 9) {
        const double ax[3] = {0.0, p->w / 2, p->w}, ay[3] = {0.0, p->h / 2, p->h};
        p->anchor = {ax[bp % 3], ay[bp / 3]};
    }
    p->aid = r.u16_(q + 0xA);
    const std::size_t e = q + size;
    if (t == "PIC2") {
        // material u16 at +4, uv '>hh' x 4 at +0x10, corners '>I' x 4 at +0x20 (preview fields, not kept)
        p->material = r.u16_(e + 4);
        r.has(e + 0x10, 0x20);
    } else if (t == "TBX2") {
        // '>HHHhhHH' at e, colours at +0x10/+0x14, slen at +0x1E (text: a clipped slice)
        p->material = r.u16_(e + 4);
        r.has(e, 0x20);
    } else if (t == "WIN2") {
        p->material = r.u16_(e + 0x24);
    }
    if (!r.ok) return bad("BLO pane cut");
    return p;
}

}  // namespace

// _place(p, parent): J2DPane::makeMatrix, parent * T(x, y) * R(-rz) * S(sx, sy)
void blo_place(BloPane& p, const std::array<std::array<double, 3>, 2>& parent) {
    if (p.type == "SCREEN") {
        p.m = parent;
    } else {
        const double th = (-p.rz) * (kPi / 180.0);  // math.radians: x * (pi / 180)
        const double c = std::cos(th), s = std::sin(th);
        const double loc[2][3] = {{c * p.sx, -s * p.sy, p.x}, {s * p.sx, c * p.sy, p.y}};
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 3; ++j)
                p.m[i][j] = parent[i][0] * loc[0][j] + parent[i][1] * loc[1][j] + (j == 2 ? parent[i][2] : 0.0);
    }
    for (auto& ch : p.children) blo_place(*ch, p.m);
}

Result<Blo> blo_read(View d) {
    Blo blo;
    blo.magic = latin1_to_utf8(d.subspan(0, std::min<std::size_t>(8, d.size())));
    blo.root = std::make_unique<BloPane>();
    blo.root->type = "SCREEN";
    blo.root->name = "<screen>";
    std::vector<BloPane*> stack{blo.root.get()};
    BloPane* last = blo.root.get();
    std::size_t o = 0x20;
    Rd r{d};
    while (o + 8 <= d.size()) {
        const std::string t = tag4(d, o);
        const u32 sz = r.u32_(o + 4);
        blo.blocks.push_back(t);
        if (t == "INF1") {
            blo.root->w = r.u16_(o + 8);
            blo.root->h = r.u16_(o + 10);
            if (!r.ok) return bad("BLO INF1 cut");
        } else if (t == "TEX1") {
            auto rr = blo_resrefs(d, o);
            if (!rr) return std::unexpected(rr.error());
            blo.tex = std::move(*rr);
        } else if (t == "FNT1") {
            auto rr = blo_resrefs(d, o);
            if (!rr) return std::unexpected(rr.error());
            blo.fnt = std::move(*rr);
        } else if (t == "MAT1") {
            if (auto m = blo_mat1(blo, d, o); !m) return std::unexpected(m.error());
        } else if (t == "BGN1") {
            stack.push_back(last);
        } else if (t == "END1") {
            if (stack.empty()) return bad("BLO END1 without BGN1");  // pop from empty list
            stack.pop_back();
        } else if (t == "PAN2" || t == "PIC2" || t == "TBX2" || t == "WIN2") {
            auto p = blo_pane(d, o, t);
            if (!p) return std::unexpected(p.error());
            if (stack.empty()) return bad("BLO pane outside the tree");  // stack[-1] IndexError
            last = p->get();
            stack.back()->children.push_back(std::move(*p));
            auto it = std::find_if(blo.counts.begin(), blo.counts.end(), [&](const auto& c) { return c.first == t; });
            if (it == blo.counts.end())
                blo.counts.emplace_back(t, 1);
            else
                ++it->second;
        } else if (t == "EXT1" || sz == 0) {
            break;
        }
        // Python loops forever on another block with size 0 (o += 0); refuse it instead.
        if (sz == 0) return bad("BLO block of size 0");
        o += sz;
    }
    blo_place(*blo.root, {{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}});
    return blo;
}

// ---------------------------------------------------------------------------------------------- hdtext.parse_blo
// _res_ref(b, blk)
Result<std::vector<std::string>> res_ref(View b, std::size_t blk) {
    Rd r{b};
    const u32 size = r.u32_(blk + 4), off = r.u32_(blk + 12);
    if (!r.ok) return bad("BLO TEX1/FNT1 cut");
    const std::size_t base = blk + off;
    std::vector<std::string> out;
    if (off >= size) return out;
    const u16 n = r.u16_(base);
    if (!r.ok) return bad("BLO TEX1/FNT1 cut");
    for (u32 i = 0; i < n; ++i) {
        const std::size_t rp = base + r.u16_(base + 2 + 2 * i);
        const u8 typ = r.u8_(rp), ln = r.u8_(rp + 1);
        if (!r.ok) return bad("BLO resource reference cut");
        std::string name = latin1_to_utf8(r.slice(rp + 2, ln));
        std::replace(name.begin(), name.end(), '\\', '/');
        if (const auto s = name.rfind('/'); s != std::string::npos) name = name.substr(s + 1);
        if (typ == 2 || typ == 3 || typ == 4) {
            // re.sub(r'\.(bti|bfn)$', '', name, flags=re.I): '$' also matches before a final '\n'
            auto strip_at = [&](std::size_t end) {
                if (end < 4) return false;
                const std::string ext = ascii_lower(name.substr(end - 4, 4));
                if (ext != ".bti" && ext != ".bfn") return false;
                name.erase(end - 4, 4);
                return true;
            };
            if (!strip_at(name.size()) && !name.empty() && name.back() == '\n') strip_at(name.size() - 1);
        }
        out.push_back(std::move(name));
    }
    return out;
}

// _name_tab(b, o)
Result<std::vector<std::string>> name_tab(View b, std::size_t o) {
    Rd r{b};
    const u16 n = r.u16_(o);
    if (!r.ok) return bad("BLO name table cut");
    std::vector<std::string> out;
    for (u32 i = 0; i < n; ++i) {
        const std::size_t s = o + r.u16_(o + 4 + i * 4 + 2);
        if (!r.ok) return bad("BLO name table cut");
        std::size_t e = s;
        while (e < b.size() && b[e] != 0) ++e;
        if (e >= b.size()) return bad("BLO name without NUL");  // b.index(b'\0', ...) ValueError
        out.push_back(latin1_to_utf8(b.subspan(s, e - s)));
    }
    return out;
}

// _mat1(b, blk, tex_names)
Result<std::vector<Mat1Entry>> mat1(View b, std::size_t blk, const std::vector<std::string>& tex_names) {
    Rd r{b};
    const u16 cnt = r.u16_(blk + 8);
    u32 hdr[24];
    for (int k = 0; k < 24; ++k) hdr[k] = r.u32_(blk + 0xC + 4 * k);  // struct.unpack_from('>24I', ...)
    if (!r.ok) return bad("BLO MAT1 header cut");
    const std::size_t init = blk + hdr[0], ids = blk + hdr[1];
    const u32 names_off = hdr[2];
    const std::size_t matcolor = blk + hdr[5], texno = blk + hdr[11];
    std::vector<std::string> names;
    if (names_off) {
        auto nt = name_tab(b, blk + names_off);
        if (!nt) return std::unexpected(nt.error());
        names = std::move(*nt);
    } else {
        names.assign(cnt, std::string());
    }
    std::vector<Mat1Entry> out;
    for (u32 i = 0; i < cnt; ++i) {
        const u16 mi = r.u16_(ids + 2 * i);
        const std::size_t d = init + std::size_t{mi} * 0xE8;
        Mat1Entry m;
        for (int k = 0; k < 8; ++k) {
            const u16 ti = r.u16_(d + 0x38 + 2 * k);
            if (!r.ok) return bad("BLO material cut");
            if (ti == 0xFFFF) continue;
            const u16 tn = r.u16_(texno + 2 * std::size_t{ti});
            if (!r.ok) return bad("BLO material texture cut");
            m.textures.push_back(tn < tex_names.size() ? tex_names[tn] : "#" + std::to_string(tn));
        }
        const u16 mc = r.u16_(d + 0x8);
        if (!r.ok) return bad("BLO material cut");
        if (mc != 0xFFFF) m.mat_color = color_hex(r, matcolor + 4 * std::size_t{mc});
        const u8 b3 = r.u8_(d + 3), b4 = r.u8_(d + 4);
        if (!r.ok) return bad("BLO material cut");
        const u32 tg = b3 != 0xFF ? r.u8_(blk + hdr[8] + b3) : 0u;     // newTexGenNum
        const u32 st = b4 != 0xFF ? r.u8_(blk + hdr[16] + b4) : 0xFFu;  // newTevStageNum
        if (!r.ok) return bad("BLO material cut");
        std::string tev[2];
        for (int k = 0; k < 2; ++k) {                                   // newTevColor (GXColorS10)
            const u16 ci = r.u16_(d + 0x92 + 2 * k);
            if (!r.ok) return bad("BLO material cut");
            u8 lo[4] = {0, 0, 0, 0};
            if (ci != 0xFFFF) {
                const std::size_t c0 = blk + hdr[14] + 8 * std::size_t{ci};
                for (int j = 0; j < 4; ++j) lo[j] = static_cast<u8>(r.u16_(c0 + 2 * j) & 0xFF);  // '>4h', v & 0xFF
                if (!r.ok) return bad("BLO material colour cut");
            }
            tev[k] = "#" + hex_of(View(lo, 4));
        }
        // J2DPicture(stream, materials): black/white interpolation colours
        if ((tg == 1 && st != 1) || (tg != 1 && st != tg + 1)) {
            m.black = tev[0];
            m.white = tev[1];
        } else {
            m.black = "#00000000";
            m.white = "#ffffffff";
        }
        m.name = i < names.size() ? names[i] : std::string();
        out.push_back(std::move(m));
    }
    return out;
}

namespace {

Json str_list(const std::vector<std::string>& v) {
    JsonArray a;
    for (const auto& s : v) a.emplace_back(s);
    return a;
}

Json f2(double a, double b) { return JsonArray{Json(a), Json(b)}; }

// int(s[-2:], 16) for the '#rrggbbaa' strings _mat1 makes (a clipped slice can make it shorter)
std::optional<i64> last2_hex(const std::string& s) {
    const std::string t = s.size() >= 2 ? s.substr(s.size() - 2) : s;
    if (t.empty()) return std::nullopt;
    i64 v = 0;
    for (char c : t) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        v = v * 16 + d;
    }
    return v;
}

struct PaneRec {
    std::string name;
    double anc[2] = {0, 0};  // _anchor_abs
};

}  // namespace

// parse_blo(b): J2DScreen::makeHierarchyPanes / J2DPane::makePaneExStream
Result<ParsedBlo> parse_blo(View b) {
    if (b.size() < 8 || (std::memcmp(b.data(), "SCRNblo2", 8) != 0 && std::memcmp(b.data(), "SCRNblo1", 8) != 0))
        return bad("not a SCRN blo");
    Rd r{b};
    std::size_t o = 0x20;
    std::optional<u16> inf_w, inf_h;
    std::vector<std::string> tex;
    std::vector<Mat1Entry> mats;
    std::vector<JsonObject> panes;
    std::vector<PaneRec> recs;
    std::vector<std::optional<std::size_t>> stack{std::nullopt};
    std::optional<std::size_t> last;
    std::set<std::string> used;
    while (o + 8 <= b.size()) {
        const std::string tag = tag4(b, o);
        const u32 sz = r.u32_(o + 4);
        if (sz < 8) return bad("bad block size");
        if (tag == "INF1") {
            inf_w = r.u16_(o + 8);
            inf_h = r.u16_(o + 10);
            if (!r.ok) return bad("BLO INF1 cut");
        } else if (tag == "TEX1") {
            auto t = res_ref(b, o);
            if (!t) return std::unexpected(t.error());
            tex = std::move(*t);
        } else if (tag == "MAT1") {
            auto m = mat1(b, o, tex);
            if (!m) return std::unexpected(m.error());
            mats = std::move(*m);
        } else if (tag == "BGN1") {
            stack.push_back(last);
        } else if (tag == "END1") {
            if (stack.empty()) return bad("BLO END1 without BGN1");
            stack.pop_back();
        } else if (tag == "EXT1") {
            break;
        } else if (tag == "PAN2" || tag == "PIC2" || tag == "TBX2" || tag == "WIN2") {
            if (stack.empty()) return bad("BLO pane outside the tree");
            const std::size_t p = tag == "PAN2" ? o : o + 8;
            const u32 psz = r.u32_(p + 4);
            const u8 vis = r.u8_(p + 0xC), basep = r.u8_(p + 0xD);
            double f[9];
            for (int k = 0; k < 9; ++k) f[k] = r.f32_(p + 0x20 + 4 * k);
            if (!r.ok) return bad("BLO pane cut");
            const double w = f[0], h = f[1], sx = f[2], sy = f[3], rx = f[4], ry = f[5], rz = f[6], tx = f[7], ty = f[8];
            PaneRec rec;
            rec.name = tag8(r.slice(p + 0x10, 8));
            const std::optional<std::size_t> parent = stack.back();
            const double ax = (basep % 3) / 2.0, ay = (basep / 3) / 2.0;
            if (!parent) {
                rec.anc[0] = tx;
                rec.anc[1] = ty;
            } else {
                const PaneRec& pa = recs[*parent];
                rec.anc[0] = pa.anc[0] + tx;
                rec.anc[1] = pa.anc[1] + ty;
            }
            JsonObject pane;
            pane.emplace_back("name", rec.name);
            pane.emplace_back("kind", tag);
            pane.emplace_back("parent", parent ? Json(recs[*parent].name) : Json(nullptr));
            pane.emplace_back("depth", static_cast<i64>(stack.size()) - 1);
            pane.emplace_back("visible", vis != 0);
            pane.emplace_back("anchor", f2(ax, ay));
            pane.emplace_back("pos", f2(tx, ty));
            pane.emplace_back("size", f2(w, h));
            pane.emplace_back("scale", f2(sx, sy));
            pane.emplace_back("rotate", JsonArray{Json(rx), Json(ry), Json(rz)});
            const double x0 = rec.anc[0] - ax * w, y0 = rec.anc[1] - ay * h;
            pane.emplace_back("rect", JsonArray{Json(io::py_round(x0, 2)), Json(io::py_round(y0, 2)),
                                                Json(io::py_round(x0 + w, 2)), Json(io::py_round(y0 + h, 2))});
            const std::size_t q = p + psz;
            u16 matno = 0xFFFF;
            if (tag == "PIC2") {
                matno = r.u16_(q + 4);
                JsonArray uv;
                for (int k = 0; k < 4; ++k)
                    uv.emplace_back(f2(r.u16_(q + 0x10 + 4 * k) / 256.0, r.u16_(q + 0x12 + 4 * k) / 256.0));
                if (!r.ok) return bad("BLO picture cut");
                pane.emplace_back("uv", std::move(uv));
                JsonArray cc;
                for (int k = 0; k < 4; ++k) cc.emplace_back(color_hex(r, q + 0x20 + 4 * k));
                pane.emplace_back("corner_colors", std::move(cc));
            } else if (tag == "TBX2") {
                // struct.unpack_from('>HhhHHBB', b, q + 4)
                matno = r.u16_(q + 4);
                const i16 cs = r.i16_(q + 6), ls = r.i16_(q + 8);
                const u16 fx = r.u16_(q + 0xA), fy = r.u16_(q + 0xC);
                const u8 hb = r.u8_(q + 0xE), vb = r.u8_(q + 0xF);
                const u16 slen = r.u16_(q + 0x1E);
                if (!r.ok) return bad("BLO text box cut");
                pane.emplace_back("font_size", JsonArray{Json(static_cast<i64>(fx)), Json(static_cast<i64>(fy))});
                pane.emplace_back("char_space", static_cast<i64>(cs));
                pane.emplace_back("line_space", static_cast<i64>(ls));
                pane.emplace_back("hbind", static_cast<i64>(hb));
                pane.emplace_back("vbind", static_cast<i64>(vb));
                pane.emplace_back("char_color", color_hex(r, q + 0x10));
                pane.emplace_back("grad_color", color_hex(r, q + 0x14));
                View raw = r.slice(q + 0x20, slen);
                for (std::size_t k = 0; k < raw.size(); ++k)  // .split(b'\0')[0]
                    if (raw[k] == 0) {
                        raw = raw.subspan(0, k);
                        break;
                    }
                // designer placeholder text (EN or Shift-JIS)
                auto sj = shift_jis_to_utf8(raw);
                pane.emplace_back("text", sj ? std::move(*sj) : cp1252_to_utf8(raw));
            } else if (tag == "WIN2") {
                matno = 0xFFFF;
            }
            if (matno != 0xFFFF && matno < mats.size()) {
                const Mat1Entry& m = mats[matno];
                pane.emplace_back("material", m.name);
                pane.emplace_back("textures", str_list(m.textures));
                pane.emplace_back("mat_color", m.mat_color ? Json(*m.mat_color) : Json(nullptr));
                if (tag == "PIC2") {
                    pane.emplace_back("black", m.black);
                    pane.emplace_back("white", m.white);
                }
                if (m.mat_color) {
                    auto a = last2_hex(*m.mat_color);
                    if (!a) return bad("BLO material colour");  // int(..., 16) ValueError
                    pane.emplace_back("alpha", *a);
                }
                for (const auto& t : m.textures) used.insert(t);
            }
            last = recs.size();
            recs.push_back(std::move(rec));
            panes.push_back(std::move(pane));
        }
        o += sz;
    }
    ParsedBlo out;
    out.textures_used.assign(used.begin(), used.end());  // sorted(set): UTF-8 byte order = code point order
    out.panes = static_cast<u32>(panes.size());
    JsonArray pj;
    for (auto& p : panes) pj.emplace_back(std::move(p));
    JsonObject top;
    top.emplace_back("width", inf_w ? Json(static_cast<i64>(*inf_w)) : Json(nullptr));
    top.emplace_back("height", inf_h ? Json(static_cast<i64>(*inf_h)) : Json(nullptr));
    top.emplace_back("textures_used", str_list(out.textures_used));
    top.emplace_back("panes", std::move(pj));
    out.json = Json(std::move(top));
    return out;
}

}  // namespace hdf::ui::detail
