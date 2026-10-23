// hdf_convert: containers, compression, PNG, JSON and Python number helpers (module C1 "io"; src/io/*).
//
// Ports: formats/yaz0.py (Yaz0 only; no Yay0 anywhere, pipeline 3), formats/rarc.py (Rarc/Member reader),
// formats/rarc_write.py (build, build_from_rarc), pipeline/rarcx.py (rebuild_same_layout, entry_index),
// formats/hdpack.py (read_any, Tmpk, load_member), pipeline/common.py (png_bytes, align, exists_ci), the
// json.dumps styles of the 2 kept JSON files, Python round/repr. zlib 1.3.2 (third_party) for inflate/deflate.
#pragma once

#include "hdf/core.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hdf::io {

// ---------------------------------------------------------------------------------------------- big-endian
inline u16 be16(View d, std::size_t o) { return static_cast<u16>((d[o] << 8) | d[o + 1]); }
inline u32 be32(View d, std::size_t o) {
    return (static_cast<u32>(d[o]) << 24) | (static_cast<u32>(d[o + 1]) << 16) | (static_cast<u32>(d[o + 2]) << 8) |
           static_cast<u32>(d[o + 3]);
}
inline void put16(Bytes& b, std::size_t o, u16 v) {
    b[o] = static_cast<u8>(v >> 8);
    b[o + 1] = static_cast<u8>(v);
}
inline void put32(Bytes& b, std::size_t o, u32 v) {
    b[o] = static_cast<u8>(v >> 24);
    b[o + 1] = static_cast<u8>(v >> 16);
    b[o + 2] = static_cast<u8>(v >> 8);
    b[o + 3] = static_cast<u8>(v);
}
// common.align: round up to a multiple of a (a > 0).
inline u64 align(u64 v, u64 a) { return (v + a - 1) / a * a; }

// ---------------------------------------------------------------------------------------------- Yaz0, gzip, PNG
bool is_yaz0(View in);
Result<Bytes> yaz0_decode(View in);                                  // yaz0.decompress_yaz0
Result<Bytes> maybe_yaz0(View in);                                   // yaz0.maybe_decompress (copy if not Yaz0)
Result<Bytes> gzip_inflate(View in, std::stop_token stop = {});      // inflateInit2(15+16), 1 MB steps
Result<Bytes> read_any(View in, std::stop_token stop = {});          // hdpack.read_any: gunzip if 1f 8b
Bytes deflate9(View in);                                             // compress2 level 9 (zlib 1.3.2)
u32 crc32(View in, u32 crc = 0);
Bytes png_encode_rgba(u32 w, u32 h, View rgba);                      // = common.png_bytes (filter 0, one IDAT)
std::string png_pixel_hash(u32 w, u32 h, View rgba);                 // = hdf_builder.png_pixel_hash (diagnostic)
std::string png_pixel_hash(View png);                                // same, from PNG bytes (compare fallback)
Result<std::pair<std::array<u32, 2>, Bytes>> png_decode(View png);   // tests / compare only: {w, h}, RGBA8

// ---------------------------------------------------------------------------------------------- RARC
// Layout and field names as rarc.py's docstring (JKRArchive SArcHeader/SArcDataInfo/SDIDirEntry/SDIFileEntry).
inline constexpr u8 kRarcFile = 0x01, kRarcDir = 0x02, kRarcCompressed = 0x04, kRarcMram = 0x10,
                    kRarcAram = 0x20, kRarcDvd = 0x40, kRarcYaz0 = 0x80;

struct RarcNode {
    std::string type;  // 4 chars: "ROOT", "DZR ", "BMDR", ...
    std::string name;
    u16 hash = 0;
    u16 count = 0;     // entry count
    u32 first = 0;     // first entry index
};

struct RarcEntry {
    u16 id = 0;
    u16 hash = 0;
    u8 flags = 0;
    std::string name;
    u32 off = 0;       // data offset (file) or node index (dir)
    u32 size = 0;
};

// rarc.Member: a file reached by the node/entry walk ('.'/'..' skipped).
struct RarcMember {
    std::string path;  // "<root>/<dir>/<name>", as rarc.py builds it
    std::string name;
    u8 flags = 0;
    u32 offset = 0;    // relative to the data base
    u32 size = 0;
    u16 id = 0;        // entry id
    u32 entry = 0;     // entry index (rarcx.entry_index)
};

struct Rarc {
    Bytes buf;                     // the archive, Yaz0 already decoded
    bool was_compressed = false;   // the input was Yaz0
    u32 file_size = 0, hdr_size = 0, data_len = 0, mram = 0, aram = 0, dvd = 0;
    u32 num_nodes = 0, num_entries = 0, str_len = 0;
    u32 node_base = 0, entry_base = 0, str_base = 0, data_base = 0;  // absolute offsets in buf
    u16 next_id = 0;
    u8 sync = 0;
    std::vector<RarcNode> nodes;
    std::vector<RarcEntry> entries;
    std::vector<RarcMember> members;  // walk order (rarc.Rarc.files())

