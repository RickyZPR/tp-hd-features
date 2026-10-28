#!/usr/bin/env python3
"""make_repo.py - regenerate the public GitHub repository folder (run on the developer PC only).

usage:
  python -I -B tools/dist/make_repo.py --hd "<TP HD content folder>" [--gc <GC extraction root>]
                                       [--oracle <oracle .dusk>] [--out <folder>] [--draft] [--dry-run]

  --hd       the TP HD dump's content folder: only read, for the text scan (as in make_release.py).
  --gc       the GC extraction (default: <project>/cache/gc_extract): its message text too.
  --oracle   the pinned 2.0.0 oracle package (default: <project>/cache/oracle/hd_features-oracle-2.0.0.dusk): the
             "never ship these bytes" reference of the scan. Only read.
  --out      the repository folder (default: <project>/publish/tp-hd-features). Everything in it except .git/
             is deleted and written again, but only after the scan passed: never edit it by hand. Edit the
             project files or repo_docs/ instead.
  --draft    a missing repo document (README.md, DISCLAIMER.md, LICENSE in repo_docs/) or a failed disclaimer
             check is only a warning (the files are written, with DO-NOT-PUBLISH-DRAFT.txt). The content scan
             still fails on any hit.
  --dry-run  stage and scan in a temporary folder, report, and delete it: --out is not touched (no marker either).

Steps:
  1. Allowlist check: every project Python module the allowlisted tools import (traced) is allowlisted, and
     every allowlisted Python file is used. third_party/ holds only pinned files (THIRD_PARTY_PINS, the same as
     its UPSTREAM.md tables) and its own notes; mods/hdfeatures/package/res/ only its 3 pinned files.
  2. Copies the allowlisted project files (FILES, TREES, third_party/, the package res files) and everything in
     repo_docs/ into a new temporary folder next to --out. Line endings as .gitattributes says (* text=auto
     eol=lf: LF; third_party/** -text, *.png binary and docs/showcase.gif binary: the bytes as they are). Writes
     the .gitignore.
  3. Scans every file there with make_release.py's Scanner (Nintendo magic numbers, files equal to a generated
     game file or to an HD-art PNG, 6-word runs of any TP HD or GC message, personal paths and e-mails) plus
     repo rules: never-ship folders, names and file types; text only (except the 2 pinned package PNGs and the
     pinned README animation, SHOWCASE); the author name only as the mod id or "Ricky"; the HD chest / talk-tag
     records written as hex bytes or as their float positions; exact copies of research notes or generated files;
     no repo file is git-ignored, and every never-ship file type is (and refused: MUST_REFUSE). The pinned
     third_party/ files are checked by hash only (not text-scanned).
  4. Disclaimer check: README alert box, DISCLAIMER.md, MIT LICENSE, NOTICE.md, the release notes of
     make_release.py, the in-game line.
  5. Only if all of that passed: --out is emptied (except .git/) and the scanned files are copied into it.
Exit 0 = ready to commit; 1 = a problem was found: --out is NOT changed, and DO-NOT-PUBLISH-SCAN-FAILED.txt is
written into it (not git-ignored, so `git status` shows it); 2 = bad arguments. This script never runs git and
never touches GitHub. The retired 2.0.0 player builder (tools/dist/legacy/) is never copied.
"""
import argparse
import ast
import fnmatch
import hashlib
import os
import re
import shutil
import stat
import struct
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import make_release as mr  # noqa: E402  (its Scanner, pins, release notes and personal patterns)

REPO_NAME = 'tp-hd-features'
DOCS = os.path.join(PROJECT, 'repo_docs')
REQUIRED_DOCS = ('README.md', 'DISCLAIMER.md', 'LICENSE')
FAIL_MARK = 'DO-NOT-PUBLISH-SCAN-FAILED.txt'     # never git-ignored: `git status` must show it
DRAFT_MARK = 'DO-NOT-PUBLISH-DRAFT.txt'

# ============================================================================================= allowlist
# (a) build the mod and its package from source: CMake + the Dusklight SDK (fetched by cmake/FetchDusklight.cmake),
#     the converter (mods/hdfeatures/convert) and the vendored zlib / zstd (third_party/).
# (b) the developer tools: the Python pipeline (the converter's spec and oracle maker), tools/dev (tables and
#     golden lists for the converter's tests), the release and repo scripts.
# The asset pipeline modules (import-traced from the entries below; a listed module nothing uses is an error).
PIPELINE_FORMATS = ['rarc', 'rarc_write', 'yaz0', 'hd2gc', 'hdpack', 'gtx2img', 'gxtex', 'j3d', 'j3d_dlcheck', 'dzx',
                    'bmg']                                     # bmg: make_release's text scan
PIPELINE_CAVE = ['losbin', 'bake_los', 'cave_patch']
PIPELINE_CRITIC = ['mat4fix', 'matstride', 'j3dcheck']
# tools/dev: every script is either published (DEV) or kept on the developer PC (DEV_LOCAL_ONLY); a new one stops
# the script until it is put in one of the two lists. golden_detect.py stays local: its Python spec is the retired
# player builder (tools/dist/legacy/hdf_inputs.py), which is never public.
DEV = ['gen_tables', 'gen_facts_inc', 'gen_io_vectors', 'golden', 'golden_cave', 'golden_j3d', 'golden_stage',
       'golden_tex', 'golden_text', 'golden_ui']
DEV_LOCAL_ONLY = ['golden_detect']
FILES = ['.gitattributes', 'CMakeLists.txt', 'cmake/FetchDusklight.cmake',
         'mods/hdfeatures/CMakeLists.txt', 'mods/hdfeatures/mod.json.in',
         'tools/build_assets.py', 'tools/gen_stamp_table.py', 'tools/formats/objnames.json',
         'art/hdfeatures/make.py', 'art/hdfeatures/.gitignore',
         'tools/dist/base_art.py', 'tools/dist/make_release.py', 'tools/dist/make_repo.py', 'tools/dist/hd_files.txt']
FILES += ['tools/formats/%s.py' % n for n in PIPELINE_FORMATS]
FILES += ['tools/cave/%s.py' % n for n in PIPELINE_CAVE] + ['tools/critic/%s.py' % n for n in PIPELINE_CRITIC]
FILES += ['tools/dev/%s.py' % n for n in DEV]
# Whole folders: every file in them must have one of these extensions (anything else stops the script).
TREES = [('mods/hdfeatures/src', ('.cpp', '.hpp', '.inc')),
         ('mods/hdfeatures/docs', ('.md',)),
         ('mods/hdfeatures/convert', ('.cpp', '.hpp', '.inc', '.txt', '.in')),   # sources, data/*.inc, cli/, tests/
         ('tools/pipeline', ('.py',))]
