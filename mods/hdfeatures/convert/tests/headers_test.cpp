// Compile check: every public header must build in the mod, which force-includes the game's global.h. This file
// defines the same macro names first (values as global.h, include/global.h of Dusklight v2.0.3) plus the
// dolphin-style global typedefs, then includes all public headers. It needs no game header.
//
// The standard headers come first: aurora's dolphin/types.h defines nullptr as NULL under MSVC without
// /Zc:__cplusplus (the mod's flags), and MSVC's <stop_token>/<utility> do not compile after that macro
// (VERIFIED here). So mod sources (src/hdfiles, R1/R2) must include hdf/*.hpp and the standard headers
// before any game header. global.h itself does not pull dolphin/types.h into mods (only with DUSK_BUILDING_GAME).
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;
typedef std::int8_t s8;
typedef std::int16_t s16;
typedef std::int32_t s32;
typedef std::int64_t s64;
typedef float f32;
typedef double f64;

#define _global_h_
#define VERSION_GCN_USA 0
#define VERSION_GCN_PAL 1
#define VERSION_GCN_JPN 2
#define PLATFORM_GCN (VERSION >= VERSION_GCN_USA && VERSION <= VERSION_GCN_JPN)
#define REGION_USA (VERSION == VERSION_GCN_USA)
#define DEBUG 0
#define MSL_INLINE inline
#define ARRAY_SIZE(o) (s32)(sizeof(o) / sizeof(o[0]))
#define ARRAY_SIZEU(o) (sizeof(o) / sizeof(o[0]))
#define ALIGN_PREV(X, N) ((X) & ~((N)-1))
#define ALIGN_NEXT(X, N) ALIGN_PREV(((X) + (N)-1), N)
#define IS_ALIGNED(X, N) (((X) & ((N)-1)) == 0)
#define ROUND(n, a) (((u32)(n) + (a)-1) & ~((a)-1))
#define TRUNC(n, a) (((u32)(n)) & ~((a)-1))
#define UNUSED(x) ((void)(x))
#define GLUE(a, b) a##b
#define GLUE2(a, b) GLUE(a, b)
#define STATIC_ASSERT(...)
#define M_PI 3.14159265358979323846f
#define COMPOUND_LITERAL(x) (x)
#define FAST_DIV(x, n) (x >> (n / 2))
#define SQUARE(x) ((x) * (x))
#define TYPEOF(value) decltype(value)
#define POINTER_ADD_TYPE(type_, ptr_, offset_) ((type_)((uintptr_t)(ptr_) + (uintptr_t)(offset_)))
#define POINTER_ADD(ptr_, offset_) POINTER_ADD_TYPE(TYPEOF(ptr_), ptr_, offset_)
#define DEAD_STRING(s) OSReport(s)
// aurora's dolphin/types.h: MSVC reports __cplusplus 199711L without /Zc:__cplusplus (the mod's flags), so after
// a game header nullptr is a macro for NULL. The hdf headers' own code must still compile.
#ifndef nullptr
#define nullptr NULL
#endif

#include "hdf/cave.hpp"
#include "hdf/core.hpp"
#include "hdf/detect.hpp"
#include "hdf/facts.hpp"
#include "hdf/generate.hpp"
#include "hdf/inputs.hpp"
#include "hdf/io.hpp"
#include "hdf/j3d.hpp"
#include "hdf/outputs.hpp"
#include "hdf/source.hpp"
#include "hdf/stage.hpp"
#include "hdf/tex.hpp"
#include "hdf/text.hpp"
#include "hdf/ui.hpp"

#include "test.hpp"

HDF_TEST(headers_compile_under_game_macros) {
    // The global typedefs and hdf's own coexist.
    u8 a = 1;
    hdf::u8 b = a;
    HDF_CHECK_EQ(int(b), 1);
    HDF_CHECK(hdf::kPi != static_cast<double>(M_PI));  // M_PI is the float one here; the library never uses it
}
