// Module G: the cache stamp (cache.json) and the files-<set8> folder (plan 2.4 with Corrections #9 and #10).
//
//   prepare_files: drop a stamp that names files-<set8>, remove a leftover files-<set8>, make it empty.
//   generate():    writes the 176 files into it through a FolderSink (each file flushed, fix-wave1 L5).
//   commit:        check the records and the files on disk, write cache.json.tmp (flushed), then MoveFileExW it
//                  over cache.json (REPLACE_EXISTING | WRITE_THROUGH, retries). cache.json is the only commit
//                  point: a cancel, a crash or a power cut before the move leaves no stamp for this folder.
//   cleanup:       remove the files-* / tmp-* folders the stamp doesn't name (retries, never fatal).
// cache.json is read back with a small bounded JSON parser: the file is in the user's folder and may be cut,
// edited or garbage, so every field is type-checked and nothing in it is trusted as a path (the folder name must
// be folder_name(set)).
#include "gen_internal.hpp"

#include "../base/base_util.hpp"  // os_error_text

#include "hdf/io.hpp"
#include "hdf/source.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <system_error>
#include <thread>
#include <tuple>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hdf {
namespace gen {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxStampBytes = std::size_t(4) << 20;
constexpr int kMaxDepth = 16;
constexpr std::size_t kMaxValues = 100000;

fs::path u8path(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string path_text(const fs::path& p) {
    // for messages only; never throws on odd names
    std::string out;
    for (wchar_t c : p.native()) out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    return out;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

// The ASCII lower-case name of a folder entry ("" when it has other characters: never one of ours).
std::string ascii_name(const fs::path& p) {
    std::string out;
    for (wchar_t c : p.filename().native()) {
        if (c <= 0x20 || c >= 0x7F) return {};
        out += lower(static_cast<char>(c));
    }
    return out;
}

std::string fmt(const char* f, auto... args) {
    char b[512];
    std::snprintf(b, sizeof b, f, args...);
    return b;
}

// ---------------------------------------------------------------------------------------------- JSON reader
struct JValue {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double num = 0;
    bool integral = false;  // written without '.', 'e' and fits i64
    i64 ival = 0;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj;

    const JValue* get(std::string_view key) const {
        const JValue* found = nullptr;
        for (const auto& [k, v] : obj)
            if (k == key) found = &v;  // json.loads: the last one wins
        return found;
    }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Result<JValue> document() {
        JValue v;
        if (auto ok = value(v, 0); !ok) return std::unexpected(ok.error());
        ws();
        if (i_ != s_.size()) return bad("text after the value");
        return v;
    }

private:
    std::string_view s_;
    std::size_t i_ = 0, values_ = 0;

    std::unexpected<Error> bad(const std::string& what) const {
        return fail("cache.bad", "cache.json: " + what + " at byte " + std::to_string(i_));
    }
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool lit(std::string_view w) {
        if (s_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }

    Result<void> value(JValue& v, int depth) {
        if (depth > kMaxDepth) return bad("nested too deep");
        if (++values_ > kMaxValues) return bad("too many values");
        ws();
        if (i_ >= s_.size()) return bad("unexpected end");
        const char c = s_[i_];
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') {
            v.kind = JValue::Str;
            return string(v.str);
        }
        if (lit("true")) {
            v.kind = JValue::Bool;
            v.b = true;
            return {};
        }
        if (lit("false")) {
            v.kind = JValue::Bool;
            return {};
        }
        if (lit("null")) return {};
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return bad("unexpected character");
    }

    Result<void> object(JValue& v, int depth) {
        v.kind = JValue::Obj;
        ++i_;  // {
        ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return {};
        }
        for (;;) {
            ws();
            if (i_ >= s_.size() || s_[i_] != '"') return bad("expected a key");
            std::string key;
            if (auto ok = string(key); !ok) return ok;
            ws();
            if (i_ >= s_.size() || s_[i_] != ':') return bad("expected ':'");
            ++i_;
            JValue item;
            if (auto ok = value(item, depth + 1); !ok) return ok;
            v.obj.emplace_back(std::move(key), std::move(item));
            ws();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return {};
            }
            return bad("expected ',' or '}'");
        }
    }

    Result<void> array(JValue& v, int depth) {
        v.kind = JValue::Arr;
        ++i_;  // [
        ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return {};
        }
        for (;;) {
            JValue item;
            if (auto ok = value(item, depth + 1); !ok) return ok;
            v.arr.push_back(std::move(item));
            ws();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return {};
            }
            return bad("expected ',' or ']'");
        }
    }

    static void put_utf8(std::string& out, u32 cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(u32& out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= u32(c - '0');
            else if (c >= 'a' && c <= 'f') out |= u32(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= u32(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    Result<void> string(std::string& out) {
        ++i_;  // "
        for (;;) {
            if (i_ >= s_.size()) return bad("unterminated string");
            if (out.size() > (std::size_t(64) << 10)) return bad("string too long");
            const char c = s_[i_++];
            if (c == '"') return {};
            if (static_cast<unsigned char>(c) < 0x20) return bad("control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return bad("unterminated escape");
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    u32 cp = 0;
                    if (!hex4(cp)) return bad("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        u32 lo = 0;
                        if (!(lit("\\u") && hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)) return bad("lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return bad("lone surrogate");
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return bad("bad escape");
            }
        }
    }

    Result<void> number(JValue& v) {
        const std::size_t st = i_;
        bool integral = true;
        if (s_[i_] == '-') ++i_;
        if (i_ >= s_.size() || !(s_[i_] >= '0' && s_[i_] <= '9')) return bad("bad number");
        if (s_[i_] == '0') {
            ++i_;
        } else {
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            integral = false;
            ++i_;
            if (i_ >= s_.size() || !(s_[i_] >= '0' && s_[i_] <= '9')) return bad("bad number");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            integral = false;
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ >= s_.size() || !(s_[i_] >= '0' && s_[i_] <= '9')) return bad("bad number");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        const char* a = s_.data() + st;
        const char* b = s_.data() + i_;
        v.kind = JValue::Num;
        if (std::from_chars(a, b, v.num).ec != std::errc()) return bad("bad number");
        if (integral) v.integral = std::from_chars(a, b, v.ival).ec == std::errc();
        return {};
    }
};

// ---------------------------------------------------------------------------------------------- fields
std::unexpected<Error> bad_field(std::string_view name) {
    return fail("cache.bad", "cache.json: field '" + std::string(name) + "' is missing or has the wrong type");
}

Result<std::string> str_field(const JValue& o, std::string_view name, bool required) {
    const JValue* v = o.get(name);
    if (!v) {
        if (required) return bad_field(name);
        return std::string();
    }
    if (v->kind != JValue::Str) return bad_field(name);
    return v->str;
}

Result<i64> int_field(const JValue& o, std::string_view name, bool required) {
    const JValue* v = o.get(name);
    if (!v) {
        if (required) return bad_field(name);
        return i64(0);
    }
    if (v->kind != JValue::Num || !v->integral) return bad_field(name);
    return v->ival;
}

// ---------------------------------------------------------------------------------------------- disk helpers
#ifdef _WIN32
void clear_read_only(const fs::path& root) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code tec;
        if (it->is_symlink(tec)) it.disable_recursion_pending();
        SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
}
#endif

// The 100 ms pause between two retries, in 10 ms steps. false = a stop was requested (wave 4, G L3).
bool retry_pause(const std::stop_token& stop) {
    for (int i = 0; i < 10; ++i) {
        if (stop.stop_requested()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !stop.stop_requested();
}

// Removes a folder (a link: the link only, never what it points at), retrying for up to budget_ms (antivirus
// and the indexer keep new files open for a moment, Correction #9). Wave 4 (G L3): a stop request ends the
// retries (false), so a locked file never holds a game exit for seconds.
bool remove_tree(const fs::path& p, int budget_ms, const std::stop_token& stop = {}) {
    const auto t0 = Clock::now();
    for (int attempt = 0;; ++attempt) {
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(p, ec);
        if (ec || !fs::exists(st)) return true;
        if (fs::is_symlink(st)) {
            fs::remove(p, ec);
        } else {
#ifdef _WIN32
            if (attempt > 0) clear_read_only(p);
#endif
            fs::remove_all(p, ec);
        }
        std::error_code ec2;
        if (!fs::exists(fs::symlink_status(p, ec2))) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() >= budget_ms) return false;
        if (!retry_pause(stop)) return false;
    }
}

bool remove_file(const fs::path& p, int budget_ms, const std::stop_token& stop = {}) {
    const auto t0 = Clock::now();
    for (;;) {
        std::error_code ec;
        fs::remove(p, ec);
        std::error_code ec2;
        if (!fs::exists(fs::symlink_status(p, ec2))) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() >= budget_ms) return false;
        if (!retry_pause(stop)) return false;
    }
}

Stamp make_stamp(const GenOut& out, const StampInfo& info, std::span<const outputs::ExpectedFile> expected,
                 std::string_view set) {
    Stamp s;
    s.format = kStampFormat;
    s.mod = std::string(kModId);
    s.mod_version = info.mod_version;
    s.converter = kConverterVersion;
    s.set = std::string(set);
    s.folder = folder_name(set);
    s.made = info.made.empty() ? utc_now() : info.made;
    s.seconds = out.seconds;
    s.dusklight = info.dusklight.empty() ? std::string(kDusklightVersion) : info.dusklight;
    s.gc = info.gc;
    s.hd = info.hd;
    for (const outputs::ExpectedFile& e : expected)  // sorted by rel; equal to the records (checked)
        s.files.emplace_back(std::string(e.rel), std::make_pair(e.size, std::string(e.sha256)));
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- JSON text
std::string stamp_json(const Stamp& s) {
    auto q = [](std::string_view v) { return io::json_dumps(io::Json(std::string(v))); };  // ASCII, \u escapes
    std::string j = "{\n";
    j += " \"format\": " + std::to_string(s.format) + ",\n";
    j += " \"mod\": " + q(s.mod) + ",\n";
    j += " \"mod_version\": " + q(s.mod_version) + ",\n";
    j += " \"converter\": " + std::to_string(s.converter) + ",\n";
    j += " \"set\": " + q(s.set) + ",\n";
    j += " \"folder\": " + q(s.folder) + ",\n";
    j += " \"made\": " + q(s.made) + ",\n";
    j += " \"seconds\": " + io::py_float_repr(io::py_round(s.seconds, 3)) + ",\n";
    j += " \"dusklight\": " + q(s.dusklight) + ",\n";
    j += " \"gc\": " + q(s.gc) + ",\n";
    j += " \"hd\": {\"kind\": " + q(s.hd.kind) + ", \"location\": " + q(s.hd.location) + ", \"title\": " +
         q(s.hd.title) + ", \"version\": " + std::to_string(s.hd.version) + "},\n";
    j += " \"files\": {";
    for (std::size_t i = 0; i < s.files.size(); ++i) {
        const auto& [rel, f] = s.files[i];
        j += i ? ",\n  " : "\n  ";
        j += q(rel) + ": [" + std::to_string(f.first) + ", " + q(f.second) + "]";
    }
    j += s.files.empty() ? "}\n}\n" : "\n }\n}\n";
    return j;
}

Result<Stamp> parse_stamp(std::string_view text) {
    if (text.size() > kMaxStampBytes) return fail("cache.bad", "cache.json is too large");
    Parser parser(text);
    auto doc = parser.document();
    if (!doc) return std::unexpected(doc.error());
    if (doc->kind != JValue::Obj) return fail("cache.bad", "cache.json is not an object");
    const JValue& o = *doc;
    Stamp s;
    auto format = int_field(o, "format", true);
    if (!format) return std::unexpected(format.error());
    if (*format != kStampFormat) return fail("cache.bad", "cache.json format " + std::to_string(*format));
    s.format = static_cast<int>(*format);
    auto conv = int_field(o, "converter", true);
    if (!conv) return std::unexpected(conv.error());
    if (*conv < 0 || *conv > 1000000) return bad_field("converter");
    s.converter = static_cast<int>(*conv);
    for (auto [name, dst, req] : {std::tuple{"mod", &s.mod, true}, std::tuple{"mod_version", &s.mod_version, false},
                                  std::tuple{"set", &s.set, true}, std::tuple{"folder", &s.folder, true},
                                  std::tuple{"made", &s.made, false}, std::tuple{"dusklight", &s.dusklight, false},
                                  std::tuple{"gc", &s.gc, false}}) {
        auto v = str_field(o, name, req);
        if (!v) return std::unexpected(v.error());
        *dst = std::move(*v);
    }
    if (const JValue* v = o.get("seconds")) {
        if (v->kind != JValue::Num) return bad_field("seconds");
        s.seconds = v->num;
    }
    if (const JValue* hd = o.get("hd")) {
        if (hd->kind != JValue::Obj) return bad_field("hd");
        for (auto [name, dst] : {std::pair{"kind", &s.hd.kind}, std::pair{"location", &s.hd.location},
                                 std::pair{"title", &s.hd.title}}) {
            auto v = str_field(*hd, name, false);
            if (!v) return std::unexpected(v.error());
            *dst = std::move(*v);
        }
        auto ver = int_field(*hd, "version", false);
        if (!ver || *ver < 0 || *ver > 0xFFFFFFFFll) return bad_field("hd.version");
        s.hd.version = static_cast<u32>(*ver);
    }
    const JValue* files = o.get("files");
    if (!files || files->kind != JValue::Obj) return bad_field("files");
    std::map<std::string_view, int> seen;
    for (const auto& [rel, v] : files->obj) {
        if (!seen.emplace(rel, 0).second) return fail("cache.bad", "cache.json lists " + rel + " twice");
        if (v.kind != JValue::Arr || v.arr.size() != 2 || v.arr[0].kind != JValue::Num || !v.arr[0].integral ||
            v.arr[0].ival < 0 || v.arr[1].kind != JValue::Str)
            return fail("cache.bad", "cache.json: bad entry for " + rel);
        s.files.emplace_back(rel, std::make_pair(static_cast<u64>(v.arr[0].ival), v.arr[1].str));
    }
    return s;
}

std::string utc_now() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

Result<void> replace_file(const fs::path& from, const fs::path& to, int budget_ms, std::stop_token stop) {
#ifdef _WIN32
    const auto t0 = Clock::now();
    for (;;) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {};
        const DWORD e = GetLastError();
        const bool transient = e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
        if (!transient ||
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() >= budget_ms) {
            const std::error_code ec(int(e), std::system_category());
            return fail("io.write", path_text(to.filename()) + ": " + base::os_error_text(ec));
        }
        if (!retry_pause(stop)) return fail("stopped");  // wave 4, G L3: a stop ends the retries
    }
#else
    (void)budget_ms;
    (void)stop;
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) return fail("io.write", path_text(to.filename()) + ": " + ec.message());
    return {};
#endif
}

// ---------------------------------------------------------------------------------------------- validate
Result<void> validate_stamp_with(const fs::path& data_dir, const Stamp& stamp,
                                 std::span<const outputs::ExpectedFile> expected, std::string_view set) {
    if (stamp.format != kStampFormat) return fail("cache.bad", "format " + std::to_string(stamp.format));
    if (stamp.mod != kModId) return fail("cache.bad", "another mod id");
    if (stamp.converter != kConverterVersion) return fail("cache.bad", "converter " + std::to_string(stamp.converter));
    if (stamp.set != set) return fail("cache.set", "the stamp is for set " + stamp.set.substr(0, 8));
    if (stamp.folder != folder_name(set)) return fail("cache.bad", "folder name");
    if (stamp.files.size() != expected.size())
        return fail("cache.bad", fmt("%zu files listed, %zu expected", stamp.files.size(), expected.size()));
    std::map<std::string_view, const std::pair<u64, std::string>*> listed;
    for (const auto& [rel, f] : stamp.files) listed.emplace(rel, &f);
    for (const outputs::ExpectedFile& e : expected) {
        auto it = listed.find(e.rel);
        if (it == listed.end() || it->second->first != e.size || it->second->second != e.sha256)
            return fail("cache.bad", "file list differs at " + std::string(e.rel));
    }
    const fs::path folder = data_dir / u8path(stamp.folder);
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return fail("cache.incomplete", "no folder " + stamp.folder);
    for (const outputs::ExpectedFile& e : expected) {
        const fs::path p = folder / u8path(e.rel);
        std::error_code sec;
        const fs::file_status st = fs::status(p, sec);
        if (sec || !fs::is_regular_file(st)) return fail("cache.incomplete", "missing " + std::string(e.rel));
        const u64 n = fs::file_size(p, sec);
        if (sec || n != e.size)
            return fail("cache.incomplete", fmt("%s: size %llu, expected %llu", std::string(e.rel).c_str(),
                                                static_cast<unsigned long long>(n), static_cast<unsigned long long>(e.size)));
    }
    return {};
}

// ---------------------------------------------------------------------------------------------- commit
Result<void> commit_with(const fs::path& data_dir, const fs::path& files_dir, const GenOut& out, const StampInfo& info,
                         std::span<const outputs::ExpectedFile> expected, std::string_view set, std::stop_token stop) {
    std::error_code ec;
    const fs::path want = data_dir / u8path(folder_name(set));
    if (!fs::equivalent(files_dir, want, ec) || ec)
        return fail("cache.folder", path_text(files_dir.filename()) + " is not " + folder_name(set) + " in the data folder");
    if (auto ok = check_against(expected, out.files, false); !ok) return ok;
    for (const outputs::ExpectedFile& e : expected) {  // every file on disk with its size (plan 2.4 step 2)
        const fs::path p = files_dir / u8path(e.rel);
        std::error_code sec;
        if (!fs::is_regular_file(p, sec) || fs::file_size(p, sec) != e.size || sec)
            return fail("out.missing", std::string(e.rel));
    }
    Stamp s = make_stamp(out, info, expected, set);
    std::string text = stamp_json(s);
    // wave2_G_verify L1: the text is read back with parse_stamp and checked with validate_stamp_with before it is
    // written, so a stamp that the next start would refuse (and remake at every start) is never committed. The
    // writer passes a lone surrogate through as \udXXX, which the reader refuses: such a location (or one too long
    // for the reader) is dropped, and the next start just searches as usual.
    auto back = parse_stamp(text);
    if (!back && !s.hd.location.empty()) {
        s.hd.location.clear();
        text = stamp_json(s);
        back = parse_stamp(text);
    }
    if (!back) return fail("cache.bad", "the new cache.json doesn't read back: " + back.error().detail);
    if (auto ok = validate_stamp_with(data_dir, *back, expected, set); !ok) return ok;
    // The .tmp is small (one write piece): written without the stop token, so a stop that comes after a finished
    // make still keeps it. Only the waits (the replace retries, cleanup) end early on a stop (wave 4, G L3).
    FolderSink stamp_sink(data_dir);  // flushes the .tmp before it returns (fix-wave1 L5)
    if (auto ok = stamp_sink.write(kStampTmpFile, View(reinterpret_cast<const u8*>(text.data()), text.size())); !ok)
        return ok;
    if (auto ok = replace_file(data_dir / kStampTmpFile, data_dir / kStampFile, 5000, stop); !ok) {
        remove_file(data_dir / kStampTmpFile, 0);
        return ok;
    }
    cleanup(data_dir, &s, stop);  // plan 2.4 step 5; never fatal
    return {};
}

Result<fs::path> prepare_files_with(const fs::path& data_dir, std::string_view set, std::stop_token stop) {
    if (stop.stop_requested()) return fail("stopped");
    // wave 4, G L3: a stop ends the retries of a locked file early ("stopped", nothing made yet)
    const auto not_removed = [&stop](std::string what) {
        return stop.stop_requested() ? fail("stopped") : fail("io.write", std::move(what));
    };
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    if (ec) return fail("io.write", "data folder: " + base::os_error_text(ec));
    const std::string name = folder_name(set);
    const fs::path stamp = data_dir / kStampFile;
    if (fs::exists(fs::symlink_status(stamp, ec))) {
        auto old = read_stamp(data_dir);
        if (!old || iequals(old->folder, name)) {
            if (!remove_file(stamp, 5000, stop)) return not_removed("can't remove the old cache.json");
        }
    }
    remove_file(data_dir / kStampTmpFile, 1000, stop);
    if (stop.stop_requested()) return fail("stopped");
    const fs::path folder = data_dir / u8path(name);
    if (!remove_tree(folder, 5000, stop)) return not_removed("can't remove the old " + name);
    fs::create_directory(folder, ec);
    if (ec) return fail("io.write", name + ": " + base::os_error_text(ec));
    return folder;
}

}  // namespace gen

// ---------------------------------------------------------------------------------------------- public API
Result<Stamp> read_stamp(const std::filesystem::path& data_dir) {
    namespace fs = std::filesystem;
    const fs::path p = data_dir / kStampFile;
    std::error_code ec;
    const fs::file_status st = fs::status(p, ec);
    if (!fs::exists(st)) return fail("cache.missing", "no cache.json");
    if (!fs::is_regular_file(st)) return fail("cache.bad", "cache.json is not a file");
    const u64 n = fs::file_size(p, ec);
    if (ec || n > gen::kMaxStampBytes) return fail("cache.bad", "cache.json is unreadable or too large");
    std::ifstream f(p, std::ios::binary);
    if (!f) return fail("cache.bad", "cache.json can't be opened");
    std::string text(static_cast<std::size_t>(n), '\0');
    f.read(text.data(), static_cast<std::streamsize>(n));
    if (static_cast<u64>(f.gcount()) != n) return fail("cache.bad", "cache.json can't be read");
    return gen::parse_stamp(text);
}

Result<void> validate_stamp(const std::filesystem::path& data_dir, const Stamp& stamp) {
    return gen::validate_stamp_with(data_dir, stamp, outputs::expected(), set_id());
}

Result<void> commit(const std::filesystem::path& data_dir, const std::filesystem::path& files_dir, const GenOut& out,
                    const StampInfo& info, std::stop_token stop) {
    return gen::commit_with(data_dir, files_dir, out, info, outputs::expected(), set_id(), std::move(stop));
}

Result<std::filesystem::path> prepare_files(const std::filesystem::path& data_dir, std::stop_token stop) {
    return gen::prepare_files_with(data_dir, set_id(), std::move(stop));
}

u32 cleanup(const std::filesystem::path& data_dir, const Stamp* keep, std::stop_token stop) {
    namespace fs = std::filesystem;
    u32 removed = 0;
    std::vector<fs::path> victims;
    std::error_code ec;
    for (fs::directory_iterator d(data_dir, ec), end; !ec && d != end; d.increment(ec)) {
        const std::string name = gen::ascii_name(d->path());
        if (!(name.starts_with("files-") || name.starts_with("tmp-"))) continue;
        if (keep && gen::iequals(name, keep->folder)) continue;
        std::error_code sec;
        const fs::file_status st = d->symlink_status(sec);
        if (sec || !(fs::is_directory(st) || fs::is_symlink(st))) continue;  // folders (and links) only
        victims.push_back(d->path());
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (const fs::path& v : victims) {
        if (stop.stop_requested()) break;  // wave 4, G L3: the rest waits for the next start (never fatal)
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        const int budget = spent >= 5000 ? 0 : 1000;  // with retries, but never more than about 5 s in all
        if (gen::remove_tree(v, budget, stop)) ++removed;
    }
    gen::remove_file(data_dir / kStampTmpFile, 0);
    return removed;
}

}  // namespace hdf
