# Zstandard 1.5.7, decoder only (vendored)

- Upstream: https://github.com/facebook/zstd (Meta Platforms, Inc. and affiliates). Zstandard is dual-licensed
  BSD-3 / GPLv2; this project uses it under the **BSD-3** licence, see `LICENSE`.
- Release tarball: https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  - sha256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3` (2,434,947 bytes).
  - Equal to the upstream `zstd-1.5.7.tar.gz.sha256` release file, and to the hash Dusklight's aurora pins
    (`dusklight/extern/aurora/extern/CMakeLists.txt:318`). Dusklight v2.0.3 ships zstd 1.5.7 too.
- Why: only the Cemu `.wua` (ZArchive) reader needs zstd. Only decompression is vendored.
- Files:
  - `zstd.h`, `zstd_errors.h`, `LICENSE`: copied unchanged from the tarball (`lib/zstd.h`, `lib/zstd_errors.h`,
    `LICENSE`). `zstd.h` includes `zstd_errors.h`.
  - `zstddeclib.c`: the upstream single-file decoder, made from the tarball with upstream's own script and the
    command that `build/single_file_libs/create_single_file_decoder.sh` runs:

    ```
    cd zstd-1.5.7/build/single_file_libs
    python3 combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c
    ```

    It was run here with Python 3.10.6 (`python -I`, absolute paths). On Windows Python writes CRLF; the result
    was converted to LF, which is what the same command writes on Linux (no input file has a CR byte). The
    settings it bakes in (`zstddeclib-in.c`): `ZSTD_LEGACY_SUPPORT 0`, `ZSTD_DISABLE_ASM 1`,
    `ZSTD_STRIP_ERROR_STRINGS`, `DEBUGLEVEL 0`, `XXH_NAMESPACE ZSTD_`.
- Build: compiled as C, warnings off (`/W0`), with `ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0` (libs.md), in the
  `hdf_third_party` target of `mods/hdfeatures/convert/CMakeLists.txt`. Single-threaded (the default).

| sha256 | file |
|---|---|
| 7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8 | LICENSE |
| 9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b | zstd.h |
| 66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a | zstd_errors.h |
| 5eb38abe0a7eea13674b312a006df72002d4192f0f0b4d09e6f03fb927a1f3d0 | zstddeclib.c |