TRACE_ENTRIES = ('tools/build_assets.py', 'art/hdfeatures/make.py')       # + every tools/dist and tools/dev file
TRACE_DIRS = ('formats', 'critic', 'cave', 'pipeline', 'dist', 'dev')     # the tools' sys.path folders

# The package's own res files: with SHOWCASE, the only binary files of the repo (the neutral icon and banner drawn
# by tools/dist/base_art.py) and the notices text. Pinned in make_release.PINNED_RES.
PACKAGE_RES_DIR = 'mods/hdfeatures/package/res'
PACKAGE_RES = {'%s/%s' % (PACKAGE_RES_DIR, n.split('/', 1)[1]): pin for n, pin in mr.PINNED_RES.items()}
# The README's animation (repo_docs/docs/showcase.gif): a 6 s silent gameplay recording of the mod in Dusklight.
# Not a game file. The one image outside the package: this path with exactly these bytes (size, SHA-256). Any other
# image, another .gif included, is still a never-ship file type. Scanned as a binary file, like the package PNGs.
SHOWCASE = {'docs/showcase.gif': (6843810, '813a558542fe8900c5657dd93cb1b2878a2145b06a67187e927609ab84ddf510')}
BINARY_OK = {rel for rel in PACKAGE_RES if rel.endswith('.png')} | set(SHOWCASE)
# third_party/: vendored upstream files, kept byte for byte. Their SHA-256 (the same as the tables in
# third_party/*/UPSTREAM.md; both are checked). They are not text-scanned: licence texts and C tables are not ours.
THIRD_PARTY_DOCS = ('third_party/README.md', 'third_party/zlib/UPSTREAM.md', 'third_party/zstd/UPSTREAM.md')
THIRD_PARTY_PINS = {
    'third_party/zlib/LICENSE': 'e32ff4e00d9d94930537635291da39e7e612703334bf6fde8c7f1686fe8a45a2',
    'third_party/zlib/adler32.c': '9cd1443a24ff2a3053961695bd432035c58347386a420d3388232376ebabe211',
    'third_party/zlib/compress.c': '41f1c7ad886012571cd481a2b1b6a83812e5125e90d995e298fde34d6c015614',
    'third_party/zlib/crc32.c': 'da37f3483e77c20f64c28cf55d62d7bed9d62af33eb401be906835d84539b9c9',
    'third_party/zlib/crc32.h': '9a2223575183ac2ee8a247f20bf3ac066e8bd0140369556bdbdffc777435749e',
    'third_party/zlib/deflate.c': 'ea9ca4eb51b1a71aaf214b99f4a9fab177253d6268cae8e4c26d44d1812e52c2',
    'third_party/zlib/deflate.h': '21f4ea0973dab272fb26e93305ccf8628c8dc07e6c5df09a9861825b1920ba5d',
    'third_party/zlib/gzguts.h': 'db7690315cda93c0742701365d5218932ff0e2e6b8713c95019553502ada7d2b',
    'third_party/zlib/inffast.c': 'f96787a11bb7131ffd2221fb40db27f6a5a1dfb12f59c0bc388807010749c34a',
    'third_party/zlib/inffast.h': '05cc5dc9ff1da7b8b52a4bd8bda0d8a5c236a2f39efe84b941516ea13857e6c5',
    'third_party/zlib/inffixed.h': 'fd7c4b1dbdacb283421d0675744aaecda44e840e183b5131448bac94564f6adb',
    'third_party/zlib/inflate.c': '90221ea7d762e7044057f6ad5013e969086c6ba3d3051716b7aa786d7a6362ec',
    'third_party/zlib/inflate.h': 'e6a43bd0e24ab2cf5fa1dca630859e9a2f6a721f033f75faf179f5c0299e4fe2',
    'third_party/zlib/inftrees.c': 'f7ee9ce5925fc9d1579c76e574feed85f5ca4ccddc446a7795578854bbc65743',
    'third_party/zlib/inftrees.h': 'd3a7ea301d8ec514823d052a2e42af4636bc81a0cf07052f526030f208068fb8',
    'third_party/zlib/trees.c': 'a16217d5ea6d8342ff08afe5207b1dab620f05610e121dda55b4f9aae9de4f68',
    'third_party/zlib/trees.h': 'bb0a9d3ca88ee00c81adb7c636e73b97085f6ef1b52d6d58edbe2b6dc3adeb4d',
    'third_party/zlib/uncompr.c': '953f5381650838d507b01499790bd64524433ea6ea29ea9325bf542af10cfe7d',
    'third_party/zlib/zconf.h': 'cb7c2c84211473b4699223edd363d3207b43b9578e739b5bf638f42204ea6e0f',
    'third_party/zlib/zlib.h': '818667d6ab6a37fe7469cb06a7f0cb2c2cb2f2c948a03e5accf1a4a74bf3020a',
    'third_party/zlib/zutil.c': '1279908a15580224a2c1f377555ffdd150327fed71801aa77fc35978dcb741c8',
    'third_party/zlib/zutil.h': 'a12243cc6d338ffad0c8e3500fbc05bc7fdcdc25a469393b1c2f7e5df307fff3',
    'third_party/zstd/LICENSE': '7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8',
    'third_party/zstd/zstd.h': '9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b',
    'third_party/zstd/zstd_errors.h': '66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a',
    'third_party/zstd/zstddeclib.c': '5eb38abe0a7eea13674b312a006df72002d4192f0f0b4d09e6f03fb927a1f3d0',
}

# ============================================================================================= repo rules
TOP_NEVER = {'research', 'reference', 'cache', 'dusklight', 'testenv', 'backups', 'dist', 'publish', 'template',
             'build', 'repo_docs'}
ANY_NEVER = {'res', 'overlay', '__pycache__', 'candidates', 'work', 'output', 'output (do not share)', '.git',
             'legacy'}
# The retired 2.0.0 player builder (tools/dist/legacy/): never public, under any path.
RETIRED = {'build-hd-features.bat', 'hdf_builder.py', 'hdf_inputs.py', 'discimg.py', 'test_discimg.py',
           'retired.md'}
