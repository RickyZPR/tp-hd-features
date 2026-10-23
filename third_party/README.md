# third_party

Code from other projects, vendored unchanged so the converter (`mods/hdfeatures/convert`, library `hdf_convert`)
builds without downloads. Each folder has the upstream `LICENSE` and an `UPSTREAM.md` with the per-file SHA-256.
Keep these bytes exactly: `.gitattributes` should mark `third_party/** -text`.

| Library | Version | Licence | Source (official release) | sha256 of the release file | Used for |
|---|---|---|---|---|---|
| zlib | 1.3.2 | zlib | https://zlib.net/zlib-1.3.2.tar.gz (same bytes: https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz) | `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16` | gzip inflate of the TP HD `.pack.gz` files, deflate level 9 for PNGs, CRC-32 |
| Zstandard | 1.5.7 (decoder only) | BSD-3 (dual BSD/GPLv2 upstream; BSD-3 chosen) | https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz | `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3` | reading Cemu `.wua` archives |

How the hashes were checked (2026-10-22):
- zlib: the zlib.net and GitHub release files are identical, and the zlib.net `.asc` signature is good for Mark
  Adler's key `5ED46A6721D365587791E2AA783FCD8E58BCAFBA`.
- zstd: equal to upstream's `zstd-1.5.7.tar.gz.sha256` release file and to the hash Dusklight's aurora pins.

`zstd/zstddeclib.c` is generated from the tarball by upstream's `combine.py`; `zstd/UPSTREAM.md` has the exact
command.

Not vendored: picosha2 (SHA-256 comes from Windows CNG in the library) and ZArchive (the converter has its own
`.wua` reader).

Binary notices: mod.dll links zlib and zstd, and its texture untiling follows AMD's addrlib, so the `.dusk` carries
`res/THIRD-PARTY-NOTICES.txt` with three licence texts: zlib's, Zstandard's (BSD-3) and AMD's addrlib notice.
`hdf-convert.exe` is a developer tool and is never shipped.
