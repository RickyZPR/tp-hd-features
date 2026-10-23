// The embedded expected-output list, the drop list, the set id and the output checks (C2).
// Tables: data/expected_outputs.inc (tools/dev/gen_tables.py, from the pinned oracle). Ports
// hdf_builder.check_outputs; the PNG pixel comparison is only a diagnostic (Correction #17).
#include "hdf/outputs.hpp"

#include "hdf/io.hpp"
#include "hdf/source.hpp"

#include "base_util.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>

namespace hdf::outputs {

#include "expected_outputs.inc"

static_assert(std::size(kExpectedTable) == kKeptCount);
static_assert(std::size(kDropTable) == kDroppedCount);
static_assert(kExpectedTableBytes == kKeptBytes);
static_assert(kDropTableBytes == kDroppedBytes);
static_assert(kTableOracleSha256 == kOracleSha256 && kTableOracleSize == kOracleSize,
              "data/expected_outputs.inc was made from another oracle than outputs.hpp pins");

namespace {

constexpr bool tables_ok() {
    std::size_t overlays = 0;
    u64 overlay_bytes = 0;
    for (std::size_t i = 0; i < std::size(kExpectedTable); ++i) {
        if (i > 0 && !(kExpectedTable[i - 1].rel < kExpectedTable[i].rel)) return false;
        if (kExpectedTable[i].sha256.size() != 64) return false;
        if (kExpectedTable[i].overlay != kExpectedTable[i].rel.starts_with("overlay/")) return false;
        if (kExpectedTable[i].overlay) {
            ++overlays;
            overlay_bytes += kExpectedTable[i].size;
        }
    }
    for (std::size_t i = 1; i < std::size(kDropTable); ++i)
        if (!(kDropTable[i - 1] < kDropTable[i])) return false;
    return overlays == kOverlayCount && overlay_bytes == kOverlayBytes;
}
static_assert(tables_ok(), "expected_outputs.inc: order, hashes or overlay totals are off");

// The package files that are not converter outputs (never "extra").
constexpr std::string_view kPackageOnly[] = {"mod.json", "lib/windows-amd64/mod.dll", "res/icon.png",
                                             "res/banner.png"};

bool is_package_only(std::string_view rel) {
    return std::find(std::begin(kPackageOnly), std::end(kPackageOnly), rel) != std::end(kPackageOnly);
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    return base::lower_ascii(s.substr(s.size() - suffix.size())) == suffix;
}

std::string hex_offset(u64 v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// "size A vs B, first difference at 0x.." plus the RARC member when io can read both archives.
std::string diff_detail(std::string_view rel, View made, View oracle) {
    std::string d = "size " + std::to_string(made.size()) + " vs oracle " + std::to_string(oracle.size());
    const auto mm = std::mismatch(made.begin(), made.end(), oracle.begin(), oracle.end());
    d += ", first difference at " + hex_offset(static_cast<u64>(mm.first - made.begin()));
    if (ends_with_ci(rel, ".arc")) {
        auto a = io::rarc_read(made);
        auto b = io::rarc_read(oracle);
        if (a && b) {
            std::map<std::string, const io::RarcMember*, std::less<>> theirs;
            for (const io::RarcMember& m : b->members) theirs.emplace(m.path, &m);
            for (const io::RarcMember& m : a->members) {
                auto it = theirs.find(m.path);
                if (it == theirs.end()) {
                    d += ", member only in the made file: " + m.path;
                    break;
                }
                const View x = a->raw(m), y = b->raw(*it->second);
                if (!std::equal(x.begin(), x.end(), y.begin(), y.end())) {
                    d += ", first differing member: " + m.path;
                    break;
                }
            }
            if (a->members.size() != b->members.size())
                d += ", members " + std::to_string(a->members.size()) + " vs " + std::to_string(b->members.size());
        }
    }
    return d;
}

}  // namespace

std::span<const ExpectedFile> expected() { return kExpectedTable; }
std::span<const std::string_view> drop_list() { return kDropTable; }
std::string_view embedded_set_id() { return kTableSetId; }

const ExpectedFile* find(std::string_view rel) {
    auto it = std::lower_bound(std::begin(kExpectedTable), std::end(kExpectedTable), rel,
                               [](const ExpectedFile& f, std::string_view r) { return f.rel < r; });
    if (it == std::end(kExpectedTable) || it->rel != rel) return nullptr;
    return &*it;
}

bool is_dropped(std::string_view rel) { return std::binary_search(std::begin(kDropTable), std::end(kDropTable), rel); }

std::string compute_set_id(std::span<const ExpectedFile> files) {
    std::vector<std::string> lines;
    lines.reserve(files.size());
    for (const ExpectedFile& f : files)
        lines.push_back(std::string(f.rel) + '\t' + std::to_string(f.size) + '\t' + std::string(f.sha256) + '\n');
    std::sort(lines.begin(), lines.end());
    Sha256 h;
    for (const std::string& l : lines) h.update(View(reinterpret_cast<const u8*>(l.data()), l.size()));
    return h.finish_hex();
}

std::string CompareReport::summary() const {
    const u32 accounted = identical + png_pixels + dropped;
    const std::size_t total = kKeptCount + kDroppedCount;
    char b[256];
    std::snprintf(b, sizeof b,
                  "compare: %u identical, %u PNG pixels only, %u DIFF, %u missing, %u extra, %u dropped by design; "
                  "%u/%zu accounted for: %s",
                  identical, png_pixels, diff, missing, extra, dropped, accounted, total, pass() ? "PASS" : "FAIL");
    return b;
}

CompareReport check(std::span<const OutputRecord> made) {
    CompareReport r;
    std::map<std::string_view, const OutputRecord*> by_rel;
    for (const OutputRecord& m : made) by_rel.emplace(m.rel, &m);
    for (const ExpectedFile& e : kExpectedTable) {
        auto it = by_rel.find(e.rel);
        if (it == by_rel.end()) {
            r.rows.push_back({Verdict::Missing, std::string(e.rel), {}});
            ++r.missing;
            continue;
        }
        const OutputRecord& m = *it->second;
        if (m.size == e.size && m.sha256 == e.sha256) {
            r.rows.push_back({Verdict::Identical, std::string(e.rel), {}});
            ++r.identical;
        } else {
            r.rows.push_back({Verdict::Diff, std::string(e.rel),
                              "size " + std::to_string(m.size) + " vs " + std::to_string(e.size) +
                                  (m.sha256 == e.sha256 ? "" : ", sha256 differs")});
            ++r.diff;
        }
        by_rel.erase(it);
    }
    for (const auto& [rel, m] : by_rel) {
        if (is_dropped(rel)) {
            r.rows.push_back({Verdict::Dropped, std::string(rel), "made anyway"});
            ++r.dropped;
        } else {
            r.rows.push_back({Verdict::Extra, std::string(rel), {}});
            ++r.extra;
        }
    }
    std::stable_sort(r.rows.begin(), r.rows.end(), [](const Verdict& a, const Verdict& b) { return a.rel < b.rel; });
    return r;
}

Result<CompareReport> compare(const std::filesystem::path& out_dir, const std::filesystem::path& oracle,
                              std::stop_token stop) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto u8s = [](const fs::path& p) { return base::utf8_lossy(p); };

