// `hdf-convert test io`: the oracle test of module C1 (plan 3.3).
//
//   hdf-convert test io --oracle <hd_features-oracle-2.0.0.dusk> [--hd <TP HD title or content folder>]
//                       [--gc <GC files folder>] [--golden <io.tsv>] [--verbose]
//
//   (a) RARC: rarc_write(rarc_read(x)) == x for the 55 GC-layout archives of the oracle, and
//       rarc_rebuild_same_layout(rarc_read(x)) == x for Obj_Stamp.arc and O_gD_puL4.arc.
//   (b) JSON: the writer re-dumps the oracle's JSON files byte for byte, each with the style it was made with
//       (cave/hdtext/flows.json and cave/amiibo/layout.json are the 2 kept ones; all 9 are checked).
//   (c) Yaz0/gzip/TMPK/RARC on the real inputs: sizes, SHA-256 and tables equal the golden list made by
//       tools/dev/golden.py io (needs --hd, --gc and the list; default <exe folder>/golden/io.tsv).
//   (d) PNG: decode each generated oracle PNG and encode its pixels again: the bytes must be the oracle's
//       (zlib 1.3.2 level 9). The 58 kept PNGs must pass; the 21 dropped ones are checked too.
//
// Exit 0 when every part ran and passed, 1 when a part failed, 3 when (c) could not run (no --hd/--gc/list).
// The oracle is read through a small zip reader and SHA-256 through Windows CNG, both local to this file, so
// the test does not depend on the base module (C2). The oracle's own hash must be outputs::kOracleSha256.
#include "../cli.hpp"

#include "hdf/io.hpp"
#include "hdf/outputs.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace {

using hdf::Bytes;
using hdf::u16;
using hdf::u32;
using hdf::u64;
using hdf::u8;
using hdf::View;
namespace io = hdf::io;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double secs(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

// ---------------------------------------------------------------------------------------------- SHA-256 (CNG)
class Sha {
public:
    Sha() {
#ifdef _WIN32
        ok_ = BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg_, &h_, nullptr, 0, nullptr, 0, 0) == 0;
#endif
    }
    ~Sha() {
#ifdef _WIN32
        if (h_) BCryptDestroyHash(h_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
#endif
    }
    Sha(const Sha&) = delete;
    Sha& operator=(const Sha&) = delete;
    void update(View d) {
#ifdef _WIN32
        while (!d.empty()) {
            const ULONG n = static_cast<ULONG>(std::min<std::size_t>(d.size(), 1u << 30));
            BCryptHashData(h_, const_cast<PUCHAR>(d.data()), n, 0);
            d = d.subspan(n);
        }
#else
        (void)d;
#endif
    }
    std::string hex() {
        u8 out[32] = {};
#ifdef _WIN32
        if (ok_) BCryptFinishHash(h_, out, 32, 0);
#endif
        static constexpr char k[] = "0123456789abcdef";
        std::string s;
        for (u8 b : out) {
            s += k[b >> 4];
            s += k[b & 15];
        }
        return s;
    }

private:
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE h_ = nullptr;
#endif
    bool ok_ = false;
};

std::string sha_hex(View d) {
    Sha s;
    s.update(d);
    return s.hex();
}

// ---------------------------------------------------------------------------------------------- files
hdf::Result<Bytes> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return hdf::fail("io.read", "can't open " + hdf::cli::to_utf8(p));
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    Bytes b(static_cast<std::size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char*>(b.data()), n)) return hdf::fail("io.read", "can't read " + hdf::cli::to_utf8(p));
    return b;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// case-insensitive lookup of a '/' path under root (common.exists_ci per component)
std::optional<fs::path> find_ci(const fs::path& root, std::string_view rel) {
    fs::path cur = root;
    std::size_t s = 0;
    while (s <= rel.size()) {
        std::size_t j = rel.find('/', s);
        if (j == std::string_view::npos) j = rel.size();
        const std::string want = lower(std::string(rel.substr(s, j - s)));
        std::error_code ec;
        std::optional<fs::path> hit;
        for (const auto& e : fs::directory_iterator(cur, ec)) {
            if (lower(hdf::cli::to_utf8(e.path().filename())) == want) {
                hit = e.path();
                break;
            }
        }
        if (!hit) return std::nullopt;
        cur = *hit;
        s = j + 1;
    }
    return cur;
}

