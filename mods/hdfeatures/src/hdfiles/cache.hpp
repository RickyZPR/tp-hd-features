#pragma once

// HD Features: the stored game files in the mod's data folder (module R1; plan 2.4 with Corrections #9, #10
// and #15).
//
//   <data_dir>\cache.json                the stamp (hdf::read_stamp / validate_stamp, module G)
//   <data_dir>\files-<set8>\res\...      the 120 res files, same relative paths as the old package
//   <data_dir>\files-<set8>\overlay\...  the 56 overlay files
//
// No game headers and no SDK here: only the converter library (hdf_convert) and the standard library, so the
// setup code (R2) can use it too. Include it before any game header (the forced global.h is fine; dolphin
// headers are not: they turn nullptr into a macro).
//
// Every file is hashed when it is read, against the stamp (Correction #10). The stamp's list equals the list
// built into the mod (validate_stamp checks that), so a file that passes is exactly the file the oracle tests.

#include "hdf/core.hpp"
#include "hdf/generate.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hdfiles::cache {

struct Entry {
    std::string rel;           // "res/..." or "overlay/res/..." (as in the stamp)
    std::uint64_t size = 0;
    std::string sha256;        // lower-case hex
    bool overlay = false;
};

class Cache {
public:
    // read_stamp + validate_stamp (module G). Errors: "cache.missing", "cache.bad", "cache.set" (another mod
    // version made it), "cache.incomplete" (a file is missing or has another size).
    static hdf::Result<Cache> open(const std::filesystem::path& data_dir);

    const std::filesystem::path& data_dir() const { return data_dir_; }
    const std::filesystem::path& files_dir() const { return files_dir_; }
    const hdf::Stamp& stamp() const { return stamp_; }
    std::string_view set8() const { return std::string_view(stamp_.set).substr(0, 8); }

    // rel as in the stamp ("res/stamps/...", "overlay/res/..."); looked up case-insensitively.
    const Entry* find(std::string_view rel) const;
    std::filesystem::path path_of(const Entry& entry) const;
    // The whole file, checked against the stamp: size, then SHA-256. Errors: "cache.incomplete" (gone),
    // "cache.hash" (other size or other bytes), "io.read" (can't be opened or read now, e.g. locked: not damage).
    hdf::Result<hdf::Bytes> read(const Entry& entry) const;
    hdf::Result<hdf::Bytes> read(std::string_view rel) const;

    // The 56 overlay entries, in stamp order (sorted by rel).
    std::vector<const Entry*> overlays() const;
    std::size_t file_count() const { return entries_.size(); }
    std::uint64_t overlay_bytes() const;

private:
    std::filesystem::path data_dir_, files_dir_;
    hdf::Stamp stamp_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;  // hdf::lower_key(rel) -> entries_
};

// Correction #15: while R2's worker makes files it holds <data_dir>\lock open without sharing. True when that
// file exists and can't be opened now (another Dusklight is making files in this folder).
bool another_instance_is_making(const std::filesystem::path& data_dir);

// Delete the files-* / tmp-* folders the valid stamp doesn't name (hdf::cleanup; never fatal). Skipped while
// another instance is making files. Returns the number removed.
std::uint32_t cleanup_leftovers(const Cache& cache);

// Correction #10: a stored file failed its check. Remove cache.json, so the next start makes the files again
// (the next start's cleanup then removes the folder). Never fatal; returns false if cache.json stays.
bool drop(const std::filesystem::path& data_dir);

// "C:/Users/<name>/AppData/..." -> "%APPDATA%/..." style shortening for log lines (Correction #19: players share
// logs). Falls back to the last 2 parts of the path.
std::string short_path(const std::filesystem::path& path);

// UTF-8 <-> path, never throwing.
std::filesystem::path u8_path(std::string_view utf8);
std::string path_utf8(const std::filesystem::path& path);

}  // namespace hdfiles::cache