NAME_NEVER = {'resume.md', 'private_scan_patterns.txt', FAIL_MARK.lower(), DRAFT_MARK.lower()} | RETIRED
EXT_NEVER = set(mr.BAD_EXT) | {'.dusk', '.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp', '.tif', '.tiff', '.ico',
                               '.zip', '.7z', '.rar', '.exe', '.dll', '.lib', '.pdb', '.obj', '.gci', '.sav',
                               '.log', '.ttf', '.otf', '.mp4', '.webm', '.wav', '.ogg', '.csv', '.bat'}
AUTHOR_NAME = 'Ricky'                     # mod.json.in "author" and the LICENSE holder: allowed as written
# Made-up test data that looks like a personal path but is not: {file: exact texts the personal check accepts}.
# None at the moment (gen_test.cpp's made-up path no longer starts in C:\Users).
NOT_PERSONAL = {}

GITIGNORE_HEAD = """# tp-hd-features: never commit these. Written by tools/dist/make_repo.py.

# Game files made from YOUR OWN games by tools/build_assets.py (the Python pipeline) or by the converter's
# developer tool (hdf-convert). They contain Nintendo data: never commit or share them.
mods/hdfeatures/res/
mods/hdfeatures/overlay/
"""
# File types of the games and of what is made from them: GameCube disc and its extraction (sys/ + files/), the
# Wii U title (code/ content/ meta/, NUS files), disc images, saves, and the game formats the tools read or write.
GAME_EXT = sorted(set(mr.BAD_EXT) - {'.pyc'} | {
    '.ast', '.aw', '.baa', '.bin', '.bnr', '.dol', '.img', '.jpc', '.map', '.rel', '.str', '.thp',      # GC disc
    '.bfn', '.binput', '.dat', '.gtx', '.gz', '.rpx', '.tga',                                           # TP HD
    '.app', '.h3', '.tmd', '.tik', '.cert',                                                              # Wii U NUS
    '.iso', '.gcm', '.rvz', '.wia', '.gcz', '.ciso', '.wbfs', '.nfs', '.tgc', '.wua', '.wux', '.wud',    # images
    '.gci', '.sav', '.raw'})                                                                             # saves
GAME_NAMES = ['app.xml', 'cos.xml', 'meta.xml']                  # Wii U title files with a generic extension
OTHER_EXT = ['.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp', '.tif', '.tiff', '.ico', '.mp4', '.webm', '.wav',
             '.ogg', '.ttf', '.otf', '.zip', '.7z', '.rar', '.exe', '.dll', '.lib', '.pdb', '.obj', '.log', '.csv',
             '.bat']
GITIGNORE_TAIL = """art/hdfeatures/*.png
art/candidates/
art/work/
*.dusk
output (DO NOT SHARE)/

# The same files wherever they are written, for example `build_assets.py --out-dir` or `hdf-convert make --out`
# into a folder inside the repository (a scratch folder outside it is best): HD's text, the game-made tables and
# the made set with its stamp.
hdtext/
files-*/
tmp-*/
cache.json
cache.json.tmp
floors.json
stamps.json
book.json
layout.json
confirm.json

# The retired 2.0.0 player builder: never public.
hdf_builder.py
hdf_inputs.py
discimg.py
test_discimg.py

# Your own copies of the games and anything taken out of them (disc images, extracted discs, the Wii U title,
# saves): never commit them, not even one file.
""" + ''.join('*%s\n' % e for e in GAME_EXT) + ''.join('%s\n' % n for n in GAME_NAMES) + """
# Images, media, fonts, archives, binaries, scripts, logs and tables: this repository has none, except the
# package's two neutral images (drawn by tools/dist/base_art.py, no game art) and the README's gameplay animation,
# below.
""" + ''.join('*%s\n' % e for e in OTHER_EXT) + ''.join('!/%s\n' % r for r in sorted(BINARY_OK)) + """
# Dusklight's source code: cmake/FetchDusklight.cmake downloads it at configure time.
/dusklight/

# Build trees and outputs (hdf-convert writes its test outputs into the build folder).
build/
build-*/
cmake-build-*/
/dist/
/publish/
mod_data/
.vs/
.vscode/
.idea/
CMakeUserPresets.json

# Python.
__pycache__/
*.pyc

# System and private files.
Thumbs.db
desktop.ini
.DS_Store
.env

# Developer-only material that stays on the developer's PC.
/research/
/reference/
/cache/
/testenv/
/backups/
/tools/dist/legacy/
/RESUME.md
/private_scan_patterns.txt
"""
# Paths that must be git-ignored (checked on every run, with every EXT_NEVER type added).
MUST_IGNORE = ['mods/hdfeatures/res/icon.png', 'mods/hdfeatures/res/cave/floors.json',
               'mods/hdfeatures/overlay/res/Stage/D_SB11/R00_00.arc', 'output (DO NOT SHARE)/hd_features.dusk',
               'cache/legacy-generated/res/cave/floors.json', 'cache/legacy-generated/res/stamps/stamps/stamps.json',
               'cache/legacy-generated/overlay/res/Stage/D_SB11/R00_00.arc',
               'cache/oracle/hd_features-oracle-2.0.0.dusk', 'cache/gc_extract/files/res/Msgus/bmgres.arc',
               'mods/hdfeatures/package/res/other.png', 'mods/hdfeatures/package/res/x/icon.png',
               'mods/hdfeatures/package/res/hd_features.dusk',
               'tools/dist/legacy/hdf_builder.py', 'tools/dist/legacy/hdf_inputs.py', 'tools/dist/legacy/discimg.py',
               'tools/dist/legacy/test_discimg.py', 'tools/dist/legacy/release/BUILD-HD-FEATURES.bat',
               'tools/dist/legacy/release/README.txt', 'tools/dist/BUILD-HD-FEATURES.bat',
               'dist/hd_features-2.1.0.dusk', 'dist/RELEASE-NOTES-2.1.0.md', 'hd_features-2.1.0.dusk',
               'mod_data/local.ricky.hdfeatures/cache.json',
               'build-conv/hdf-oracle-out/res/Stage/D_SB11/STG_00.arc', 'build-hdf/mods/hd_features.dusk',
               'gc/sys/main.dol', 'gc/sys/boot.bin', 'gc/sys/bi2.bin', 'gc/sys/apploader.img', 'gc/sys/fst.bin',
               'gc/files/opening.bnr', 'gc/files/RELS.arc', 'gc/files/map/Final/Release/frameworkF.map',
               'gc/files/Audiores/Z2Sound.baa', 'tphd/code/Zelda.rpx', 'tphd/code/app.xml', 'tphd/code/cos.xml',
               'tphd/meta/meta.xml', 'tphd/meta/iconTex.tga', 'tphd/content/Common/x.pack', 'nus/00000000.app',
               'nus/00000001.h3', 'nus/title.tmd', 'nus/title.tik', 'nus/title.cert', 'Twilight Princess HD.wua',
               'TP.rvz', 'dusklight/README.md', 'build-hdfeatures/build.ninja', 'cmake-build-debug/x',
               'out/build/x64-Release/build.ninja', '.vs/x', '.idea/x', 'dist/HD-Features-v2.0.0.zip',
               'HD-Features-v2.0.0.zip', 'sdk/windows-amd64.lib', 'screenshot.png', 'research/notes.md',
               'tools/__pycache__/x.pyc', 'Thumbs.db', 'desktop.ini', '.env', 'RESUME.md',
               'private_scan_patterns.txt',
               # wave 4 review L3: a build_assets.py --out-dir oracle and an hdf-convert make output inside the repo
               'oracle/res/stamps/hdtext/messages.json', 'oracle/res/cave/hdtext/README.md',
               'oracle/res/cave/hdtext/flows.json', 'oracle/res/cave/floors.json',
               'oracle/res/stamps/stamps/stamps.json', 'oracle/res/stamps/stampbook/book.json',
               'oracle/res/cave/amiibo/layout.json', 'oracle/res/cave/confirm/confirm.json',
               'scratch/oracle/res/cave/floors.json', 'made/cache.json', 'made/cache.json.tmp',
               'made/files-efd9a593/res/stamps/stamps/stamps.json', 'made/files-efd9a593/res/x.md',
               'made/tmp-1/x.md', 'tools/dist/hdf_builder.py', 'tools/dist/hdf_inputs.py', 'tools/dist/discimg.py',
               'tools/dist/test_discimg.py',
               # the README animation's exception is that one path: any other image stays ignored
               'docs/other.gif', 'docs/showcase.png', 'docs/x/showcase.gif', 'mods/hdfeatures/docs/showcase.gif']
