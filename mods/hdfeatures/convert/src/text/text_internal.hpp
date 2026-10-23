// hdf_convert module TX (text): internal API shared by src/text/*.cpp and tests/text_test.cpp.
//
// A function-by-function port of tools/pipeline/hdtext.py (text side). The Python names are kept in the
// comments ("hdtext.<name>"). Number rules (plan 3.1): FontMetrics.width is a plain left-to-right double sum
// (hdtext.py:568), int(round(x)) is io::py_round_int, int(x * 0.99) truncates, sorts are stable, and dict order
// is kept with vectors.
#pragma once

#include "hdf/core.hpp"
#include "hdf/io.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdf::text::detail {

// ---------------------------------------------------------------------------------------------- BMG
// hdtext.Entry
struct Entry {
    u32 index = 0;
    u16 id = 0;
    std::array<u8, 20> entry{};  // the INF1 entry as stored
    Bytes text;                  // DAT1 text up to and including the NUL (hdtext.Bmg slicing rules)
};

// hdtext.Bmg: MESGbmg1 reader (INF1, DAT1, FLW1, FLI1).
struct Bmg {
    std::string name;
    u8 encoding = 0;
    std::vector<Entry> entries;
    std::map<u16, u32> by_id;                     // id -> entries index (setdefault: first wins)
    std::vector<std::array<u8, 8>> nodes;         // FLW1 nodes
    std::vector<u16> edges;                       // FLW1 edges
    std::vector<std::pair<u32, u16>> flows;       // FLI1: flow id (v >> 16) -> start node, insertion order
    std::map<u32, u16> flow_index;                // the same, for lookups

    const Entry* get(u16 id) const;               // by_id.get
    std::optional<u16> flow(u32 id) const;        // flows.get
};

Result<Bmg> parse_bmg(View buf, std::string name);              // hdtext.Bmg(buf, name)
Result<Bmg> load_bmg(View arc, std::string_view member, std::string name);  // hdtext.load_bmg (arc bytes)

// ---------------------------------------------------------------------------------------------- tokens
// ('c', byte) | ('t', group, id, args). Equality is Python's tuple equality.
struct Tok {
    bool tag = false;
    u8 c = 0;      // the character byte, or the tag group
    u16 id = 0;    // tag id
    Bytes args;    // tag arguments
    bool operator==(const Tok&) const = default;
    static Tok ch(u8 b) { return Tok{false, b, 0, {}}; }
    static Tok t(u8 g, u16 i, Bytes a = {}) { return Tok{true, g, i, std::move(a)}; }
    bool is_c(u8 b) const { return !tag && c == b; }
};
using Toks = std::vector<Tok>;
using Line = std::vector<Tok>;

Result<Toks> tokens(View text);       // hdtext.tokens (stops at the NUL; "truncated control tag")
Bytes encode(const Toks& toks);       // hdtext.encode (adds one NUL)
std::string show(View text);          // hdtext.show (diagnostics only)
bool gc_tag_known(u8 g, u16 i);       // hdtext.gc_tag_known
bool valid_encoded_text(View t);      // hdtext.valid_encoded_text

// ---------------------------------------------------------------------------------------------- conversion
// hdtext._ascii for every byte 0x80-0xFF (cp1252 'replace', NFKD, combining marks dropped, ASCII 'ignore',
// '?' if empty), precomputed with Python 3 unicodedata (table sha256 9f83d410...1332a6, see convert.cpp).
std::string_view ascii_fallback(u8 c);
// CP1252 0x80-0x9F as Unicode (src/stamps/hdtext.hpp:245; undefined bytes -> U+FFFD as Python's 'replace').
char32_t cp1252(u8 c);
struct Converted {
    Bytes text;                       // GC text bytes incl. NUL
    std::vector<std::string> notes;   // hdtext's conversion notes
};
Result<Converted> convert_text(View text, const std::set<u8>& gc_chars);  // hdtext.convert_text
Result<std::string> preview(View gc_text);                                 // hdtext.preview (UTF-8)
Result<Bytes> strip_revo(View text);                                       // hdtext._strip_revo

// ---------------------------------------------------------------------------------------------- line fitting
inline constexpr std::array<u8, 10> kWrapBoxKinds = {0, 1, 2, 5, 8, 9, 13, 14, 15, 16};  // WRAP_BOX_KINDS
inline constexpr std::size_t kLineMax = 4;   // LINE_MAX
inline constexpr int kNameChars = 8;         // NAME_CHARS
inline constexpr int kFontSizeX = 23;        // FONT_SIZE_X
inline constexpr int kCharSpace = 1;         // CHAR_SPACE
bool wrap_box_kind(u8 box);

