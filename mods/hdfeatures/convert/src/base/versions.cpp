// The vendored zlib and zstd versions (third_party/README.md). The PNG bytes depend on zlib 1.3.2 at level 9
// (libs 2): fail the build if another zlib or zstd is picked up.
#include "hdf/source.hpp"

#include <zlib.h>
#include <zstd.h>

static_assert(ZLIB_VERNUM == 0x1320, "hdf_convert needs the vendored zlib 1.3.2 (third_party/zlib)");
static_assert(ZSTD_VERSION_NUMBER == 10507, "hdf_convert needs the vendored zstd 1.5.7 (third_party/zstd)");

namespace hdf {

LibVersions lib_versions() {
    return LibVersions{zlibVersion(), ZSTD_versionString()};
}

}  // namespace hdf
