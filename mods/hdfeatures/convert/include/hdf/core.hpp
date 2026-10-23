// hdf_convert: shared core types (plan 3.2; owner C2, frozen after wave 1).
//
// The converter library is pure C++23: no SDK, no game headers. Its public headers are also included by the
// mod (src/hdfiles, R1/R2), which force-includes the game's global.h, so these headers must not use the
// names global.h defines as macros (DEBUG, ROUND, TRUNC, SQUARE, M_PI, UNUSED, GLUE, ARRAY_SIZE, VERSION_*,
// REGION_*, PLATFORM_*) and must not rely on M_PI. Use hdf::kPi.
//
// Conventions for every module:
// - Paths inside a Source are '/'-separated, relative, and looked up case-insensitively (common.exists_ci).
// - Output paths ("rel") are relative to the package root as in today's .dusk: "res/..." or "overlay/res/...".
// - Errors: Error{code, detail}. Codes are short dotted ids, e.g. "todo", "stopped", "hd.missing",
//   "hd.version", "hd.hash", "gc.hash", "io.read", "io.write", "fmt.<module>" for a parse failure.
// - Long work checks the stop token at least every 50 ms of work (plan 2.3) and returns Error{"stopped"}.
// - Numbers: std::nearbyint for Python round, floor division for // and % on negatives, std::stable_sort,
//   vectors of pairs where Python relies on dict order, /fp:precise (plan 3.1, libs 3).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdf {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using Bytes = std::vector<u8>;
using View = std::span<const u8>;

// Python's math.pi. Never M_PI: under the game's global.h it is a float.
inline constexpr double kPi = 3.141592653589793;

struct Error {
    std::string code;    // e.g. "hd.missing", "gc.hash", "io.write", "todo"
    std::string detail;  // one short line for the log; never game data
};

template <class T>
using Result = std::expected<T, Error>;

// return hdf::fail("io.read", "...") from any function returning Result<T>.
inline std::unexpected<Error> fail(std::string code, std::string detail = {}) {
    return std::unexpected<Error>(Error{std::move(code), std::move(detail)});
}

// The error every unported function returns in wave 1.
inline std::unexpected<Error> todo(std::string_view what) {
    return fail("todo", std::string(what));
}

// Read-only tree of files: '/' paths, case-insensitive. Implementations: source.hpp (FolderSource, WuaSource,
// LayeredSource, MemorySource, ZipSource) and, in the mod, DvdSource (fills a MemorySource).
// read() must be safe to call from the worker thread; implementations that cache use their own lock.
class Source {
public:
    virtual ~Source() = default;
    virtual std::optional<u64> size(std::string_view path) const = 0;
    virtual Result<Bytes> read(std::string_view path, std::stop_token stop = {}) const = 0;
    virtual std::string describe() const = 0;  // for the log and cache.json; short, may be shortened by the mod
};

// Where outputs go: a folder (FolderSink, the cache in data_dir) or memory (MemorySink, overlays).
class Sink {
public:
    virtual ~Sink() = default;
    virtual Result<void> write(std::string_view rel, View data) = 0;
};

// Written by the worker, read by the game thread (plan 3.2). stage = hdf::GenStage (generate.hpp).
struct Progress {
    std::atomic<u32> stage{0}, done{0}, total{0};  // weighted steps
};

// memfit.model_stats / memfit.arc_stats: the numbers memfit.estimate needs (Correction #11: moved here from
// stage.hpp; produced by j3d, used by stage for MEM0).
struct ModelStats {
    int room = -1;   // room number for a D_SB11 room archive, -1 otherwise
    u64 arc = 0;     // archive bytes (arc_stats 'arc'); 0 for a single model
    u32 models = 0;  // .bmd/.bdl members
    u32 mat = 0;     // MAT3/MAT4/MAT2 counts summed
    u32 shp = 0;     // SHP1 counts
    u32 jnt = 0;     // JNT1 counts
    u32 tex = 0;     // TEX1 counts
    u32 anm = 0;     // .btk .brk .bck .btp .bpk .bva .bla .bxa members
    u32 files = 0;   // all members
    u64 dl = 0;      // j3d_dlcheck.pc_dl_bytes(...)['copy'], summed
};

}  // namespace hdf
