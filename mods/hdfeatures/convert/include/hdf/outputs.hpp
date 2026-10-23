// hdf_convert: the embedded expected-output list, the drop list, the set id and the output checks
// (owner C2; src/base/outputs.cpp, data/expected_outputs.inc made by tools/dev/gen_tables.py from the oracle:
// paths, sizes and SHA-256 only). Ports hdf_builder.check_outputs; png_pixel_hash stays a CLI diagnostic
// (Correction #17).
#pragma once

#include "hdf/core.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hdf {

// One made file. rel as in the package ("res/...", "overlay/res/..."); sha256 of the bytes (lower-case hex).
// (Declared here rather than in generate.hpp so the checks below need no driver header.)
struct OutputRecord {
    std::string rel;
    u64 size = 0;
    std::string sha256;
    bool overlay = false;  // rel starts with "overlay/"
};

namespace outputs {

struct ExpectedFile {
    std::string_view rel;     // sorted by rel (byte order)
    u64 size = 0;
    std::string_view sha256;  // byte SHA-256, PNGs included (Correction #17)
    bool overlay = false;
};

// The 176 kept files (74,408,567 B; 56 overlays = 50,327,200 B). Plan 4 + 7.1.
std::span<const ExpectedFile> expected();
// The 30 files left out by design (1,165,694 B), plan 4 "Drop list".
std::span<const std::string_view> drop_list();

inline constexpr std::size_t kKeptCount = 176;
inline constexpr std::size_t kOverlayCount = 56;
inline constexpr std::size_t kDroppedCount = 30;
inline constexpr u64 kKeptBytes = 74408567;
inline constexpr u64 kOverlayBytes = 50327200;
inline constexpr u64 kDroppedBytes = 1165694;

// The test oracle (Correction #3): cache/oracle/hd_features-oracle-2.0.0.dusk. compare/test refuse another file.
inline constexpr std::string_view kOracleSha256 = "a7a48bf643e621b89e3860fe9d7c48b26e76ec9b6cf518af8c5ff2b42bc68f59";
inline constexpr u64 kOracleSize = 41757577;

const ExpectedFile* find(std::string_view rel);
bool is_dropped(std::string_view rel);

// SHA-256 over the sorted lines "rel \t size \t sha256 \n" (plan 2.4 with Correction #17: byte hashes).
std::string compute_set_id(std::span<const ExpectedFile> files);
// The set id tools/dev/gen_tables.py computed for expected() (self-test checks it equals compute_set_id).
std::string_view embedded_set_id();

struct Verdict {
    enum Kind { Identical, PngPixelsOnly, Diff, Missing, Extra, Dropped } kind = Diff;
    std::string rel;
    std::string detail;  // sizes, first differing offset, archive member if known
};

struct CompareReport {
    std::vector<Verdict> rows;
    u32 identical = 0, png_pixels = 0, diff = 0, missing = 0, extra = 0, dropped = 0;
    // 176 identical (PNG pixel-only differences are accepted and reported), 30 dropped, nothing else.
    bool pass() const { return diff == 0 && missing == 0 && extra == 0; }
    std::string summary() const;  // one line
};

// Made files (records) against the embedded list (hdf_builder.check_outputs).
CompareReport check(std::span<const OutputRecord> made);

// A made folder (the 176 + cache.json) against the oracle (.dusk or an unpacked folder), byte for byte, with
// the PNG pixel fallback. Refuses an oracle .dusk whose SHA-256 is not kOracleSha256.
Result<CompareReport> compare(const std::filesystem::path& out_dir, const std::filesystem::path& oracle,
                              std::stop_token stop = {});

}  // namespace outputs
}  // namespace hdf
