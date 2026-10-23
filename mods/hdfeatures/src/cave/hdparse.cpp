// Cave of Shadows: game-independent parsing of res/cave/hdtext (see hdparse.hpp).

#include "hdparse.hpp"

#include <charconv>
#include <cstring>
#include <system_error>

namespace cave::hd {

// =================================================================================================
// JSON
// =================================================================================================
namespace json {

const Value* Value::find(std::string_view key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    for (const auto& [name, value] : members) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

namespace {

constexpr int kMaxDepth = 64;

struct Parser {
    std::string_view s;
    std::size_t pos = 0;
    std::string error;

    bool fail(const char* what) {
        if (error.empty()) {
            error = std::string(what) + " at offset " + std::to_string(pos);
        }
        return false;
    }

    void skip_ws() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
            ++pos;
        }
    }

    bool literal(std::string_view word) {
        if (s.substr(pos, word.size()) != word) {
            return fail("bad literal");
        }
        pos += word.size();
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(std::uint32_t& out) {
        if (pos + 4 > s.size()) {
            return fail("short \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("bad \\u escape");
            }
        }
        return true;
    }

    bool string(std::string& out) {
        if (pos >= s.size() || s[pos] != '"') {
            return fail("expected string");
        }
        ++pos;
        out.clear();
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos >= s.size()) {
                break;
            }
            const char e = s[pos++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t low = 0;
                    if (pos + 2 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u') {
                        return fail("unpaired surrogate");
                    }
                    pos += 2;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return fail("bad low surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default:
                return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool number(double& out) {
        const std::size_t start = pos;
        if (pos < s.size() && s[pos] == '-') {
            ++pos;
        }
        while (pos < s.size() &&
               ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' ||
                   s[pos] == '+' || s[pos] == '-'))
        {
            ++pos;
        }
        const char* first = s.data() + start;
        const char* last = s.data() + pos;
        const auto [ptr, ec] = std::from_chars(first, last, out);
        if (ec != std::errc{} || ptr != last || start == pos) {
            pos = start;
            return fail("bad number");
        }
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        skip_ws();
        if (pos >= s.size()) {
            return fail("unexpected end");
        }
        const char c = s[pos];
        if (c == 'n') {
            out.kind = Value::Kind::Null;
            return literal("null");
        }
        if (c == 't') {
            out.kind = Value::Kind::Bool;
            out.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            out.kind = Value::Kind::Bool;
            out.boolean = false;
            return literal("false");
        }
        if (c == '"') {
            out.kind = Value::Kind::String;
            return string(out.string);
        }
        if (c == '[') {
            ++pos;
            out.kind = Value::Kind::Array;
            skip_ws();
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
                return true;
            }
            while (true) {
                Value item;
                if (!value(item, depth + 1)) {
                    return false;
                }
                out.items.push_back(std::move(item));
                skip_ws();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == ']') {
                    ++pos;
                    return true;
                }
                return fail("expected , or ]");
            }
        }
        if (c == '{') {
            ++pos;
            out.kind = Value::Kind::Object;
            skip_ws();
            if (pos < s.size() && s[pos] == '}') {
                ++pos;
                return true;
            }
            while (true) {
                skip_ws();
                std::string key;
                if (!string(key)) {
                    return false;
                }
                skip_ws();
                if (pos >= s.size() || s[pos] != ':') {
                    return fail("expected :");
                }
                ++pos;
                Value member;
                if (!value(member, depth + 1)) {
                    return false;
                }
                out.members.emplace_back(std::move(key), std::move(member));
                skip_ws();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == '}') {
                    ++pos;
                    return true;
                }
                return fail("expected , or }");
            }
        }
        out.kind = Value::Kind::Number;
        return number(out.number);
    }
};

}  // namespace

bool parse(std::string_view text, Value& out, std::string* error) {
    Parser parser;
    parser.s = text;
    // Tolerate a UTF-8 byte order mark.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        parser.pos = 3;
    }
    out = Value{};
    bool ok = parser.value(out, 0);
    if (ok) {
        parser.skip_ws();
        if (parser.pos != text.size()) {
            ok = parser.fail("trailing data");
        }
    }
    if (!ok && error != nullptr) {
        *error = parser.error;
    }
    return ok;
}

}  // namespace json

