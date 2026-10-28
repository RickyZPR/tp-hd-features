# Building TP HD Features

Players don't need this page. Download the `.dusk` from Releases instead (see the [README](README.md)).

This repository has no game files. The tests below need your own copies of the games, just like a player.
Never commit or share anything made from the games (see [DISCLAIMER.md](DISCLAIMER.md)). The only exception is the
README's short gameplay recording, `docs/showcase.gif`.

## What you need

- Windows 10 or 11, 64-bit.
- Visual Studio 2022 or newer with the "Desktop development with C++" workload. It includes MSVC, CMake and Ninja.
  Tested with Visual Studio 2026 (MSVC 19.51).
- Git. CMake uses it to download Dusklight's source.
- An internet connection for the first configure.
- A short folder path, like `C:\src\tp-hd-features`. With very long paths (about 140 characters or more), MSVC
  fails and CMake says the compiler cannot compile a simple test program.
- For the oracle tests and to test in the game: Dusklight v2.0.3, your *Twilight Princess* GameCube disc
  (USA version, GZ2E01) and your *Twilight Princess HD* (Wii U, v81 update).
- For the developer tools only: Python 3.10 or newer, 64-bit. `tools/dist/base_art.py` needs Pillow,
  `art/hdfeatures/make.py` needs numpy and Pillow.

## 1. Build the mod

Open "x64 Native Tools Command Prompt for VS" and go to the repository folder. Then:

```
cmake -S . -B build-hdfeatures -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-hdfeatures
```

The first configure downloads:
- Dusklight v2.0.3's source (git tag `v2.0.3` of https://github.com/TwilitRealm/dusklight) into `dusklight/`,
  through `cmake/FetchDusklight.cmake`. The mod is built against its mod SDK.