    // The oracle: the pinned .dusk (Correction #3), or a folder whose kept files equal the pinned table.
    std::unique_ptr<Source> theirs;
    bool oracle_is_zip = false;
    if (fs::is_regular_file(oracle, ec)) {
        auto h = base::sha256_file(oracle, stop);
        if (!h) return std::unexpected(h.error());
        if (h->first != kOracleSize || h->second != kOracleSha256)
            return fail("oracle.pin", u8s(oracle.filename()) + " is not the pinned oracle (size " +
                                          std::to_string(h->first) + ", sha256 " + h->second + ")");
        auto z = ZipSource::open(oracle);
        if (!z) return std::unexpected(z.error());
        theirs = std::move(*z);
        oracle_is_zip = true;
    } else if (fs::is_directory(oracle, ec)) {
        theirs = std::make_unique<FolderSource>(oracle);
    } else {
        return fail("oracle.missing", u8s(oracle));
    }

    // The made side: a folder (the data dir with files-<set8>, or the files folder itself) or a .dusk/.zip.
    std::unique_ptr<Source> mine;
    std::vector<std::string> mine_names;
    if (fs::is_regular_file(out_dir, ec)) {
        auto z = ZipSource::open(out_dir);
        if (!z) return std::unexpected(z.error());
        mine_names = (*z)->list();
        mine = std::move(*z);
    } else if (fs::is_directory(out_dir, ec)) {
        fs::path root = out_dir;
        const std::string set = std::string(embedded_set_id());
        const fs::path files = out_dir / ("files-" + set.substr(0, 8));
        const bool data_dir = fs::is_directory(files, ec);
        if (data_dir) root = files;
        auto f = std::make_unique<FolderSource>(root);
        auto names = f->list(stop);
        if (!names) return std::unexpected(names.error());
        for (std::string& n : *names) {
            if (!data_dir && (n == "cache.json" || n == "cache.json.tmp")) continue;
            mine_names.push_back(std::move(n));
        }
        mine = std::move(f);
    } else {
        return fail("out.missing", u8s(out_dir));
    }

