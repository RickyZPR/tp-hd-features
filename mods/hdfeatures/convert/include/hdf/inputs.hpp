// hdf_convert: the embedded input lists and the input checks (owner C2; src/base/inputs.cpp,
// data/inputs.inc made by tools/dev/gen_tables.py: paths, sizes and SHA-256 only, no game bytes).
//
// Correction #1(a): every HD and GC input is hashed in full and compared with the embedded SHA-256 before any
// format parser sees it. Converters only ever get inputs through CheckedSource, so they only run on the exact
// bytes the oracle tests cover.
#pragma once

#include "hdf/core.hpp"
#include "hdf/source.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::inputs {

struct InputFile {
    std::string_view path;    // lower-case, '/' separated. HD: under the title's content/. GC: disc path, no '/'.
    u64 size = 0;
    std::string_view sha256;  // lower-case hex
};

// The 136 TP HD files (tools/dist/hd_files.txt minus the 2 icon-only packs), USA v81 = the [AZAP01] bytes.
std::span<const InputFile> hd_files();
// The 13 GC files (plan 2.5): res/Msgus/bmgres{,1..8,99}.arc, res/Fontus/fontres.arc,
// res/Stage/R_SP160/STG_00.arc, res/Particle/Pscene200.jpc.
std::span<const InputFile> gc_files();

inline constexpr std::size_t kHdCount = 136;
inline constexpr std::size_t kGcCount = 13;
inline constexpr std::string_view kGcGameId = "GZ2E01";
// GZ2E01 particle budget: largest res/Particle/Pscene*.jpc (Pscene020.jpc, 271,136 B) - particles.SAFETY 4,096.
inline constexpr u64 kPsceneLargest = 271136;
inline constexpr u64 kPsceneSafety = 4096;
inline constexpr u64 kPsceneBudget = kPsceneLargest - kPsceneSafety;  // 267,040
// TP HD base title ids (low half). Identity comes from the files, not from these (runtime 8).
inline constexpr std::string_view kTitleUsa = "1019E500";
inline constexpr std::string_view kTitleEur = "1019E600";
inline constexpr std::string_view kTitleJpn = "1019C800";
inline constexpr u32 kHdUpdateVersion = 81;

const InputFile* find_hd(std::string_view path);  // case-insensitive, nullptr if not listed
const InputFile* find_gc(std::string_view path);

struct Problem {
    std::string path;
    std::string what;  // "missing", "size <found> != <expected>", "sha256 differs"
};

struct CheckReport {
    u32 total = 0, present = 0, size_ok = 0, hash_ok = 0;
    std::vector<Problem> problems;
    bool ok() const { return problems.empty() && total > 0 && size_ok == total; }
};

// Existence + size only (fast; detection uses it to pick a candidate).
CheckReport check_sizes(const Source& src, std::span<const InputFile> list);
// Full SHA-256 of every file (check-inputs, and the start of generate()). p.done/p.total count bytes.
Result<CheckReport> check_hashes(const Source& src, std::span<const InputFile> list, Progress* p,
                                 std::stop_token stop = {});

// One TP HD location as layers, update first (hdf_inputs.open_hd without the Cemu lookups, which belong to
// detect): a .wua (every top "<16 hex>_v<N>" of a TP HD title id that has content/, 0005000E updates before
// the base), a content folder (los.bin or res/Stage), a title folder (content/), or a folder holding exactly
// one title folder; a title folder at .../00050000/<id> also gets the installed update .../0005000e/<id>.
// Errors: "hd.encrypted" (.wux .wud, NUS .app .tmd .tik .h3), "hd.not_found", "hd.not_tphd", "wua.*", "io.*".
// The CLI's --hd uses it; detect::open may build on it.
Result<std::unique_ptr<LayeredSource>> open_hd_location(const std::filesystem::path& p);

// Wraps a Source: read() returns a listed file only after its full SHA-256 matched the embedded value.
// Errors: "<kind>.unlisted" (path not in the list), "<kind>.missing", "<kind>.size", "<kind>.hash"
// with kind "hd" or "gc". size() answers only for listed paths.
class CheckedSource final : public Source {
public:
    CheckedSource(const Source& inner, std::span<const InputFile> list, std::string kind);
    std::optional<u64> size(std::string_view path) const override;
    Result<Bytes> read(std::string_view path, std::stop_token stop = {}) const override;
    std::string describe() const override;

private:
    const Source& inner_;
    std::span<const InputFile> list_;
    std::string kind_;
};

}  // namespace hdf::inputs