# Paths the repo rules must refuse (checked on every run): the image exceptions are exact paths, not file types.
MUST_REFUSE = ['docs/other.gif', 'docs/showcase.png', 'docs/x/showcase.gif', 'mods/hdfeatures/docs/showcase.gif',
               'showcase.gif', 'mods/hdfeatures/package/res/other.png']


# ============================================================================================= helpers
def rel_of(p, root):
    return os.path.relpath(p, root).replace('\\', '/')


def upstream_pins():
    """{path: sha256} from the tables of third_party/*/UPSTREAM.md."""
    out = {}
    for rel in THIRD_PARTY_DOCS[1:]:
        with open(os.path.join(PROJECT, *rel.split('/')), encoding='utf-8') as f:
            for sha, name in re.findall(r'^\|\s*([0-9a-f]{64})\s*\|\s*([^|\s]+)\s*\|', f.read(), re.M):
                out['%s/%s' % (os.path.dirname(rel), name)] = sha
    return out


def collect():
    """{repo path: source path}, [errors]."""
    files, errors = {}, []
    for rel in FILES + sorted(PACKAGE_RES) + list(THIRD_PARTY_DOCS) + sorted(THIRD_PARTY_PINS):
        p = os.path.join(PROJECT, *rel.split('/'))
        if os.path.isfile(p):
            files[rel] = p
        else:
            errors.append('allowlisted file is missing: %s' % rel)
    for top, exts in TREES:
        root = os.path.join(PROJECT, *top.split('/'))
        if not os.path.isdir(root):
            errors.append('allowlisted folder is missing: %s/' % top)
            continue
        for dp, dn, fn in os.walk(root):
            dn[:] = sorted(d for d in dn if d != '__pycache__')
            for f in sorted(fn):
                rel = rel_of(os.path.join(dp, f), PROJECT)
                if f.lower().endswith(exts):
                    files[rel] = os.path.join(dp, f)
                else:
                    errors.append('unexpected file in %s/: %s (not copied: allowlist it or move it away)'
                                  % (top, rel))
    # Folders that must hold exactly their pinned files: anything new there is a decision, not a copy.
    for top, allowed in (('third_party', set(THIRD_PARTY_PINS) | set(THIRD_PARTY_DOCS)),
                         (PACKAGE_RES_DIR, set(PACKAGE_RES))):
        for dp, dn, fn in os.walk(os.path.join(PROJECT, *top.split('/'))):
            for f in fn:
                rel = rel_of(os.path.join(dp, f), PROJECT)
                if rel not in allowed:
                    errors.append('unexpected file in %s/: %s (pin it in make_repo.py / make_release.py, or move '
                                  'it away)' % (top, rel))
    dev = os.path.join(PROJECT, 'tools', 'dev')
    for f in sorted(os.listdir(dev)) if os.path.isdir(dev) else ():
        if os.path.isfile(os.path.join(dev, f)) and os.path.splitext(f)[0] not in DEV + DEV_LOCAL_ONLY:
            errors.append('new file tools/dev/%s: put it in DEV (published) or DEV_LOCAL_ONLY in make_repo.py' % f)
    for f in sorted(os.listdir(HERE)):                       # tools/dist: the listed files, plus legacy/ (local)
        if f in ('legacy', '__pycache__') and os.path.isdir(os.path.join(HERE, f)):
            continue
        if 'tools/dist/' + f not in FILES:
            errors.append('new entry tools/dist/%s: allowlist it in FILES, or move it to tools/dist/legacy/' % f)
    up = upstream_pins()
    if up != THIRD_PARTY_PINS:
        for rel in sorted(set(up) | set(THIRD_PARTY_PINS)):
            if up.get(rel) != THIRD_PARTY_PINS.get(rel):
                errors.append('third_party: %s is pinned as %s here but %s in its UPSTREAM.md' % (
                    rel, (THIRD_PARTY_PINS.get(rel) or 'nothing')[:16], (up.get(rel) or 'nothing')[:16]))
    if os.path.isdir(DOCS):
        for dp, dn, fn in os.walk(DOCS):
            dn[:] = sorted(d for d in dn if d not in ('__pycache__', '.git'))
            for f in sorted(fn):
                rel = rel_of(os.path.join(dp, f), DOCS)
                if rel in files or rel == '.gitignore':
                    errors.append('repo_docs/%s would replace a project file of the same path' % rel)
                else:
                    files[rel] = os.path.join(dp, f)
    for rel in sorted(SHOWCASE):
        if rel not in files:
            errors.append('pinned file is missing: repo_docs/%s (README.md shows it)' % rel)
    for d in REQUIRED_DOCS:
        if d not in files:
            errors.append('DOCS: repo_docs/%s is missing' % d)
    return files, errors


