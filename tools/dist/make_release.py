#!/usr/bin/env python3
"""make_release.py - make the HD Features release file (run on the developer PC only).

usage:
  python -I -B tools/dist/make_release.py --hd "<TP HD content folder>" [--gc <GC extraction root>]
                                          [--dusk <hd_features.dusk>] [--oracle <oracle .dusk>]
                                          [--out <dir>] [--dry-run]

  --hd       the TP HD dump's content folder (it has res/Msgus/bmgres*.arc): only read, for the text scan.
  --gc       the GC extraction (default: <project>/cache/gc_extract): its message text and sys/main.dol, only read.
  --dusk     the package CMake built (default: <project>/build-hdf/mods/hd_features.dusk).
  --oracle   the pinned 2.0.0 oracle package (default: <project>/cache/oracle/hd_features-oracle-2.0.0.dusk; its
             SHA-256 must be the pin in tools/dev/gen_tables.py). Only read: its game files are the "never ship
             these bytes" list of the scan.
  --out      output folder (default: <project>/dist).
  --dry-run  do every check and the whole scan, but write and delete nothing.

The release is the .dusk itself: players drop it into Dusklight's mods folder. There is no zip and nothing to run.
  1. Package check (plan Correction #12): exactly mod.json (id local.ricky.hdfeatures, version RELEASE_VERSION,
     the version in mods/hdfeatures/CMakeLists.txt), lib/windows-amd64/mod.dll (a 64-bit DLL), res/icon.png,
     res/banner.png and res/THIRD-PARTY-NOTICES.txt. The 3 res files must equal mods/hdfeatures/package/res and
     the pins below. Directory entries are dropped: the package is repacked with fixed dates, in the same order.
     A personal build (a name with PERSONAL, or a build tree with HDF_PERSONAL_ART set) is refused.
  2. Content scan (Scanner) of the repacked .dusk, each file in it and the release texts: game-format magic
     numbers and extensions, real game-format headers inside a binary (a PNG only with IHDR right after the
     signature), any file whose sha256 or PNG pixels equal a game file of the oracle or an HD-art PNG of art/,
     32-byte runs shared with those files or with GC main.dol, the HD-only chest / talk-tag records and their
     positions, 6-word runs of any TP HD (every language) or GC message, the HD stamp names in the DLL, personal
     paths and e-mail addresses (extra private patterns: <project>/private_scan_patterns.txt, never shipped).
     5-word runs, whole short messages and the author's name outside the mod id are printed as notes to read.
  3. Only if every check passed: <out>/hd_features-<version>.dusk, <out>/hd_features-<version>.dusk.sha256 and
     <out>/RELEASE-NOTES-<version>.md (the GitHub release text).
Exit 0 = passed (and written, unless --dry-run); 1 = a problem: nothing is written, and an older release file of
the same version in --out is removed; 2 = bad arguments or a missing input.
"""
import argparse
import ast
import glob
import hashlib
import io
import json
import os
import re
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(PROJECT, 'tools')

MOD_ID = 'local.ricky.hdfeatures'
RELEASE_VERSION = '2.1.0'               # must equal mod.json and HDFEATURES_VERSION in mods/hdfeatures/CMakeLists.txt
DUSKLIGHT_VERSION = '2.0.3'
ZIP_TIME = (2026, 1, 1, 0, 0, 0)        # every entry of the repacked .dusk (no build date in the release)
DLL = 'lib/windows-amd64/mod.dll'
PACKAGE_RES = os.path.join(PROJECT, 'mods', 'hdfeatures', 'package', 'res')
# The 3 res files (size, sha256). A changed icon, banner or notices text is checked by eye, then pinned here again.
PINNED_RES = {
    'res/icon.png': (112855, '4f421c78a5b2b8862c0adadff34316bceff8e60d136c3e1a77137de268084853'),
    'res/banner.png': (247018, '76dd9e6fc47f53966ff71c3f974b930869d60194c0288bf0d817c2fc048b2ac8'),
    'res/THIRD-PARTY-NOTICES.txt': (4262, '95ded77c9f5ee0ba7dcdbfd73e60f6cb3de36612f437a45a971bc2704bfdf77e'),
}
ALLOWED = ('mod.json', DLL) + tuple(PINNED_RES)
DEFAULT_DUSK = os.path.join(PROJECT, 'build-hdf', 'mods', 'hd_features.dusk')
DEFAULT_ORACLE = os.path.join(PROJECT, 'cache', 'oracle', 'hd_features-oracle-2.0.0.dusk')
GEN_TABLES = os.path.join(TOOLS, 'dev', 'gen_tables.py')     # holds the oracle pin (ORACLE_SHA256, ORACLE_SIZE)
CMAKE_MOD = os.path.join(PROJECT, 'mods', 'hdfeatures', 'CMakeLists.txt')


