// Module TX: the build (hdtext.build text part, _cave_spec, _stamps_spec, stage_msg_group, _write_bin, the
// flows.json writer). Outputs: res/cave/hdtext/{messages.bin,flows.json}, res/stamps/hdtext/messages.bin.
// messages.json, README.md and stamps/hdtext/flows.json are on the drop list (plan 4); stamps flows.json is
// still made for TextDiag, as an extra check.
#include "hdf/text.hpp"

#include "hdf/facts.hpp"
#include "hdf/inputs.hpp"
#include "hdf/io.hpp"
#include "text_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace hdf::text {

namespace detail {

// hdtext.stage_msg_group: STAG mMsgGroup (d_stage.h stage_stag_info_class +0x28) of a STG_00.arc.
Result<std::optional<u8>> stage_msg_group(View stg_00_arc) {
    auto a = io::rarc_read(stg_00_arc);
    if (!a) return std::unexpected(a.error());
    const io::RarcMember* m = a->get("stage.dzs");
    if (!m) return std::optional<u8>{};
    auto d = a->data(*m);
    if (!d) return std::unexpected(d.error());
    const View b = *d;
    if (b.size() < 4) return fail("fmt.text", "stage.dzs truncated");
    // stagedata.chunk_map(d) = {tag: [tag, count, offset, header_offset]} (cave_patch.chunks; a later tag wins)
    const i32 n = static_cast<i32>(io::be32(b, 0));
    std::optional<u32> stag;
    for (i32 i = 0; i < n; ++i) {
        const std::size_t h = 4 + static_cast<std::size_t>(i) * 12;
        if (h + 12 > b.size()) return fail("fmt.text", "stage.dzs chunk table truncated");
        if (std::memcmp(b.data() + h, "STAG", 4) == 0) stag = io::be32(b, h + 8);
    }
    if (!stag) return std::optional<u8>{};
    if (static_cast<u64>(*stag) + 0x28 >= b.size()) return fail("fmt.text", "STAG out of range");
    return std::optional<u8>(b[*stag + 0x28]);
}

// hdtext._write_bin into memory: 'HDTX' u32 version=1 u32 count, then per record (4-byte aligned)
// u8 file u8 group u16 id u8 action u8 language(0) u16 text_size u8 entry[20] u8 text[text_size], little-endian.
Bytes write_bin(const std::vector<Message>& msgs) {
    Bytes b = {'H', 'D', 'T', 'X'};
    auto le16 = [&](u32 v) {
        b.push_back(static_cast<u8>(v));
        b.push_back(static_cast<u8>(v >> 8));
    };
    auto le32 = [&](u32 v) {
        le16(v & 0xFFFF);
        le16(v >> 16);
    };
    le32(1);  // SCHEMA_VERSION
    le32(static_cast<u32>(msgs.size()));
    for (const Message& m : msgs) {
        b.push_back(m.file_no);
        b.push_back(m.group);
        le16(m.id);
        b.push_back(static_cast<u8>(m.action));
        b.push_back(0);
        le16(static_cast<u32>(m.text.size()));
        b.insert(b.end(), m.entry.begin(), m.entry.end());
        b.insert(b.end(), m.text.begin(), m.text.end());
        while (b.size() % 4) b.push_back(0);
    }
    return b;
}

}  // namespace detail

namespace {

using namespace detail;
using io::Json;
using io::JsonArray;
using io::JsonObject;

// hdtext.FILES (dict order)
struct FileSpec {
    const char* file;
    const char* arc;
};
constexpr std::array<FileSpec, 3> kFiles = {{{"zel_00", "bmgres.arc"}, {"zel_04", "bmgres4.arc"}, {"zel_05", "bmgres5.arc"}}};

struct SpecItem {
    std::string file;
    u16 id;
    std::string category, use;
    bool required;
};

// hdtext._cave_spec: (file, id, category, use, required)
std::vector<SpecItem> cave_spec() {
    std::vector<SpecItem> out = {{"zel_00", 177, "wallet", "item-get", true},
                                 {"zel_00", 412, "wallet", "item-text", true},
                                 {"zel_00", 413, "cave", "ui", true},
                                 {"zel_00", 668, "wallet", "item-text", true}};
    for (int i : {414, 415, 855}) out.push_back({"zel_00", static_cast<u16>(i), "cave", "ui", true});
    for (u16 i = 1254; i < 1264; ++i) out.push_back({"zel_00", i, "record", "ui", false});
    out.push_back({"zel_00", 854, "record", "ui", true});
    for (u16 i = 1524; i < 1574; ++i) out.push_back({"zel_00", i, "amiibo", "ui", i == 1528 || i == 1530 || i == 1568});
    for (u16 i = 5048; i < 5069; ++i) out.push_back({"zel_05", i, i >= 5056 ? "cave" : "other", "flow", false});
    return out;
}

}  // namespace