def _imports(path):
    """Top-level names a Python file imports (anywhere in it), plus 'x' for a 'x.py' string constant."""
    with open(path, encoding='utf-8') as f:
        tree = ast.parse(f.read(), path)
    names = set()
    for n in ast.walk(tree):
        if isinstance(n, ast.Import):
            names.update(a.name.split('.')[0] for a in n.names)
        elif isinstance(n, ast.ImportFrom) and n.level == 0 and n.module:
            names.add(n.module.split('.')[0])
        elif isinstance(n, ast.Constant) and isinstance(n.value, str) and re.fullmatch(r'\w+\.py', n.value):
            names.add(n.value[:-3])
    return names


def trace(files):
    """[errors]: project modules imported but not allowlisted; allowlisted Python files nothing uses."""
    py = {rel for rel in files if rel.endswith('.py') and os.path.commonpath(
        [os.path.abspath(files[rel]), PROJECT]) == PROJECT and not files[rel].startswith(DOCS)}
    tools = os.path.join(PROJECT, 'tools')
    entries = [r for r in py if r.startswith(('tools/dist/', 'tools/dev/')) or r in TRACE_ENTRIES]
    reached, stack, errors = set(), list(entries), []
    while stack:
        rel = stack.pop()
        if rel in reached:
            continue
        reached.add(rel)
        here = os.path.dirname(os.path.join(PROJECT, *rel.split('/')))
        for name in sorted(_imports(os.path.join(PROJECT, *rel.split('/')))):
            for d in [os.path.join(tools, s) for s in TRACE_DIRS] + [here, tools]:
                cand = os.path.join(d, name + '.py')
                if os.path.isfile(cand):
                    target = rel_of(cand, PROJECT)
                    if target in py:
                        stack.append(target)
                    else:
                        errors.append('%s imports %s, which is not in the repo allowlist' % (rel, target))
                    break
    for rel in sorted(py - reached):
        if not rel.endswith('__init__.py'):
            errors.append('allowlisted but unused (nothing imports it): %s' % rel)
    return errors, len(reached)


def gitignore():
    """The repo .gitignore: build_assets.py's own rules (so it never appends to it) + the repo rules."""
    with open(os.path.join(PROJECT, 'tools', 'build_assets.py'), encoding='utf-8') as f:
        tree = ast.parse(f.read())
    block = next(n.value.value for n in tree.body if isinstance(n, ast.Assign) and
                 any(getattr(t, 'id', None) == 'GITIGNORE_BLOCK' for t in n.targets))
    rules, have = [], set(GITIGNORE_HEAD.splitlines()) | set(GITIGNORE_TAIL.splitlines())
    for line in block.splitlines():
        line = line.strip()
        if line and not line.startswith('#') and line not in rules and line not in have:
            rules.append(line)
    return GITIGNORE_HEAD + '\n'.join(rules) + '\n' + GITIGNORE_TAIL


def _glob_re(pat):
    """A .gitignore glob as a regex: '*' and '?' never cross '/'. No '**' or '[...]' (this file has none)."""
    return ''.join('[^/]*' if c == '*' else '[^/]' if c == '?' else re.escape(c) for c in pat)


def git_ignored(rel, text):
    """The rule of the .gitignore `text` that makes git ignore the file `rel`, or None. Matches like git on
    Windows (core.ignorecase): a rule without '/' matches a file or folder name at any depth, a rule with a '/'
    (or a leading '/') matches from the top, a trailing '/' matches folders only, the last matching rule wins
    and a '!' rule re-includes. As in git, nothing inside an ignored folder can be re-included."""
    rules = []
    for line in text.splitlines():
        rule = line.strip()
        if not rule or rule.startswith('#'):
            continue
        if '**' in rule or '[' in rule:
            return 'unsupported rule %r' % rule
        neg = rule.startswith('!')
        body = rule[1:] if neg else rule
        dir_only = body.endswith('/')
        pat = body.rstrip('/')
        rules.append((neg, dir_only, '/' in pat, re.compile(_glob_re(pat.lstrip('/')), re.I), rule))
    parts = rel.split('/')
    for i in range(1, len(parts) + 1):
        is_dir = i < len(parts)
        hit = None
        for neg, dir_only, anchored, rx, rule in rules:
            if dir_only and not is_dir:
                continue
            if rx.fullmatch('/'.join(parts[:i]) if anchored else parts[i - 1]):
                hit = None if neg else rule
        if hit:
            return hit                   # an ignored folder (is_dir) hides everything in it
    return None


def gitignore_problems(staged, text):
    """[problems]: a repo file the .gitignore hides from `git add`, or a never-ship path it lets through."""
    probs = []
    for rel in sorted(staged):
        r = git_ignored(rel, text)
        if r:
            probs.append('%s: git would ignore it (.gitignore rule %r), so it would be missing from the repo' % (rel, r))
    must = MUST_IGNORE + ['x/file%s' % e for e in sorted(EXT_NEVER | set(GAME_EXT) | set(OTHER_EXT))]
    for rel in must:
        if not git_ignored(rel, text):
            probs.append('.gitignore: does not ignore %s (a never-commit path)' % rel)
    return probs