def release_name(version):
    return 'hd_features-%s.dusk' % version


# ============================================================================================= release notes
# Short and plain: what it is, the disclaimer in 4 lines, the install in 4 steps, requirements, what's new.
# Each entry starts with its own heading. 2.1.0 is the first public release (2.0.x was never published).
WHATS_NEW = {
    '2.1.0': """\
## First public release

- One file to drop in, and nothing to run. The mod makes its game files itself, inside Dusklight.
- Mods > HD Features has a "Game files" section: choose your TP HD folder, try again, make the files again, or delete them.
""",
}

NOTES = """\
# HD Features {version}

An unofficial fan mod for Dusklight {dusklight}, a PC port of *Twilight Princess* (GameCube).
It adds two things from *Twilight Princess HD*: the 50 stamps and the Cave of Shadows.

## Please read first

- Unofficial: not affiliated with, endorsed by or supported by the Dusklight team (TwilitRealm) or by Nintendo.
- Please don't ask the Dusklight team for help with this mod. Open an issue on this repository instead.
- Made with a lot of help from an AI (Anthropic's Claude). Dusklight does not accept contributions that are primarily AI-generated, so this mod is kept completely separate, out of respect for that rule.
- For personal use with games you own. This file has no Nintendo files in it. With great respect for the Dusklight team.

## Install

1. Set up Dusklight {dusklight} with your own *Twilight Princess* GameCube disc (USA, GZ2E01).
2. Have your own *Twilight Princess HD* (Wii U, update v81) as a game folder or a `.wua`, the way Cemu uses it. A `.wux` or `.wud` must be converted to `.wua` in Cemu once.
3. Put `{name}` in Dusklight's `mods` folder, or drag it onto the Dusklight window.
4. Start Dusklight. The first time, HD Features finds your TP HD and makes its game files (usually under a minute). If it can't find it, it asks once: open Mods > HD Features and choose the folder.

## Requirements

- Windows 10 or 11, 64-bit.
- Dusklight {dusklight}.
- Your own *Twilight Princess* GameCube disc, USA version (GZ2E01), set up in Dusklight.
- Your own *Twilight Princess HD* for Wii U with update v81: a game folder (code, content, meta) or a `.wua`.
- About 200 MB of free disk space. The game files take about 75 MB in Dusklight's data folder.
- Keep your TP HD: a later version of HD Features may need it again.

{whats_new}
To uninstall: in Mods > HD Features, press "Delete the game files", close Dusklight, then delete the `.dusk` from the `mods` folder.

SHA-256 of `{name}` ({size:,} bytes): `{sha}`
"""


def release_notes(version, name='hd_features.dusk', size=0, sha='-' * 64):
    if version not in WHATS_NEW:
        raise SystemExit('make_release.py has no release notes for %s: add WHATS_NEW[%r] first' % (version, version))
    return NOTES.format(version=version, dusklight=DUSKLIGHT_VERSION, name=name, size=size, sha=sha,
                        whats_new=WHATS_NEW[version])


# ============================================================================================= package
def png_pixel_hash(data):
    """sha256 of a PNG's header + decompressed image data (the same for any zlib build)."""
    import zlib
    o, idat, ihdr = 8, b'', b''
    while o + 8 <= len(data):
        n = int.from_bytes(data[o:o + 4], 'big')
        t = data[o + 4:o + 8]
        if t == b'IHDR':
            ihdr = data[o + 8:o + 8 + n]
        elif t == b'IDAT':
            idat += data[o + 8:o + 8 + n]
        elif t == b'IEND':
            break
        o += 12 + n
    return hashlib.sha256(ihdr + zlib.decompress(idat)).hexdigest()


def read_mod_json(zf):
    """The package's mod.json as a dict, or None (missing, twice, or not JSON)."""
    names = [i.filename for i in zf.infolist() if not i.is_dir() and i.filename == 'mod.json']
    if len(names) != 1:
        return None
    try:
        return json.loads(zf.read('mod.json').decode('utf-8'))
    except ValueError:
        return None


def oracle_pin():
    """(size, sha256) of the pinned oracle, read from tools/dev/gen_tables.py (one place for the pin)."""
    with open(GEN_TABLES, encoding='utf-8') as f:
        tree = ast.parse(f.read(), GEN_TABLES)
    pins = {}
    for n in tree.body:
        if isinstance(n, ast.Assign) and isinstance(n.value, ast.Constant):
            for t in n.targets:
                if getattr(t, 'id', None) in ('ORACLE_SHA256', 'ORACLE_SIZE'):
                    pins[t.id] = n.value.value
    if set(pins) != {'ORACLE_SHA256', 'ORACLE_SIZE'}:
        raise SystemExit('%s: no ORACLE_SHA256 / ORACLE_SIZE' % GEN_TABLES)
    return pins['ORACLE_SIZE'], pins['ORACLE_SHA256']


