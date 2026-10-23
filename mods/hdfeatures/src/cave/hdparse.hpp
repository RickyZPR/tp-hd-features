#pragma once

// Cave of Shadows: game-independent parsing of the HD text/flow resources that the mod makes from the player's own
// TP HD copy (C++ port of tools/build_assets.py) into its stored res/cave/hdtext/ (never distributed).
//
// Nothing in here touches the game or a mod service, so it can be unit-tested on its own (the static
// verification harness compiles this file to WebAssembly and runs it against the generated files).
//
//   res/cave/hdtext/messages.bin  'HDTX' u32 version=1 u32 count, then per record (4-byte aligned):
//                            u8 file u8 group u16 id u8 action u8 language u16 text_size u8 entry[20]
//                            u8 text[text_size]                         (all little-endian)
//   res/cave/hdtext/flows.json    [ {flow_id, group, start, nodes: [...]}, ... ]   (schema: hdtext.py)

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cave::hd {

// ------------------------------------------------------------------------------------------------
// Minimal JSON reader (RFC 8259 subset: everything flows.json uses; \u escapes become UTF-8).
// ------------------------------------------------------------------------------------------------
namespace json {

struct Value {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;                              // Array
    std::vector<std::pair<std::string, Value>> members;    // Object (document order)

    bool is_null() const { return kind == Kind::Null; }
    bool is_number() const { return kind == Kind::Number; }
    bool is_string() const { return kind == Kind::String; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_object() const { return kind == Kind::Object; }
    // Object lookup; nullptr when absent or not an object.
    const Value* find(std::string_view key) const;
};

// Parses a whole document. On failure returns false and, when `error` is set, a short reason.
bool parse(std::string_view text, Value& out, std::string* error = nullptr);

}  // namespace json

// ------------------------------------------------------------------------------------------------
// messages.bin
// ------------------------------------------------------------------------------------------------
enum class Action : std::uint8_t { Native = 0, Override = 1, Register = 2 };

struct MessageRec {
    std::uint8_t file = 0;       // 0 = zel_00, 4 = zel_04, 5 = zel_05
    std::uint8_t group = 0;      // MessageService / FlowService group
    std::uint16_t id = 0;        // HD message id
    Action action = Action::Native;
    std::uint8_t language = 0;   // MessageLanguage (0 English)
    std::array<std::uint8_t, 20> entry{};  // INF1 entry, offset + id zeroed (register_message)
    std::vector<std::uint8_t> text;        // GC-encoded DAT1 text incl. the terminating NUL
};

bool parse_messages_bin(std::span<const std::uint8_t> data, std::vector<MessageRec>& out,
    std::string* error = nullptr);

// Plain UTF-8 rendering of a GC-encoded message text for RmlUi / toasts / logs: control tags are
// dropped, except the player-name tag which becomes `playerName`; '\n' is kept; bytes >= 0x80 are
// decoded as Windows-1252 (the extractor already reduced the set to ASCII, VERIFIED in its report).
std::string to_plain(std::span<const std::uint8_t> text, std::string_view playerName = "Link");

// True when `text` is a valid GC-encoded message: tags well-formed, exactly one terminating NUL at
// the end (the same rules as the host's register_message validation, src/dusk/mods/svc/flow.cpp).
bool valid_encoded_text(std::span<const std::uint8_t> text);

// ------------------------------------------------------------------------------------------------
// flows.json
// ------------------------------------------------------------------------------------------------
inline constexpr std::uint16_t kEnd = 0xFFFF;

enum class NodeType : std::uint8_t { Message, Branch, Event };

struct FlowNode {
    std::uint16_t n = 0;          // HD node index (unique within flows.json)
    NodeType type = NodeType::Message;
    // message
    std::uint8_t subtype = 0;
    std::uint16_t msg_id = 0;     // HD message id
    std::uint8_t msg_file = 0;    // 0/4/5 from the "zel_NN:id" key
    Action msg_action = Action::Native;
    std::int32_t gc_index = -1;   // GC INF1 index for native/override messages, -1 when null
    std::uint16_t unknown = 0;    // FLW1 node bytes 6-7
    // branch
    std::uint16_t query = 0;      // dMsgFlow_c query table index (HD numbering, == FlowGameQuery)
    std::uint16_t param = 0;
    std::vector<std::uint16_t> results;  // node indices, kEnd for END
    // event
    std::uint8_t event = 0;       // dMsgFlow_c event table index (HD numbering, == FlowGameEvent)
    std::array<std::uint8_t, 4> params{};
    // message / event
    std::uint16_t next = kEnd;
    bool hd_only = false;
};

struct FlowDef {
    std::uint16_t flow_id = 0;
    std::uint16_t group = 0;
    std::uint16_t start = kEnd;
    std::vector<FlowNode> nodes;

    const FlowNode* node(std::uint16_t n) const;
};

bool parse_flows_json(std::string_view text, std::vector<FlowDef>& out, std::string* error = nullptr);

// Structural check: every target (next/results) is END or a node of the same flow, the start node
// exists, branches have at least one result, and message nodes name a usable message
// (gc_index for native/override, or a "register" message). Returns false with a reason otherwise.
bool validate_flow(const FlowDef& flow, std::string* error = nullptr);

// ------------------------------------------------------------------------------------------------
// Static walk of a flow, used for the arrival intro (flow 1120), which HD starts from the
// stage's cutscene instead of a talk: branches are evaluated with `evaluate`, events are collected,
// and the walk stops at the first message node (the one message the cutscene shows) or at END.
// The events after that message are collected too (they run once the message is up).
// ------------------------------------------------------------------------------------------------
struct WalkResult {
    const FlowNode* message = nullptr;          // first message node reached, or nullptr
    std::vector<const FlowNode*> events;        // event nodes on the path, in order
    bool ok = false;                            // false: broken graph or step limit hit
};

using BranchEvaluator = std::uint16_t (*)(const FlowNode& branch, void* user);

WalkResult walk_flow(const FlowDef& flow, BranchEvaluator evaluate, void* user, int maxSteps = 64);

}  // namespace cave::hd
