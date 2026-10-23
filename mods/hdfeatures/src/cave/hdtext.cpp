// Cave of Shadows: HD text and flows (see hdtext.hpp).

#include "stamps/prelude.hpp"  // library headers before any game header (see prelude.hpp)

#include "hdtext.hpp"

#include "cave.hpp"
#include "hdparse.hpp"
#include "hdfiles/files.hpp"

#include <mods/svc/flow.hpp>
#include <mods/svc/log.hpp>
#include <mods/svc/message.h>
#include <mods/svc/stage.h>

#include "d/d_com_inf_game.h"
#include "d/d_save.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cave::hdtext {
namespace {

using hd::Action;
using hd::FlowDef;
using hd::FlowNode;
using hd::MessageRec;
using hd::NodeType;

constexpr std::uint16_t kGroup = 5;                     // D_SB11 STAG message group (zel_05)
constexpr std::uint16_t kSaveBitLabelCount = 822;       // dSv_event_flag_c::saveBitLabels[822]
// MessageService resolves a custom message for the active language only (svc/flow.cpp
// node_resolves): register the English text for every language the game can be set to.
constexpr std::array<std::uint8_t, 6> kLanguages = {MESSAGE_LANGUAGE_ENGLISH, MESSAGE_LANGUAGE_GERMAN,
    MESSAGE_LANGUAGE_FRENCH, MESSAGE_LANGUAGE_SPANISH, MESSAGE_LANGUAGE_ITALIAN,
    MESSAGE_LANGUAGE_JAPANESE};

// HD dMsgFlow_c table indices that v2.0.3 does not have (tphd: query054/055, event043-045).
constexpr std::uint16_t kHdQuery054 = 53;
constexpr std::uint16_t kHdQuery055 = 54;
constexpr std::uint8_t kHdEvent043 = 43;
constexpr std::uint8_t kHdEvent044 = 44;
constexpr std::uint8_t kHdEvent045 = 45;

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
std::vector<MessageRec> g_texts;                  // messages.bin
std::vector<FlowDef> g_flows;                     // flows.json
bool g_textLoaded = false;
bool g_hdFlows = false;
std::string g_loadProblem;

std::vector<mods::flow::RegisteredMessage> g_registered;
std::map<std::pair<std::uint8_t, std::uint16_t>, MessageId> g_registeredIds;  // (file, HD id) -> id
std::vector<mods::flow::MessageOverride> g_overrides;
std::vector<mods::flow::Graph> g_graphs;
std::map<std::uint16_t, std::uint16_t> g_flowStarts;  // flow id -> custom start node
std::vector<StageActorHandle> g_stageEdits;
std::vector<std::uint16_t> g_tagFlows;                 // flows with a talk tag in D_SB11
std::vector<std::pair<std::uint16_t, int>> g_tagRooms;  // (flow, room) of every talk tag added

mods::flow::Query g_qLabel;
mods::flow::Query g_qIntro;
mods::flow::Query g_qStub;
mods::flow::Query g_qReward;
mods::flow::Event g_eLabelSet;
mods::flow::Event g_eLabelClear;
mods::flow::Event g_eWithdraw;
mods::flow::Event g_eStub;
mods::flow::Event g_eReward;

std::uint16_t g_fallbackArrival = 0;              // own-wording arrival message (no HD text)
std::uint16_t g_fallbackTurnBack = 0;             // own-wording turn-back message while Midna is gravely ill

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint16_t clamp_result(std::uint16_t value, const FlowQueryContext* query) {
    const std::uint16_t count = query != nullptr ? query->result_count : 1;
    return count == 0 ? 0 : std::min<std::uint16_t>(value, static_cast<std::uint16_t>(count - 1));
}

bool game_label(std::uint16_t label) {
    if (label == 0 || label == 0xFFFF) {
        return false;
    }
    if (label >= kSaveBitLabelCount) {
        return cave::hd_label(label);
    }
    return dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[label]) != 0;
}

void set_game_label(std::uint16_t label, bool on) {
    if (label == 0 || label == 0xFFFF) {
        return;
    }
    if (label >= kSaveBitLabelCount) {
        cave::set_hd_label(label, on);
        return;
    }
    const u16 bit = dSv_event_flag_c::saveBitLabels[label];
    if (on) {
        dComIfGs_onEventBit(bit);
    } else {
        dComIfGs_offEventBit(bit);
    }
}

bool load_file(const char* path, std::vector<std::uint8_t>& out) {
    return hdfiles::load(path, out);  // the stored game files (hdfiles/files.hpp), checked when read
}

const MessageRec* find_text(std::uint8_t file, std::uint16_t id) {
    for (const MessageRec& rec : g_texts) {
        if (rec.file == file && rec.id == id) {
            return &rec;
        }
    }
    return nullptr;
}

