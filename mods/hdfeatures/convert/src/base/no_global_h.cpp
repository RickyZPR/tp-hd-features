// The converter must never be compiled with the mod's flags: FEATURES game force-includes global.h, which makes
// M_PI a float and adds ROUND/SQUARE/DEBUG macros (libs 5, plan 6 risk table). This file fails the build if that
// happens. Keep it in the library; never include global.h here.
#include "hdf/core.hpp"

#if defined(M_PI)
#error "hdf_convert: M_PI is defined: global.h (or _USE_MATH_DEFINES) leaked into the converter library"
#endif
#if defined(_global_h_) || defined(ROUND) || defined(SQUARE)
#error "hdf_convert: the game's global.h leaked into the converter library"
#endif
#if defined(_M_FP_FAST) || defined(__FAST_MATH__)
#error "hdf_convert: build with /fp:precise (clang/gcc: no -ffast-math), never /fp:fast"
#endif

namespace hdf {
static_assert(kPi == 3.141592653589793, "kPi must be Python's math.pi");
}  // namespace hdf
