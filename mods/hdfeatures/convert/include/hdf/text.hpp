// hdf_convert: HD message text for the cave and the stamps (module TX "text"; src/text/*).
//
// Ports: pipeline/hdtext.py text part (Bmg, Entry, tokens, encode, convert_text, gc_line_limits, FontMetrics,
// the rewrap family, build_message, decode_node, reachable, predecessors, neighbourhood, flows_reaching,
// label_names via facts, stage_msg_group, _cave_spec, _stamps_spec, _write_bin, the flows.json writer),
// formats/bmg.py. CP1252 table as in src/stamps/hdtext.hpp:245. FontMetrics.width is a left-to-right double
// sum (hdtext.py:568). GC bmgres files are read in sorted order (7.3).
#pragma once

#include "hdf/core.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

// <windows.h> (wingdi.h) defines TextOut as a macro (TextOutW). Keep this header's names intact whatever the
// include order; a file that includes <windows.h> before this header can use hdf::text::Output.
#pragma push_macro("TextOut")
#undef TextOut

namespace hdf::text {

struct TextOut {
    Bytes cave_messages_bin;    // res/cave/hdtext/messages.bin  ('HDTX', 7,504 B)
    Bytes cave_flows_json;      // res/cave/hdtext/flows.json    (11,133 B)
    Bytes stamps_messages_bin;  // res/stamps/hdtext/messages.bin (11,336 B)
};
using Output = TextOut;  // the same type, for files where <windows.h> renames TextOut

// Optional diagnostics for the oracle test's golden lists (hdf-convert test text). They hold game text in
// memory: never write them to a file except as hashes.
struct TextDiag {
    std::vector<std::pair<int, int>> line_limits;       // gc_line_limits: box kind -> px
    std::vector<std::pair<std::string, std::string>> rewrapped;  // "file:group:id" -> re-wrapped text (shown form)

    // ---- added by module TX in wave 2 (additive)
    struct Rewrap {                     // one message whose rewrap() gave a note, in output order
        std::string part;               // "cave" | "stamps"
        std::string file;               // "zel_00" | "zel_04" | "zel_05"
        u16 id = 0;
        u8 box = 0;                     // INF1 box kind
        int limit = 0;                  // px
        Bytes text;                     // the re-wrapped GC text incl. NUL
        std::string note;               // hdtext's note (numbers only)
    };
    std::vector<Rewrap> rewraps;
    std::vector<std::pair<int, int>> line_samples;      // box kind -> GC messages measured
    struct Message {                    // one messages.bin record (+ hdtext.preview), cave then stamps
        std::string part, file;
        u8 group = 0;
        u16 id = 0;
        u8 action = 0;                  // 0 native, 1 override, 2 register
        Bytes text;
        std::array<u8, 20> entry{};
        std::string preview;            // UTF-8
    };
    std::vector<Message> messages;
    std::vector<std::pair<std::string, int>> stage_groups;  // "D_SB11", "R_SP160_GC", "R_SP160_HD" -> group, -1 none
    std::vector<u8> gc_chars;           // sorted (_gc_chars)
    int font_cell = 0, font_cell_w = 0;
    double font_name = 0;               // FontMetrics.name
    Bytes stamps_flows_json;            // res/stamps/hdtext/flows.json (a dropped output; extra check only)
    double seconds = 0;                 // wall time of build()
};

// hd: CheckedSource over the 136 HD inputs; gc: CheckedSource over the 13 GC inputs.
// Errors: "stopped", "fmt.text" (a parse failure or requested text missing), and the sources' own errors.
Result<TextOut> build(const Source& hd, const Source& gc, std::stop_token stop, TextDiag* diag = nullptr);

}  // namespace hdf::text

#pragma pop_macro("TextOut")