// GC INF1 index of a native/override message, taken from the flows' message nodes.
std::int32_t gc_index_of(std::uint8_t file, std::uint16_t id) {
    for (const FlowDef& flow : g_flows) {
        for (const FlowNode& node : flow.nodes) {
            if (node.type == NodeType::Message && node.msg_file == file && node.msg_id == id &&
                node.gc_index >= 0)
            {
                return node.gc_index;
            }
        }
    }
    return -1;
}

mods::flow::RegisteredMessage register_raw(
    std::uint16_t group, const std::array<std::uint8_t, 20>& entry, std::span<const std::uint8_t> text) {
    MessageEntryData data{};
    std::memcpy(data.bytes, entry.data(), sizeof(data.bytes));
    std::vector<MessageVariantData> variants;
    variants.reserve(kLanguages.size());
    for (const std::uint8_t language : kLanguages) {
        variants.push_back({language, data, text.data(), text.size()});
    }
    MessageId id{};
    MessageHandle handle{};
    const ModResult result = svc_message != nullptr ?
                                 svc_message->register_message(mod_ctx, group, variants.data(),
                                     variants.size(), &id, &handle) :
                                 MOD_UNAVAILABLE;
    return {id, handle, result};
}

mods::flow::RegisteredMessage register_built(std::uint16_t group, const mods::flow::MessageBuilder& builder) {
    std::vector<mods::flow::MessageVariant> variants;
    for (const std::uint8_t language : kLanguages) {
        variants.push_back(builder.build(static_cast<MessageLanguage>(language)));
    }
    return mods::flow::register_message(group, variants);
}

// ---------------------------------------------------------------------------------------------
// Custom queries / events (critic #8 + tphd's HD-only table entries)
// ---------------------------------------------------------------------------------------------
std::uint16_t query_label(ModContext*, const FlowQueryContext* query, void*) {
    // Same polarity as dMsgFlow_c::query001: 0 when the label IS set (critic #6).
    const bool set = query != nullptr && game_label(query->parameter);
    return clamp_result(set ? 0 : 1, query);
}

std::uint16_t query_intro(ModContext*, const FlowQueryContext* query, void*) {
    return clamp_result(cave::intro_query(), query);
}

std::uint16_t query_stub(ModContext*, const FlowQueryContext* query, void*) {
    return clamp_result(1, query);  // tphd's query054 stub answers 1 (INFERRED)
}

std::uint16_t query_reward(ModContext*, const FlowQueryContext* query, void*) {
    if (query != nullptr && query->phase == FLOW_QUERY_PHASE_PROBE) {
        return clamp_result(2, query);
    }
    return clamp_result(cave::reward_query(), query);
}

void event_label(const FlowEventContext* event, bool on) {
    if (event == nullptr) {
        return;
    }
    set_game_label(be16(event->parameters), on);
    set_game_label(be16(event->parameters + 2), on);
}

void event_label_set(ModContext*, const FlowEventContext* event, void*) {
    event_label(event, true);
}

void event_label_clear(ModContext*, const FlowEventContext* event, void*) {
    event_label(event, false);
}

// Midna's menu (flow 1111 node 1607, or the fallback menu): HD runs event044 right after "Yes".
void event_withdraw(ModContext*, const FlowEventContext*, void*) {
    cave::flow_withdraw(false);
}

void event_stub(ModContext*, const FlowEventContext*, void*) {}

void event_reward(ModContext*, const FlowEventContext*, void*) {
    cave::flow_reward();
}

// FlowService has no unregister for queries/events: they are registered once per mod load (the HD
// Features switch turns the cave off and on with shutdown()/initialize(), which must not register them
// again) and stay until the loader drops the mod.
bool g_callbacksTried = false;
bool g_callbacksOk = false;

bool register_callbacks() {
    if (g_callbacksTried) {
        return g_callbacksOk;
    }
    g_callbacksTried = true;
    g_qLabel = mods::flow::register_query("cave: HD event label", query_label);
    g_qIntro = mods::flow::register_query("cave: HD query 55 (arrival)", query_intro);
    g_qStub = mods::flow::register_query("cave: HD query 54", query_stub);
    g_qReward = mods::flow::register_query("cave: reward talk", query_reward);
    g_eLabelSet = mods::flow::register_event("cave: set HD event label", event_label_set);
    g_eLabelClear = mods::flow::register_event("cave: clear HD event label", event_label_clear);
    g_eWithdraw = mods::flow::register_event("cave: HD event 44 (withdraw)", event_withdraw);
    g_eStub = mods::flow::register_event("cave: HD event 43/45", event_stub);
    g_eReward = mods::flow::register_event("cave: reward", event_reward);
    const bool ok = g_qLabel && g_qIntro && g_qStub && g_qReward && g_eLabelSet && g_eLabelClear &&
                    g_eWithdraw && g_eStub && g_eReward;
    if (!ok) {
        mods::log::warn("could not register the cave's flow queries/events; HD flows are off");
    }
    g_callbacksOk = ok;
    return ok;
}

