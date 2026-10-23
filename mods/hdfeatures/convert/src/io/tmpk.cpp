// TMPK packs (module C1). Port of tools/formats/hdpack.py: Tmpk (header, entry table, NUL-terminated names),
// Tmpk.data, Tmpk.get, and read_any + Tmpk for a .pack.gz.
//
//   0x00 'TMPK'  u32 num_files  u32 alignment  u32 0
//   0x10 num_files x { u32 name_off, u32 data_off, u32 size, u32 0 }   (offsets from the start)
#include "io_internal.hpp"

#include <algorithm>
#include <cstring>

namespace hdf::io {

namespace {

constexpr std::size_t kMaxName = 255;

// os.path.basename as Python runs it on Windows (ntpath): the part after the last '/' or '\'.
std::string_view basename_nt(std::string_view p) {
    const std::size_t i = p.find_last_of("/\\");
    return i == std::string_view::npos ? p : p.substr(i + 1);
}

}  // namespace

View Tmpk::data(const TmpkEntry& e) const {
    const u64 b0 = std::min<u64>(buf.size(), e.off);
    const u64 b1 = std::min<u64>(buf.size(), static_cast<u64>(e.off) + e.size);  // Python slices clamp
    return View(buf.data() + b0, static_cast<std::size_t>(b1 - b0));
}

// Tmpk.get: the first entry whose name is `name`, ends with "/name", or whose basename is `name`.
std::optional<View> Tmpk::get(std::string_view name) const {
    const std::string slash = "/" + std::string(name);
    for (const TmpkEntry& e : entries)
        if (e.name == name || ends_with(e.name, slash) || basename_nt(e.name) == name) return data(e);
    return std::nullopt;
}

Result<Tmpk> tmpk_read(Bytes inflated) {
    Tmpk t;
    t.buf = std::move(inflated);
    const View b = t.buf;
    const u64 size = b.size();
    if (size < 4 || std::memcmp(b.data(), "TMPK", 4) != 0) return fail("fmt.io", "not TMPK");
    if (size < 16) return fail("fmt.io", "TMPK: header truncated");
    t.count = be32(b, 4);
    t.alignment = be32(b, 8);
    t.unk = be32(b, 12);
    if (0x10 + static_cast<u64>(t.count) * 16 > size) return fail("fmt.io", "TMPK: entry table out of range");
    t.entries.reserve(t.count);
    for (u32 i = 0; i < t.count; ++i) {
        const std::size_t o = 0x10 + static_cast<std::size_t>(i) * 16;
        const u32 no = be32(b, o);
        if (no >= size) return fail("fmt.io", "TMPK: name out of range");
        // at most kMaxName bytes before the NUL (fix-wave1, as RARC L2: entries naming one huge "name" would
        // otherwise copy it once per entry). Real packs: longest name 95 B (61 packs, VERIFIED fix-wave1).
        const std::size_t span = static_cast<std::size_t>(std::min<u64>(size - no, kMaxName + 1));
        const void* z = std::memchr(b.data() + no, 0, span);
        if (!z) return fail("fmt.io", "TMPK: name not terminated or longer than 255 bytes");
        TmpkEntry e;
        e.name.assign(reinterpret_cast<const char*>(b.data() + no), static_cast<const u8*>(z) - (b.data() + no));
        e.off = be32(b, o + 4);
        e.size = be32(b, o + 8);
        t.entries.push_back(std::move(e));
    }
    return t;
}

Result<Tmpk> tmpk_from_gz(View pack_gz, std::stop_token stop) {
    auto raw = read_any(pack_gz, stop);
    if (!raw) return std::unexpected(raw.error());
    return tmpk_read(std::move(*raw));
}

}  // namespace hdf::io