    View raw(const RarcMember& m) const;                 // Member.raw()
    Result<Bytes> data(const RarcMember& m) const;       // Member.data(): Yaz0 members decoded
    // Rarc.get: path, path suffix, then basename, ignoring case. ASCII case only: Python's str.lower() also folds
    // non-ASCII letters, but every RARC and TMPK name in the inputs and the oracle is ASCII (VERIFIED fix-wave1:
    // 85 + 57 archives, 61 packs), so the results are the same (fix-wave1 L8). rarc_entry_index follows the same
    // rule. (Tmpk::get compares exactly, as hdpack.py does.)
    const RarcMember* get(std::string_view key) const;
    std::string_view root_name() const;                  // nodes[0].name
};

// Member path -> new bytes (Python dict `replace`, looked up by full member path or by the path under the root).
using Replace = std::map<std::string, Bytes, std::less<>>;

Result<Rarc> rarc_read(View in);                                          // rarc.Rarc(buf)
// rarc_write.build(root_name, entries): GC retail layout; entries in order, paths relative to the root.
Bytes rarc_build(std::string_view root_name, std::span<const std::pair<std::string, Bytes>> entries);
Result<Bytes> rarc_write(const Rarc& arc, const Replace& replace = {});   // = rarc_write.build_from_rarc
Result<Bytes> rarc_rebuild_same_layout(const Rarc& arc, const Replace& replace = {});  // = rarcx.rebuild_same_layout
std::optional<u32> rarc_entry_index(const Rarc& arc, std::string_view member_path);    // = rarcx.entry_index
u16 rarc_name_hash(std::string_view name);                                // rarc_write.name_hash (h*3 + c)

// ---------------------------------------------------------------------------------------------- TMPK
struct TmpkEntry {
    std::string name;
    u32 off = 0;
    u32 size = 0;
};

// hdpack.Tmpk over an inflated pack (keeps the bytes; members are views into them).
struct Tmpk {
    Bytes buf;
    u32 count = 0, alignment = 0, unk = 0;
    std::vector<TmpkEntry> entries;

    View data(const TmpkEntry& e) const;
    std::optional<View> get(std::string_view name) const;  // Tmpk.get: exact, "/"-suffix, then basename
};

Result<Tmpk> tmpk_read(Bytes inflated);                              // takes ownership of the inflated pack
Result<Tmpk> tmpk_from_gz(View pack_gz, std::stop_token stop = {});  // read_any + tmpk_read

// ---------------------------------------------------------------------------------------------- Python numbers
double py_round(double x, int ndigits);    // round(x, n): to_chars(fixed, n) then from_chars (libs 3)
i64 py_round_int(double x);                // int(round(x)): half to even, exact, any rounding mode (fix-wave1 L9)
std::string py_float_repr(double x);       // repr(float) as json.dumps writes it
i64 floor_div(i64 a, i64 b);               // Python a // b
i64 floor_mod(i64 a, i64 b);               // Python a % b

// ---------------------------------------------------------------------------------------------- JSON writer
// A JSON value with Python's dict order (insertion order) for objects.
struct Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::vector<std::pair<std::string, Json>>;

struct Json {
    // null, bool, int, float (written with py_float_repr), UTF-8 string, array, object
    std::variant<std::monostate, bool, i64, double, std::string, JsonArray, JsonObject> v;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : v(b) {}
    Json(int i) : v(static_cast<i64>(i)) {}
    Json(i64 i) : v(i) {}
    Json(u32 i) : v(static_cast<i64>(i)) {}
    Json(double d) : v(d) {}
    Json(const char* s) : v(std::string(s)) {}
    Json(std::string s) : v(std::move(s)) {}
    Json(JsonArray a) : v(std::move(a)) {}
    Json(JsonObject o) : v(std::move(o)) {}
};

struct JsonStyle {
    int indent = -1;               // -1: no newlines (json.dumps indent=None)
    bool ensure_ascii = true;      // \uXXXX (lower-case hex, surrogate pairs) for non-ASCII
    std::string item_sep = ", ";   // json.dumps separators; with indent >= 0 Python uses ","
    std::string key_sep = ": ";
};

// json.dumps(v, ...) with Python's escapes. The 2 kept files:
//   cave/amiibo/layout.json  = json_dumps(v, {.indent = 1, .item_sep = ","}) + "\n"          (hdtext.py:1205)
//   cave/hdtext/flows.json   = json_lines(items, {.ensure_ascii = false, .item_sep = ",", .key_sep = ":"})
std::string json_dumps(const Json& v, const JsonStyle& style = {});
// '[\n' + ',\n'.join(json.dumps(x) for x in items) + '\n]\n' (hdtext._write_json_lines)
std::string json_lines(std::span<const Json> items, const JsonStyle& style);

}  // namespace hdf::io