// =================================================================================================
// messages.bin
// =================================================================================================
namespace {

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool set_error(std::string* error, const std::string& text) {
    if (error != nullptr) {
        *error = text;
    }
    return false;
}

}  // namespace

bool valid_encoded_text(std::span<const std::uint8_t> text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (text[offset] == 0) {
            return offset + 1 == text.size();
        }
        if (text[offset] == 0x1A) {
            if (offset + 2 > text.size()) {
                return false;
            }
            const std::size_t tagSize = text[offset + 1];
            if (tagSize < 5 || tagSize > text.size() - offset) {
                return false;
            }
            // MSGTAG_COLOR (group 0xFF, type 0) takes 1, 4 or 8 argument bytes.
            if (text[offset + 2] == 0xFF && text[offset + 3] == 0 && text[offset + 4] == 0) {
                const std::size_t args = tagSize - 5;
                if (args != 1 && args != 4 && args != 8) {
                    return false;
                }
            }
            offset += tagSize;
        } else {
            ++offset;
        }
    }
    return false;
}

bool parse_messages_bin(std::span<const std::uint8_t> data, std::vector<MessageRec>& out,
    std::string* error) {
    out.clear();
    if (data.size() < 12 || std::memcmp(data.data(), "HDTX", 4) != 0) {
        return set_error(error, "not an HDTX file");
    }
    const std::uint32_t version = le32(data.data() + 4);
    const std::uint32_t count = le32(data.data() + 8);
    if (version != 1) {
        return set_error(error, "unsupported HDTX version " + std::to_string(version));
    }
    if (count > 4096) {
        return set_error(error, "implausible record count " + std::to_string(count));
    }
    std::size_t offset = 12;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        constexpr std::size_t kHeader = 8 + 20;
        if (offset + kHeader > data.size()) {
            return set_error(error, "record " + std::to_string(i) + " header truncated");
        }
        const std::uint8_t* p = data.data() + offset;
        MessageRec rec;
        rec.file = p[0];
        rec.group = p[1];
        rec.id = le16(p + 2);
        if (p[4] > 2) {
            return set_error(error, "record " + std::to_string(i) + " has an unknown action");
        }
        rec.action = static_cast<Action>(p[4]);
        rec.language = p[5];
        const std::uint16_t textSize = le16(p + 6);
        std::memcpy(rec.entry.data(), p + 8, rec.entry.size());
        if (offset + kHeader + textSize > data.size()) {
            return set_error(error, "record " + std::to_string(i) + " text truncated");
        }
        rec.text.assign(p + kHeader, p + kHeader + textSize);
        if (!valid_encoded_text(rec.text)) {
            return set_error(error, "record " + std::to_string(i) + " (id " + std::to_string(rec.id) +
                                        ") has malformed text");
        }
        offset += kHeader + textSize;
        offset = (offset + 3) & ~static_cast<std::size_t>(3);
        out.push_back(std::move(rec));
    }
    return true;
}