// ---------------------------------------------------------------------------------------------- zip
struct ZipEntry {
    std::string name;
    u16 method = 0;
    u32 crc = 0, csize = 0, usize = 0, lho = 0;
};

u16 le16(View d, std::size_t o) { return static_cast<u16>(d[o] | (d[o + 1] << 8)); }
u32 le32(View d, std::size_t o) {
    return static_cast<u32>(d[o]) | (static_cast<u32>(d[o + 1]) << 8) | (static_cast<u32>(d[o + 2]) << 16) |
           (static_cast<u32>(d[o + 3]) << 24);
}

struct Zip {
    Bytes file;
    std::vector<ZipEntry> entries;

    hdf::Result<void> open(Bytes data) {
        file = std::move(data);
        const View d = file;
        if (d.size() < 22) return hdf::fail("fmt.zip", "too small");
        std::size_t e = d.size() - 22;
        const std::size_t stop = d.size() > 22 + 65535 ? d.size() - 22 - 65535 : 0;
        while (le32(d, e) != 0x06054b50) {
            if (e == stop) return hdf::fail("fmt.zip", "no end of central directory");
            --e;
        }
        const u16 count = le16(d, e + 10);
        std::size_t o = le32(d, e + 16);
        for (u16 i = 0; i < count; ++i) {
            if (o + 46 > d.size() || le32(d, o) != 0x02014b50) return hdf::fail("fmt.zip", "bad central directory");
            ZipEntry z;
            z.method = le16(d, o + 10);
            z.crc = le32(d, o + 16);
            z.csize = le32(d, o + 20);
            z.usize = le32(d, o + 24);
            const u16 nl = le16(d, o + 28), xl = le16(d, o + 30), cl = le16(d, o + 32);
            z.lho = le32(d, o + 42);
            if (o + 46 + nl > d.size()) return hdf::fail("fmt.zip", "bad name");
            z.name.assign(reinterpret_cast<const char*>(d.data() + o + 46), nl);
            entries.push_back(std::move(z));
            o += 46 + static_cast<std::size_t>(nl) + xl + cl;
        }
        return {};
    }

    hdf::Result<Bytes> read(const ZipEntry& z) const {
        const View d = file;
        if (static_cast<u64>(z.lho) + 30 > d.size() || le32(d, z.lho) != 0x04034b50)
            return hdf::fail("fmt.zip", "bad local header: " + z.name);
        const std::size_t start = z.lho + 30 + static_cast<std::size_t>(le16(d, z.lho + 26)) + le16(d, z.lho + 28);
        if (start + static_cast<u64>(z.csize) > d.size()) return hdf::fail("fmt.zip", "data out of range: " + z.name);
        const View c = d.subspan(start, z.csize);
        Bytes out;
        if (z.method == 0) {
            out.assign(c.begin(), c.end());
        } else if (z.method == 8) {
            out.resize(z.usize);
            z_stream zs{};
            if (inflateInit2(&zs, -15) != Z_OK) return hdf::fail("io.zlib", "inflateInit2");
            zs.next_in = const_cast<Bytef*>(c.data());
            zs.avail_in = static_cast<uInt>(c.size());
            zs.next_out = out.data();
            zs.avail_out = static_cast<uInt>(out.size());
            const int rc = inflate(&zs, Z_FINISH);
            const uLong total = zs.total_out;
            inflateEnd(&zs);
            if (rc != Z_STREAM_END || total != z.usize) return hdf::fail("fmt.zip", "inflate failed: " + z.name);
        } else {
            return hdf::fail("fmt.zip", "unsupported method: " + z.name);
        }
        if (io::crc32(out) != z.crc) return hdf::fail("fmt.zip", "CRC mismatch: " + z.name);
        return out;
    }
};

