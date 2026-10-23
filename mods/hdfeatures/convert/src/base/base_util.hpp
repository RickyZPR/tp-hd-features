// Internal helpers of src/base (C2): file access in bounded pieces, path keys, sink path checks.
// Not a public header: only src/base/*.cpp, the CLI (cli/*.cpp) and the C2 unit tests include it.
#pragma once

#include "hdf/core.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace hdf::base {

// UTF-8 text of a path. Never throws (fix-wave1 L1): NTFS allows names with an unpaired UTF-16 surrogate, and
// std::filesystem::path::u8string() throws on them.
//   utf8_exact: nothing when the path holds such a name. Listings skip those entries (a name that can't be
//               written as UTF-8 is never one of the fixed input names).
//   utf8_lossy: such characters become U+FFFD. For messages and for comparisons with ASCII names only.
// generic = true writes '/' as the separator (path::generic_u8string()).
std::optional<std::string> utf8_exact(const std::filesystem::path& p, bool generic = false);
std::string utf8_lossy(const std::filesystem::path& p, bool generic = false);

// An OS error as "<text> (os error N)" for an "io.write" detail (wave 4 review L3): the text is in the Windows
// display language, so the mod picks its panel words by the number N (src/hdfiles/lines.cpp). Other categories:
// the text only.
inline std::string os_error_text(const std::error_code& ec) {
    std::string text = ec.message();
    if (ec.category() == std::system_category()) text += " (os error " + std::to_string(ec.value()) + ")";
    return text;
}

// Correction #5: never read more than this in one call, and check the stop token between pieces.
inline constexpr u64 kReadPiece = u64(4) << 20;
// No single input or oracle entry is near this; it only stops a damaged size field from asking for all RAM.
inline constexpr u64 kMaxFileBytes = u64(1) << 30;

inline char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
std::string lower_ascii(std::string_view s);

// The whole file, read in pieces of kReadPiece with a stop check between them. Errors: "io.missing",
// "io.open", "io.read", "io.too_big", "stopped".
Result<Bytes> read_file(const std::filesystem::path& p, std::stop_token stop = {}, u64 max = kMaxFileBytes);

// SHA-256 of a whole file, read in pieces with stop checks: {size, lower-case hex}.
Result<std::pair<u64, std::string>> sha256_file(const std::filesystem::path& p, std::stop_token stop = {});

// A file opened once for reads at given offsets (WuaArchive, ZipSource). Not thread-safe by itself: the owner
// serialises calls.
class RandomFile {
public:
    static Result<std::unique_ptr<RandomFile>> open(const std::filesystem::path& p);
    ~RandomFile();
    RandomFile(const RandomFile&) = delete;
    RandomFile& operator=(const RandomFile&) = delete;
    u64 size() const { return size_; }
    // Exactly n bytes at offset into out (out must hold n). Fails past the end ("io.read").
    Result<void> read_at(u64 offset, u8* out, u64 n);

private:
    RandomFile() = default;
    void* handle_ = nullptr;  // HANDLE on Windows, FILE* elsewhere
    u64 size_ = 0;
};

// rel for a Sink: relative, '/'-separated (a '\\' is refused), no empty, "." or ".." parts, no ':' (drive
// letters, NTFS streams), no control characters, at most 32 parts and 400 bytes.
bool safe_rel(std::string_view rel);

}  // namespace hdf::base