std::string to_plain(std::span<const std::uint8_t> text, std::string_view playerName) {
    // Windows-1252 0x80-0x9F (0 = undefined -> '?').
    static constexpr std::uint16_t kCp1252High[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026,
        0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019,
        0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E,
        0x0178};
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::uint8_t c = text[i];
        if (c == 0) {
            break;
        }
        if (c == 0x1A) {
            if (i + 1 >= text.size() || text[i + 1] < 5 || i + text[i + 1] > text.size()) {
                break;
            }
            const std::uint8_t group = text[i + 2];
            const std::uint16_t type = static_cast<std::uint16_t>((text[i + 3] << 8) | text[i + 4]);
            if (group == 0 && type == 0) {
                out.append(playerName);
            }
            i += text[i + 1];
            continue;
        }
        if (c < 0x80) {
            if (c == '\n' || c >= 0x20) {
                out.push_back(static_cast<char>(c));
            }
        } else {
            std::uint32_t cp = c;
            if (c < 0xA0) {
                cp = kCp1252High[c - 0x80];
                if (cp == 0) {
                    cp = '?';
                }
            }
            if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        ++i;
    }
    // Trailing blank lines (HD pads some texts with "\n\n") are not wanted outside a message box.
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

// =================================================================================================
// flows.json
// =================================================================================================
const FlowNode* FlowDef::node(std::uint16_t n) const {
    for (const FlowNode& candidate : nodes) {
        if (candidate.n == n) {
            return &candidate;
        }
    }
    return nullptr;
}

namespace {

bool get_u16(const json::Value& obj, std::string_view key, std::uint16_t& out, bool nullIsEnd = false) {
    const json::Value* v = obj.find(key);
    if (v == nullptr) {
        return false;
    }
    if (v->is_null()) {
        if (!nullIsEnd) {
            return false;
        }
        out = kEnd;
        return true;
    }
    if (!v->is_number() || v->number < 0 || v->number > 65535 ||
        static_cast<double>(static_cast<std::uint32_t>(v->number)) != v->number)
    {
        return false;
    }
    out = static_cast<std::uint16_t>(v->number);
    return true;
}

bool get_bool(const json::Value& obj, std::string_view key) {
    const json::Value* v = obj.find(key);
    return v != nullptr && v->kind == json::Value::Kind::Bool && v->boolean;
}

bool parse_action(std::string_view text, Action& out) {
    if (text == "native") {
        out = Action::Native;
    } else if (text == "override") {
        out = Action::Override;
    } else if (text == "register") {
        out = Action::Register;
    } else {
        return false;
    }
    return true;
}

bool parse_hex8(std::string_view text, std::array<std::uint8_t, 4>& out) {
    if (text.size() != 8) {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, value, 16);
        if (ec != std::errc{} || ptr != text.data() + i * 2 + 2) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

bool parse_node(const json::Value& v, FlowNode& node, std::string& why) {
    if (!v.is_object()) {
        why = "node is not an object";
        return false;
    }
    if (!get_u16(v, "n", node.n)) {
        why = "node without n";
        return false;
    }
    const json::Value* type = v.find("type");
    if (type == nullptr || !type->is_string()) {
        why = "node " + std::to_string(node.n) + " without type";
        return false;
    }
    node.hd_only = get_bool(v, "hd_only");
    if (type->string == "message") {
        node.type = NodeType::Message;
        std::uint16_t subtype = 0;
        get_u16(v, "subtype", subtype);
        node.subtype = static_cast<std::uint8_t>(subtype);
        get_u16(v, "unknown", node.unknown);
        if (!get_u16(v, "msg_id", node.msg_id) || !get_u16(v, "next", node.next, true)) {
            why = "message node " + std::to_string(node.n) + " incomplete";
            return false;
        }
        const json::Value* key = v.find("message");
        if (key == nullptr || !key->is_string() || key->string.size() < 6 ||
            key->string.compare(0, 4, "zel_") != 0)
        {
            why = "message node " + std::to_string(node.n) + " without a message key";
            return false;
        }
        node.msg_file = static_cast<std::uint8_t>((key->string[4] - '0') * 10 + (key->string[5] - '0'));
        const json::Value* action = v.find("action");
        if (action == nullptr || !action->is_string() || !parse_action(action->string, node.msg_action)) {
            why = "message node " + std::to_string(node.n) + " has no usable action";
            return false;
        }
        const json::Value* gc = v.find("gc_index");
        if (gc != nullptr && gc->is_number()) {
            node.gc_index = static_cast<std::int32_t>(gc->number);
        }
        return true;
    }
    if (type->string == "branch") {
        node.type = NodeType::Branch;
        if (!get_u16(v, "query", node.query) || !get_u16(v, "param", node.param)) {
            why = "branch node " + std::to_string(node.n) + " incomplete";
            return false;
        }
        const json::Value* results = v.find("results");
        if (results == nullptr || !results->is_array() || results->items.empty() ||
            results->items.size() > 255)
        {
            why = "branch node " + std::to_string(node.n) + " without results";
            return false;
        }
        for (const json::Value& r : results->items) {
            if (r.is_null()) {
                node.results.push_back(kEnd);
            } else if (r.is_number() && r.number >= 0 && r.number < 65535) {
                node.results.push_back(static_cast<std::uint16_t>(r.number));
            } else {
                why = "branch node " + std::to_string(node.n) + " has a bad result";
                return false;
            }
        }
        return true;
    }
    if (type->string == "event") {
        node.type = NodeType::Event;
        std::uint16_t event = 0;
        const json::Value* params = v.find("params");
        if (!get_u16(v, "event", event) || event > 255 || params == nullptr || !params->is_string() ||
            !parse_hex8(params->string, node.params) || !get_u16(v, "next", node.next, true))
        {
            why = "event node " + std::to_string(node.n) + " incomplete";
            return false;
        }
        node.event = static_cast<std::uint8_t>(event);
        return true;
    }
    why = "node " + std::to_string(node.n) + " has unknown type " + type->string;
    return false;
}

}  // namespace

bool parse_flows_json(std::string_view text, std::vector<FlowDef>& out, std::string* error) {
    out.clear();
    json::Value root;
    std::string why;
    if (!json::parse(text, root, &why)) {
        return set_error(error, "flows.json: " + why);
    }
    if (!root.is_array()) {
        return set_error(error, "flows.json: top level is not an array");
    }
    for (const json::Value& f : root.items) {
        FlowDef flow;
        if (!f.is_object() || !get_u16(f, "flow_id", flow.flow_id) || !get_u16(f, "group", flow.group) ||
            !get_u16(f, "start", flow.start))
        {
            return set_error(error, "flows.json: flow without flow_id/group/start");
        }
        const json::Value* nodes = f.find("nodes");
        if (nodes == nullptr || !nodes->is_array()) {
            return set_error(error, "flows.json: flow " + std::to_string(flow.flow_id) + " has no nodes");
        }
        for (const json::Value& n : nodes->items) {
            FlowNode node;
            if (!parse_node(n, node, why)) {
                return set_error(error, "flows.json: flow " + std::to_string(flow.flow_id) + ": " + why);
            }
            if (flow.node(node.n) != nullptr) {
                return set_error(error, "flows.json: flow " + std::to_string(flow.flow_id) +
                                            " repeats node " + std::to_string(node.n));
            }
            flow.nodes.push_back(std::move(node));
        }
        out.push_back(std::move(flow));
    }
    return true;
}

bool validate_flow(const FlowDef& flow, std::string* error) {
    const auto target_ok = [&](std::uint16_t t) { return t == kEnd || flow.node(t) != nullptr; };
    if (flow.node(flow.start) == nullptr) {
        return set_error(error, "flow " + std::to_string(flow.flow_id) + ": start node missing");
    }
    for (const FlowNode& node : flow.nodes) {
        const std::string where = "flow " + std::to_string(flow.flow_id) + " node " + std::to_string(node.n);
        switch (node.type) {
        case NodeType::Message:
            if (!target_ok(node.next)) {
                return set_error(error, where + ": dangling next");
            }
            if (node.msg_action != Action::Register && node.gc_index < 0) {
                return set_error(error, where + ": native/override message without a GC index");
            }
            break;
        case NodeType::Branch:
            if (node.results.empty()) {
                return set_error(error, where + ": branch without results");
            }
            for (std::uint16_t r : node.results) {
                if (!target_ok(r)) {
                    return set_error(error, where + ": dangling result");
                }
            }
            break;
        case NodeType::Event:
            if (!target_ok(node.next)) {
                return set_error(error, where + ": dangling next");
            }
            break;
        }
    }
    return true;
}

WalkResult walk_flow(const FlowDef& flow, BranchEvaluator evaluate, void* user, int maxSteps) {
    WalkResult result;
    std::uint16_t at = flow.start;
    bool afterMessage = false;
    for (int step = 0; step < maxSteps; ++step) {
        if (at == kEnd) {
            result.ok = true;
            return result;
        }
        const FlowNode* node = flow.node(at);
        if (node == nullptr) {
            return result;
        }
        switch (node->type) {
        case NodeType::Message:
            if (afterMessage) {
                // A second message: the cutscene can only show one; stop before it.
                result.ok = true;
                return result;
            }
            result.message = node;
            afterMessage = true;
            at = node->next;
            break;
        case NodeType::Branch: {
            std::uint16_t r = evaluate != nullptr ? evaluate(*node, user) : 0;
            if (r >= node->results.size()) {
                r = static_cast<std::uint16_t>(node->results.size() - 1);
            }
            at = node->results[r];
            break;
        }
        case NodeType::Event:
            result.events.push_back(node);
            at = node->next;
            break;
        }
    }
    return result;
}

}  // namespace cave::hd