// ---------------------------------------------------------------------------------------------- JSON reader
// json.loads as far as the oracle files need it: objects keep their order (a repeated key keeps its first
// place and takes the last value, as a Python dict); numbers with '.' or an exponent are floats, the rest ints;
// \u escapes give UTF-8 (surrogate pairs joined; a lone surrogate kept as WTF-8, which the writer escapes back).
class JsonReader {
public:
    explicit JsonReader(std::string_view s) : s_(s) {}

    hdf::Result<io::Json> parse() {
        io::Json v;
        if (!value(v)) return hdf::fail("fmt.json", "parse error at byte " + std::to_string(i_));
        ws();
        if (i_ != s_.size()) return hdf::fail("fmt.json", "extra data at byte " + std::to_string(i_));
        return v;
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool lit(std::string_view w) {
        if (s_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }
    static void put_utf8(std::string& o, u32 c) {
        if (c < 0x80) {
            o += static_cast<char>(c);
        } else if (c < 0x800) {
            o += static_cast<char>(0xC0 | (c >> 6));
            o += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            o += static_cast<char>(0xE0 | (c >> 12));
            o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (c >> 18));
            o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    bool hex4(u32& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<u32>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& o) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                o += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                u32 c1 = 0;
                if (!hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 <= 0xDBFF && s_.substr(i_, 2) == "\\u") {
                    const std::size_t save = i_;
                    i_ += 2;
                    u32 c2 = 0;
                    if (hex4(c2) && c2 >= 0xDC00 && c2 <= 0xDFFF) {
                        c1 = 0x10000 + (((c1 - 0xD800) << 10) | (c2 - 0xDC00));
                    } else {
                        i_ = save;
                    }
                }
                put_utf8(o, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool number(io::Json& v) {
        const std::size_t b = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        bool is_float = false;
        if (i_ < s_.size() && s_[i_] == '.') {
            is_float = true;
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            is_float = true;
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        const char* p0 = s_.data() + b;
        const char* p1 = s_.data() + i_;
        if (p0 == p1) return false;
        if (is_float) {
            double d = 0;
            if (std::from_chars(p0, p1, d).ec != std::errc()) return false;
            v = io::Json(d);
        } else {
            hdf::i64 n = 0;
            if (std::from_chars(p0, p1, n).ec != std::errc()) return false;
            v = io::Json(n);
        }
        return true;
    }
    bool value(io::Json& v) {
        ws();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            io::JsonObject o;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                v = io::Json(std::move(o));
                return true;
            }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                ++i_;
                io::Json item;
                if (!value(item)) return false;
                auto it = std::find_if(o.begin(), o.end(), [&](const auto& kv) { return kv.first == k; });
                if (it != o.end()) it->second = std::move(item);
                else o.emplace_back(std::move(k), std::move(item));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    v = io::Json(std::move(o));
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            io::JsonArray a;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                v = io::Json(std::move(a));
                return true;
            }
            for (;;) {
                io::Json item;
                if (!value(item)) return false;
                a.push_back(std::move(item));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    v = io::Json(std::move(a));
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            std::string s;
            if (!str(s)) return false;
            v = io::Json(std::move(s));
            return true;
        }
        if (lit("true")) return v = io::Json(true), true;
        if (lit("false")) return v = io::Json(false), true;
        if (lit("null")) return v = io::Json(nullptr), true;
        if (lit("NaN")) return v = io::Json(std::nan("")), true;
        if (lit("Infinity")) return v = io::Json(HUGE_VAL), true;
        if (lit("-Infinity")) return v = io::Json(-HUGE_VAL), true;
        return number(v);
    }
};

// ---------------------------------------------------------------------------------------------- helpers
bool ends_with(std::string_view s, std::string_view x) { return s.size() >= x.size() && s.substr(s.size() - x.size()) == x; }

// The plan's drop list for PNGs (plan 4, Pass criteria 1), used when outputs::drop_list() is not filled yet.
bool png_dropped(std::string_view rel) {
    if (!hdf::outputs::drop_list().empty()) return hdf::outputs::is_dropped(rel);
    if (rel == "res/cave/colossal_wallet.png" || rel == "res/stamps/stamps/stamp_background.png") return true;
    if (rel.rfind("res/cave/amiibo/", 0) == 0) {
        static constexpr std::string_view kKept[] = {"popup.png", "tt_input_glow.png", "tt_input_prompt.png",
                                                     "amiibo_wolflink.png", "font_amiibo.png", "los_heart.png"};
        const std::string_view base = rel.substr(16);
        return std::find(std::begin(kKept), std::end(kKept), base) == std::end(kKept);
    }
    return false;
}

struct Tally {
    int ok = 0, bad = 0;
    void add(bool pass) { (pass ? ok : bad)++; }
};

// ---------------------------------------------------------------------------------------------- (a) RARC
bool part_rarc(const Zip& z, bool verbose) {
    const auto t0 = Clock::now();
    static constexpr std::string_view kSameLayout[] = {"overlay/res/Object/Obj_Stamp.arc",
                                                       "overlay/res/Object/O_gD_puL4.arc"};
    Tally gc, same;
    for (const ZipEntry& e : z.entries) {
        if (!ends_with(e.name, ".arc")) continue;
        auto data = z.read(e);
        const bool is_same = std::find(std::begin(kSameLayout), std::end(kSameLayout), e.name) != std::end(kSameLayout);
        Tally& t = is_same ? same : gc;
        if (!data) {
            std::printf("  (a) %s: %s\n", e.name.c_str(), data.error().detail.c_str());
            t.add(false);
            continue;
        }
        auto arc = io::rarc_read(*data);
        if (!arc) {
            std::printf("  (a) %s: read: %s\n", e.name.c_str(), arc.error().detail.c_str());
            t.add(false);
            continue;
        }
        auto out = is_same ? io::rarc_rebuild_same_layout(*arc) : io::rarc_write(*arc);
        const bool pass = out && *out == *data;
        t.add(pass);
        if (!pass) {
            std::size_t first = 0;
            if (out)
                while (first < std::min(out->size(), data->size()) && (*out)[first] == (*data)[first]) ++first;
            std::printf("  (a) DIFF %s (%s): %zu vs %zu bytes, first difference at 0x%zx\n", e.name.c_str(),
                        is_same ? "same layout" : "GC layout", out ? out->size() : 0, data->size(), first);
        } else if (verbose) {
            std::printf("  (a) ok %s (%zu B, %zu members, %s)\n", e.name.c_str(), data->size(), arc->members.size(),
                        is_same ? "same layout" : "GC layout");
        }
    }
    std::printf("(a) RARC: GC layout %d/%d identical, same layout %d/%d identical (%.2f s)\n", gc.ok, gc.ok + gc.bad,
                same.ok, same.ok + same.bad, secs(t0));
    return gc.bad == 0 && same.bad == 0 && gc.ok == 55 && same.ok == 2;
}

// ---------------------------------------------------------------------------------------------- (b) JSON
bool part_json(const Zip& z, bool verbose) {
    const auto t0 = Clock::now();
    enum Style { kIndent1, kLines, kFloors };
    struct File {
        std::string_view rel;
        Style style;
        bool kept;
    };
    static constexpr File kFiles[] = {
        {"res/cave/amiibo/layout.json", kIndent1, true},     {"res/cave/hdtext/flows.json", kLines, true},
        {"res/stamps/hdtext/flows.json", kLines, false},     {"res/cave/hdtext/messages.json", kLines, false},
        {"res/stamps/hdtext/messages.json", kLines, false},  {"res/stamps/stamps/stamps.json", kIndent1, false},
        {"res/cave/floors.json", kFloors, false},            {"res/stamps/stampbook/book.json", kIndent1, false},
        {"res/cave/confirm/confirm.json", kIndent1, false},
    };
    Tally kept, extra;
    for (const File& f : kFiles) {
        auto it = std::find_if(z.entries.begin(), z.entries.end(), [&](const ZipEntry& e) { return e.name == f.rel; });
        Tally& t = f.kept ? kept : extra;
        if (it == z.entries.end()) {
            std::printf("  (b) missing in the oracle: %.*s\n", int(f.rel.size()), f.rel.data());
            t.add(false);
            continue;
        }
        auto data = z.read(*it);
        if (!data) {
            t.add(false);
            continue;
        }
        const std::string text(data->begin(), data->end());
        auto v = JsonReader(text).parse();
        if (!v) {
            std::printf("  (b) %.*s: %s\n", int(f.rel.size()), f.rel.data(), v.error().detail.c_str());
            t.add(false);
            continue;
        }
        std::string out;
        if (f.style == kIndent1) {
            out = io::json_dumps(*v, {.indent = 1, .item_sep = ","}) + "\n";  // json.dumps(indent=1) + '\n'
        } else {
            const io::JsonArray* items = std::get_if<io::JsonArray>(&v->v);
            if (!items) {
                t.add(false);
                continue;
            }
            if (f.style == kLines) {  // hdtext._write_json_lines
                out = io::json_lines(*items, {.ensure_ascii = false, .item_sep = ",", .key_sep = ":"});
            } else {  // cavebuild floors.json: one floor per line, json.dumps default style, ' ' prefix
                out = "[\n";
                for (std::size_t i = 0; i < items->size(); ++i) {
                    if (i) out += ",\n";
                    out += " " + io::json_dumps((*items)[i]);
                }
                out += "\n]\n";
            }
        }
        const bool pass = out == text;
        t.add(pass);
        if (!pass) {
            std::size_t first = 0;
            while (first < std::min(out.size(), text.size()) && out[first] == text[first]) ++first;
            std::printf("  (b) DIFF %.*s: %zu vs %zu bytes, first difference at %zu\n", int(f.rel.size()), f.rel.data(),
                        out.size(), text.size(), first);
        } else if (verbose) {
            std::printf("  (b) ok %.*s (%zu B)\n", int(f.rel.size()), f.rel.data(), text.size());
        }
    }
    std::printf("(b) JSON: kept %d/%d identical, other %d/%d identical (%.2f s)\n", kept.ok, kept.ok + kept.bad, extra.ok,
                extra.ok + extra.bad, secs(t0));
    return kept.bad == 0 && extra.bad == 0 && kept.ok == 2 && extra.ok == 7;
}

// ---------------------------------------------------------------------------------------------- (d) PNG
bool part_png(const Zip& z, bool verbose) {
    const auto t0 = Clock::now();
    Tally kept, dropped;
    double encode_s = 0;
    for (const ZipEntry& e : z.entries) {
        if (!ends_with(e.name, ".png") || e.name == "res/icon.png" || e.name == "res/banner.png") continue;
        Tally& t = png_dropped(e.name) ? dropped : kept;
        auto data = z.read(e);
        if (!data) {
            t.add(false);
            continue;
        }
        auto img = io::png_decode(*data);
        if (!img) {
            std::printf("  (d) %s: decode: %s\n", e.name.c_str(), img.error().detail.c_str());
            t.add(false);
            continue;
        }
        const auto t1 = Clock::now();
        const Bytes out = io::png_encode_rgba(img->first[0], img->first[1], img->second);
        encode_s += secs(t1);
        const bool pass = out == *data;
        t.add(pass);
        if (!pass)
            std::printf("  (d) DIFF %s: %zu vs %zu bytes\n", e.name.c_str(), out.size(), data->size());
        else if (verbose)
            std::printf("  (d) ok %s (%ux%u)\n", e.name.c_str(), img->first[0], img->first[1]);
    }
    std::printf("(d) PNG: kept %d/%d byte-identical, dropped %d/%d byte-identical (encode %.2f s, total %.2f s)\n",
                kept.ok, kept.ok + kept.bad, dropped.ok, dropped.ok + dropped.bad, encode_s, secs(t0));
    return kept.bad == 0 && dropped.bad == 0 && kept.ok == 58 && dropped.ok == 21;
}

// ---------------------------------------------------------------------------------------------- (c) golden list
struct Golden {
    std::vector<std::vector<std::string>> rows;
    std::map<std::string, int> counts;  // from the "# counts" line
};

hdf::Result<Golden> read_golden(const fs::path& p) {
    auto data = read_file(p);
    if (!data) return std::unexpected(data.error());
    Golden g;
    std::string_view s(reinterpret_cast<const char*>(data->data()), data->size());
    while (!s.empty()) {
        std::size_t nl = s.find('\n');
        if (nl == std::string_view::npos) nl = s.size();
        std::string_view line = s.substr(0, nl);
        s.remove_prefix(std::min(s.size(), nl + 1));
        if (line.empty()) continue;
        if (line.rfind("# counts ", 0) == 0) {
            std::string_view rest = line.substr(9);
            while (!rest.empty()) {
                std::size_t sp = rest.find(' ');
                if (sp == std::string_view::npos) sp = rest.size();
                const std::string_view kv = rest.substr(0, sp);
                const std::size_t eq = kv.find('=');
                if (eq != std::string_view::npos) g.counts[std::string(kv.substr(0, eq))] = std::stoi(std::string(kv.substr(eq + 1)));
                rest.remove_prefix(std::min(rest.size(), sp + 1));
            }
            continue;
        }
        if (line[0] == '#') continue;
        std::vector<std::string> cols;
        std::size_t b = 0;
        for (;;) {
            const std::size_t t = line.find('\t', b);
            cols.emplace_back(line.substr(b, t == std::string_view::npos ? std::string_view::npos : t - b));
            if (t == std::string_view::npos) break;
            b = t + 1;
        }
        g.rows.push_back(std::move(cols));
    }
    return g;
}

std::string member_table(const io::Rarc& a) {
    std::string t;
    for (const io::RarcMember& m : a.members) {
        t += m.path;
        t += '\t' + std::to_string(m.flags) + '\t' + std::to_string(m.offset) + '\t' + std::to_string(m.size) + '\t' +
             std::to_string(m.id) + '\t' + std::to_string(m.entry) + '\n';
    }
    return t;
}

std::string tmpk_table(const io::Tmpk& t) {
    std::string s;
    for (const io::TmpkEntry& e : t.entries) s += e.name + '\t' + std::to_string(e.off) + '\t' + std::to_string(e.size) + '\n';
    return s;
}

View sv_bytes(const std::string& s) { return View(reinterpret_cast<const u8*>(s.data()), s.size()); }

bool part_golden(const fs::path& hd_in, const fs::path& gc, const Golden& g, bool verbose) {
    const auto t0 = Clock::now();
    fs::path hd = hd_in;
    if (fs::is_directory(hd / "content")) hd = hd / "content";
    // group the rows per input file, in file order
    std::vector<std::pair<std::string, std::string>> order;
    std::map<std::pair<std::string, std::string>, std::vector<const std::vector<std::string>*>> by_file;
    for (const auto& r : g.rows) {
        if (r.size() < 3) continue;
        const auto key = std::make_pair(r[1], r[2]);
        if (!by_file.count(key)) order.push_back(key);
        by_file[key].push_back(&r);
    }
    std::map<std::string, Tally> tally;  // per kind
    std::map<std::string, int> found;    // what the C++ side found per kind
    double t_yaz0 = 0, t_gzip = 0, t_rarc = 0, t_read = 0;
    auto check = [&](const std::string& kind, bool pass, const std::string& what) {
        tally[kind].add(pass);
        if (!pass) std::printf("  (c) DIFF %s %s\n", kind.c_str(), what.c_str());
    };
    for (const auto& key : order) {
        const auto& [src, path] = key;
        const auto& rows = by_file[key];
        auto file = find_ci(src == "hd" ? hd : gc, path);
        auto t1 = Clock::now();
        auto raw = file ? read_file(*file) : hdf::Result<Bytes>(hdf::fail("io.read", "missing"));
        t_read += secs(t1);
        if (!raw) {
            check("input", false, src + " " + path + ": missing");
            continue;
        }
        std::map<std::string, const std::vector<std::string>*> one;
        std::map<u32, const std::vector<std::string>*> nested;
        for (const auto* r : rows) {
            if ((*r)[0] == "nested") nested[static_cast<u32>(std::stoul((*r)[3]))] = r;
            else one[(*r)[0]] = r;
        }
        if (auto it = one.find("input"); it != one.end()) {
            const auto& r = *it->second;
            check("input", std::to_string(raw->size()) == r[3] && sha_hex(*raw) == r[4], src + " " + path);
        }
        const bool gz = raw->size() >= 2 && (*raw)[0] == 0x1f && (*raw)[1] == 0x8b;
        if (gz) {
            ++found["gzip"];
            t1 = Clock::now();
            auto d = io::gzip_inflate(*raw);
            t_gzip += secs(t1);
            auto it = one.find("gzip");
            if (it == one.end() || !d) {
                check("gzip", false, src + " " + path + (d ? ": not in the list" : ": " + d.error().detail));
                continue;
            }
            const auto& r = *it->second;
            check("gzip", std::to_string(d->size()) == r[4] && sha_hex(*d) == r[5], src + " " + path);
            if (d->size() >= 4 && std::memcmp(d->data(), "TMPK", 4) == 0) {
                ++found["tmpk"];
                auto t = io::tmpk_read(std::move(*d));
                auto tr = one.find("tmpk");
                if (!t || tr == one.end()) {
                    check("tmpk", false, src + " " + path);
                    continue;
                }
                const auto& q = *tr->second;
                check("tmpk",
                      std::to_string(t->count) == q[3] && std::to_string(t->alignment) == q[4] &&
                          std::to_string(t->unk) == q[5] && sha_hex(sv_bytes(tmpk_table(*t))) == q[6],
                      src + " " + path);
            }
            continue;
        }
        if (io::is_yaz0(*raw)) {
            ++found["yaz0"];
            t1 = Clock::now();
            auto d = io::yaz0_decode(*raw);
            t_yaz0 += secs(t1);
            auto it = one.find("yaz0");
            if (it == one.end() || !d) {
                check("yaz0", false, src + " " + path);
                continue;
            }
            const auto& r = *it->second;
            check("yaz0", std::to_string(d->size()) == r[4] && sha_hex(*d) == r[5], src + " " + path);
        } else if (!(raw->size() >= 4 && std::memcmp(raw->data(), "RARC", 4) == 0)) {
            continue;  // los.bin, Pscene*.jpc: the input line only
        }
        t1 = Clock::now();
        auto a = io::rarc_read(*raw);
        t_rarc += secs(t1);
        auto ar = one.find("rarc");
        if (!a || ar == one.end()) {
            check("rarc", false, src + " " + path + (a ? ": not in the list" : ": " + a.error().detail));
            continue;
        }
        ++found["rarc"];
        const auto& r = *ar->second;
        check("rarc",
              std::to_string(a->num_nodes) == r[3] && std::to_string(a->num_entries) == r[4] &&
                  std::to_string(a->members.size()) == r[5] && sha_hex(sv_bytes(member_table(*a))) == r[6],
              src + " " + path);
        std::set<u32> seen;
        for (const io::RarcMember& m : a->members) {
            const View mr = a->raw(m);
            if (!io::is_yaz0(mr)) continue;
            ++found["nested"];
            seen.insert(m.entry);
            t1 = Clock::now();
            auto d = io::yaz0_decode(mr);
            t_yaz0 += secs(t1);
            auto nt = nested.find(m.entry);
            if (!d || nt == nested.end()) {
                check("nested", false, src + " " + path + " entry " + std::to_string(m.entry));
                continue;
            }
            const auto& q = *nt->second;
            check("nested", std::to_string(mr.size()) == q[4] && std::to_string(d->size()) == q[5] && sha_hex(*d) == q[6],
                  src + " " + path + " entry " + std::to_string(m.entry));
        }
        for (const auto& [ei, q] : nested)
            if (!seen.count(ei)) check("nested", false, src + " " + path + " entry " + std::to_string(ei) + " not found");
        if (verbose) std::printf("  (c) %s %s: %zu members\n", src.c_str(), path.c_str(), a->members.size());
    }
    bool pass = true;
    std::printf("(c) golden list (%zu lines, %zu inputs):\n", g.rows.size(), order.size());
    for (const char* kind : {"input", "yaz0", "rarc", "nested", "gzip", "tmpk"}) {
        const Tally& t = tally[kind];
        const int want = kind == std::string("input") ? static_cast<int>(order.size()) : g.counts.count(kind) ? g.counts.at(kind) : -1;
        const bool ok = t.bad == 0 && t.ok == want && (kind == std::string("input") || found[kind] == want);
        pass = pass && ok;
        std::printf("    %-7s %3d/%3d equal (list %d, C++ found %d)%s\n", kind, t.ok, t.ok + t.bad, want,
                    kind == std::string("input") ? t.ok + t.bad : found[kind], ok ? "" : "  FAIL");
    }
    std::printf("    time: read %.2f s, Yaz0 %.2f s, gzip %.2f s, RARC read %.3f s, total %.2f s (SHA-256 included)\n",
                t_read, t_yaz0, t_gzip, t_rarc, secs(t0));
    return pass;
}

int run(const hdf::cli::OracleArgs& a) {
    if (!a.oracle) {
        std::fprintf(stderr, "hdf-convert test io: --oracle <hd_features-oracle-2.0.0.dusk> is required\n");
        return hdf::cli::kExitUsage;
    }
    auto file = read_file(*a.oracle);
    if (!file) return hdf::cli::report("test io", file.error());
    const std::string oh = sha_hex(*file);
    if (oh != hdf::outputs::kOracleSha256 || file->size() != hdf::outputs::kOracleSize) {
        std::fprintf(stderr, "hdf-convert test io: the oracle's SHA-256 is %s, not the pinned %.*s: refused\n", oh.c_str(),
                     int(hdf::outputs::kOracleSha256.size()), hdf::outputs::kOracleSha256.data());
        return hdf::cli::kExitUsage;
    }
    Zip z;
    if (auto ok = z.open(std::move(*file)); !ok) return hdf::cli::report("test io", ok.error());
    std::printf("oracle: %s (%zu entries, SHA-256 pinned)\n", hdf::cli::to_utf8(*a.oracle).c_str(), z.entries.size());

    const bool ra = part_rarc(z, a.verbose);
    const bool rb = part_json(z, a.verbose);
    const bool rd = part_png(z, a.verbose);

    bool rc_ran = false, rc = false;
    fs::path golden_path;
    if (auto gp = a.args ? a.args->path("golden") : std::nullopt) {
        golden_path = *gp;
    } else {
#ifdef _WIN32
        wchar_t exe[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        golden_path = fs::path(std::wstring(exe, n)).parent_path() / "golden" / "io.tsv";
#endif
    }
    if (a.hd && a.gc && !golden_path.empty() && fs::exists(golden_path)) {
        auto g = read_golden(golden_path);
        if (!g) {
            std::printf("(c) golden list: %s\n", g.error().detail.c_str());
        } else {
            rc_ran = true;
            rc = part_golden(*a.hd, *a.gc, *g, a.verbose);
        }
    } else {
        std::printf("(c) skipped: needs --hd, --gc and the golden list (%s); make it with\n"
                    "    python -I -B tools/dev/golden.py io --hd <..> --gc <..> --out <build-conv*/golden/io.tsv>\n",
                    golden_path.empty() ? "--golden" : hdf::cli::to_utf8(golden_path).c_str());
    }
    const bool pass = ra && rb && rd && (!rc_ran || rc);
    std::printf("test io: (a) %s, (b) %s, (c) %s, (d) %s\n", ra ? "pass" : "FAIL", rb ? "pass" : "FAIL",
                rc_ran ? (rc ? "pass" : "FAIL") : "skipped", rd ? "pass" : "FAIL");
    if (!pass) return hdf::cli::kExitFail;
    return rc_ran ? hdf::cli::kExitOk : hdf::cli::kExitTodo;
}

}  // namespace

HDF_ORACLE_TEST("io", "RARC write(read(x)) == x for the 55 + 2 oracle archives; JSON re-dump of the 9 oracle JSONs; Yaz0/gzip/TMPK/RARC golden list; PNG re-encode of the 79 oracle PNGs (58 kept)", &run);