namespace detail {

// hdtext.STAMP_ITEMS (stamp index 0-49 order; = facts.STAMP_ITEM_IDS, checked in the unit test)
std::vector<u16> stamp_items() {
    std::vector<u16> v = {0x13, 0x14, 0x15, 0x3B, 0x3C, 0x4D, 0x4E, 0x52, 0x57};
    for (u16 i = 0x85; i < 0x90; ++i) v.push_back(i);
    for (u16 i = 0x92; i < 0x9C; ++i) v.push_back(i);
    for (u16 i = 0xA8; i < 0xB0; ++i) v.push_back(i);
    for (u16 i = 0xB6; i < 0xC0; ++i) v.push_back(i);
    v.push_back(0xD8);
    v.push_back(0xD9);
    return v;
}

}  // namespace detail

namespace {

// hdtext._stamps_spec
std::vector<SpecItem> stamps_spec() {
    std::vector<SpecItem> out;
    for (u16 it : stamp_items()) out.push_back({"zel_00", static_cast<u16>(101 + it), "stamp-get", "item-get", true});
    for (u16 i = 2800; i < 2807; ++i) out.push_back({"zel_00", i, "stamp-ui", "ui", i == 2800 || i == 2801 || i == 2806});
    for (u16 i = 2890; i < 2940; ++i) out.push_back({"zel_00", i, "stamp-name", "ui", true});
    out.push_back({"zel_04", 5464, "stamp-npc", "shop", true});
    out.push_back({"zel_04", 5465, "stamp-npc", "shop", true});
    return out;
}

// sorted(STAMP_NPC_NODES.items() + STAMP_NPC_SETTERS.items())
struct NpcNode {
    u32 node;
    const char* why;
};
constexpr std::array<NpcNode, 5> kStampNpc = {{
    {1711, "sets temp flag 18 (Agitha stamp pending) in flow 1813"},
    {1728, "sets temp flag 18 (Agitha stamp pending) in flow 1814"},
    {1755, "Agitha gives the Agitha stamp (event008 1, 0xBD)"},
    {2686, "Malo Mart sells the Malo Mart stamp (event008 1, 0xBE)"},
    {2690, "Gengle gives the Rupee stamp (event008 1, 0x98)"},
}};

constexpr std::array<u32, 4> kCaveFlows = {1111, 1120, 1121, 1122};  // CAVE_FLOWS

std::string ascii_lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

const Json* get(const JsonObject& o, std::string_view key) {
    for (const auto& [k, x] : o)
        if (k == key) return &x;
    return nullptr;
}

// One message list with its key index (Python: msgs + bykey/seen, which always hold the same keys).
struct MsgList {
    std::vector<Message> msgs;
    std::map<std::string, std::size_t> by_key;
    void add(Message m) {
        const std::string k = m.key();
        if (by_key.contains(k)) return;
        by_key.emplace(k, msgs.size());
        msgs.push_back(std::move(m));
    }
};

class Builder {
public:
    Builder(const Source& hd, const Source& gc, std::stop_token stop) : hd_(hd), gc_(gc), stop_(stop) {}

    Result<TextOut> run(TextDiag* diag);

private:
    Result<Bytes> read(const Source& s, std::string_view path) {
        if (stop_.stop_requested()) return fail("stopped", "text");
        return s.read(path, stop_);
    }
    Result<std::optional<u8>> group_of(const Source& s, std::string_view path) {
        if (!s.size(path)) return std::optional<u8>{};  // common.exists_ci -> None
        auto b = read(s, path);
        if (!b) return std::unexpected(b.error());
        return stage_msg_group(*b);
    }
    Result<void> collect(const std::vector<SpecItem>& spec, MsgList& out);
    MsgRef msg_ref_for(const Bmg& b, const Bmg& g, const std::string& fname, MsgList& sink, std::string cat);

