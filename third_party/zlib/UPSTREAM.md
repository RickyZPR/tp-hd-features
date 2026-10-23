# zlib 1.3.2 (vendored)

- Upstream: https://zlib.net/ (Jean-loup Gailly and Mark Adler). Licence: the zlib licence, see `LICENSE`.
- Release tarball: https://zlib.net/zlib-1.3.2.tar.gz
  - sha256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16` (1,502,830 bytes).
  - The same bytes are on https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz.
  - The detached signature https://zlib.net/zlib-1.3.2.tar.gz.asc verifies with Mark Adler's key
    `5ED4 6A67 21D3 6558 7791  E2AA 783F CD8E 58BC AFBA` (checked 2026-10-22).
- Why this version: Dusklight v2.0.3 ships z.dll 1.3.2, and zlib 1.3.2 at level 9 reproduces the oracle's PNG
  IDAT streams byte for byte (research/dropin/libs.md). Never swap in zlib-ng or miniz for deflate.
- Files: copied unchanged from the tarball's top folder. Only what the converter needs (inflate, deflate,
  compress2, uncompress, crc32, adler32). The gz* file API (gz*.c) is left out; its header `gzguts.h` is kept
  because `zutil.c` includes it.
- Build: no defines. Compiled as C, warnings off (`/W0`), in the `hdf_third_party` target of
  `mods/hdfeatures/convert/CMakeLists.txt`.

| sha256 | file |
|---|---|
| e32ff4e00d9d94930537635291da39e7e612703334bf6fde8c7f1686fe8a45a2 | LICENSE |
| 9cd1443a24ff2a3053961695bd432035c58347386a420d3388232376ebabe211 | adler32.c |
| 41f1c7ad886012571cd481a2b1b6a83812e5125e90d995e298fde34d6c015614 | compress.c |
| da37f3483e77c20f64c28cf55d62d7bed9d62af33eb401be906835d84539b9c9 | crc32.c |
| 9a2223575183ac2ee8a247f20bf3ac066e8bd0140369556bdbdffc777435749e | crc32.h |
| ea9ca4eb51b1a71aaf214b99f4a9fab177253d6268cae8e4c26d44d1812e52c2 | deflate.c |
| 21f4ea0973dab272fb26e93305ccf8628c8dc07e6c5df09a9861825b1920ba5d | deflate.h |
| db7690315cda93c0742701365d5218932ff0e2e6b8713c95019553502ada7d2b | gzguts.h |
| f96787a11bb7131ffd2221fb40db27f6a5a1dfb12f59c0bc388807010749c34a | inffast.c |
| 05cc5dc9ff1da7b8b52a4bd8bda0d8a5c236a2f39efe84b941516ea13857e6c5 | inffast.h |
| fd7c4b1dbdacb283421d0675744aaecda44e840e183b5131448bac94564f6adb | inffixed.h |
| 90221ea7d762e7044057f6ad5013e969086c6ba3d3051716b7aa786d7a6362ec | inflate.c |
| e6a43bd0e24ab2cf5fa1dca630859e9a2f6a721f033f75faf179f5c0299e4fe2 | inflate.h |
| f7ee9ce5925fc9d1579c76e574feed85f5ca4ccddc446a7795578854bbc65743 | inftrees.c |
| d3a7ea301d8ec514823d052a2e42af4636bc81a0cf07052f526030f208068fb8 | inftrees.h |
| a16217d5ea6d8342ff08afe5207b1dab620f05610e121dda55b4f9aae9de4f68 | trees.c |
| bb0a9d3ca88ee00c81adb7c636e73b97085f6ef1b52d6d58edbe2b6dc3adeb4d | trees.h |
| 953f5381650838d507b01499790bd64524433ea6ea29ea9325bf542af10cfe7d | uncompr.c |
| cb7c2c84211473b4699223edd363d3207b43b9578e739b5bf638f42204ea6e0f | zconf.h |
| 818667d6ab6a37fe7469cb06a7f0cb2c2cb2f2c948a03e5accf1a4a74bf3020a | zlib.h |
| 1279908a15580224a2c1f377555ffdd150327fed71801aa77fc35978dcb741c8 | zutil.c |
| a12243cc6d338ffad0c8e3500fbc05bc7fdcdc25a469393b1c2f7e5df307fff3 | zutil.h |