def eol_rules(text):
    """[(pattern, 'lf' | 'crlf' | 'keep')] from a .gitattributes text (only what this repo uses)."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        pat, attrs = line.split()[0], line.split()[1:]
        eol = None
        for a in attrs:
            if a in ('-text', 'binary'):
                eol = 'keep'
            elif a.startswith('eol='):
                eol = a[4:]
        if eol:
            out.append((pat, eol))
    return out


def eol_for(rel, rules):
    eol = 'keep'
    for pat, e in rules:
        if fnmatch.fnmatchcase(rel, pat) or fnmatch.fnmatchcase(rel.rsplit('/', 1)[-1], pat):
            eol = e
    return eol


def _force_remove(func, p, _exc):
    os.chmod(p, stat.S_IWRITE)
    func(p)


def wipe(out):
    """Delete everything in out except .git/ (a repo made there by hand survives a regeneration)."""
    for n in os.listdir(out):
        if n == '.git':
            continue
        p = os.path.join(out, n)
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p, onerror=_force_remove)
        else:
            os.chmod(p, stat.S_IWRITE)
            os.remove(p)


def write_tree(root, files):
    for rel, data in sorted(files.items()):
        p = os.path.join(root, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)


def read_tree(root):
    """{path: bytes} of every file under root, except .git/."""
    out = {}
    for dp, dn, fn in os.walk(root):
        dn[:] = sorted(d for d in dn if d != '.git')
        for f in sorted(fn):
            p = os.path.join(dp, f)
            with open(p, 'rb') as fh:
                out[rel_of(p, root)] = fh.read()
    return out


def mark(out, name, head, lines):
    """Write a do-not-publish marker into out (created if needed). It is not git-ignored, so `git status` shows it."""
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(head + '\n\n' + '\n'.join(lines) + '\n')


def check_out_folder(out):
    out = os.path.normcase(os.path.abspath(out))
    proj = os.path.normcase(PROJECT)
    if os.path.basename(out) != REPO_NAME:
        raise SystemExit('--out must be a folder named %s (it is wiped)' % REPO_NAME)
    if out == proj or proj.startswith(out + os.sep):
        raise SystemExit('--out must not be the project or one of its parents')
    if out.startswith(proj + os.sep) and not out.startswith(os.path.join(proj, 'publish') + os.sep):
        raise SystemExit('--out inside the project must be under publish/')


# ============================================================================================= repo scan
def _byte_runs(text):
    """Byte strings written in a text file: 0x.. lists, \\x.. escapes, 0x........ words (both byte orders),
    long hex strings."""
    out = []
    for m in re.finditer(r'(?:0x[0-9a-fA-F]{2}\b[\s,]*){8,}', text):
        out.append(bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group())))
    for m in re.finditer(r'(?:\\x[0-9a-fA-F]{2}){8,}', text):
        out.append(bytes(int(x, 16) for x in re.findall(r'\\x([0-9a-fA-F]{2})', m.group())))
    for m in re.finditer(r'(?:0x[0-9a-fA-F]{8}\b[uUlL]*[\s,]*){3,}', text):
        words = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{8})', m.group())]
        out.append(b''.join(struct.pack('>I', w) for w in words))
        out.append(b''.join(struct.pack('<I', w) for w in words))
    for m in re.finditer(r'\b(?:[0-9a-fA-F]{2}){12,}\b', text):
        out.append(bytes.fromhex(m.group()))
    return out


def _author_hits(text, author):
    """The author name anywhere except the mod id (local.<author>. / local_<author>_) and 'Ricky' as written."""
    bad = []
    for m in re.finditer(r'(?<![A-Za-z0-9])%s(?![A-Za-z0-9])' % re.escape(author), text, re.I):
        before, after = text[max(0, m.start() - 8):m.start()], text[m.end():m.end() + 2]
        if re.search(r'local(?:\.|_|\\\.)$', before) and after[:1] in ('.', '_', '\\'):
            continue
        if m.group() == AUTHOR_NAME:
            continue
        bad.append(text[max(0, m.start() - 25):m.end() + 25].replace('\n', ' '))
    return bad


class RepoScan(object):
    # Project folders whose files must never be copied into the repo (generated game files, research notes).
    NEVER_COPY = ('research', os.path.join('mods', 'hdfeatures', 'res'), os.path.join('mods', 'hdfeatures', 'overlay'),
                  os.path.join('cache', 'legacy-generated'))

    def __init__(self, sc, own=()):
        """own: sha256 of the allowlisted files themselves (research/ keeps snapshots of some sources: a copy
        of our own source file is not a research note)."""
        self.sc = sc
        self.problems = sc.problems
        self.positions = []
        for label, rec in sc.records:
            if label.endswith('position (big-endian)') and len(rec) == 12:
                xyz = struct.unpack('>3f', rec)
                if all(v == v and abs(v) < 1e7 for v in xyz):
                    self.positions.append((label, xyz))
        self.extra_ref = {}                      # sha256 -> project file that must never be copied
        for top in self.NEVER_COPY:
            for dp, dn, fn in os.walk(os.path.join(PROJECT, top)):
                for f in fn:
                    p = os.path.join(dp, f)
                    try:
                        if os.path.getsize(p) <= 64 << 20:
                            with open(p, 'rb') as fh:
                                h = hashlib.sha256(fh.read()).hexdigest()
                            if not (top == 'research' and h in own):
                                self.extra_ref[h] = rel_of(p, PROJECT)
                    except OSError:
                        pass

    def path_rules(self, rel):
        parts = rel.split('/')
        low = [x.lower() for x in parts]
        package_res = rel in PACKAGE_RES
        if low[0] in TOP_NEVER or low[0].startswith('build-'):
            self.problems.append('%s: never-ship top folder %s/' % (rel, parts[0]))
        for x in low[:-1]:
            if x in ANY_NEVER and not (package_res and x == 'res'):
                self.problems.append('%s: never-ship folder %s/' % (rel, x))
        if low[-1] in NAME_NEVER:
            self.problems.append('%s: never-ship file name%s' % (
                rel, ' (the retired player builder)' if low[-1] in RETIRED else ''))
        ext = os.path.splitext(low[-1])[1]
        if ext in EXT_NEVER and rel not in BINARY_OK:
            self.problems.append('%s: never-ship file type %s' % (rel, ext))
        for label, p in self.sc.personal:
            m = p.search(rel)
            if m:
                self.problems.append('%s: the path has %s: %r' % (rel, label, m.group()))

    def pinned(self, rel, data):
        """Checks of a pinned file (package res, third_party). True: it is pinned (and needs no text scan)."""
        h = hashlib.sha256(data).hexdigest()
        if rel in THIRD_PARTY_PINS:
            if h != THIRD_PARTY_PINS[rel]:
                self.problems.append('%s: sha256 %s, not the pinned upstream %s' % (rel, h[:16],
                                                                                  THIRD_PARTY_PINS[rel][:16]))
            return True
        if rel in PACKAGE_RES or rel in SHOWCASE:
            size, sha = PACKAGE_RES.get(rel) or SHOWCASE[rel]
            if (len(data), h) != (size, sha):
                self.problems.append('%s: %d bytes sha256 %s, not the pinned %d bytes %s' % (
                    rel, len(data), h[:16], size, sha[:16]))
            if rel in BINARY_OK:
                # PNG: same bytes / pixels as a game file? GIF: game-file bytes or HD records inside?
                self.sc.scan(rel, data, folders=False, text=False)
                return True
        return False

    def content(self, rel, data):
        if self.pinned(rel, data):
            return
        h = hashlib.sha256(data).hexdigest()
        if h in self.extra_ref:
            self.problems.append('%s: same bytes as the project file %s' % (rel, self.extra_ref[h]))
        if b'\0' in data:
            self.problems.append('%s: not a text file (NUL bytes)' % rel)
            return
        try:
            text = data.decode('utf-8')
        except UnicodeDecodeError as e:
            self.problems.append('%s: not UTF-8 text (%s)' % (rel, e))
            return
        if b'\r' in data:
            self.problems.append('%s: CR line endings left' % rel)
        self.sc.scan(rel, data, folders=False, text=True, allow=NOT_PERSONAL.get(rel, ()))
        for s in _author_hits(text, mr.AUTHOR):
            self.problems.append('%s: the name %r outside the mod id: %r' % (rel, mr.AUTHOR, s))
        runs = _byte_runs(text)
        for label, rec in self.sc.records:
            if len(rec) >= 12 and any(rec in r for r in runs):
                self.problems.append('%s: the %s written as bytes' % (rel, label))
        nums = {round(float(x), 1) for x in re.findall(r'-?\d+\.\d+', text)}

        def has(v):
            r = round(v, 1)
            return any(round(r + d, 1) in nums for d in (-0.1, 0.0, 0.1))
        for label, xyz in self.positions:
            big = [v for v in xyz if abs(v) >= 10]
            if len(big) >= 2 and all(has(v) for v in big):
                self.problems.append('%s: the numbers of the %s (%s)' % (
                    rel, label, ', '.join('%.1f' % v for v in xyz)))


# ============================================================================================= disclaimer
NOT_AFF = r'not\W+(?:\w+\W+){0,3}?(?:affiliated|endorsed)|no\W+affiliation|unaffiliated'


def disclaimer_checks(out):
    """[issues]: the user's disclaimer must be in every place it was asked for."""
    issues = []

    def read(rel):
        p = os.path.join(out, *rel.split('/'))
        if not os.path.isfile(p):
            return None
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.read()

    def need(rel, text, what, pat):
        if text is not None and not re.search(pat, text, re.I | re.S):
            issues.append('%s: does not mention %s' % (rel, what))

    readme = read('README.md')
    if readme is None:
        issues.append('README.md is missing (put it in repo_docs/)')
    else:
        head = '\n'.join(readme.splitlines()[:40]) + '\n'
        m = re.search(r'^>\s*\[!IMPORTANT\][^\n]*\n((?:>[^\n]*\n)+)', head, re.M)
        if not m:
            issues.append('README.md: no "> [!IMPORTANT]" alert box in its first 40 lines')
        else:
            need('README.md alert box', m.group(1), '"not affiliated"', NOT_AFF)
            need('README.md alert box', m.group(1), 'Dusklight', r'Dusklight')
            need('README.md alert box', m.group(1), 'the AI help', r'\bAI\b')
            need('README.md alert box', m.group(1), 'personal use', r'personal')
        need('README.md', readme, 'a link to DISCLAIMER.md', r'\(\s*(?:\./)?DISCLAIMER\.md(?:#[^)\s]*)?\s*\)')
        need('README.md', readme, 'the mod_data folder (uninstall)', r'mod_data')
        need('README.md', readme, 'the .wua conversion', r'\.wux.*\.wua')
    disc = read('DISCLAIMER.md')
    if disc is None:
        issues.append('DISCLAIMER.md is missing (put it in repo_docs/)')
    for what, pat in (('"not affiliated"', NOT_AFF), ('Dusklight', r'Dusklight'), ('TwilitRealm', r'TwilitRealm'),
                      ('https://twilitrealm.dev/', r'https://twilitrealm\.dev'), ('the no-AI policy / AI help',
                                                                                  r'\bAI\b'),
                      ('Claude', r'\bClaude\b'), ('Nintendo', r'Nintendo'), ('trademarks', r'trademark'),
                      ('Dolphin', r'Dolphin'), ('Cemu', r'Cemu'), ('no warranty', r'warranty'),
                      ('zeldaret/tp', r'zeldaret'), ('respect / thanks', r'respect|thank'),
                      ('personal use', r'personal')):
        need('DISCLAIMER.md', disc, what, pat)
    lic = read('LICENSE')
    if lic is None:
        issues.append('LICENSE is missing (put it in repo_docs/)')
    need('LICENSE', lic, 'the MIT License', r'MIT License')
    need('LICENSE', lic, '"Copyright (c) 2026 %s"' % AUTHOR_NAME, r'Copyright \(c\) 2026 ' + AUTHOR_NAME)
    notice = read('NOTICE.md')
    for what, pat in (('zlib', r'zlib'), ('Zstandard (BSD-3)', r'Zstandard.*BSD'), ('AMD addrlib', r'addrlib'),
                      ('"no network"', r'no network|uses no network'), ('the data folder', r'data folder')):
        need('NOTICE.md', notice, what, pat)
    notes = mr.release_notes(mr.RELEASE_VERSION)            # the GitHub release text (make_release.py)
    for what, pat in (('"not affiliated"', NOT_AFF), ('Dusklight', r'Dusklight'), ('the AI help', r'\bAI\b'),
                      ('personal use', r'personal'), ('no Nintendo files', r'no Nintendo files')):
        need('release notes (make_release.py)', notes, what, pat)
    found = False
    for dp, dn, fn in os.walk(os.path.join(out, 'mods', 'hdfeatures', 'src')):
        for f in fn:
            with open(os.path.join(dp, f), encoding='utf-8', errors='replace') as fh:
                t = fh.read()
            for m in re.finditer(r'unofficial', t, re.I):
                win = t[max(0, m.start() - 300):m.end() + 400]
                if re.search(NOT_AFF, win, re.I) and 'Dusklight' in win:
                    found = True
    if not found:
        issues.append('in-game text (mods/hdfeatures/src): no "Unofficial ... not affiliated ... Dusklight" line')
    return issues