// ---------------------------------------------------------------------------------------------
// HD flow -> FlowService node translation
// ---------------------------------------------------------------------------------------------
struct PendingNode {
    std::uint16_t hd = 0;                 // HD node index
    FlowNodeData data{};
    bool branch = false;                  // wire with results() instead of next()
    std::vector<std::uint16_t> targets;   // HD node indices / hd::kEnd
};

std::optional<std::uint16_t> message_entry(const FlowNode& node) {
    if (node.msg_action == Action::Register) {
        const auto found = g_registeredIds.find({node.msg_file, node.msg_id});
        if (found == g_registeredIds.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    if (node.gc_index < 0 || node.gc_index >= 0x8000) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(node.gc_index);
}

bool translate(const FlowDef& flow, std::vector<PendingNode>& out, std::string& why) {
    out.clear();
    for (const FlowNode& node : flow.nodes) {
        PendingNode pending;
        pending.hd = node.n;
        switch (node.type) {
        case NodeType::Message: {
            const auto entry = message_entry(node);
            if (!entry) {
                why = "message " + std::to_string(node.msg_id) + " is not available";
                return false;
            }
            pending.data = mods::flow::message(node.subtype, *entry, hd::kEnd, node.unknown);
            pending.targets = {node.next};
            break;
        }
        case NodeType::Branch: {
            FlowQueryId query = node.query;
            if (node.query == kHdQuery055) {
                query = g_qIntro.id();
            } else if (node.query >= FLOW_QUERY_BUILTIN_COUNT) {
                if (node.query != kHdQuery054) {
                    why = "unknown HD query " + std::to_string(node.query);
                    return false;
                }
                query = g_qStub.id();
            } else if (node.query == FLOW_QUERY_EVENT_FLAG && node.param >= kSaveBitLabelCount) {
                query = g_qLabel.id();
            }
            pending.data = mods::flow::branch(0, query, node.param, 0);
            pending.branch = true;
            pending.targets = node.results;
            break;
        }
        case NodeType::Event: {
            FlowEventId event = node.event;
            const std::uint16_t p0 = be16(node.params.data());
            const std::uint16_t p1 = be16(node.params.data() + 2);
            if (node.event == kHdEvent044) {
                event = g_eWithdraw.id();
            } else if (node.event == kHdEvent043 || node.event == kHdEvent045) {
                event = g_eStub.id();
            } else if (node.event >= FLOW_EVENT_BUILTIN_COUNT) {
                why = "unknown HD event " + std::to_string(node.event);
                return false;
            } else if ((node.event == FLOW_EVENT_SET_EVENT_FLAG ||
                           node.event == FLOW_EVENT_CLEAR_EVENT_FLAG) &&
                       ((p0 >= kSaveBitLabelCount && p0 != 0xFFFF) ||
                           (p1 >= kSaveBitLabelCount && p1 != 0xFFFF)))
            {
                event = node.event == FLOW_EVENT_SET_EVENT_FLAG ? g_eLabelSet.id() : g_eLabelClear.id();
            }
            pending.data = mods::flow::event(event, 0, node.params);
            pending.targets = {node.next};
            break;
        }
        }
        out.push_back(std::move(pending));
    }
    return true;
}

// Adds translated nodes to `builder`; returns the custom id of the flow's start node.
std::uint16_t add_translated(mods::flow::GraphBuilder& builder, const FlowDef& flow,
    const std::vector<PendingNode>& nodes) {
    std::map<std::uint16_t, mods::flow::NodeRef> refs;
    for (const PendingNode& node : nodes) {
        refs.emplace(node.hd, builder.add_node(node.data));
    }
    for (const PendingNode& node : nodes) {
        std::vector<std::uint16_t> targets;
        targets.reserve(node.targets.size());
        for (const std::uint16_t target : node.targets) {
            if (target == hd::kEnd) {
                targets.push_back(mods::flow::kEnd);
            } else {
                const auto found = refs.find(target);
                targets.push_back(found != refs.end() ? found->second.id() : mods::flow::kEnd);
            }
        }
        const mods::flow::NodeRef& ref = refs.at(node.hd);
        if (node.branch) {
            ref.results(targets);
        } else {
            ref.next(targets.empty() ? mods::flow::kEnd : targets.front());
        }
    }
    const auto start = refs.find(flow.start);
    return start != refs.end() ? start->second.id() : 0;
}

// The floor-41 reward talk: HD flow 1122's lines (the Great Fairy's voice) around the mod's own
// reward logic. Node structure is the mod's: the HD flow's zone/memory switches and item gives are
// replaced by one custom event (no new .gci bits; the reward rules live in cave.cpp).
bool add_reward_flow(mods::flow::GraphBuilder& builder, std::uint16_t& outStart) {
    struct Line {
        std::uint8_t file;
        std::uint16_t id;
    };
    const auto entry = [](Line line) -> std::optional<std::uint16_t> {
        const MessageRec* rec = find_text(line.file, line.id);
        if (rec == nullptr) {
            return std::nullopt;
        }
        if (rec->action == Action::Register) {
            const auto found = g_registeredIds.find({line.file, line.id});
            if (found == g_registeredIds.end()) {
                return std::nullopt;
            }
            return found->second;
        }
        const std::int32_t index = gc_index_of(line.file, line.id);
        if (index < 0) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(index);
    };
    const Line reward[] = {{kZel05, 5068}, {kZel05, 7521}};
    const Line again[] = {{kZel05, 7546}, {kZel05, 7547}, {kZel05, 7548}, {kZel05, 7525}};
    std::vector<std::uint16_t> rewardIds;
    std::vector<std::uint16_t> againIds;
    for (const Line line : reward) {
        const auto id = entry(line);
        if (!id) {
            return false;
        }
        rewardIds.push_back(*id);
    }
    for (const Line line : again) {
        const auto id = entry(line);
        if (!id) {
            return false;
        }
        againIds.push_back(*id);
    }

    const auto chain = [&](const std::vector<std::uint16_t>& ids) -> std::uint16_t {
        const mods::flow::NodeRef give = builder.add_event(g_eReward.id(), {0, 0, 0, 0});
        give.next(mods::flow::kEnd);
        std::uint16_t next = give.id();
        for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
            const mods::flow::NodeRef msg = builder.add_message(*it);
            msg.next(next);
            next = msg.id();
        }
        return next;
    };
    const std::uint16_t first = chain(rewardIds);
    const std::uint16_t second = chain(againIds);
    const mods::flow::NodeRef branch = builder.add_branch(g_qReward.id(), 0);
    branch.results({first, second, mods::flow::kEnd});
    outStart = branch.id();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Own-wording fallback (no res/cave/hdtext): Midna's withdraw menu and the arrival message. The texts and
// the menu layout are the mod's own; only the message attributes (Midna's box and voice, the light
// spirit box) follow the game's conventions.
// ---------------------------------------------------------------------------------------------
bool build_fallback() {
    const auto midna = mods::flow::MessageStyle{}
                           .speaker(0x15)
                           .box_kind(MESSAGE_BOX_MIDNA)
                           .draw_type(MESSAGE_DRAW_TYPED);
    mods::flow::MessageBuilder ask{midna};
    ask.text("Hm? What do you need?").await_choice();
    mods::flow::MessageBuilder menu{midna};
    menu.options("Leave this place", "Never mind");
    mods::flow::MessageBuilder confirm{midna};
    confirm.text("Head back up to where we came from?").await_choice();
    mods::flow::MessageBuilder yesNo{midna};
    yesNo.options("Yes", "No");

    auto askMsg = register_built(kGroup, ask);
    auto menuMsg = register_built(kGroup, menu);
    auto confirmMsg = register_built(kGroup, confirm);
    auto yesNoMsg = register_built(kGroup, yesNo);
    if (!askMsg || !menuMsg || !confirmMsg || !yesNoMsg) {
        return false;
    }

    mods::flow::GraphBuilder builder{kGroup};
    const auto withdraw = builder.add_event(g_eWithdraw.id(), {0, 0, 0, 0});
    withdraw.next(mods::flow::kEnd);
    const auto confirmSelect = builder.add_branch(FLOW_QUERY_SELECT_2_CANCEL, 0);
    confirmSelect.results({withdraw.id(), mods::flow::kEnd, mods::flow::kEnd});
    const auto yesNoNode = builder.add_message(yesNoMsg.id());
    yesNoNode.next(confirmSelect.id());
    const auto confirmNode = builder.add_message(confirmMsg.id());
    confirmNode.next(yesNoNode.id());
    const auto confirmVertical = builder.add_event(FLOW_EVENT_SELECT_VERTICAL, {0, 0, 0, 3});
    confirmVertical.next(confirmNode.id());
    const auto menuSelect = builder.add_branch(FLOW_QUERY_SELECT_2_CANCEL, 0);
    menuSelect.results({confirmVertical.id(), mods::flow::kEnd, mods::flow::kEnd});
    const auto menuNode = builder.add_message(menuMsg.id());
    menuNode.next(menuSelect.id());
    const auto askNode = builder.add_message(askMsg.id());
    askNode.next(menuNode.id());
    const auto vertical = builder.add_event(FLOW_EVENT_SELECT_VERTICAL, {0, 0, 0, 3});
    vertical.next(askNode.id());
    auto graph = builder.commit();
    if (!graph) {
        mods::log::warn("own-wording Midna menu graph failed (result {})", static_cast<int>(graph.result()));
        return false;
    }
    g_flowStarts[kFlowMidna] = vertical.id();
    g_graphs.push_back(std::move(graph));
    g_registered.push_back(std::move(askMsg));
    g_registered.push_back(std::move(menuMsg));
    g_registered.push_back(std::move(confirmMsg));
    g_registered.push_back(std::move(yesNoMsg));

    mods::flow::MessageBuilder arrival{mods::flow::MessageStyle{}
                                           .speaker(0x42)
                                           .box_kind(MESSAGE_BOX_LIGHT_SPIRIT)
                                           .draw_type(MESSAGE_DRAW_TYPED_SLOW)};
    arrival.text("Prove your strength in wolf form...\nDescend, floor by floor, as far as you can.");
    auto arrivalMsg = register_built(kGroup, arrival);
    if (arrivalMsg) {
        g_fallbackArrival = arrivalMsg.id();
        g_registered.push_back(std::move(arrivalMsg));
    }
    // In place of HD's 5062 (flow 1120's turn-back while Midna is gravely ill; fix L2).
    mods::flow::MessageBuilder turnBack{mods::flow::MessageStyle{}
                                            .speaker(0x42)
                                            .box_kind(MESSAGE_BOX_LIGHT_SPIRIT)
                                            .draw_type(MESSAGE_DRAW_TYPED_SLOW)};
    turnBack.text("Your path lies elsewhere for now...\nReturn to the world above.");
    auto turnBackMsg = register_built(kGroup, turnBack);
    if (turnBackMsg) {
        g_fallbackTurnBack = turnBackMsg.id();
        g_registered.push_back(std::move(turnBackMsg));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// HD install
// ---------------------------------------------------------------------------------------------
void load_resources() {
    // Again on every initialize() (the HD Features switch): start from nothing.
    g_texts.clear();
    g_flows.clear();
    g_textLoaded = false;
    g_loadProblem.clear();
    std::vector<std::uint8_t> bytes;
    std::string why;
    if (!load_file("cave/hdtext/messages.bin", bytes)) {
        g_loadProblem = hdfiles::problem("cave/hdtext/messages.bin");
        return;
    }
    if (!hd::parse_messages_bin(bytes, g_texts, &why)) {
        g_texts.clear();
        g_loadProblem = "res/cave/hdtext/messages.bin: " + why;
        return;
    }
    g_textLoaded = true;
    if (!load_file("cave/hdtext/flows.json", bytes)) {
        g_loadProblem = hdfiles::problem("cave/hdtext/flows.json");
        return;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!hd::parse_flows_json(text, g_flows, &why)) {
        g_flows.clear();
        g_loadProblem = why;
        return;
    }
    for (const FlowDef& flow : g_flows) {
        if (!hd::validate_flow(flow, &why)) {
            g_flows.clear();
            g_loadProblem = "flows.json: " + why;
            return;
        }
    }
}

void register_flow_messages() {
    for (const MessageRec& rec : g_texts) {
        if (rec.action != Action::Register || rec.group != kGroup) {
            continue;
        }
        auto message = register_raw(rec.group, rec.entry, rec.text);
        if (!message) {
            mods::log::warn("HD message zel_05:{} not registered (result {})", rec.id,
                static_cast<int>(message.result()));
            continue;
        }
        g_registeredIds[{rec.file, rec.id}] = message.id();
        g_registered.push_back(std::move(message));
    }
}

// Overrides only for HD messages the installed flows show through a GC index whose GC text differs.
void install_used_overrides() {
    for (const FlowDef& flow : g_flows) {
        for (const FlowNode& node : flow.nodes) {
            if (node.type != NodeType::Message || node.msg_action != Action::Override) {
                continue;
            }
            const MessageRec* rec = find_text(node.msg_file, node.msg_id);
            if (rec == nullptr) {
                continue;
            }
            for (const std::uint8_t language : kLanguages) {
                auto handle = mods::flow::override_message(
                    rec->group, rec->id, static_cast<MessageLanguage>(language), rec->text);
                if (handle) {
                    g_overrides.push_back(std::move(handle));
                }
            }
        }
    }
}

bool build_hd_graphs() {
    mods::flow::GraphBuilder builder{kGroup};
    std::map<std::uint16_t, std::uint16_t> starts;
    for (const FlowDef& flow : g_flows) {
        if (flow.group != kGroup ||
            (flow.flow_id != kFlowMidna && flow.flow_id != kFlowArrival && flow.flow_id != kFlowSectionEnd))
        {
            continue;  // 1122 is replaced by add_reward_flow (see there)
        }
        std::vector<PendingNode> nodes;
        std::string why;
        if (!translate(flow, nodes, why)) {
            mods::log::warn("HD flow {} skipped: {}", flow.flow_id, why);
            continue;
        }
        const std::uint16_t start = add_translated(builder, flow, nodes);
        if (start != 0) {
            starts[flow.flow_id] = start;
        }
    }
    std::uint16_t rewardStart = 0;
    if (add_reward_flow(builder, rewardStart)) {
        starts[kFlowReward] = rewardStart;
    } else {
        mods::log::warn("reward talk not built (HD lines missing); the floor-41 reward uses notices");
    }
    if (starts.empty()) {
        return false;
    }
    auto graph = builder.commit();
    if (!graph) {
        mods::log::warn("HD flow graph commit failed (result {})", static_cast<int>(graph.result()));
        return false;
    }
    g_graphs.push_back(std::move(graph));
    g_flowStarts = std::move(starts);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Tag_ms stage edits: HD starts flow 1121 from Tag_ms actors on floors 5 (room 10) and 19 (room 48);
// the asset pipeline removes them (a missing flow), so they come back here, pointing at the mod's
// graphs. Their positions and sizes are HD's: the converter copies HD's two records from the player's own
// HD copy into res/cave/hd_tags.bin (tools/pipeline/cavebuild.py hd_tag_records), read below; without
// that file these two talks are left out. The switches (cos_sections_spec.md M8, VERIFIED:
// d_a_tag_msg.cpp getParam reads on = angle.x low byte, off = high byte; HD R10 angle.x 0x8AC3, R48
// 0xB0C3): on C3, off 8A (floor 5) / B0 (floor 19). C3 is set by the floor's swc00 type-11 area once
// the tier ALLdie chose its "locked" switch C2 (hooks.cpp pre_alldieActionTimer); the talk's end then
// sets the off switch, the floor's own gate switch (0x80 + room), so the gate opens after the voice has
// spoken. While the next section is unlocked the ALLdie sets 8A/B0 directly: no talk. The floor-41 tag
// carries the reward talk (flow id 1122, HD's Great Fairy talk).
//
// Tag size: daTag_Msg_c multiplies the record's scale bytes by 10 (fopAc create) and then by 100
// (d_a_tag_msg.cpp:224-225), so sx/sz 40 = an XZ radius of 400 and sy 25 = +-250 in height. The floor-41
// tag is centred in the arrival corridor (z 2700, between the arrival switch area at z 2381 and the
// l8warp at z 3776; the corridor's torches stand at x -4659 / -3542, i.e. about 1,120 wide). FIX (review
// "cave entry" #3): sx 60 (radius 600) so the whole corridor width triggers it; the l8warp stays 1,076
// away, outside the radius.
// ---------------------------------------------------------------------------------------------
struct TagSpec {
    std::uint8_t room;
    std::uint16_t flow;
    float x, y, z;
    std::uint8_t onSwitch, offSwitch;
    std::uint8_t sx, sy, sz;
    bool fromHd;  // place and size from HD's record of this room (res/cave/hd_tags.bin), not the fields above
};

constexpr TagSpec kTags[] = {
    {10, kFlowSectionEnd, 0.0f, 0.0f, 0.0f, 0xC3, 0x8A, 0, 0, 0, true},    // floor 5 (HD angle.x 0x8AC3)
    {48, kFlowSectionEnd, 0.0f, 0.0f, 0.0f, 0xC3, 0xB0, 0, 0, 0, true},    // floor 19 (HD angle.x 0xB0C3)
    {14, kFlowReward, -4100.0f, -48780.0f, 2700.0f, 0xFF, 0xC7, 60, 25, 60, false},  // floor 41 entrance (own)
};

// res/cave/hd_tags.bin: 'HDFT' u8 version=1 u8 count u16 0, then per tag u8 room, u8[3] 0, u8 record[0x24]
// (HD's SCOB record: name[8], parameters, position, angle, set id, scale x/y/z, pad).
constexpr char kHdTagsPath[] = "cave/hd_tags.bin";
constexpr std::size_t kHdTagEntrySize = 4 + 0x24;

std::uint32_t get_be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
        std::uint32_t{p[3]};
}

float get_bef(const std::uint8_t* p) {
    const std::uint32_t bits = get_be32(p);
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// The tag with HD's place and size for this room, or nullopt (file missing, or HD's record is not the
// Tag_ms with these switches).
std::optional<TagSpec> hd_tag(const TagSpec& spec, const std::vector<std::uint8_t>& file) {
    if (file.size() < 8 || std::memcmp(file.data(), "HDFT", 4) != 0 || file[4] != 1 ||
        file.size() != 8 + std::size_t{file[5]} * kHdTagEntrySize)
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < file[5]; ++i) {
        const std::uint8_t* e = file.data() + 8 + i * kHdTagEntrySize;
        const std::uint8_t* rec = e + 4;
        if (e[0] != spec.room) {
            continue;
        }
        const std::uint16_t angleX = static_cast<std::uint16_t>((rec[0x18] << 8) | rec[0x19]);
        if (std::memcmp(rec, "Tag_ms\0", 7) != 0 ||
            angleX != static_cast<std::uint16_t>((spec.offSwitch << 8) | spec.onSwitch))
        {
            mods::log::warn("res/{}: room {} is not HD's Tag_ms with switches {:#04x}/{:#04x}; talk left out",
                kHdTagsPath, spec.room, spec.onSwitch, spec.offSwitch);
            return std::nullopt;
        }
        TagSpec tag = spec;
        tag.x = get_bef(rec + 0x0C);
        tag.y = get_bef(rec + 0x10);
        tag.z = get_bef(rec + 0x14);
        tag.sx = rec[0x20];
        tag.sy = rec[0x21];
        tag.sz = rec[0x22];
        tag.fromHd = false;
        return tag;
    }
    return std::nullopt;
}

void put_be16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

void put_bef(std::uint8_t* p, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_be32(p, bits);
}

// stage_tgsc_data_class (SCOB record, big-endian like the .dzr data). FIX (review "cave entry" #1, HIGH):
// StageService accepts only 0x20 (ACTR) or 0x23 (TGSC/SCOB) bytes (dusk/mods/svc/stage.cpp kTgscEntrySize,
// is_valid_record_size); a 0x24-byte record made add_actor fail with MOD_INVALID_ARGUMENT. The struct's
// 0x24th byte is alignment padding after the three scale bytes: the host copies the 0x23 bytes into a
// zeroed stage_tgsc_data_class and reads the scale when size > 0x20 (d_stage.cpp
// dusk_stage_svc_new_actor_create).
constexpr std::size_t kTgscRecordSize = 0x23;
std::array<std::uint8_t, kTgscRecordSize> tag_record(const TagSpec& tag) {
    std::array<std::uint8_t, kTgscRecordSize> rec{};
    std::memcpy(rec.data(), "Tag_ms", 6);
    // field_0x5dd (order the "SPEAK" event) | save labels 0xFFF/0xFFF (unused in switch mode).
    put_be32(rec.data() + 0x08, 0x02FFFFFFu);
    put_bef(rec.data() + 0x0C, tag.x);
    put_bef(rec.data() + 0x10, tag.y);
    put_bef(rec.data() + 0x14, tag.z);
    put_be16(rec.data() + 0x18, static_cast<std::uint16_t>((tag.offSwitch << 8) | tag.onSwitch));
    put_be16(rec.data() + 0x1A, 0x8000);  // angle.y (unused with field_0x5dd)
    put_be16(rec.data() + 0x1C, tag.flow);
    put_be16(rec.data() + 0x1E, 0xFFFF);  // setID
    rec[0x20] = tag.sx;
    rec[0x21] = tag.sy;
    rec[0x22] = tag.sz;
    return rec;
}

void register_stage_tags() {
    if (svc_stage == nullptr) {
        mods::log::warn("StageService unavailable: no section-end/reward talks");
        return;
    }
    if (!cave::feature_ok(cave::Feature::FlowRemap)) {
        // Without the dMsgFlow_c::init hook a tag would start a flow id GC does not have (an empty talk).
        mods::log::warn("flow hook missing: no section-end/reward talks");
        return;
    }
    std::vector<std::uint8_t> hdTags;
    if (!load_file(kHdTagsPath, hdTags)) {
        mods::log::warn("{}: no section-end talks on floors 5 and 19", hdfiles::problem(kHdTagsPath));
    }
    for (const TagSpec& spec : kTags) {
        if (flow_start(spec.flow) == 0) {
            continue;
        }
        std::optional<TagSpec> placed = spec;
        if (spec.fromHd) {
            placed = hdTags.empty() ? std::nullopt : hd_tag(spec, hdTags);
            if (!placed) {
                if (!hdTags.empty()) {
                    mods::log::warn("res/{}: no usable record for D_SB11 room {}; talk left out", kHdTagsPath,
                        spec.room);
                }
                continue;
            }
        }
        const TagSpec& tag = *placed;
        const auto record = tag_record(tag);
        StageActorHandle handle = 0;
        const ModResult result = svc_stage->add_actor(
            mod_ctx, kStageName, tag.room, -1, record.data(), record.size(), &handle);
        if (result == MOD_OK) {
            g_stageEdits.push_back(handle);
            if (std::find(g_tagFlows.begin(), g_tagFlows.end(), tag.flow) == g_tagFlows.end()) {
                g_tagFlows.push_back(tag.flow);
            }
            g_tagRooms.emplace_back(tag.flow, static_cast<int>(tag.room));
        } else {
            mods::log::warn("could not add the flow {} tag to D_SB11 room {} (result {})", tag.flow,
                tag.room, static_cast<int>(result));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Flow 1120 evaluation for the cutscene message
// ---------------------------------------------------------------------------------------------
std::uint16_t evaluate_branch(const FlowNode& node, void*) {
    if (node.query == kHdQuery055) {
        return cave::intro_query();
    }
    if (node.query == FLOW_QUERY_EVENT_FLAG) {
        return game_label(node.param) ? 0 : 1;
    }
    if (node.query == kHdQuery054) {
        return 1;
    }
    mods::log::warn("flow 1120: branch query {} evaluated as 0", node.query);
    return 0;
}

void run_event(const FlowNode& node) {
    const std::uint16_t p0 = be16(node.params.data());
    const std::uint16_t p1 = be16(node.params.data() + 2);
    switch (node.event) {
    case FLOW_EVENT_SET_EVENT_FLAG:
        set_game_label(p0, true);
        set_game_label(p1, true);
        break;
    case FLOW_EVENT_CLEAR_EVENT_FLAG:
        set_game_label(p0, false);
        set_game_label(p1, false);
        break;
    case kHdEvent044:
        // Flow 1120 node 1645 comes after MSG 5062, but this walk runs before 5062 is shown: the Withdraw waits
        // for the cutscene (and that message) to end (cave.cpp flow_withdraw, the pending Withdraw).
        cave::flow_withdraw(true);
        break;
    case kHdEvent043:
    case kHdEvent045:
        break;
    default:
        mods::log::warn("flow 1120: event {} ignored in the cutscene", node.event);
        break;
    }
}

}  // namespace

// =================================================================================================
// Public API
// =================================================================================================
void initialize() {
    shutdown();  // the switch may turn the cave on again: nothing of a previous install may stay
    load_resources();
    if (!g_loadProblem.empty()) {
        mods::log::warn("HD text: {}; using the mod's own wording", g_loadProblem);
    }
    if (!register_callbacks()) {
        return;
    }
    if (g_textLoaded && !g_flows.empty()) {
        register_flow_messages();
        install_used_overrides();
        g_hdFlows = build_hd_graphs();
        if (!g_hdFlows) {
            mods::log::warn("HD flows could not be installed; using the mod's own Midna menu");
        }
    }
    if (!g_hdFlows) {
        build_fallback();
    }
    register_stage_tags();
    mods::log::info("{}", summary());
}

void shutdown() {
    if (svc_stage != nullptr) {
        for (const StageActorHandle handle : g_stageEdits) {
            svc_stage->remove_actor_edit(mod_ctx, handle);
        }
    }
    g_stageEdits.clear();
    g_tagFlows.clear();
    g_tagRooms.clear();
    g_graphs.clear();
    g_overrides.clear();
    g_registered.clear();
    g_registeredIds.clear();
    g_flowStarts.clear();
    g_fallbackArrival = 0;
    g_fallbackTurnBack = 0;
    g_hdFlows = false;
}

bool text_loaded() {
    return g_textLoaded;
}

bool hd_flows() {
    return g_hdFlows;
}

std::string text(std::uint8_t file, std::uint16_t id) {
    const MessageRec* rec = find_text(file, id);
    return rec != nullptr ? hd::to_plain(rec->text, "Link") : std::string{};
}

std::string text_or(std::uint8_t file, std::uint16_t id, const char* fallback) {
    std::string value = text(file, id);
    if (value.empty() && fallback != nullptr) {
        value = fallback;
    }
    return value;
}

std::uint16_t flow_start(std::uint16_t flowId) {
    const auto found = g_flowStarts.find(flowId);
    return found != g_flowStarts.end() ? found->second : 0;
}

bool talk_tag(std::uint16_t flowId) {
    return std::find(g_tagFlows.begin(), g_tagFlows.end(), flowId) != g_tagFlows.end();
}

bool talk_tag_in_room(std::uint16_t flowId, int room) {
    return std::find(g_tagRooms.begin(), g_tagRooms.end(), std::make_pair(flowId, room)) != g_tagRooms.end();
}

std::uint16_t arrival_message() {
    if (g_hdFlows) {
        for (const FlowDef& flow : g_flows) {
            if (flow.flow_id != kFlowArrival) {
                continue;
            }
            const hd::WalkResult walk = hd::walk_flow(flow, evaluate_branch, nullptr);
            if (!walk.ok) {
                mods::log::warn("flow 1120 walk failed; no arrival message");
                break;
            }
            for (const FlowNode* event : walk.events) {
                run_event(*event);
            }
            if (walk.message == nullptr) {
                return 0;
            }
            if (walk.message->msg_action == Action::Register) {
                const auto found = g_registeredIds.find({walk.message->msg_file, walk.message->msg_id});
                return found != g_registeredIds.end() ? found->second : 0;
            }
            return walk.message->msg_id;  // native / override: the GC id is the HD id
        }
    }
    // Own wording (or a broken 1120), following flow 1120's two paths. Turn-back (nodes 1640/1641 -> 5062 -> event044):
    // HD lets the amiibo take you in while Midna is gravely ill and sends you straight back from floor 1; that path
    // never reaches node 1638, so label 829 stays as it is (fix I3). Otherwise one message, then 1638 clears 829.
    if (cave::midna_gravely_ill()) {
        cave::flow_withdraw(true);
        return g_fallbackTurnBack;
    }
    cave::set_hd_label(829, false);
    return g_fallbackArrival;
}

std::string summary() {
    std::string text;
    if (g_hdFlows) {
        text = "HD text: " + std::to_string(g_texts.size()) + " messages, flows";
        for (const auto& [id, start] : g_flowStarts) {
            text += " " + std::to_string(id);
        }
        text += " installed, " + std::to_string(g_stageEdits.size()) + " talk tag(s)";
    } else if (!g_flowStarts.empty()) {
        text = "HD text unavailable (" + (g_loadProblem.empty() ? std::string("install failed") : g_loadProblem) +
               "): own-wording Midna menu";
    } else {
        text = "HD text and flows unavailable (" + g_loadProblem + ")";
    }
    return text;
}

}  // namespace cave::hdtext