    CompareReport r;
    std::map<std::string, std::string> mine_by_key;  // lower key -> real name (folders ignore case)
    for (const std::string& n : mine_names)
        if (!is_package_only(n)) mine_by_key.emplace(lower_key(n), n);

    for (const ExpectedFile& e : kExpectedTable) {
        if (stop.stop_requested()) return fail("stopped");
        const std::string rel(e.rel);
        auto ob = theirs->read(rel, stop);
        if (!ob) {
            if (ob.error().code == "stopped") return std::unexpected(ob.error());
            return fail("oracle.pin", "the oracle has no " + rel);
        }
        if (ob->size() != e.size || sha256_hex(*ob) != e.sha256)
            return fail("oracle.pin", oracle_is_zip ? "the embedded table differs from the oracle at " + rel
                                                    : "the oracle folder differs from the pinned oracle at " + rel);
        auto it = mine_by_key.find(lower_key(rel));
        if (it == mine_by_key.end()) {
            r.rows.push_back({Verdict::Missing, rel, {}});
            ++r.missing;
            continue;
        }
        const std::string note = it->second == rel ? std::string() : " (stored as " + it->second + ")";
        mine_by_key.erase(it);
        auto mb = mine->read(rel, stop);
        if (!mb) {
            if (mb.error().code == "stopped") return std::unexpected(mb.error());
            r.rows.push_back({Verdict::Diff, rel, "unreadable: " + mb.error().code + " " + mb.error().detail});
            ++r.diff;
            continue;
        }
        if (*mb == *ob) {
            r.rows.push_back({Verdict::Identical, rel, note});
            ++r.identical;
            continue;
        }
        if (ends_with_ci(rel, ".png")) {
            const std::string a = io::png_pixel_hash(*mb), b = io::png_pixel_hash(*ob);
            if (!a.empty() && a == b) {
                r.rows.push_back({Verdict::PngPixelsOnly, rel, "deflate bytes differ, pixels identical" + note});
                ++r.png_pixels;
                continue;
            }
        }
        r.rows.push_back({Verdict::Diff, rel, diff_detail(rel, *mb, *ob) + note});
        ++r.diff;
    }

    for (std::string_view d : kDropTable) {
        if (stop.stop_requested()) return fail("stopped");
        const std::string rel(d);
        std::string detail;
        auto it = mine_by_key.find(lower_key(rel));
        if (it == mine_by_key.end()) {
            detail = "not made";
        } else {
            mine_by_key.erase(it);
            auto mb = mine->read(rel, stop);
            auto ob = theirs->read(rel, stop);
            if (!mb || !ob) {
                if ((!mb && mb.error().code == "stopped") || (!ob && ob.error().code == "stopped")) return fail("stopped");
                detail = !ob ? "made; not in the oracle" : "made; unreadable";
            } else {
                detail = *mb == *ob ? "made, identical" : "made, differs (not checked)";
            }
        }
        r.rows.push_back({Verdict::Dropped, rel, detail});
        ++r.dropped;
    }

    for (const auto& [key, name] : mine_by_key) {
        r.rows.push_back({Verdict::Extra, name, {}});
        ++r.extra;
    }
    std::stable_sort(r.rows.begin(), r.rows.end(), [](const Verdict& a, const Verdict& b) { return a.rel < b.rel; });
    return r;
}

}  // namespace hdf::outputs