    const Source& hd_;
    const Source& gc_;
    std::stop_token stop_;
    std::map<std::string, Bmg> H_, G_;
    std::map<std::string, u8> fgroup_;
    Ctx ctx_;
    std::vector<std::string> missing_;
};

// build().collect
Result<void> Builder::collect(const std::vector<SpecItem>& spec, MsgList& out) {
    for (const SpecItem& s : spec) {
        if (stop_.stop_requested()) return fail("stopped", "text");
        auto m = build_message(H_.at(s.file), G_.at(s.file), s.file, s.id, s.category, s.use, fgroup_.at(s.file), ctx_);
        if (!m) return std::unexpected(m.error());
        if (!*m) {
            if (s.required) missing_.push_back(s.file + ":" + std::to_string(s.id));
            continue;
        }
        out.add(std::move(**m));
    }
    return {};
}

// build().msg_ref_for
MsgRef Builder::msg_ref_for(const Bmg& b, const Bmg& g, const std::string& fname, MsgList& sink, std::string cat) {
    return [this, &b, &g, fname, &sink, cat](std::optional<u16> mid) -> Result<JsonObject> {
        JsonObject r;
        if (!mid) {
            r.emplace_back("message", Json());
            r.emplace_back("gc_index", Json());
            return r;
        }
        const std::string key = fname + ":" + std::to_string(*mid);
        auto it = sink.by_key.find(key);
        if (it == sink.by_key.end()) {
            auto m = build_message(b, g, fname, *mid, cat, "flow", fgroup_.at(fname), ctx_);
            if (!m) return std::unexpected(m.error());
            if (!*m) {
                r.emplace_back("message", Json());
                r.emplace_back("gc_index", Json());
                r.emplace_back("note", "HD entry " + std::to_string(*mid) + " has no text");
                return r;
            }
            sink.add(std::move(**m));
            it = sink.by_key.find(key);
        }
        const Message& m = sink.msgs[it->second];
        r.emplace_back("message", key);
        r.emplace_back("action", std::string(action_name(m.action)));
        const bool usable = m.action == Action::Native || m.action == Action::Override;
        r.emplace_back("gc_index", usable && m.gc_index ? Json(static_cast<i64>(*m.gc_index)) : Json());
        return r;
    };
}

Result<TextOut> Builder::run(TextDiag* diag) {
    const auto t0 = std::chrono::steady_clock::now();
    // --- the 3 HD and GC message files
    std::map<std::string, Bytes> gc_arcs;  // read each GC archive once (each read hashes it in full)
    for (const FileSpec& f : kFiles) {
        const std::string path = std::string("res/msgus/") + f.arc;
        auto h = read(hd_, path);
        if (!h) return std::unexpected(h.error());
        auto hb = load_bmg(*h, f.file, std::string(f.arc) + ":" + f.file);
        if (!hb) return std::unexpected(hb.error());
        H_.emplace(f.file, std::move(*hb));
        auto g = read(gc_, path);
        if (!g) return std::unexpected(g.error());
        auto gb = load_bmg(*g, f.file, std::string(f.arc) + ":" + f.file);
        if (!gb) return std::unexpected(gb.error());
        G_.emplace(f.file, std::move(*gb));
        gc_arcs.emplace(path, std::move(*g));
    }
    // --- every GC zel_*.bmg: charset + window widths (bmgres*.arc in sorted order, plan 7.3)
    std::vector<std::string> gc_paths;
    for (const inputs::InputFile& f : inputs::gc_files()) {
        const std::string p(f.path);
        const std::string base = p.substr(p.rfind('/') + 1);
        if (p.starts_with("res/msgus/") && base.starts_with("bmgres") && base.ends_with(".arc")) gc_paths.push_back(p);
    }
    std::stable_sort(gc_paths.begin(), gc_paths.end());
    std::vector<Bmg> gc_all;
    for (const std::string& p : gc_paths) {
        auto it = gc_arcs.find(p);
        if (it == gc_arcs.end()) {
            auto g = read(gc_, p);
            if (!g) return std::unexpected(g.error());
            it = gc_arcs.emplace(p, std::move(*g)).first;
        }
        auto a = io::rarc_read(it->second);
        if (!a) return std::unexpected(a.error());
        for (const io::RarcMember& m : a->members) {
            const std::string low = ascii_lower(m.name);
            if (!low.starts_with("zel_") || !low.ends_with(".bmg") || m.name.find("unit") != std::string::npos) continue;
            auto d = a->data(m);
            if (!d) return std::unexpected(d.error());
            auto b = parse_bmg(*d, p.substr(p.rfind('/') + 1) + ":" + m.name);
            if (!b) return std::unexpected(b.error());
            gc_all.push_back(std::move(*b));
        }
    }
    if (stop_.stop_requested()) return fail("stopped", "text");
    // _gc_chars
    for (const Bmg& b : gc_all)
        for (const Entry& e : b.entries) {
            auto toks = tokens(e.text);
            if (!toks) continue;
            for (const Tok& t : *toks)
                if (!t.tag && t.c >= 0x80) ctx_.gc_chars.insert(t.c);
        }
    // FontMetrics(gc)
    auto font_arc = read(gc_, "res/fontus/fontres.arc");
    if (!font_arc) return std::unexpected(font_arc.error());
    auto fa = io::rarc_read(*font_arc);
    if (!fa) return std::unexpected(fa.error());
    const io::RarcMember* bfn = fa->get("rodan_b_24_22.bfn");
    if (!bfn) return fail("fmt.text", "fontres.arc has no rodan_b_24_22.bfn");
    auto bfn_data = fa->data(*bfn);
    if (!bfn_data) return std::unexpected(bfn_data.error());
    auto fm = font_metrics(*bfn_data);
    if (!fm) return std::unexpected(fm.error());
    std::vector<const Bmg*> gc_ptrs;
    for (const Bmg& b : gc_all) gc_ptrs.push_back(&b);
    ctx_.fm = &*fm;
    ctx_.limits = gc_line_limits(gc_ptrs, *fm);
    if (stop_.stop_requested()) return fail("stopped", "text");
    // stage message groups (D_SB01 (GC) only fed a log line and is not a GC input: plan 2.5)
    auto g_sb11 = group_of(hd_, "res/stage/d_sb11/stg_00.arc");
    if (!g_sb11) return std::unexpected(g_sb11.error());
    auto g_sp160 = group_of(gc_, "res/stage/r_sp160/stg_00.arc");
    if (!g_sp160) return std::unexpected(g_sp160.error());
    auto g_sp160_hd = group_of(hd_, "res/stage/r_sp160/stg_00.arc");
    if (!g_sp160_hd) return std::unexpected(g_sp160_hd.error());
    fgroup_ = {{"zel_00", u8{0}}, {"zel_04", g_sp160->value_or(4)}, {"zel_05", g_sb11->value_or(5)}};

    // --- cave
    MsgList cave;
    if (auto r = collect(cave_spec(), cave); !r) return std::unexpected(r.error());
    const Bmg& b5 = H_.at("zel_05");
    const Bmg& g5 = G_.at("zel_05");
    JsonArray cave_flows;
    for (u32 fid : kCaveFlows) {
        if (stop_.stop_requested()) return fail("stopped", "text");
        const auto start = b5.flow(fid);
        if (!start) {
            missing_.push_back("flow " + std::to_string(fid));
            continue;
        }
        const MsgRef ref = msg_ref_for(b5, g5, "zel_05", cave, "cave");
        JsonArray nodes;
        for (u16 i : reachable(b5, *start)) {
            auto n = decode_node(b5, i, ref);
            if (!n) return std::unexpected(n.error());
            nodes.emplace_back(std::move(*n));
        }
        JsonArray hd_only = hd_only_summary(nodes);
        JsonObject f;
        f.emplace_back("flow_id", static_cast<i64>(fid));
        f.emplace_back("file", "zel_05");
        f.emplace_back("group", static_cast<i64>(fgroup_.at("zel_05")));
        f.emplace_back("start", static_cast<i64>(*start));
        f.emplace_back("gc_has_flow", g5.flow(fid).has_value());
        f.emplace_back("hd_only", std::move(hd_only));
        f.emplace_back("nodes", std::move(nodes));
        cave_flows.emplace_back(std::move(f));
    }

    // --- stamps
    MsgList stamps;
    if (auto r = collect(stamps_spec(), stamps); !r) return std::unexpected(r.error());
    const Bmg& b4 = H_.at("zel_04");
    const Bmg& g4 = G_.at("zel_04");
    JsonArray stamp_flows;
    for (const NpcNode& npc : kStampNpc) {
        if (stop_.stop_requested()) return fail("stopped", "text");
        if (npc.node >= b4.nodes.size()) {
            missing_.push_back("zel_04 node " + std::to_string(npc.node));
            continue;
        }
        const std::vector<u32> fl = flows_reaching(b4, npc.node);
        const std::vector<u32> keep = neighbourhood(b4, npc.node);
        const MsgRef ref = msg_ref_for(b4, g4, "zel_04", stamps, "stamp-npc");
        JsonArray nodes;
        for (u32 i : keep) {
            auto n = decode_node(b4, i, ref);
            if (!n) return std::unexpected(n.error());
            const Json* type = get(*n, "type");
            const Json* gci = get(*n, "gc_index");
            if (type && std::get<std::string>(type->v) == "message" && gci && std::holds_alternative<i64>(gci->v)) {
                const i64 want = std::get<i64>(gci->v);
                JsonArray gc_nodes;
                for (std::size_t k = 0; k < g4.nodes.size(); ++k) {
                    const auto& gn = g4.nodes[k];
                    if (gn[0] == 1 && ((gn[2] << 8) | gn[3]) == want) gc_nodes.emplace_back(static_cast<i64>(k));
                }
                n->emplace_back("gc_nodes", std::move(gc_nodes));
            }
            nodes.emplace_back(std::move(*n));
        }
        JsonArray hd_only = hd_only_summary(nodes);
        JsonArray reach;
        for (u32 f : fl) reach.emplace_back(static_cast<i64>(f));
        JsonObject f;
        f.emplace_back("flow_id", fl.empty() ? Json() : Json(static_cast<i64>(fl[0])));
        f.emplace_back("flows_reaching", std::move(reach));
        f.emplace_back("file", "zel_04");
        f.emplace_back("group", static_cast<i64>(fgroup_.at("zel_04")));
        const auto hs = fl.empty() ? std::nullopt : b4.flow(fl[0]);
        f.emplace_back("start", hs ? Json(static_cast<i64>(*hs)) : Json());
        f.emplace_back("gc_has_flow", !fl.empty() && g4.flow(fl[0]).has_value());
        const auto gs = fl.empty() ? std::nullopt : g4.flow(fl[0]);
        f.emplace_back("gc_start", gs ? Json(static_cast<i64>(*gs)) : Json());
        f.emplace_back("focus", JsonArray{Json(static_cast<i64>(npc.node))});
        f.emplace_back("focus_note", npc.why);
        f.emplace_back("hd_only", std::move(hd_only));
        f.emplace_back("nodes", std::move(nodes));
        stamp_flows.emplace_back(std::move(f));
    }
    if (!missing_.empty()) {
        std::string s;
        for (const std::string& m : missing_) s += (s.empty() ? "" : ", ") + m;
        return fail("fmt.text", "missing " + s);
    }

    // --- write
    const io::JsonStyle style{.indent = -1, .ensure_ascii = false, .item_sep = ",", .key_sep = ":"};
    TextOut out;
    out.cave_messages_bin = write_bin(cave.msgs);
    const std::string cf = io::json_lines(cave_flows, style);
    out.cave_flows_json.assign(cf.begin(), cf.end());
    out.stamps_messages_bin = write_bin(stamps.msgs);

    if (diag) {
        for (const auto& [k, v] : ctx_.limits.limit) diag->line_limits.emplace_back(k, v);
        for (const auto& [k, v] : ctx_.limits.samples) diag->line_samples.emplace_back(k, v);
        for (const auto& [part, list] : {std::pair<const char*, const MsgList*>{"cave", &cave}, {"stamps", &stamps}}) {
            for (const Message& m : list->msgs) {
                for (const std::string& note : m.notes) {
                    if (note.starts_with("lines re-wrapped") || note.starts_with("selection message")) {
                        TextDiag::Rewrap r{part, m.file, m.id, m.entry[9], ctx_.limits.get(m.entry[9]), m.text, note};
                        diag->rewrapped.emplace_back(m.file + ":" + std::to_string(m.group) + ":" + std::to_string(m.id),
                                                     show(m.text));
                        diag->rewraps.push_back(std::move(r));
                    }
                }
                diag->messages.push_back(TextDiag::Message{part, m.file, m.group, m.id, static_cast<u8>(m.action),
                                                           m.text, m.entry, m.preview});
            }
        }
        diag->stage_groups = {{"D_SB11", g_sb11->has_value() ? int(**g_sb11) : -1},
                              {"R_SP160_GC", g_sp160->has_value() ? int(**g_sp160) : -1},
                              {"R_SP160_HD", g_sp160_hd->has_value() ? int(**g_sp160_hd) : -1}};
        diag->gc_chars.assign(ctx_.gc_chars.begin(), ctx_.gc_chars.end());
        diag->font_cell = fm->cell;
        diag->font_cell_w = fm->cell_w;
        diag->font_name = fm->name;
        const std::string sf = io::json_lines(stamp_flows, style);
        diag->stamps_flows_json.assign(sf.begin(), sf.end());
        diag->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    return out;
}

}  // namespace

Result<TextOut> build(const Source& hd, const Source& gc, std::stop_token stop, TextDiag* diag) {
    Builder b(hd, gc, stop);
    return b.run(diag);
}

}  // namespace hdf::text
