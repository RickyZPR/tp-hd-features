// The embedded input lists and the input checks (C2). Tables: data/inputs.inc (tools/dev/gen_tables.py).
// Correction #1(a): CheckedSource hands a file to a converter only after its full SHA-256 matched.
#include "hdf/inputs.hpp"

#include "base_util.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <system_error>

namespace hdf::inputs {

#include "inputs.inc"

static_assert(std::size(kHdTable) == kHdCount);
static_assert(std::size(kGcTable) == kGcCount);

namespace {

constexpr bool sorted_unique(std::span<const InputFile> t) {
    for (std::size_t i = 1; i < t.size(); ++i)
        if (!(t[i - 1].path < t[i].path)) return false;
    return true;
}
static_assert(sorted_unique(kHdTable) && sorted_unique(kGcTable), "inputs.inc must be sorted by path");

const InputFile* find_in(std::span<const InputFile> t, std::string_view path) {
    const std::string key = lower_key(path);
    auto it = std::lower_bound(t.begin(), t.end(), std::string_view(key),
                               [](const InputFile& f, std::string_view k) { return f.path < k; });
    if (it == t.end() || it->path != key) return nullptr;
    return &*it;
}

std::string size_problem(u64 found, u64 expected) {
    return "size " + std::to_string(found) + " != " + std::to_string(expected);
}

}  // namespace

std::span<const InputFile> hd_files() { return kHdTable; }
std::span<const InputFile> gc_files() { return kGcTable; }
const InputFile* find_hd(std::string_view path) { return find_in(kHdTable, path); }
const InputFile* find_gc(std::string_view path) { return find_in(kGcTable, path); }

CheckReport check_sizes(const Source& src, std::span<const InputFile> list) {
    CheckReport r;
    r.total = static_cast<u32>(list.size());
    for (const InputFile& f : list) {
        const auto n = src.size(f.path);
        if (!n) {
            r.problems.push_back({std::string(f.path), "missing"});
            continue;
        }
        ++r.present;
        if (*n != f.size) {
            r.problems.push_back({std::string(f.path), size_problem(*n, f.size)});
            continue;
        }
        ++r.size_ok;
    }
    return r;
}

Result<CheckReport> check_hashes(const Source& src, std::span<const InputFile> list, Progress* p,
                                 std::stop_token stop) {
    CheckReport r;
    r.total = static_cast<u32>(list.size());
    if (p) {
        u64 total = 0;
        for (const InputFile& f : list) total += f.size;
        p->done = 0;
        p->total = static_cast<u32>(std::min<u64>(total, 0xFFFFFFFFu));
    }
    for (const InputFile& f : list) {
        if (stop.stop_requested()) return fail("stopped");
        const auto n = src.size(f.path);
        if (!n) {
            r.problems.push_back({std::string(f.path), "missing"});
        } else {
            ++r.present;
            if (*n != f.size) {
                r.problems.push_back({std::string(f.path), size_problem(*n, f.size)});
            } else {
                ++r.size_ok;
                auto data = src.read(f.path, stop);
                if (!data) {
                    if (data.error().code == "stopped") return std::unexpected(data.error());
                    r.problems.push_back({std::string(f.path), "read failed: " + data.error().code + " " +
                                                                   data.error().detail});
                } else if (data->size() != f.size) {
                    r.problems.push_back({std::string(f.path), size_problem(data->size(), f.size)});
                } else if (sha256_hex(*data) != f.sha256) {
                    r.problems.push_back({std::string(f.path), "sha256 differs"});
                } else {
                    ++r.hash_ok;
                }
            }
        }
        if (p) p->done += static_cast<u32>(std::min<u64>(f.size, 0xFFFFFFFFu));
    }
    return r;
}

// ---------------------------------------------------------------------------------------------- CheckedSource
CheckedSource::CheckedSource(const Source& inner, std::span<const InputFile> list, std::string kind)
    : inner_(inner), list_(list), kind_(std::move(kind)) {}

std::optional<u64> CheckedSource::size(std::string_view path) const {
    if (!find_in(list_, path)) return std::nullopt;
    return inner_.size(path);
}

Result<Bytes> CheckedSource::read(std::string_view path, std::stop_token stop) const {
    const InputFile* f = find_in(list_, path);
    if (!f) return fail(kind_ + ".unlisted", std::string(path));
    const auto n = inner_.size(f->path);
    if (!n) return fail(kind_ + ".missing", std::string(f->path));
    if (*n != f->size) return fail(kind_ + ".size", std::string(f->path) + ": " + size_problem(*n, f->size));
    auto data = inner_.read(f->path, stop);
    if (!data) {
        if (data.error().code == "io.missing") return fail(kind_ + ".missing", std::string(f->path));
        return data;
    }
    if (data->size() != f->size)
        return fail(kind_ + ".size", std::string(f->path) + ": " + size_problem(data->size(), f->size));
    if (sha256_hex(*data) != f->sha256) return fail(kind_ + ".hash", std::string(f->path));
    return data;
}

std::string CheckedSource::describe() const { return inner_.describe() + " (" + kind_ + ", hash-checked)"; }

// ---------------------------------------------------------------------------------------------- open_hd_location
namespace {

namespace fs = std::filesystem;

// Never throws (fix-wave1 L1): only used for messages and for comparisons with ASCII names, which a U+FFFD
// replacement can't match by accident.
std::string utf8_of(const fs::path& p) { return base::utf8_lossy(p); }

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool is_hex(std::string_view s) {
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool is_tphd_low(std::string_view low8) {
    const std::string u = upper(std::string(low8));
    return u == kTitleUsa || u == kTitleEur || u == kTitleJpn;
}

// "<16 hex>_v<digits>" -> title id (upper-case), else "".
std::string top_title(std::string_view top) {
    if (top.size() < 19 || top[16] != '_' || (top[17] != 'v' && top[17] != 'V')) return {};
    if (!is_hex(top.substr(0, 16))) return {};
    for (char c : top.substr(18))
        if (c < '0' || c > '9') return {};
    return upper(std::string(top.substr(0, 16)));
}

// <title_id ...>XXXXXXXXXXXXXXXX< in a meta.xml (bounded search; the text is untrusted, Correction #1(b)).
std::string meta_title(std::string_view xml) {
    const std::size_t a = xml.find("<title_id");
    if (a == std::string_view::npos) return {};
    const std::size_t b = xml.find('>', a);
    if (b == std::string_view::npos) return {};
    std::size_t i = b + 1;
    while (i < xml.size() && (xml[i] == ' ' || xml[i] == '\t' || xml[i] == '\r' || xml[i] == '\n')) ++i;
    if (i + 16 > xml.size() || !is_hex(xml.substr(i, 16))) return {};
    return upper(std::string(xml.substr(i, 16)));
}

Result<std::unique_ptr<LayeredSource>> open_wua(const fs::path& p) {
    auto archive = WuaArchive::open(p);
    if (!archive) return std::unexpected(archive.error());
    std::shared_ptr<const WuaArchive> a = *archive;
    struct Top {
        std::string name, title;
    };
    std::vector<Top> tp;
    std::string seen;
    for (const std::string& top : a->tops()) {
        if (!a->has_folder(top + "/content")) continue;
        std::string tid;
        if (a->size(top + "/meta/meta.xml").value_or(0) <= 64 * 1024) {
            if (auto meta = a->read(top + "/meta/meta.xml")) tid = meta_title(std::string_view(
                reinterpret_cast<const char*>(meta->data()), meta->size()));
        }
        if (tid.empty()) tid = top_title(top);
        if (!seen.empty()) seen += ", ";
        seen += tid.empty() ? top : tid;
        if (tid.size() == 16 && is_tphd_low(std::string_view(tid).substr(8))) tp.push_back({top, tid});
    }
    if (tp.empty())
        return fail("hd.not_tphd", utf8_of(p.filename()) + " holds no Twilight Princess HD (titles: " +
                                       (seen.empty() ? std::string("none") : seen) + ")");
    // updates (0005000E) first, then the base (00050000); otherwise the archive's sorted order
    std::stable_sort(tp.begin(), tp.end(), [](const Top& x, const Top& y) {
        return (x.title.compare(0, 8, "0005000E") == 0) > (y.title.compare(0, 8, "0005000E") == 0);
    });
    auto src = std::make_unique<LayeredSource>();
    for (const Top& t : tp) src->add(std::make_unique<WuaSource>(a, t.name));
    return src;
}

// .../00050000/<low> -> .../0005000e/<low> when that update has content/ (hdf_inputs._sibling_update).
std::optional<fs::path> sibling_update(const fs::path& title_dir) {
    const fs::path t = title_dir.lexically_normal();
    const fs::path low = t.filename(), group = t.parent_path();
    if (base::lower_ascii(utf8_of(group.filename())) != "00050000") return std::nullopt;
    std::error_code ec;
    for (fs::directory_iterator d(group.parent_path(), ec), end; !ec && d != end; d.increment(ec)) {
        if (base::lower_ascii(utf8_of(d->path().filename())) != "0005000e") continue;
        const fs::path cand = d->path() / low;
        if (is_dir(cand / "content")) return cand;
    }
    return std::nullopt;
}

bool has_nus_files(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator d(dir, ec), end; !ec && d != end; d.increment(ec)) {
        const std::string ext = base::lower_ascii(utf8_of(d->path().extension()));
        if (ext == ".app" || ext == ".tmd") return true;
    }
    return false;
}

}  // namespace

Result<std::unique_ptr<LayeredSource>> open_hd_location(const fs::path& p) {
    if (is_file(p)) {
        const std::string ext = base::lower_ascii(utf8_of(p.extension()));
        if (ext == ".wux" || ext == ".wud")
            return fail("hd.encrypted", utf8_of(p.filename()) + " is an encrypted disc dump: convert it to .wua in Cemu");
        if (ext == ".app" || ext == ".tmd" || ext == ".tik" || ext == ".h3")
            return fail("hd.encrypted", "NUS files are encrypted: install the game in Cemu or convert it to .wua");
        return open_wua(p);
    }
    if (!is_dir(p)) return fail("hd.not_found", utf8_of(p));
    fs::path content, title_dir;
    if (is_file(p / "los.bin") || is_dir(p / "res" / "Stage")) {
        content = p;
        title_dir = p.parent_path();
    } else if (is_dir(p / "content")) {
        content = p / "content";
        title_dir = p;
    } else {
        std::vector<fs::path> subs;
        std::error_code ec;
        u32 n = 0;
        for (fs::directory_iterator d(p, ec), end; !ec && d != end && ++n <= 20000; d.increment(ec)) {
            std::error_code tec;
            if (d->is_directory(tec) && is_dir(d->path() / "content")) subs.push_back(d->path());
        }
        if (subs.size() != 1) {
            if (has_nus_files(p))
                return fail("hd.encrypted", "NUS files are encrypted: install the game in Cemu or convert it to .wua");
            return fail("hd.not_found", utf8_of(p) + " is not a Twilight Princess HD folder (no content/ with los.bin)");
        }
        title_dir = subs[0];
        content = subs[0] / "content";
    }
    auto src = std::make_unique<LayeredSource>();
    if (auto upd = sibling_update(title_dir)) src->add(std::make_unique<FolderSource>(*upd / "content"));
    src->add(std::make_unique<FolderSource>(content));
    return src;
}

}  // namespace hdf::inputs