- {fmt} 12.1.0 (Dusklight's SDK gets it).
- Dusklight's link stub, unless you link against your own Dusklight install:
  `-DDUSK_GAME_EXE="<Dusklight folder>/sdk/windows-amd64.lib"`.

To use a Dusklight checkout you already have: `-DDUSKLIGHT_DIR="<folder>"`.
To build without internet, also pass `-DDUSK_GAME_EXE=...` (above) and
`-DFETCHCONTENT_SOURCE_DIR_FMT="<fmt 12.1.0 source folder>"`.

The result is `build-hdfeatures/mods/hd_features.dusk`, the player package. It holds exactly `mod.json`,
`lib/windows-amd64/mod.dll`, `res/icon.png`, `res/banner.png` and `res/THIRD-PARTY-NOTICES.txt`, and no game
files: the mod makes them on the player's PC. Configure stops if `mods/hdfeatures/package/res/` holds anything else.

`mod.dll` contains the converter library (`mods/hdfeatures/convert`, target `hdf_convert`) and the vendored zlib
and zstd (`third_party/`). Add `-DHDF_BUILD_CLI=ON` to also build the developer tool `hdf-convert`. It is never
packaged.

## 2. Build and test the converter

The converter also builds on its own, without Dusklight's source:

```
cmake -S mods/hdfeatures/convert -B build-conv -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-conv
build-conv\hdf-convert self-test
```

- `self-test` runs the unit tests on synthetic data only. It needs no games. `ctest --test-dir build-conv -R hdf_unit`
  runs the same.
- `hdf-convert` without arguments lists its commands.

## 3. The oracle tests (needs your own games; outputs stay in a scratch folder)

The converter must make the same files, byte for byte, as the Python asset pipeline (`tools/build_assets.py`),
which is its spec. The pipeline's output is the oracle.

1. Make the oracle with the Python pipeline, into a scratch folder:

   ```
   python -I -B tools/build_assets.py --hd "<TP HD content folder>" --gc "<GC extraction folder>" --out-dir "<scratch>\oracle"
   ```

   The GC extraction is an extracted disc folder (Dolphin: right-click the game > Properties > Filesystem >
   Extract Entire Disc).
2. Make the files with the converter, then compare:

   ```
   build-conv\hdf-convert make --hd "<TP HD folder or .wua>" --gc "<GC extraction folder>\files" --out "<scratch>\made" --time
   build-conv\hdf-convert compare --out "<scratch>\made" --oracle "<scratch>\oracle"
   ```

   A pass is 176 identical files plus 30 files left out by design.
3. Module tests: `hdf-convert test list`. Most need a golden list made from the Python pipeline by
   `tools/dev/golden*.py` (see each script's help). `test detect` needs the retired Python spec, which is not in
   this repository.
4. CTest runs the oracle tests too, when you configure with `-DHDF_TEST_HD=...`, `-DHDF_TEST_GC=...` and
   `-DHDF_TEST_ORACLE=...` (and optionally `-DHDF_TEST_HD_WUA=...`). They write into the build folder.

- Inside the repository folder, `hdf-convert` only writes into a `build-*` folder. A scratch folder outside it is
  fine too.
- Delete the outputs when you are done. They are made from your games: never share them.
- The maintainer's oracle is a pinned `.dusk` (its SHA-256 is in `tools/dev/gen_tables.py`). `compare` refuses
  any other `.dusk` as the oracle, but accepts a folder like the one above.

A change to the Python pipeline means: make the oracle again (without `--src`), regenerate the embedded tables
(`tools/dev/gen_tables.py`, `tools/dev/gen_facts_inc.py`) and get a new set id. Every player's game files are
then made again at their next start.

## 4. Make the release (maintainers)

```
python -I -B tools/dist/make_release.py --hd "<TP HD content folder>" --gc "<GC extraction folder>" --dusk build-hdfeatures/mods/hd_features.dusk
```

- It checks that the package holds exactly the 5 files above (version, pinned icon, banner and notices), drops
  folder entries, repacks it with fixed dates, and scans it for Nintendo content and personal details.
- Only after `SCAN PASSED` does it write `dist/hd_features-<version>.dusk`, its `.sha256` file and
  `dist/RELEASE-NOTES-<version>.md`. The release is that `.dusk` itself: no zip, and nothing to run.
- `--dry-run` checks and scans without writing anything.
- The scan compares against the maintainer's pinned oracle (`--oracle`).
- Extra personal patterns for the scan can go in `private_scan_patterns.txt` at the repository root
  (one regular expression per line; git-ignored).

`tools/dist/make_repo.py` makes the public repository folder from the maintainer's working tree and scans it
the same way.

The icon and banner were drawn by `tools/dist/base_art.py`
(`python -I -B tools/dist/base_art.py --fonts "<Dusklight folder>/res" --out <folder>`). If you change them, check
them by eye, then update their pins in `tools/dist/make_release.py`.

## Layout

| Folder | What |
|---|---|
| `mods/hdfeatures/src/` | The mod (C++): `stamps/`, `cave/`, `hdfiles/` (finds your TP HD, makes and keeps the game files), and the shared panel and switches |
| `mods/hdfeatures/convert/` | The converter: library sources, embedded tables (`data/*.inc`), the `hdf-convert` tool (`cli/`) and its tests (`tests/`) |
| `mods/hdfeatures/package/res/` | The package's icon, banner and third-party notices |
| `mods/hdfeatures/docs/` | How each part works |
| `third_party/` | zlib and zstd, unchanged upstream files (their SHA-256s are in each `UPSTREAM.md`) |
| `tools/build_assets.py`, `tools/pipeline/`, `tools/formats/`, `tools/cave/`, `tools/critic/` | The Python asset pipeline: the converter's spec and oracle maker |
| `tools/dev/` | The embedded tables and the golden lists for the converter's tests |
| `tools/dist/` | The release and repository scripts, and `base_art.py` |
| `art/hdfeatures/make.py` | Draws an HD-style icon and banner from your own TP HD, for your own builds only. Never share them |
| `cmake/` | Downloads Dusklight's source |

Some code comments name the developer's own notes (`research/...`, `DESIGN.md`) and the retired Python builder of
2.0.0 (`hdf_builder.py`, `hdf_inputs.py`), which parts of the C++ were ported from. Those files are not in this
repository. The comments themselves say what was found.