# ============================================================================================= main
def main(argv=None):
    ap = argparse.ArgumentParser(description='HD Features public repository folder')
    ap.add_argument('--hd', required=True)
    ap.add_argument('--gc', default=os.path.join(PROJECT, 'cache', 'gc_extract'))
    ap.add_argument('--oracle', default=mr.DEFAULT_ORACLE)
    ap.add_argument('--out', default=os.path.join(PROJECT, 'publish', REPO_NAME))
    ap.add_argument('--draft', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args(argv)
    out = os.path.abspath(a.out)
    check_out_folder(out)
    gc = a.gc if os.path.isdir(os.path.join(a.gc, 'res')) else os.path.join(a.gc, 'files')
    if not os.path.isfile(a.oracle):
        print('error: %s not found (pass --oracle)' % a.oracle)
        return 2
    if mr.file_sha256(a.oracle) != mr.oracle_pin():
        print('error: %s is not the pinned oracle (tools/dev/gen_tables.py ORACLE_SHA256)' % a.oracle)
        return 2

    # 1. allowlist
    files, errors = collect()
    terr, n_traced = trace(files)
    errors += terr
    doc_missing = [e for e in errors if e.startswith('DOCS: ')]
    errors = [e for e in errors if not e.startswith('DOCS: ')]

    # 2. stage into a new temporary folder next to --out: --out itself is only changed once everything passed
    #    (never with --dry-run)
    with open(os.path.join(PROJECT, '.gitattributes'), encoding='utf-8') as f:
        rules = eol_rules(f.read())
    for rel in sorted(THIRD_PARTY_PINS)[:1] + sorted(BINARY_OK):
        if eol_for(rel, rules) != 'keep':
            errors.append('.gitattributes: %s would get its line endings changed (needs third_party/** -text, '
                          '*.png binary and docs/showcase.gif binary)' % rel)
    staged, own = {}, set()
    for rel, src in sorted(files.items()):
        with open(src, 'rb') as f:
            data = f.read()
        if not src.startswith(DOCS):
            own.add(hashlib.sha256(data).hexdigest())
        eol = eol_for(rel, rules)
        if eol == 'lf':
            data = data.replace(b'\r\n', b'\n')
        elif eol == 'crlf':
            data = mr.to_crlf(data)
        staged[rel] = data
        if not src.startswith(DOCS):
            own.add(hashlib.sha256(data).hexdigest())
    staged['.gitignore'] = gitignore().encode('utf-8')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='.staging-', dir=os.path.dirname(out))
    try:
        write_tree(tmp, staged)
        print('staged %d files into the temporary folder %s (%d Python files import-traced)' % (
            len(staged), tmp, n_traced))

        # 3. scan what was written (read back from disk, plus anything else that is there)
        print('scanning ...')
        sc = mr.Scanner(a.oracle, a.hd, gc)
        rs = RepoScan(sc, own)
        on_disk = read_tree(tmp)
        for rel in sorted(set(on_disk) ^ set(staged)):
            rs.problems.append('%s: on disk but not staged, or staged but not on disk' % rel)
        for rel, data in sorted(on_disk.items()):
            rs.path_rules(rel)
            rs.content(rel, data)
        for rel in MUST_REFUSE:                  # self-test: the path rules still refuse these (no repo file)
            n = len(rs.problems)
            rs.path_rules(rel)
            refused = len(rs.problems) > n
            del rs.problems[n:]
            if not refused:
                rs.problems.append('%s: the repo rules let this never-ship path through (MUST_REFUSE)' % rel)
        gi = on_disk.get('.gitignore', b'').decode('utf-8')
        for line in gitignore().splitlines():
            if line and not line.startswith('#') and line not in gi.splitlines():
                rs.problems.append('.gitignore: rule %r missing' % line)
        rs.problems += gitignore_problems(on_disk, gi)
        print('  %d messages compared (%s), %d reference hashes (+%d project files), %d HD records' % (
            sc.message_count, ', '.join('%s %s' % l for l in sc.languages), len(sc.ref_sha), len(rs.extra_ref),
            len(sc.records)))

        # 4. disclaimer
        issues = disclaimer_checks(tmp)

        # report
        print()
        total = 0
        for rel, data in sorted(on_disk.items()):
            total += len(data)
            print('  %-62s %9d' % (rel, len(data)))
        print('  %d files, %d bytes' % (len(on_disk), total))
        notes = sorted(set(sc.notes))
        if notes:
            print()
            print('%d note(s) to read (not failures):' % len(notes))
            for n in notes:
                print('  note: ' + n)
        probs = errors + sorted(set(rs.problems))
        soft = [s[len('DOCS: '):] for s in doc_missing] + issues
        print()
        if soft:
            print('%s: %d disclaimer / document issue(s)' % (
                'WARNING (--draft)' if a.draft else 'DISCLAIMER CHECK FAILED', len(soft)))
            for s in soft:
                print('  ' + s)
            if not a.draft:
                probs += soft
        if probs:
            print('REPO SCAN FAILED: %d problem(s). %s was NOT changed; do not publish it.' % (len(probs), out))
            for p in probs[:80]:
                print('  ' + p)
            if not a.dry_run:
                mark(out, FAIL_MARK, 'make_repo.py found problems, so it did not update this folder. Do not '
                                     'commit or publish it until make_repo.py passes again.', probs)
            return 1
        if a.dry_run:
            print('DRY RUN PASSED: %d files scanned; %s was not touched.' % (len(on_disk), out))
            return 0

        # 5. only now: replace --out (its .git/ is kept) with the scanned files, and check the copy
        os.makedirs(out, exist_ok=True)
        wipe(out)
        write_tree(out, staged)
        back = read_tree(out)
        bad = sorted(set(back) ^ set(staged)) + sorted(r for r in set(back) & set(staged) if back[r] != staged[r])
        if bad:
            print('REPO COPY FAILED: %s differs from the scanned files: %s' % (out, ', '.join(bad[:5])))
            mark(out, FAIL_MARK, 'The copy into this folder differs from the scanned files. Do not commit or '
                                 'publish it until make_repo.py passes again.', bad)
            return 1
        if soft:
            mark(out, DRAFT_MARK, 'Draft (--draft): the documents are not final. Do not commit or publish this '
                                  'folder until make_repo.py passes without --draft.', soft)
        print('REPO SCAN PASSED: %d files written to %s; no Nintendo-derived content or personal details found%s.'
              % (len(back), out, ' (draft: documents not final, %s written)' % DRAFT_MARK if soft else ''))
        if not soft:
            print()
            print('Next, by hand, after reading the files (this script never runs git):')
            print('  cd "%s"' % out)
            print('  git init -b main')
            print('  git config user.name "%s"' % AUTHOR_NAME)
            print('  git config user.email "<ID>+<GITHUB-USERNAME>@users.noreply.github.com"   '
                  '(GitHub: Settings > Emails)')
            print('  git add -A && git status')
            print('Before the first push, turn on GitHub > Settings > Emails > "Keep my email addresses private" and '
                  '"Block command line pushes that expose my email".')
        return 0
    finally:
        shutil.rmtree(tmp, onerror=_force_remove)


if __name__ == '__main__':
    sys.exit(main())
