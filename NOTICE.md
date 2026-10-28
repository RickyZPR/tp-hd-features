# Notices and credits

TP HD Features' own code is under the [MIT License](LICENSE).
This page lists what comes from other people, and what is used but not included.

## Nintendo

TP HD Features is an unofficial fan project. It is not affiliated with or endorsed by Nintendo.
*Twilight Princess*, *The Legend of Zelda*, GameCube, Wii U, amiibo and Miiverse are trademarks of Nintendo.

This repository and its releases contain no Nintendo files: no game data, textures, models, stages, sounds,
game text, fonts or logos. The code holds only the ids, flags, room numbers, sizes, hashes and short labels it
needs to work with the game. The mod reads the player's own copies of the games and makes everything else on the
player's PC. Those files must never be shared.

The only pictures of the game are two short gameplay recordings: the animation at the top of the README
(`docs/showcase.gif`) and the showcase video on the v2.1.0 release (`hd-features-showcase.mp4`, with game sound; its
title card shows the *Twilight Princess HD* logo and stamps). What they show belongs to Nintendo, and this project's
MIT License does not cover it.

## What the mod does on your PC

The mod reads your own game files on your PC and writes its files only to Dusklight's data folder
(`mod_data\local.ricky.hdfeatures`). It uses no network.

- It reads the GameCube files through Dusklight, from the disc Dusklight runs.
- To find *Twilight Princess HD*, it reads Cemu's settings and game list. It only reads them.

## Included in this repository

| What | Where | From | License |
|---|---|---|---|
| zlib 1.3.2 (inflate, deflate, CRC-32). Compiled into `mod.dll` | `third_party/zlib/` (unchanged upstream files) | Jean-loup Gailly and Mark Adler, https://zlib.net/ | zlib License, `third_party/zlib/LICENSE` |
| Zstandard 1.5.7, decoder only (reads `.wua` files). Compiled into `mod.dll` | `third_party/zstd/` (upstream files and its single-file decoder) | Meta Platforms, Inc. and affiliates, https://github.com/facebook/zstd | BSD-3-Clause, `third_party/zstd/LICENSE` (upstream is dual BSD-3 / GPLv2; this project uses BSD-3) |
| Wii U texture tiling (address functions) | `mods/hdfeatures/convert/src/tex/addrlib_ref.inc` and `.hpp`: a test reference, built only into the developer tool `hdf-convert`, never into `mod.dll`. `mods/hdfeatures/convert/src/tex/untile.cpp` and `tools/formats/gtx2img.py` follow the same address functions | AMD's addrlib (R600/R700 address library), as ported by decaf-emu/addrlib, from Dusklight's tphd branch (CC0 1.0) | AMD's MIT-style license, full notice below |
| Mod build helper | `cmake/FetchDusklight.cmake` (unchanged); the root `CMakeLists.txt` started from the same template | Dusklight's mod template, https://github.com/TwilitRealm/mod-template | The template has no license file. Mod authors start from it. That file is not covered by this project's MIT License |
| Names of game actors, flags and functions used in the code, and `tools/formats/objnames.json` (actor names) | `mods/hdfeatures/`, `tools/` | The [zeldaret/tp](https://github.com/zeldaret/tp) decompilation and Dusklight | CC0 1.0 (both) |
| The mod's icon and banner | `mods/hdfeatures/package/res/icon.png`, `banner.png` | Drawn by code for this project (`tools/dist/base_art.py`). The lettering uses the two fonts below; no font files are included | This project's MIT License |

The release `.dusk` carries `res/THIRD-PARTY-NOTICES.txt` with three license texts: zlib's, Zstandard's (BSD-3) and
AMD's addrlib notice.

## Downloaded when you build (not included)

- **Dusklight v2.0.3** source and its mod SDK, https://github.com/TwilitRealm/dusklight (CC0 1.0).
  `cmake/FetchDusklight.cmake` downloads it into `dusklight/` on the first configure. The mod is compiled
  against its headers.
- **Aurora** (https://github.com/encounter/aurora), Dusklight's platform layer. It comes with Dusklight's source
  (`extern/aurora`), and the mod is compiled against its headers. Copyright (c) 2022 Luke Street, MIT License.
- **Dusklight's link stub**, downloaded by Dusklight's SDK unless you link against your own Dusklight install.
- **{fmt} 12.1.0**, https://github.com/fmtlib/fmt, downloaded by Dusklight's SDK. Parts of it are compiled into
  `mod.dll`. Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors. MIT License, with an exception
  that lets compiled code be shared without the notice. Credited here anyway.

## Used on your PC if it is there (not included)

- **Fonts**, read from your Dusklight install's `res/` folder by `tools/dist/base_art.py` (developers only). It
  drew the icon and banner with Alegreya SC and Fira Sans.
  - Alegreya SC: Copyright 2011 The Alegreya Project Authors (https://github.com/huertatipografica/Alegreya).
    SIL Open Font License 1.1.
  - Fira Sans: Digitized data copyright 2012-2016, The Mozilla Foundation and Telefonica S.A.
    SIL Open Font License 1.1. Fira Sans is a trademark of The Mozilla Corporation.
- **Perpetua Titling MT Bold**: the title font of the HD-style banner (`art/hdfeatures/make.py`, developers
  only), only if it is already installed on your PC. Otherwise Alegreya SC is used.
- **Python packages** for the developer tools, if installed: numpy and Pillow.
- **Cemu**: the mod can find *Twilight Princess HD* where Cemu keeps it. This project is not affiliated with Cemu.

## AMD addrlib notice

The files named above follow, or contain a port of, the surface address functions of AMD's addrlib. Its notice:

```
Copyright © 2014 Advanced Micro Devices, Inc.
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS, AUTHORS
AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.
```