def cmake_version():
    with open(CMAKE_MOD, encoding='utf-8') as f:
        m = re.search(r'set\(HDFEATURES_VERSION\s+"([^"]+)"\)', f.read())
    return m.group(1) if m else None


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return os.path.getsize(path), h.hexdigest()


def personal_build(dusk):
    """Why this .dusk is a personal (never-share) build, or None."""
    if 'personal' in os.path.basename(dusk).lower():
        return 'its file name says PERSONAL'
    d = os.path.dirname(os.path.abspath(dusk))
    for _ in range(4):                                  # <build dir>/mods/hd_features.dusk -> <build dir>
        cache = os.path.join(d, 'CMakeCache.txt')
        if os.path.isfile(cache):
            with open(cache, encoding='utf-8', errors='replace') as f:
                for line in f:
                    m = re.match(r'HDF_PERSONAL_ART(?::\w+)?=(.*)$', line.strip())
                    if m and m.group(1).strip() and m.group(1).strip().upper() not in ('OFF', 'FALSE', '0', 'NO'):
                        return '%s sets HDF_PERSONAL_ART' % cache
            return None
        d = os.path.dirname(d)
    return None


def pe_problem(d):
    """None if d is a 64-bit Windows DLL."""
    if len(d) < 0x40 or d[:2] != b'MZ':
        return 'not a Windows program file'
    o = struct.unpack_from('<I', d, 0x3C)[0]
    if o + 24 > len(d) or d[o:o + 4] != b'PE\0\0':
        return 'no PE header'
    machine, chars = struct.unpack_from('<H', d, o + 4)[0], struct.unpack_from('<H', d, o + 22)[0]
    if machine != 0x8664:
        return 'not x64 (machine 0x%04x)' % machine
    if not chars & 0x2000:
        return 'not a DLL'
    return None


def check_package(path):
    """[(name, bytes)] in the package's own order (directory entries left out), [directory entries], meta,
    [problems]."""
    probs, entries, dirs = [], [], []
    try:
        z = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        return [], [], None, ['%s: not a readable .dusk (%s)' % (path, e)]
    with z:
        bad = z.testzip()
        if bad:
            probs.append('%s: damaged entry %s' % (path, bad))
        seen = set()
        for i in z.infolist():
            if i.is_dir():
                dirs.append(i.filename)
                continue
            if i.filename in seen:
                probs.append('entry %s is in the package twice' % i.filename)
            seen.add(i.filename)
            entries.append((i.filename, z.read(i)))
        meta = read_mod_json(z)
    names = [n for n, _ in entries]
    for n in names:
        if n not in ALLOWED:
            probs.append('extra file in the package: %s (only %s may ship)' % (n, ', '.join(ALLOWED)))
    for n in ALLOWED:
        if n not in names:
            probs.append('missing from the package: %s' % n)
    for d in dirs:
        if not any(n.startswith(d) for n in names):
            probs.append('empty folder entry in the package: %s' % d)
    data = dict(entries)
    if meta is None:
        probs.append('mod.json is missing or not JSON')
    else:
        if meta.get('id') != MOD_ID:
            probs.append('mod.json: id %r, not %s' % (meta.get('id'), MOD_ID))
        if meta.get('version') != RELEASE_VERSION:
            probs.append('mod.json: version %r, not %s (RELEASE_VERSION)' % (meta.get('version'), RELEASE_VERSION))
        if meta.get('icon') != 'res/icon.png' or meta.get('banner') != 'res/banner.png':
            probs.append('mod.json: icon/banner are not res/icon.png and res/banner.png')
        desc = meta.get('description') or ''
        if not re.search(r'unofficial', desc, re.I) or not re.search(r'not affiliated', desc, re.I):
            probs.append('mod.json: the description lost its "Unofficial ... not affiliated" words')
    cm = cmake_version()
    if cm != RELEASE_VERSION:
        probs.append('mods/hdfeatures/CMakeLists.txt: HDFEATURES_VERSION %r, not %s' % (cm, RELEASE_VERSION))
    if DLL in data:
        p = pe_problem(data[DLL])
        if p:
            probs.append('%s: %s' % (DLL, p))
    for n, (size, sha) in sorted(PINNED_RES.items()):
        if n not in data:
            continue
        got = hashlib.sha256(data[n]).hexdigest()
        if (len(data[n]), got) != (size, sha):
            probs.append('%s: %d bytes, sha256 %s, not the pinned %d bytes %s (check it, then pin it again in '
                         'PINNED_RES)' % (n, len(data[n]), got[:16], size, sha[:16]))
        src = os.path.join(PACKAGE_RES, n.split('/', 1)[1])
        try:
            with open(src, 'rb') as f:
                if f.read() != data[n]:
                    probs.append('%s differs from mods/hdfeatures/package/res (rebuild the package)' % n)
        except OSError:
            probs.append('%s: mods/hdfeatures/package/res has no such file' % n)
    why = personal_build(path)
    if why:
        probs.append('a personal build, never shared: %s' % why)
    return entries, dirs, meta, probs