// hdtext.FontMetrics over GC rodan_b_24_22.bfn.
struct FontMetrics {
    int cell = 22;
    int cell_w = 0;
    std::map<int, int> wid;  // glyph -> advance (WID1; later blocks overwrite)
    struct Map {
        int type = 0, first = 0, last = 0;
        std::vector<u16> table;
    };
    std::vector<Map> maps;
    double name = 0;
    std::array<double, 256> chars{};  // char(c) for every byte

    double char_w(int c) const;       // FontMetrics.char (computed, not cached)
    double tok(const Tok& t) const;   // FontMetrics.tok
    int width(const std::vector<Tok>& toks) const;  // FontMetrics.width: sequential double sum, py_round_int
};
Result<FontMetrics> font_metrics(View bfn);  // FontMetrics.__init__ (the .bfn bytes)

struct LineLimits {
    std::map<int, int> limit;    // box kind -> px
    std::map<int, int> samples;  // box kind -> GC messages measured (len(per[k]))
    int get(int box) const;      // limits.get(box, limits.get(0, 450))
};
LineLimits gc_line_limits(const std::vector<const Bmg*>& gc_bmgs, const FontMetrics& fm);  // hdtext.gc_line_limits

std::vector<Line> lines_of(const Toks& toks);   // hdtext._lines
bool visible(const Line& line);                 // hdtext._visible
Bytes vis_bytes(const Line& word);              // hdtext._vis_bytes
std::vector<Line> words_of(const Line& line);   // hdtext._words
Line join_words(const std::vector<Line>& words);  // hdtext._join_words
bool title_end(const Line& line);               // hdtext._title_end
bool sentence_end(const Line& line);            // hdtext._sentence_end

struct Rewrapped {
    Bytes text;
    std::optional<std::string> note;
};
Result<Rewrapped> rewrap(View text, int limit, const FontMetrics& fm);  // hdtext.rewrap

// ---------------------------------------------------------------------------------------------- messages
enum class Action : u8 { Native = 0, Override = 1, Register = 2 };  // ACTION_NO
std::string_view action_name(Action a);

// hdtext.Ctx
struct Ctx {
    std::set<u8> gc_chars;
    const FontMetrics* fm = nullptr;
    LineLimits limits;
};

// The fields of a build_message() dict that the kept outputs (messages.bin, flows.json) use.
struct Message {
    std::string file;           // "zel_00" | "zel_04" | "zel_05"
    u8 file_no = 0;             // FILE_NO
    u8 group = 0;
    u16 id = 0;
    u32 hd_index = 0;
    std::array<u8, 20> entry{};
    Bytes text;
    std::string preview;
    std::string use, category;
    Action action = Action::Register;
    std::optional<u32> gc_index;
    std::vector<std::string> notes;
    std::string key() const;    // "zel_05:5057"
};
// hdtext.build_message; nullopt when HD has no text for the id.
Result<std::optional<Message>> build_message(const Bmg& hd, const Bmg& gc, std::string_view fname, u16 mid,
                                             std::string_view category, std::string_view use, u8 group,
                                             const Ctx& ctx);

// ---------------------------------------------------------------------------------------------- flows
std::vector<u16> reachable(const Bmg& b, u32 start);                       // hdtext.reachable (visit order)
std::map<u32, std::vector<u32>> predecessors(const Bmg& b);                // hdtext.predecessors
std::vector<u32> neighbourhood(const Bmg& b, u32 node, int back = 6);      // hdtext.neighbourhood (sorted)
std::vector<u32> flows_reaching(const Bmg& b, u32 node);                   // hdtext.flows_reaching (sorted)
io::Json label_json(u32 idx);                                              // hdtext._label with facts.label_table()
// hdtext.decode_node; msg_ref(mid) returns the keys decode_node merges into a message node.
using MsgRef = std::function<Result<io::JsonObject>(std::optional<u16> mid)>;
Result<io::JsonObject> decode_node(const Bmg& b, u32 i, const MsgRef& msg_ref);
// the hd_only summary of a node list ("<type> <query/event name> @<n>")
io::JsonArray hd_only_summary(const io::JsonArray& nodes);

// ---------------------------------------------------------------------------------------------- stage, output
Result<std::optional<u8>> stage_msg_group(View stg_00_arc);  // hdtext.stage_msg_group (STAG +0x28); nullopt if absent
Bytes write_bin(const std::vector<Message>& msgs);    // hdtext._write_bin (into memory)
std::vector<u16> stamp_items();                       // hdtext.STAMP_ITEMS

}  // namespace hdf::text::detail
