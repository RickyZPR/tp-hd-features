// Private helpers shared by src/io/*.cpp (module C1). Not part of the public API.
#pragma once

#include "hdf/io.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace hdf::io {

// Largest inflate output accepted (gzip packs, PNG IDAT). The largest build input inflates to 24,238,112 B
// (res/object/filesel.pack.gz, VERIFIED by the C1 golden list). libs 2's 53.6 MB most likely counted one of the
// two icon-only packs, which are not build inputs (INFERRED).
inline constexpr std::size_t kMaxInflate = std::size_t(1) << 30;

// One zlib stream (windowBits 15), as Python's zlib.decompress. Data after the stream end is ignored.
Result<Bytes> zlib_inflate(View in, std::size_t max_out, std::stop_token stop = {});

// ASCII lower case (Python str.lower() for the ASCII names these formats use).
inline std::string ascii_lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

inline bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace hdf::io