def repack(entries):
    """The .dusk bytes: the same files in the same order, no directory entries, fixed dates and attributes."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for n, d in entries:
            zi = zipfile.ZipInfo(n, ZIP_TIME)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 3                        # as CMake writes it (Unix attributes), on any host
            zi.external_attr = 0o100644 << 16
            z.writestr(zi, d, compresslevel=9)
    out = buf.getvalue()
    with zipfile.ZipFile(io.BytesIO(out)) as z:          # read back: the same files, nothing else
        if z.testzip() is not None or [(i.filename, z.read(i)) for i in z.infolist()] != list(entries):
            raise SystemExit('the repacked .dusk did not read back the same')
    return out


# ============================================================================================= scan rules
NEVER_DIRS = ('cache', 'research', 'backups', 'testenv', 'reference', '__pycache__', 'overlay', 'dusklight', 'mods',
              'output', 'output (do not share)', 'legacy')
NEVER_PATHS = ('res/stamps/', 'res/cave/')       # generated game files inside a package
BAD_EXT = ('.arc', '.szs', '.rarc', '.bti', '.bmg', '.bmd', '.bdl', '.blo', '.bck', '.btk', '.brk', '.btp', '.bpk',
           '.bca', '.bpa', '.jpc', '.jpa', '.tpl', '.gtx', '.dzr', '.dzs', '.bin', '.pack', '.gz', '.tga', '.dds',
           '.thp', '.afc', '.aw', '.ast', '.h3', '.app', '.wua', '.wux', '.wud', '.iso', '.rvz', '.gcm', '.dol',
           '.rpx', '.rpl', '.bfres', '.sarc', '.bflyt', '.bflim', '.pyc', '.dat', '.zst')
MAGICS = {b'Yaz0': 'Yaz0', b'Yay0': 'Yay0', b'RARC': 'RARC', b'J3D1': 'J3D', b'J3D2': 'J3D', b'MESGbmg': 'BMG',
          b'JPAC': 'JPC', b'\x00\x20\xaf\x30': 'TPL', b'Gfx2': 'GTX', b'TMPK': 'TMPK', b'SCRNblo': 'BLO',
          b'\x1f\x8b': 'gzip', b'\x28\xb5\x2f\xfd': 'zstd', b'THP\x00': 'THP', b'SARC': 'SARC', b'FRES': 'BFRES',
          b'HDTX': 'HD text (mod format)', b'HDFC': 'HD chest records (mod format)',
          b'HDFT': 'HD talk-tag records (mod format)', b'\x89PNG': None}
TEXT_EXT = ('.py', '.txt', '.bat', '.json', '.md')
# Personal details that must never ship. Nothing personal is written in this file: the checks are generic (a
# Windows profile path, an e-mail address) plus what this PC says at run time (the project folder, the home folder,
# the input folders, the Windows user name in a path) plus an optional private list kept next to the project and
# never shipped: PRIVATE_PATTERNS, one regular expression per line ('#' starts a comment).
PRIVATE_PATTERNS = os.path.join(PROJECT, 'private_scan_patterns.txt')
GENERIC_PERSONAL = (r'\b[A-Z]:[\\/]+Users[\\/]+(?!%)[^\\/\s"\'<>%]+',            # C:\Users\<name>
                    r'(?<![\w.])/[a-z]/Users/(?!%)[^/\s"\'<>]+',                # /c/Users/<name> (Git Bash)
                    r'\b[\w.+-]+@(?!users\.noreply\.github\.com\b)[\w-]+(?:\.[\w-]+)*\.[a-z]{2,}\b')   # e-mail
# Public addresses that third-party licence texts must keep (zlib's LICENSE and zlib.h name its two authors). They
# are allowed anywhere; every other e-mail address is still a problem (wave-1 review M2: the drop-in .dusk carries
# res/THIRD-PARTY-NOTICES.txt with the zlib licence).
LICENCE_EMAILS = frozenset(('jloup@gzip.org', 'madler@alumni.caltech.edu'))
_COMMON_DIRS = {'users', 'appdata', 'roaming', 'local', 'temp', 'program files', 'program files (x86)', 'windows',
                'content', 'code', 'meta', 'files', 'sys', 'res', 'cache', 'gc_extract', 'mods', 'build-hdf',
                'twilitrealm', 'dusklight', 'cemu', 'documents', 'desktop', 'downloads', 'games'}


def personal_patterns(paths=()):
    """[(label, compiled regex)] for the personal-details check. paths: folders of this PC whose full path and
    distinctive folder names must not appear (the project and the home folder are always added)."""
    out = [('personal path or e-mail', re.compile(p, re.I)) for p in GENERIC_PERSONAL]
    seen = set()
    for p in (PROJECT, os.path.expanduser('~')) + tuple(x for x in paths if x):
        p = os.path.abspath(p)
        if os.path.isfile(p):
            p = os.path.dirname(p)
        for _ in range(8):                                  # the folder and its parents, down to the drive
            parts = re.split(r'[\\/]+', p.rstrip('\\/'))
            if len(parts) < 2:
                break
            full = r'[\\/]+'.join(re.escape(x) for x in parts)
            if len(parts) >= 3 and full not in seen:        # not 'C:\Users' alone (the name rule below covers
                                                            # a distinctive top folder like 'X:\My Games (old)')
                seen.add(full)
                out.append(('a folder of this PC', re.compile(full, re.I)))
            name = parts[-1]
            if name.lower() not in _COMMON_DIRS and name not in seen and (
                    len(name) >= 12 or re.search(r'[()\[\],&]', name)):
                seen.add(name)
                out.append(('a folder name of this PC', re.compile(re.escape(name), re.I)))
            p = os.path.dirname(p)
    try:
        with open(PRIVATE_PATTERNS, encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    out.append(('private pattern #%d' % len(out), re.compile(line, re.I)))
    except OSError:
        pass
    return out


AUTHOR = MOD_ID.split('.')[1]                       # the author part of the mod id (local.<author>.hdfeatures)
NAME_NOTE = re.compile(r'(?<!local\.)\b%s\b(?!\.hdfeatures)' % re.escape(AUTHOR), re.I)
WORD = re.compile(r"[^\W_]+(?:'[^\W_]+)*")          # words in any language (accented letters too)
STEP = 16                                           # reference windows of 32 bytes every 16 bytes
REF_WIN = 32


def _wordy(words, need=3):
    """At least `need` real words (3+ letters): '0 0 0 0 0' or register names are not text."""
    return sum(1 for x in words if len(x) >= 3 and any(c.isalpha() for c in x)) >= need


def _headers_inside(data):
    """Real game-format headers anywhere in a binary file (not just a 4-byte word that happens to match)."""
    out = []
    for m in re.finditer(rb'Yaz0|RARC|J3D[12]|MESGbmg1|JPAC2-1|Gfx2|SARC|\x89PNG\r\n\x1a\n', data):
        o, tag = m.start(), m.group()
        if tag == b'Yaz0' and o + 16 <= len(data):
            size = int.from_bytes(data[o + 4:o + 8], 'big')
            if 0x20 < size < 1 << 28 and data[o + 8:o + 16] == bytes(8):
                out.append('Yaz0 stream at 0x%x' % o)
        elif tag == b'RARC' and o + 0x20 <= len(data):
            size = int.from_bytes(data[o + 4:o + 8], 'big')
            if 0x40 < size < 1 << 28 and int.from_bytes(data[o + 8:o + 12], 'big') == 0x20:
                out.append('RARC archive at 0x%x' % o)
        elif tag.startswith(b'J3D') and data[o + 4:o + 8] in (b'bmd3', b'bdl4', b'bck1', b'btk1', b'brk1', b'btp1',
                                                                b'bpk1', b'bca1', b'bpa1', b'bva1', b'blk1', b'bmt3'):
            size = int.from_bytes(data[o + 8:o + 12], 'big')
            blocks = int.from_bytes(data[o + 12:o + 16], 'big')
            if 0x20 < size < 1 << 28 and 1 <= blocks <= 32:      # not just the 8-byte magic as a constant
                out.append('J3D file at 0x%x' % o)
        elif tag == b'MESGbmg1':
            size = int.from_bytes(data[o + 8:o + 12], 'big')
            if 0x20 < size < 1 << 28 and 1 <= int.from_bytes(data[o + 12:o + 16], 'big') <= 16:
                out.append('BMG text at 0x%x' % o)
        elif tag == b'JPAC2-1' and data[o + 7:o + 8] in (b'0', b'1') and o + 16 <= len(data):
            if 0 < int.from_bytes(data[o + 8:o + 10], 'big') < 0x1000:
                out.append('JPC particles at 0x%x' % o)
        elif tag == b'Gfx2' and int.from_bytes(data[o + 4:o + 8], 'big') == 0x20:
            out.append('GTX texture at 0x%x' % o)
        elif tag == b'SARC' and data[o + 4:o + 6] == b'\x00\x14':
            out.append('SARC archive at 0x%x' % o)
        elif tag.startswith(b'\x89PNG') and o > 0 and data[o + 12:o + 16] == b'IHDR':
            # a real image: its IHDR chunk follows the signature. The 8-byte signature alone is the PNG writer's
            # constant in mod.dll (wave 3 I1).
            out.append('PNG image inside at 0x%x' % o)
    return out


def to_crlf(b):
    return b.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')


# ============================================================================================= scan
class Scanner(object):
    def __init__(self, reference, hd, gc, personal_paths=()):
        """reference: a full package (the pinned oracle) whose res/ and overlay/ files must never ship."""
        self.problems = []
        self.notes = []
        self.personal = personal_patterns((hd, gc, reference) + tuple(personal_paths))
        self.ref_sha, self.ref_pix = {}, {}
        self.ref_win = {}                       # hash of a 32-byte window -> where (every STEP bytes)
        self.records = []                       # (label, bytes) of the HD-only records that must never ship
        with zipfile.ZipFile(reference) as z:
            for i in z.infolist():
                if i.is_dir() or not i.filename.startswith(('res/', 'overlay/')):
                    continue
                d = z.read(i)
                self.ref_sha[hashlib.sha256(d).hexdigest()] = 'package ' + i.filename
                if i.filename.endswith('.png'):
                    self.ref_pix[png_pixel_hash(d)] = 'package ' + i.filename
                else:
                    self._index(d, 'package ' + i.filename)
                if i.filename == 'res/stamps/stamps/hd_chests.bin':
                    self._records(d, 0x20, 'HD chest record')
                elif i.filename == 'res/cave/hd_tags.bin':
                    self._records(d, 0x24, 'HD talk-tag record')
        for cand in (os.path.join(gc, 'sys', 'main.dol'), os.path.join(os.path.dirname(gc), 'sys', 'main.dol')):
            if os.path.isfile(cand):
                with open(cand, 'rb') as f:
                    self._index(f.read(), 'GC main.dol')
                break
        else:
            self.notes.append('GC sys/main.dol not found: the shipped binaries were not compared with it')
        for p in glob.glob(os.path.join(glob.escape(PROJECT), 'art', '**', '*.png'), recursive=True):
            with open(p, 'rb') as f:
                d = f.read()
            self.ref_sha[hashlib.sha256(d).hexdigest()] = os.path.relpath(p, PROJECT)
            try:
                self.ref_pix[png_pixel_hash(d)] = os.path.relpath(p, PROJECT)
            except Exception:  # noqa: BLE001  (a PNG this simple reader cannot decode is still hashed above)
                pass
        self._messages(hd, gc)

    def _index(self, d, where):
        for o in range(0, len(d) - REF_WIN + 1, STEP):
            w = d[o:o + REF_WIN]
            # skip padding / low-variety runs and plain text (file and pane names are checked as text)
            if len(set(w)) >= 10 and sum(1 for b in w if b == 0 or 0x20 <= b < 0x7F) <= REF_WIN - 4:
                self.ref_win.setdefault(hash(w), where)

    def _records(self, d, size, what):
        """'HDFC'/'HDFT' files: u8 version, u8 count, u16 0, then per entry 4 bytes + a record of `size` bytes."""
        n = d[5] if len(d) >= 8 else 0
        for i in range(n):
            rec = d[8 + i * (size + 4) + 4:8 + (i + 1) * (size + 4)]
            self.records.append(('%s %d (whole)' % (what, i), rec))
            pos = rec[0x0C:0x18]
            self.records.append(('%s %d position (big-endian)' % (what, i), pos))
            self.records.append(('%s %d position (little-endian)' % (what, i),
                                 b''.join(pos[k:k + 4][::-1] for k in (0, 4, 8))))

    def _messages(self, hd, gc):
        sys.path.insert(0, os.path.join(TOOLS, 'formats'))
        import bmg
        self.five, self.whole, self.names = {}, {}, set()
        n = 0
        self.languages = []
        for src, root in (('HD', hd), ('GC', gc)):
            arcs = sorted(glob.glob(os.path.join(glob.escape(root), 'res', 'Msg*', 'bmgres*.arc')))
            if not any(os.path.basename(os.path.dirname(a)).lower() == 'msgus' for a in arcs):
                raise SystemExit('no res/Msgus/bmgres*.arc under %s (needed for the text scan)' % root)
            for arc in arcs:
                lang = os.path.basename(os.path.dirname(arc))
                if (src, lang) not in self.languages:
                    self.languages.append((src, lang))
                try:
                    parts = list(bmg.load_all(arc))
                except Exception as e:  # noqa: BLE001
                    self.notes.append('could not read %s %s (%s): not compared' % (src, os.path.basename(arc), e))
                    continue
                for label, d in parts:
                    try:
                        msgs = bmg.parse(d)['messages']
                    except Exception as e:  # noqa: BLE001
                        self.notes.append('could not parse %s %s %s (%s)' % (src, lang, label, e))
                        continue
                    for m in msgs:
                        t = re.sub(r'\{[0-9a-f]*\}', ' ', m['text']).replace('\\n', ' ')
                        t = re.sub(r'\\x[0-9a-f]{2}', ' ', t).strip()
                        w = WORD.findall(t.lower())
                        key = '%s %s %s %d' % (src, lang, label, m['id'])
                        n += 1
                        for k in range(len(w) - 4):
                            self.five.setdefault(hash(' '.join(w[k:k + 5])), key)
                        if 3 <= len(w) < 6:
                            self.whole.setdefault(' '.join(w), key)
                        if src == 'HD' and label.startswith('zel_00') and 2890 + 26 <= m['id'] < 2940 and len(w) >= 1:
                            self.names.add(' '.join(w))
        self.message_count = n

    @staticmethod
    def strings(data):
        out = [m.group().decode('utf-8', 'replace') for m in
               re.finditer(rb'(?:[\x20-\x7e\t\r\n]|[\xc2-\xf4][\x80-\xbf]{1,3}){4,}', data)]
        out += [m.group().decode('utf-16-le') for m in re.finditer(rb'(?:[\x20-\x7e]\x00){4,}', data)]
        return out

    def _binary(self, name, data):
        for what in _headers_inside(data):
            self.problems.append('%s: %s' % (name, what))
        for label, rec in self.records:
            if len(rec) >= 12 and rec in data:
                self.problems.append('%s: contains the %s' % (name, label))
        hits = {}
        for o in range(0, len(data) - REF_WIN + 1):
            where = self.ref_win.get(hash(data[o:o + REF_WIN]))
            if where:
                hits.setdefault(where, o)
        for where, o in sorted(hits.items(), key=lambda kv: kv[1])[:5]:
            self.problems.append('%s: bytes at 0x%x are also in %s' % (name, o, where))

    def scan(self, name, data, in_base=False, folders=True, text=None, allow=()):
        """folders=False: skip the release's folder-name rules (the caller has its own); text=True/False: treat
        the file as text / binary whatever its extension (default: by TEXT_EXT); allow: exact texts the
        personal-details check accepts in this file (made-up test data)."""
        low = name.lower().replace('\\', '/')
        for part in re.split(r'[/!]', low)[:-1] if folders else ():   # folders only (not the file name)
            if part in NEVER_DIRS or part.startswith('build-'):
                self.problems.append('%s: inside a never-ship folder (%s)' % (name, part))
        for bad in NEVER_PATHS if folders else ():
            if '!' + bad in low or low.startswith(bad):
                self.problems.append('%s: generated game files (%s)' % (name, bad))
        ext = os.path.splitext(low)[1]
        is_text = ext in TEXT_EXT if text is None else text
        if ext in BAD_EXT:
            self.problems.append('%s: game-data file type %s' % (name, ext))
        for mg, kind in MAGICS.items():
            if kind and data.startswith(mg):
                self.problems.append('%s: starts with %s magic' % (name, kind))
        h = hashlib.sha256(data).hexdigest()
        if h in self.ref_sha:
            self.problems.append('%s: same bytes as %s' % (name, self.ref_sha[h]))
        if data.startswith(b'\x89PNG'):
            try:
                ph = png_pixel_hash(data)
                if ph in self.ref_pix:
                    self.problems.append('%s: same pixels as %s' % (name, self.ref_pix[ph]))
            except Exception as e:  # noqa: BLE001
                self.problems.append('%s: unreadable PNG (%s)' % (name, e))
        if ext in ('.dusk', '.zip'):
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                for i in z.infolist():
                    if not i.is_dir():
                        self.scan(name + '!' + i.filename, z.read(i), in_base=in_base or ext == '.dusk')
            return
        if not is_text and not data.startswith(b'\x89PNG'):
            self._binary(name, data)
        texts = [data.decode('utf-8', 'replace')] if is_text else self.strings(data)
        if is_text:                                          # text files: also line by line (titles, labels)
            texts += [l for l in texts[0].splitlines() if l.strip()]
        for s in texts:
            for label, p in self.personal:
                for m in p.finditer(s):
                    if m.group().lower() in LICENCE_EMAILS or m.group() in allow:
                        continue                             # a licence author's public address (M2)
                    self.problems.append('%s: %s: %r' % (name, label, m.group()[:80]))
                    break
            if NAME_NOTE.search(s):
                self.notes.append('%s: the name "%s" (outside the mod id): %r' % (
                    name, AUTHOR, s.strip()[max(0, NAME_NOTE.search(s).start() - 30):][:70]))
            w = WORD.findall(s.lower())
            prev = None
            for k in range(len(w) - 4):
                key = self.five.get(hash(' '.join(w[k:k + 5]))) if _wordy(w[k:k + 5]) else None
                if key and prev == key:
                    self.problems.append('%s: 6 words of %s: %r' % (name, key, ' '.join(w[k - 1:k + 5])))
                    break
                if key and prev is None:
                    self.notes.append('%s: 5 words like %s: %r' % (name, key, ' '.join(w[k:k + 5])))
                prev = key
            joined = ' '.join(w)
            if joined in self.whole and _wordy(w, 2):
                self.notes.append('%s: short text equal to %s: %r' % (name, self.whole[joined], s.strip()[:60]))
            if in_base and not is_text and joined in self.names and len(w) >= 2:
                self.problems.append('%s: HD stamp name %r' % (name, s.strip()))


# ============================================================================================= main
def _write_atomic(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(description='HD Features release file (the .dusk itself)')
    ap.add_argument('--hd', required=True)
    ap.add_argument('--gc', default=os.path.join(PROJECT, 'cache', 'gc_extract'))
    ap.add_argument('--dusk', default=DEFAULT_DUSK)
    ap.add_argument('--oracle', default=DEFAULT_ORACLE)
    ap.add_argument('--out', default=os.path.join(PROJECT, 'dist'))
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args(argv)
    gc = a.gc if os.path.isdir(os.path.join(a.gc, 'res')) else os.path.join(a.gc, 'files')
    for what, p in (('--dusk', a.dusk), ('--oracle', a.oracle)):
        if not os.path.isfile(p):
            print('error: %s %s not found' % (what, p))
            return 2
    for what, p in (('--hd', a.hd), ('--gc', gc)):
        if not os.path.isdir(p):
            print('error: %s %s is not a folder' % (what, p))
            return 2
    version = RELEASE_VERSION
    name = release_name(version)
    out = os.path.abspath(a.out)
    paths = [os.path.join(out, x) for x in (name, name + '.sha256', 'RELEASE-NOTES-%s.md' % version)]

    # 1. the package
    entries, dirs, meta, probs = check_package(a.dusk)
    pin = oracle_pin()
    got = file_sha256(a.oracle)
    if got != pin:
        probs.append('%s is not the pinned oracle (%d bytes, sha256 %s; pin: %d bytes %s)' % (
            a.oracle, got[0], got[1][:16], pin[0], pin[1][:16]))
    print('package %s: %d files%s' % (a.dusk, len(entries), ', %d directory entries dropped' % len(dirs) if dirs
                                      else ''))
    for n, d in entries:
        print('  %-32s %10d  sha256 %s' % (n, len(d), hashlib.sha256(d).hexdigest()[:16]))
    dusk = repack(entries) if not probs else b''
    sha = hashlib.sha256(dusk).hexdigest()
    notes_text = release_notes(version, name, len(dusk), sha)
    sha_text = '%s  %s\n' % (sha, name)

    # 2. the scan (the whole .dusk, each file in it, the texts that go with it)
    if not probs:
        print('scanning ...')
        sc = Scanner(a.oracle, a.hd, gc, personal_paths=(a.dusk, out))
        sc.scan(name, dusk)
        sc.scan(os.path.basename(paths[2]), notes_text.encode('utf-8'))
        sc.scan(os.path.basename(paths[1]), sha_text.encode('utf-8'))
        print('  %d messages compared (%s), %d reference hashes, %d reference byte windows, %d HD records' % (
            sc.message_count, ', '.join('%s %s' % l for l in sc.languages), len(sc.ref_sha), len(sc.ref_win),
            len(sc.records)))
        for n in sorted(set(sc.notes)):
            print('  note: ' + n)
        probs += sorted(set(sc.problems))

    print()
    if probs:
        print('RELEASE FAILED: %d problem(s). Nothing was written.' % len(probs))
        for p in probs[:60]:
            print('  ' + p)
        if not a.dry_run:
            for p in paths:                       # no stale release file of this version may look current
                if os.path.exists(p):
                    os.remove(p)
                    print('  removed the older %s' % p)
        return 1
    if a.dry_run:
        print('DRY RUN PASSED: nothing written. Would write (%d bytes, sha256 %s):' % (len(dusk), sha))
        for p in paths:
            print('  ' + p)
        return 0
    os.makedirs(out, exist_ok=True)
    _write_atomic(paths[0], dusk)
    _write_atomic(paths[1], sha_text.encode('utf-8'))
    _write_atomic(paths[2], notes_text.encode('utf-8'))
    if file_sha256(paths[0]) != (len(dusk), sha):
        print('RELEASE FAILED: %s did not read back the same' % paths[0])
        return 1
    print('SCAN PASSED: no Nintendo-derived content or personal details found.')
    print('Release file: %s (%d bytes)' % (paths[0], len(dusk)))
    print('  sha256 %s' % sha)
    print('  %s' % paths[1])
    print('  %s' % paths[2])
    print('Attach the .dusk itself to the GitHub release v%s and use the release notes as its text.' % version)
    return 0


if __name__ == '__main__':
    sys.exit(main())
