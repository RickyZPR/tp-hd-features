// Module TX: message flows (hdtext._label, decode_node, reachable, predecessors, neighbourhood, flows_reaching).
//
// Label names come from facts.label_table() (the oracle was made without --src, Correction #3), i.e.
// hdf::facts::label(). Query/event names are FlowService's FlowGameQuery / FlowGameEvent enums (flow.h).
#include "text_internal.hpp"

#include "hdf/facts.hpp"

#include <algorithm>
#include <cstdio>

namespace hdf::text::detail {

namespace {

using io::Json;
using io::JsonArray;
using io::JsonObject;

constexpr u16 kLabelsGc = 822;  // LABELS_GC

// hdtext.QUERY_NAMES (flow.h, 53 entries)
constexpr std::array<std::string_view, 53> kQueryNames = {
    "SELECT_2", "EVENT_FLAG", "PLAYER_FORM", "RANDOM", "SELECT_3", "TALK_DISTANCE", "RUPEES",
    "SWORD_TUTORIAL_STEP", "SWORD_TUTORIAL_RESULT", "SWORD_TUTORIAL_COUNT", "TEMP_FLAG",
    "CHEST_FLAG", "SAVE_SWITCH", "SAVE_ITEM_FLAG", "DUNGEON_SWITCH", "DUNGEON_ITEM_FLAG",
    "ZONE_SWITCH", "ZONE_ITEM_FLAG", "ONE_ZONE_SWITCH", "ONE_ZONE_ITEM_FLAG", "EQUIPPED",
    "ITEM_OWNED", "BOMB_BAG_COUNT", "ARROWS", "EMPTY_BOTTLES", "SHOP_CLERK", "TEARS_OF_LIGHT",
    "HERDING_TIME", "LANTERN_OIL", "REGISTER", "GOATS_CAUGHT", "HEARTS", "HOLDING_LANTERN",
    "TIME_OF_DAY", "MAGIC", "SELECT_2_CANCEL", "SELECT_3_CANCEL", "BOMB_BAG_CONTENTS",
    "BOMBS_FIT", "BOMB_BAG_FILL", "WATER_BOMBS_FIT", "TRANSFORM_BLOCKED", "BOMBLINGS_FIT",
    "WARP_ALLOWED", "GOLDEN_BUGS", "UNDELIVERED_BUG", "UNUSED_46", "NEW_LETTERS", "POE_SOULS",
    "DONATION_TOTAL", "BALLOON_SCORE", "IN_WATER", "IRON_BOOTS"};

// hdtext.EVENT_NAMES (43 entries)
constexpr std::array<std::string_view, 43> kEventNames = {
    "SET_EVENT_FLAG", "CLEAR_EVENT_FLAG", "ADD_RUPEES", "REMOVE_RUPEES", "ADD_HEARTS",
    "REMOVE_HEARTS", "ADD_MAGIC", "REMOVE_MAGIC", "START_EVENT", "JUMP_FLOW", "SET_TEMP_FLAG",
    "CLEAR_TEMP_FLAG", "OPEN_DOOR", "SELECT_VERTICAL", "SET_SWITCH", "CLEAR_SWITCH",
    "SHOP_SELECT", "GIVE_ITEM", "STAGE_DIRECTION", "SET_SPEAKER", "WARP_PLAYER", "WAIT",
    "FILL_LANTERN", "FILL_BOTTLE", "SHOP_SOLD_OUT", "SET_REGISTER", "TENT_PURCHASE", "FILL_BOMBS",
    "SELL_BOMBS", "SELECT_HORIZONTAL", "FILL_ARROWS", "RETURN_RENTAL_BOMB_BAG", "FADE_IN",
    "FADE_OUT", "SET_TRADE_ITEM", "REMOVE_ITEM", "SET_SAVE_SWITCH", "CLEAR_SAVE_SWITCH",
    "RECEIVE_LETTER", "UNLOCK_MAP_REGION", "EMPTY_BOTTLE", "ADD_DONATION", "UNUSED_42"};

// hdtext.HD_QUERY_NOTES / HD_EVENT_NOTES (this project's own notes)
const char* hd_query_note(u32 q) {
    switch (q) {
    case 53: return "HD_QUERY054 (tphd stub returns 1; semantics INFERRED)";
    case 54:
        return "HD_QUERY055 (HD 0x02a64650: 1 if !0x7001, 2 if 0x7001&&0x5420&&!0x7002, "
               "3 if 0x7002&&0x7101&&!0x7004, else 0)";
    default: return nullptr;
    }
}
const char* hd_event_note(u32 ev) {
    switch (ev) {
    case 43: return "HD_EVENT043 (HD 0x02a65c30; only on an unreachable node)";
    case 44: return "HD_EVENT044 Withdraw (HD 0x02a65c84: warp sounds, then the game-over/reset path; no flags)";
    case 45: return "HD_EVENT045 (HD 0x02a65f74: demo unsuspend by its parameter; flow 1120 no-message path)";
    default: return nullptr;
    }
}

// hdtext.HD_LABELS: HD saveBitLabels[822..829] (bit, meaning in this project's words)
struct HdLabel {
    u16 index, bit;
    const char* meaning;
};
constexpr std::array<HdLabel, 8> kHdLabels = {{
    {822, 0x7008, "Great Fairy reward given (flow 1122)"},
    {823, 0x0610, "amiibo use unlocked = GC M_019 (Faron light spirit revived)"},
    {824, 0x7001, "section 1 cleared (floor-6 portal used; flow 1121, query055, tier ALLdie)"},
    {825, 0x5420, "= GC F_0685 (first Mirror shard scene)"},
    {826, 0x7002, "section 2 cleared (floor-20 portal used)"},
    {827, 0x7101, "Sols power in the Master Sword (HD flow 811 node 1173; flow 1121, tier ALLdie)"},
    {828, 0x7004, "section 3 cleared (floor-41 portal used)"},
    {829, 0x7080, "arrival intro pending (entry bookkeeping; R19 TagEv valid flag; flow 1120 clears it)"},
}};

std::string first_word(std::string_view s) { return std::string(s.substr(0, s.find(' '))); }  // .split(' ')[0]

std::string hex(View b) {
    static constexpr char k[] = "0123456789abcdef";
    std::string s;
    for (u8 x : b) {
        s += k[x >> 4];
        s += k[x & 15];
    }
    return s;
}

std::string bit_hex(u32 bit) {  // '0x%04x'
    char b[16];
    std::snprintf(b, sizeof b, "0x%04x", bit);
    return b;
}

u16 n16(const std::array<u8, 8>& n, std::size_t o) { return static_cast<u16>((n[o] << 8) | n[o + 1]); }

// dict[key] = value: replace in place (dict order kept) or append
void set(JsonObject& o, std::string_view key, Json v) {
    for (auto& [k, x] : o)
        if (k == key) {
            x = std::move(v);
            return;
        }
    o.emplace_back(std::string(key), std::move(v));
}

const Json* get(const JsonObject& o, std::string_view key) {
    for (const auto& [k, x] : o)
        if (k == key) return &x;
    return nullptr;
}

bool truthy(const Json* j) {  // Python truth of n.get(key)
    if (!j) return false;
    if (const bool* b = std::get_if<bool>(&j->v)) return *b;
    if (const i64* i = std::get_if<i64>(&j->v)) return *i != 0;
    if (const std::string* s = std::get_if<std::string>(&j->v)) return !s->empty();
    if (std::holds_alternative<std::monostate>(j->v)) return false;
    return true;
}

Json end_or(u32 v) { return v == 0xFFFF ? Json() : Json(static_cast<i64>(v)); }  // end = lambda v: None if 0xFFFF

}  // namespace

// hdtext._label with names = facts.label_table()
Json label_json(u32 idx) {
    JsonObject d;
    d.emplace_back("index", static_cast<i64>(idx));
    d.emplace_back("hd_only", idx >= kLabelsGc);
    if (idx >= kLabelsGc) {
        const HdLabel* h = nullptr;
        for (const HdLabel& x : kHdLabels)
            if (x.index == idx) h = &x;
        d.emplace_back("bit", h ? Json(bit_hex(h->bit)) : Json());
        d.emplace_back("meaning", h ? h->meaning : "unknown HD label");
    } else if (const facts::SaveLabel* l = facts::label(idx)) {
        d.emplace_back("name", std::string(l->name));
        d.emplace_back("bit", bit_hex(l->bit));
        if (!l->description.empty()) d.emplace_back("desc", std::string(l->description));
    }
    return Json(std::move(d));
}

// hdtext.decode_node
Result<JsonObject> decode_node(const Bmg& b, u32 i, const MsgRef& msg_ref) {
    if (i >= b.nodes.size()) return fail("fmt.text", b.name + ": node " + std::to_string(i) + " out of range");
    const std::array<u8, 8>& n = b.nodes[i];
    const u8 t = n[0];
    JsonObject d;
    d.emplace_back("n", static_cast<i64>(i));
    if (t == 1) {
        const u16 mi = n16(n, 2);
        std::optional<u16> mid;
        if (mi < b.entries.size()) mid = b.entries[mi].id;
        set(d, "type", "message");
        set(d, "subtype", static_cast<i64>(n[1]));
        set(d, "hd_entry", static_cast<i64>(mi));
        set(d, "msg_id", mid ? Json(static_cast<i64>(*mid)) : Json());
        set(d, "next", end_or(n16(n, 4)));
        set(d, "unknown", static_cast<i64>(n16(n, 6)));
        auto ref = msg_ref(mid);
        if (!ref) return std::unexpected(ref.error());
        for (auto& [k, v] : *ref) set(d, k, std::move(v));
    } else if (t == 2) {
        const u32 cnt = n[1], q = n16(n, 2), prm = n16(n, 4), first = n16(n, 6);
        JsonArray res;
        for (u32 k = 0; k < cnt; ++k) res.push_back(first + k < b.edges.size() ? end_or(b.edges[first + k]) : Json());
        std::string qname;
        if (q < kQueryNames.size()) {
            qname = "FLOW_QUERY_" + std::string(kQueryNames[q]);
        } else {
            char fb[32];
            std::snprintf(fb, sizeof fb, "HD_QUERY%03u", q + 1);
            qname = first_word(hd_query_note(q) ? hd_query_note(q) : fb);
        }
        set(d, "type", "branch");
        set(d, "query", static_cast<i64>(q));
        set(d, "query_name", qname);
        set(d, "param", static_cast<i64>(prm));
        set(d, "results", std::move(res));
        set(d, "hd_only", q >= kQueryNames.size());
        if (q >= kQueryNames.size()) set(d, "note", hd_query_note(q) ? hd_query_note(q) : "HD-only query");
        if (q == 1) {
            set(d, "label", label_json(prm));
            if (prm >= kLabelsGc) set(d, "hd_only", true);
        }
    } else if (t == 3) {
        const u32 ev = n[1];
        const u32 p0 = n16(n, 4), p1 = n16(n, 6), nx = n16(n, 2);
        std::string ename;
        if (ev < kEventNames.size()) {
            ename = "FLOW_EVENT_" + std::string(kEventNames[ev]);
        } else {
            char fb[32];
            std::snprintf(fb, sizeof fb, "HD_EVENT%03u", ev);
            ename = first_word(hd_event_note(ev) ? hd_event_note(ev) : fb);
        }
        set(d, "type", "event");
        set(d, "event", static_cast<i64>(ev));
        set(d, "event_name", ename);
        set(d, "params", hex(View(n.data() + 4, 4)));
        set(d, "p0", static_cast<i64>(p0));
        set(d, "p1", static_cast<i64>(p1));
        set(d, "hd_only", ev >= kEventNames.size());
        if (ev == 9) {
            set(d, "next", Json());
            set(d, "jump_flow", static_cast<i64>((static_cast<u32>(p0) << 16) | p1));
        } else {
            set(d, "next", nx == 0xFFFF ? Json() : nx < b.edges.size() ? end_or(b.edges[nx]) : Json());
        }
        if (ev >= kEventNames.size()) set(d, "note", hd_event_note(ev) ? hd_event_note(ev) : "HD-only event");
        if (ev == 0 || ev == 1) {
            JsonArray labels;
            for (u32 x : {p0, p1})
                if (x) labels.push_back(label_json(x));
            set(d, "labels", std::move(labels));
            if (p0 >= kLabelsGc || p1 >= kLabelsGc) set(d, "hd_only", true);
        }
    } else {
        set(d, "type", "type" + std::to_string(t));
    }
    set(d, "raw", hex(View(n.data(), 8)));
    return d;
}

JsonArray hd_only_summary(const JsonArray& nodes) {
    JsonArray out;
    for (const Json& j : nodes) {
        const JsonObject* o = std::get_if<JsonObject>(&j.v);
        if (!o || !truthy(get(*o, "hd_only"))) continue;
        const Json* type = get(*o, "type");
        const Json* name = get(*o, "query_name");
        if (!truthy(name)) name = get(*o, "event_name");
        const Json* nn = get(*o, "n");
        std::string s = std::get<std::string>(type->v) + " ";
        s += name && std::holds_alternative<std::string>(name->v) ? std::get<std::string>(name->v) : "None";
        s += " @" + std::to_string(std::get<i64>(nn->v));
        out.emplace_back(std::move(s));
    }
    return out;
}

// hdtext.reachable: depth-first, visit order
std::vector<u16> reachable(const Bmg& b, u32 start) {
    std::vector<u8> seen(b.nodes.size(), 0);
    std::vector<u16> order;
    std::vector<u32> stack{start};
    while (!stack.empty()) {
        const u32 i = stack.back();
        stack.pop_back();
        if (i == 0xFFFF || i >= b.nodes.size() || seen[i]) continue;
        seen[i] = 1;
        order.push_back(static_cast<u16>(i));
        const std::array<u8, 8>& n = b.nodes[i];
        if (n[0] == 1) {
            stack.push_back(n16(n, 4));
        } else if (n[0] == 2) {
            const u32 first = n16(n, 6);
            std::vector<u32> succ;
            for (u32 k = 0; k < n[1]; ++k)
                if (first + k < b.edges.size()) succ.push_back(b.edges[first + k]);
            stack.insert(stack.end(), succ.rbegin(), succ.rend());
        } else if (n[0] == 3 && n[1] != 9) {
            const u32 nx = n16(n, 2);
            if (nx < b.edges.size()) stack.push_back(b.edges[nx]);
        }
    }
    return order;
}

// hdtext.predecessors
std::map<u32, std::vector<u32>> predecessors(const Bmg& b) {
    std::map<u32, std::vector<u32>> pred;
    for (u32 i = 0; i < b.nodes.size(); ++i) {
        const std::array<u8, 8>& n = b.nodes[i];
        std::vector<u32> succ;
        if (n[0] == 1) {
            succ.push_back(n16(n, 4));
        } else if (n[0] == 2) {
            const u32 first = n16(n, 6);
            for (u32 k = 0; k < n[1]; ++k)
                if (first + k < b.edges.size()) succ.push_back(b.edges[first + k]);
        } else if (n[0] == 3 && n[1] != 9 && n16(n, 2) < b.edges.size()) {
            succ.push_back(b.edges[n16(n, 2)]);
        }
        for (u32 s : succ) pred[s].push_back(i);
    }
    return pred;
}

// hdtext.neighbourhood: the focus node, everything it leads to, and up to `back` steps of predecessors (sorted)
std::vector<u32> neighbourhood(const Bmg& b, u32 node, int back) {
    const auto pred = predecessors(b);
    std::set<u32> keep;
    for (u16 x : reachable(b, node)) keep.insert(x);
    std::set<u32> front{node};
    for (int k = 0; k < back; ++k) {
        std::set<u32> nxt;
        for (u32 x : front) {
            const auto it = pred.find(x);
            if (it != pred.end()) nxt.insert(it->second.begin(), it->second.end());
        }
        for (u32 x : keep) nxt.erase(x);
        keep.insert(nxt.begin(), nxt.end());
        front = std::move(nxt);
    }
    return std::vector<u32>(keep.begin(), keep.end());
}

// hdtext.flows_reaching
std::vector<u32> flows_reaching(const Bmg& b, u32 node) {
    std::vector<u32> out;
    for (const auto& [f, s] : b.flows) {
        const std::vector<u16> r = reachable(b, s);
        if (std::find(r.begin(), r.end(), node) != r.end()) out.push_back(f);
    }
    std::stable_sort(out.begin(), out.end());
    return out;
}

}  // namespace hdf::text::detail
