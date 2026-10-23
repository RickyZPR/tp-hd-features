#!/usr/bin/env python3
"""build_assets.py - regenerate every game-derived asset of the Dusklight HD Mods from the user's own dumps.

usage:
  python -I tools/build_assets.py --hd "<TP HD content dir>" --gc "<GC disc extraction dir>"
                                  [--out-root <project dir> | --out-dir <folder>] [--src <dusklight checkout>]
                                  [--work <dir>] [--keep-work] [--only cave|stamps] [--hd-gates] [--force]
                                  [--verbose]

  --hd        TP HD (AZAP01) 'content' folder (has los.bin, res/Stage/D_SB11, res/Object, res/LayoutRevo). Read only.
  --gc        GC (GZ2E01) extraction root (has files/res/...; the 'files' folder itself is accepted too). Read only.
  --out-root  project root (default: the folder above tools/). Must contain mods/hdfeatures (the HD Features mod).
  --out-dir   install into this folder instead (it gets res/... and overlay/... like mods/hdfeatures; for example
              a scratch folder as the converter's oracle, see BUILDING.md). No project tree is needed or touched:
              no .gitignore update and no icon/banner copy.
  --src       optional Dusklight v2.0.3 source checkout: re-runs the cave particle reference scan and reads the
              save-label names from it (developer use; the results are built into tools/pipeline/facts.py and
              the scan reports any difference). Read only.
  --work      staging folder (default: a new temp folder, removed afterwards unless --keep-work). Only its
              'stage' subfolder is cleared and written.
  --only      build just one mod's assets.
  --hd-gates  also ship the HD-only gate/lift/block object archives (unused by the patched data; only useful
              for code that extends the GC actor tables).
  --force     install even if a validation check failed (exit code stays 1).
  --no-install  build and validate in the staging folder only (implies --keep-work).

Outputs, all in the one HD Features mod (mods/hdfeatures; each feature owns its own paths, every owned folder
is cleared first, and files are only installed after every check passed):
  Cave of Shadows
  mods/hdfeatures/overlay/res/Stage/D_SB11/{STG_00,R00..R49_00}.arc
  mods/hdfeatures/overlay/res/Particle/Pscene206.jpc     (GC-format scene particles, tools/pipeline/particles.py)
  mods/hdfeatures/overlay/res/Object/{@bg0084,@bg0085,O_gD_puL4}.arc   (every file there except the stamps' one)
  mods/hdfeatures/res/cave/floors.json, colossal_wallet.png, hd_tags.bin (HD's talk-tag places)
  mods/hdfeatures/res/cave/hdtext/{messages.json,messages.bin,flows.json,README.md}   (tools/pipeline/hdtext.py:
                                                         HD's own text + flows)
  mods/hdfeatures/res/cave/amiibo/*.png + layout.json    (virtual Wolf Link amiibo scan screen art)
  mods/hdfeatures/res/cave/confirm/cosconfirm_gc.arc + confirm.json   (tools/pipeline/cosconfirm.py: HD's
                                                         "Cave of Shadows" pre-entry screen, Save and begin / Cancel:
                                                         saveres layouts + bck byte-identical + GC BTIs, read in
                                                         memory by the C++ like the stamp book archive)
  Stamps
  mods/hdfeatures/overlay/res/Object/Obj_Stamp.arc
  mods/hdfeatures/res/stamps/stamps/stamp00..49.png, stamp_frame.png, stamp_empty.png, stamp_background.png,
                                                         stamps.json, hd_chests.bin (the 7 HD-only chests' records)
  mods/hdfeatures/res/stamps/stamp_icons/stamp00..49.bti  (48x48 CMPR item-get icons from the stamp PNGs:
                                                         tools/gen_stamp_table.py build_icons)
  mods/hdfeatures/res/stamps/hdtext/{messages.json,messages.bin,flows.json,README.md}
  mods/hdfeatures/res/stamps/stampbook/stampResR_gc.arc  (tools/pipeline/stampbook.py: HD stamp book layouts +
                                                          textures as GC BTIs, read in memory by the C++)
  mods/hdfeatures/res/stamps/stampbook/{stampicon,stampbutton,stampbuticon,filesel_stampicon}.bti + book.json
                                                         (Collection-slot icon, file-select plate textures)
  Mods-menu art (only if art/hdfeatures/ has them; made by art/hdfeatures/make.py, not by this script)
  mods/hdfeatures/res/icon.png, mods/hdfeatures/res/banner.png   (copies of art/hdfeatures/{icon,banner}.png)
The two features' overlay files must not collide (checked).
Appends the ignore rules for all of it to .gitignore. Prints a validation table and a summary.
Exit code: 0 = all checks passed, 1 = a check failed, 2 = bad arguments / missing inputs.

The generated files are derived from Nintendo game data: never commit or distribute them (players make their
own with the HD Features builder, tools/dist/).
Pipeline details: tools/pipeline/cavebuild.py, stampbuild.py, stampbook.py, cosconfirm.py, memfit.py, validate.py.
The stamp book check also renders the book (GC-format data vs the HD source, needs numpy + Pillow) into
<work>/stage/stampbook_preview/, and the cave pre-entry screen check renders it into
<work>/stage/cosconfirm_preview/ (both kept with --keep-work / --work).
"""
import argparse
import hashlib
import os
import shutil
import sys
import tempfile
import traceback

sys.dont_write_bytecode = True                      # keep tools/**/__pycache__ out of the project tree
_TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_TOOLS, 'pipeline'))
import common  # noqa: E402  (also puts tools/formats, tools/critic, tools/cave on sys.path)

GITIGNORE_BLOCK = """
# --- generated by tools/build_assets.py from the user's own game dumps: local use only, never commit ---
mods/hdfeatures/overlay/
mods/hdfeatures/res/stamps/stamps/
mods/hdfeatures/res/stamps/stamp_icons/
mods/hdfeatures/res/stamps/hdtext/
mods/hdfeatures/res/stamps/stampbook/
mods/hdfeatures/res/cave/hdtext/
mods/hdfeatures/res/cave/amiibo/
mods/hdfeatures/res/cave/confirm/
mods/hdfeatures/res/cave/colossal_wallet.png
mods/hdfeatures/res/cave/hd_tags.bin
mods/hdfeatures/res/icon.png
mods/hdfeatures/res/banner.png
__pycache__/
*.pyc
"""
MOD = 'hdfeatures'                                  # mods/hdfeatures: the HD Features mod (stamps + cave)
CAVE_RES_FILES = ['floors.json', 'colossal_wallet.png', 'hd_tags.bin']   # -> res/cave/
STAMPS_OBJECTS = ['Obj_Stamp.arc']                  # the stamps' files in overlay/res/Object (the rest is the cave's)
ART_FILES = ['icon.png', 'banner.png']              # art/hdfeatures/<f> -> mods/hdfeatures/res/<f>


def _gen_stamp_table():
    """tools/gen_stamp_table.py as a module (not on sys.path under -I; nothing in it runs on import)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('gen_stamp_table', os.path.join(_TOOLS, 'gen_stamp_table.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _err(msg):
    print('error: ' + msg, file=sys.stderr)
    return 2


def _resolve_gc(p):
    for cand in (os.path.join(p, 'files'), p):
        if os.path.isdir(os.path.join(cand, 'res', 'Stage')) and os.path.isdir(os.path.join(cand, 'res', 'Object')):
            return cand
    return None


def _check_hd(p):
    need = [('los.bin',), ('res', 'Stage', 'D_SB11', 'STG_00.arc'), ('res', 'Object', 'Obj_Stamp.arc'),
            ('res', 'LayoutRevo', 'stampResR.arc'), ('res', 'LayoutRevo', 'stampResR.pack.gz'),
            ('res', 'Particle', 'Pscene206.jpc'), ('tex', 'Particle', 'Pscene206.jpc.gtx.gz'),
            ('res', 'Msgus', 'bmgres.arc'), ('res', 'Msgus', 'bmgres4.arc'), ('res', 'Msgus', 'bmgres5.arc'),
            ('res', 'LayoutRevo', 'amiibo.arc'), ('res', 'LayoutRevo', 'amiibo.pack.gz')]
    return [os.path.join(*n) for n in need if not os.path.exists(os.path.join(p, *n))]


def _check_hd_book(p):
    """Extra HD members the stamp book needs (tools/pipeline/stampbook.py)."""
    need = [('res', 'LayoutRevo', 'main2DR.arc'), ('res', 'LayoutRevo', 'main2DR.pack.gz'),
            ('res', 'LayoutRevo', 'clctresR.arc'), ('res', 'LayoutRevo', 'clctresR.pack.gz'),
            ('res', 'Object', 'fileSel.arc'), ('res', 'Object', 'fileSel.pack.gz')]
    return [os.path.join(*n) for n in need if not os.path.exists(os.path.join(p, *n))]


def _check_hd_confirm(p):
    """Extra HD members the cave pre-entry screen needs (tools/pipeline/cosconfirm.py: saveres layouts + textures,
    the hint glyphs and backdrop from main2DR / clctresR)."""
    need = [('res', 'Layout', 'saveres.arc'), ('res', 'Layout', 'saveres.pack.gz'),
            ('res', 'LayoutRevo', 'main2DR.arc'), ('res', 'LayoutRevo', 'main2DR.pack.gz'),
            ('res', 'LayoutRevo', 'clctresR.arc'), ('res', 'LayoutRevo', 'clctresR.pack.gz')]
    return [os.path.join(*n) for n in need if not os.path.exists(os.path.join(p, *n))]


def _inside(child, parent):
    child, parent = os.path.normcase(os.path.realpath(child)), os.path.normcase(os.path.realpath(parent))
    return child == parent or child.startswith(parent.rstrip('\\/') + os.sep)


def _tree_digest(root):
    h = hashlib.sha256()
    n = size = 0
    for dp, dn, fn in sorted(os.walk(root)):
        dn.sort()
        for f in sorted(fn):
            p = os.path.join(dp, f)
            d = common.read(p)
            h.update(os.path.relpath(p, root).replace('\\', '/').encode() + b'\0' + hashlib.sha256(d).digest())
            n += 1
            size += len(d)
    return n, size, h.hexdigest()


_SHOW_ROOT = [common.PROJECT]                         # paths in the install log are shown relative to this


def _rel(p):
    try:
        return os.path.relpath(p, _SHOW_ROOT[0])
    except ValueError:                                # another drive (Windows)
        return p


def _install_tree(src, dst, log):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    a = _tree_digest(src)
    b = _tree_digest(dst)
    if a != b:
        raise RuntimeError('install of %s does not match the staged files' % dst)
    log('installed %-45s %4d files %11d bytes  sha256 %s', _rel(dst), b[0], b[1], b[2][:16])
    return b


def _install_files(src_dir, dst_dir, names, log):
    os.makedirs(dst_dir, exist_ok=True)
    for n in names:
        p = os.path.join(dst_dir, n)
        if os.path.exists(p):
            os.remove(p)
    out = []
    for n in names:
        s = os.path.join(src_dir, n)
        if os.path.exists(s):
            d = common.read(s)
            common.write(os.path.join(dst_dir, n), d)
            if common.read(os.path.join(dst_dir, n)) != d:
                raise RuntimeError('install of %s failed' % n)
            out.append((n, len(d)))
            log('installed %-45s %11d bytes  sha256 %s', _rel(os.path.join(dst_dir, n)), len(d), common.sha256(d)[:16])
    return out


def _install_named(src_dir, dst_dir, names, log):
    """Install exactly `names` from src_dir into the shared dst_dir: those names are removed first, nothing
    else in dst_dir is touched. Returns (count, bytes, digest of the installed files)."""
    os.makedirs(dst_dir, exist_ok=True)
    for n in names:
        p = os.path.join(dst_dir, n)
        if os.path.exists(p):
            os.remove(p)
    h = hashlib.sha256()
    n_files = size = 0
    for n in sorted(names):
        s = os.path.join(src_dir, n)
        if not os.path.exists(s):
            continue
        d = common.read(s)
        common.write(os.path.join(dst_dir, n), d)
        if common.read(os.path.join(dst_dir, n)) != d:
            raise RuntimeError('install of %s failed' % n)
        h.update(n.encode() + b'\0' + hashlib.sha256(d).digest())
        n_files += 1
        size += len(d)
        log('installed %-45s %11d bytes  sha256 %s', _rel(os.path.join(dst_dir, n)), len(d), common.sha256(d)[:16])
    return n_files, size, h.hexdigest()


def _install_shared_dir(src_dir, dst_dir, other_names, log):
    """Install a feature's files into a folder it shares with the other feature (overlay/res/Object): every file
    there except `other_names` (the other feature's) is removed first, so stale files of this feature go."""
    os.makedirs(dst_dir, exist_ok=True)
    keep = {n.lower() for n in other_names}
    for n in os.listdir(dst_dir):
        p = os.path.join(dst_dir, n)
        if os.path.isfile(p) and n.lower() not in keep:
            os.remove(p)
    names = sorted(os.listdir(src_dir)) if os.path.isdir(src_dir) else []
    return _install_named(src_dir, dst_dir, names, log)


def _overlay_files(root):
    out = set()
    for dp, dn, fn in os.walk(root):
        for f in fn:
            out.add(os.path.relpath(os.path.join(dp, f), root).replace('\\', '/').lower())
    return out


def _update_gitignore(root, log):
    p = os.path.join(root, '.gitignore')
    old = common.read(p).decode('utf-8') if os.path.exists(p) else ''
    have = {l.strip() for l in old.splitlines()}
    want = [l for l in GITIGNORE_BLOCK.strip('\n').splitlines()]
    missing = [l for l in want if l.strip() and not l.startswith('#') and l.strip() not in have]
    if not missing:
        log('.gitignore already excludes the generated assets')
        return
    add = '\n'.join([want[0]] + missing) + '\n'
    sep = '' if (not old or old.endswith('\n')) else '\n'
    with open(p, 'a', encoding='utf-8', newline='\n') as f:
        f.write(sep + ('\n' if old else '') + add)
    log('.gitignore: appended %d rule(s): %s', len(missing), ', '.join(missing))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hd', required=True)
    ap.add_argument('--gc', required=True)
    ap.add_argument('--out-root', default=common.PROJECT)
    ap.add_argument('--out-dir')
    ap.add_argument('--src')
    ap.add_argument('--work')
    ap.add_argument('--keep-work', action='store_true')
    ap.add_argument('--only', choices=('cave', 'stamps'))
    ap.add_argument('--hd-gates', action='store_true')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--no-install', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args(argv)

    hd = os.path.abspath(a.hd)
    gc = _resolve_gc(os.path.abspath(a.gc))
    root = os.path.abspath(a.out_root)
    out_dir = os.path.abspath(a.out_dir) if a.out_dir else None
    if not os.path.isdir(hd):
        return _err('--hd %s is not a folder' % hd)
    miss = _check_hd(hd)
    if miss:
        return _err('--hd %s lacks %s' % (hd, ', '.join(miss)))
    if gc is None:
        return _err('--gc %s: no files/res/Stage + res/Object found' % a.gc)
    if out_dir:
        if _inside(out_dir, hd) or _inside(out_dir, gc):
            return _err('--out-dir must not be inside the input dumps')
        _SHOW_ROOT[0] = out_dir
    else:
        if not os.path.isfile(os.path.join(root, 'mods', MOD, 'CMakeLists.txt')):
            return _err('--out-root %s has no mods/%s/CMakeLists.txt (the HD Features mod)' % (root, MOD))
        if _inside(root, hd) or _inside(root, gc):
            return _err('--out-root must not be inside the input dumps')
    src = os.path.abspath(a.src) if a.src else None
    if src and not (os.path.isdir(os.path.join(src, 'src', 'd', 'actor')) and
                    os.path.exists(os.path.join(src, 'include', 'd', 'd_particle_name.h'))):
        return _err('--src %s is not a Dusklight source checkout (src/d/actor, include/d/d_particle_name.h)' % src)
    do_cave = a.only in (None, 'cave')
    do_stamps = a.only in (None, 'stamps')
    miss = _check_hd_book(hd) if do_stamps else []
    if miss:
        return _err('--hd %s lacks %s (stamp book)' % (hd, ', '.join(miss)))
    miss = _check_hd_confirm(hd) if do_cave else []
    if miss:
        return _err('--hd %s lacks %s (cave pre-entry screen)' % (hd, ', '.join(miss)))

    log = common.Log(a.verbose)
    log('build_assets: HD %s', hd)
    log('build_assets: GC %s', gc)
    log('build_assets: out %s', out_dir or root)
    log('build_assets: particle ids and label names from %s', src or 'the built-in tables (tools/pipeline/facts.py)')
    tmp_created = False
    if a.work:
        work = os.path.abspath(a.work)
    else:
        work = tempfile.mkdtemp(prefix='hdmods_assets_')
        tmp_created = True
    if _inside(work, hd) or _inside(work, gc):
        return _err('--work must not be inside the input dumps')
    stage = os.path.join(work, 'stage')
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    # Staged per feature (each one's overlay checked on its own), installed into the one mod's layout below.
    S = {
        'cave_overlay': os.path.join(stage, 'cave', 'overlay'),
        'cave_res': os.path.join(stage, 'cave', 'res', 'cave'),
        'stamps_overlay': os.path.join(stage, 'stamps', 'overlay'),
        'stamps_res': os.path.join(stage, 'stamps', 'res', 'stamps', 'stamps'),
        'stamps_icons': os.path.join(stage, 'stamps', 'res', 'stamps', 'stamp_icons'),
        'stamps_book': os.path.join(stage, 'stamps', 'res', 'stamps', 'stampbook'),
        'cave_confirm': os.path.join(stage, 'cave', 'res', 'cave', 'confirm'),
    }
    book_preview = os.path.join(stage, 'stampbook_preview')
    confirm_preview = os.path.join(stage, 'cosconfirm_preview')
    hdtext_out = {}                                  # tools/pipeline/hdtext.py parts for the selected mods
    if do_cave:
        hdtext_out['cave_text'] = os.path.join(stage, 'cave', 'res', 'cave', 'hdtext')
        hdtext_out['cave_amiibo'] = os.path.join(stage, 'cave', 'res', 'cave', 'amiibo')
    if do_stamps:
        hdtext_out['stamps_text'] = os.path.join(stage, 'stamps', 'res', 'stamps', 'hdtext')
    for p in S.values():
        os.makedirs(p, exist_ok=True)
    cave_out = {'stage_dir': os.path.join(S['cave_overlay'], 'res', 'Stage', 'D_SB11'),
                'obj_dir': os.path.join(S['cave_overlay'], 'res', 'Object'), 'res_dir': S['cave_res'],
                'particle_dir': os.path.join(S['cave_overlay'], 'res', 'Particle')}
    stamps_out = {'obj_dir': os.path.join(S['stamps_overlay'], 'res', 'Object'), 'stamps_dir': S['stamps_res']}
    for p in list(cave_out.values()) + list(stamps_out.values()):
        os.makedirs(p, exist_ok=True)

    import validate
    rep = validate.Report()
    cres = sres = None
    try:
        if do_cave:
            import cavebuild
            cres = cavebuild.build(hd, gc, cave_out, log, hd_gates=a.hd_gates, src_root=src)
        if do_stamps:
            import stampbuild
            sres = stampbuild.build(hd, stamps_out, log)
            # The item-get message icons (48x48 CMPR BTIs) from the stamp PNGs just made.
            sres['icons'] = _gen_stamp_table().build_icons(S['stamps_res'], S['stamps_icons'])
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        rep.add('build', 'pipeline ran', False, 'exception (see traceback above)')
    tres = None
    try:
        import hdtext
        tres = hdtext.build(hd, gc, hdtext_out, log, src=src, project=None if out_dir else root)
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        rep.add('hdtext', 'HD text / amiibo extraction ran', False, 'exception (see traceback above)')
    bres = None
    if do_stamps:
        try:
            import stampbook
            text = stampbook.hdtext_previews(os.path.join(hdtext_out['stamps_text'], 'messages.json'))
            bres = stampbook.build(hd, S['stamps_book'], log, gc=gc, preview_dir=book_preview, text=text)
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            rep.add('stampbook', 'stamp book pipeline ran', False, 'exception (see traceback above)')
    cfres = None
    if do_cave:
        try:
            import cosconfirm
            cfres = cosconfirm.build(hd, S['cave_confirm'], log, gc=gc, preview_dir=confirm_preview)
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            rep.add('cosconfirm', 'cave pre-entry screen pipeline ran', False, 'exception (see traceback above)')

    # ------------------------------------------------------------------ validation
    log('validating ...')
    totals = {'arcs': 0, 'models': 0, 'textures': 0}
    arc_rows = []
    try:
        groups = []
        if cres:
            groups += [('cave/stage', cave_out['stage_dir']), ('cave/object', cave_out['obj_dir'])]
        if sres:
            groups += [('stamps/object', stamps_out['obj_dir'])]
        for g, d in groups:
            res_g = [validate.check_archive(n, common.read(os.path.join(d, n))) for n in sorted(os.listdir(d))]
            validate.add_archive_rows(rep, g, res_g)
            for r in res_g:
                totals['arcs'] += 1
                totals['models'] += r['models']
                totals['textures'] += r['textures']
                arc_rows.append('%-14s %-16s %9d B  models %d  textures %3d  mats %s  %s' % (
                    g, r['name'], r['size'], r['models'], r['textures'], r['mat_counts'],
                    'OK' if not (r['rarc'] or r['model_probs']) else 'PROBLEM: ' + '; '.join((r['rarc'] + r['model_probs'])[:2])))
        if cres:
            validate.check_cave(rep, gc, cave_out['stage_dir'], cave_out['obj_dir'], cave_out['res_dir'], cres,
                                particle_dir=cave_out['particle_dir'])
            validate.check_overlay_paths(rep, gc, S['cave_overlay'], 'cave')
        if sres:
            validate.check_stamps(rep, stamps_out['stamps_dir'], sres)
            validate.check_overlay_paths(rep, gc, S['stamps_overlay'], 'stamps')
            icons = sorted(n for n in os.listdir(S['stamps_icons']) if n.lower().endswith('.bti'))
            bad = [n for n in icons if not (0x20 < os.path.getsize(os.path.join(S['stamps_icons'], n)) <= 0xC00)]
            want = ['stamp%02d.bti' % i for i in range(50)]
            rep.add('stamps', 'item-get icons stamp00..49.bti (48x48 CMPR, <= 0xC00 bytes)',
                    icons == want and not bad and sres.get('icons') == 50,
                    '%d icon(s)%s' % (len(icons), ('; bad size: %s' % bad[:3]) if bad else
                                      ('' if icons == want else '; missing %s' % sorted(set(want) - set(icons))[:3])))
        # One mod, one overlay/: the two features' files must not land on the same path (case-insensitive,
        # like the disc), and only the stamps may write their STAMPS_OBJECTS into the shared res/Object.
        cave_files = _overlay_files(S['cave_overlay']) if cres else set()
        stamp_files = _overlay_files(S['stamps_overlay']) if sres else set()
        clash = sorted(cave_files & stamp_files)
        owned = {('res/object/' + n).lower() for n in STAMPS_OBJECTS}
        foreign = sorted((cave_files & owned) | {f for f in stamp_files if f not in owned})
        shared = {f.rsplit('/', 1)[0] for f in cave_files} & {f.rsplit('/', 1)[0] for f in stamp_files}
        rep.add('hdfeatures', 'cave and stamps overlay paths do not collide', not clash and not foreign,
                ('collide: %s' % (clash + foreign)[:4]) if (clash or foreign) else
                '%d cave + %d stamps file(s); shared folder(s): %s' % (
                    len(cave_files), len(stamp_files), ', '.join(sorted(shared)) or 'none'))
        if tres:
            import hdtext
            hdtext.validate(rep, tres, hdtext_out)
        if bres:
            import stampbook
            stampbook.validate(rep, bres)
        if cfres:
            import cosconfirm
            cosconfirm.validate(rep, cfres)
            staged = sorted(os.listdir(S['cave_confirm']))
            rep.add('cosconfirm', 'staged folder holds exactly the outputs', staged == sorted(cosconfirm.OUT_FILES),
                    'res/cave/confirm: %s' % ', '.join(staged))
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        rep.add('validate', 'validation ran', False, 'exception (see traceback above)')

    print()
    print('VALIDATION')
    print(rep.table())
    fails = rep.failed()
    print()
    print('%d checks, %d failed, %d warnings; %d archives, %d models, %d textures checked' % (
        len(rep.rows), len(fails), sum(1 for r in rep.rows if r[2] == 'WARN'), totals['arcs'], totals['models'],
        totals['textures']))

    # ------------------------------------------------------------------ install
    installed = []
    if a.no_install:
        a.keep_work = True
        log('--no-install: staged output left in %s', stage)
    elif fails and not a.force:
        log('NOT installing: %d check(s) failed (staged output kept in %s)', len(fails), stage)
    else:
        if fails:
            log('installing despite %d failed check(s) (--force)', len(fails))
        M = out_dir or os.path.join(root, 'mods', MOD)
        OV = os.path.join(M, 'overlay')
        RES = os.path.join(M, 'res')
        if cres:
            # The cave owns overlay/res/Stage/D_SB11 and overlay/res/Particle as folders; overlay/res/Object is
            # shared with the stamps (everything there but STAMPS_OBJECTS is the cave's).
            installed.append(('overlay/res/Stage/D_SB11', _install_tree(
                cave_out['stage_dir'], os.path.join(OV, 'res', 'Stage', 'D_SB11'), log)))
            installed.append(('overlay/res/Particle', _install_tree(
                cave_out['particle_dir'], os.path.join(OV, 'res', 'Particle'), log)))
            installed.append(('overlay/res/Object (cave)', _install_shared_dir(
                cave_out['obj_dir'], os.path.join(OV, 'res', 'Object'), STAMPS_OBJECTS, log)))
            fs = _install_files(S['cave_res'], os.path.join(RES, 'cave'), CAVE_RES_FILES, log)
            installed.append(('res/cave', (len(fs), sum(s for _, s in fs), '')))
        if sres:
            installed.append(('overlay/res/Object (stamps)', _install_named(
                stamps_out['obj_dir'], os.path.join(OV, 'res', 'Object'), STAMPS_OBJECTS, log)))
            installed.append(('res/stamps/stamps', _install_tree(
                S['stamps_res'], os.path.join(RES, 'stamps', 'stamps'), log)))
            installed.append(('res/stamps/stamp_icons', _install_tree(
                S['stamps_icons'], os.path.join(RES, 'stamps', 'stamp_icons'), log)))
        if bres:
            installed.append(('res/stamps/stampbook', _install_tree(
                S['stamps_book'], os.path.join(RES, 'stamps', 'stampbook'), log)))
        if cfres:
            installed.append(('res/cave/confirm', _install_tree(
                S['cave_confirm'], os.path.join(RES, 'cave', 'confirm'), log)))
        if tres:
            for key, rel in (('cave_text', ('cave', 'hdtext')),
                             ('cave_amiibo', ('cave', 'amiibo')),
                             ('stamps_text', ('stamps', 'hdtext'))):
                if key in hdtext_out:
                    installed.append(('res/' + '/'.join(rel), _install_tree(
                        hdtext_out[key], os.path.join(RES, *rel), log)))
        if out_dir:
            log('--out-dir: icon/banner and .gitignore left to the caller')
        else:
            # The Mods-menu icon and banner (art/hdfeatures/make.py; never written here), when they exist.
            art = os.path.join(root, 'art', MOD)
            have_art = [n for n in ART_FILES if os.path.isfile(os.path.join(art, n))]
            if have_art:
                installed.append(('res/icon.png, res/banner.png', _install_named(art, RES, have_art, log)))
            else:
                log('art/%s has no icon.png / banner.png: the Mods menu shows the mod without them', MOD)
            _update_gitignore(root, log)

    # ------------------------------------------------------------------ summary
    print()
    print('SUMMARY')
    if cres:
        mem = cres['mem']
        print('  cave  : %d stage archives (%.1f MB), %d object archives (%s), %d models converted, %d MAT blocks '
              'repacked, %d models los-baked, %d textures' % (
                  len(cres['arcs']), sum(cres['arcs'].values()) / 1e6, len(cres['objects']),
                  ', '.join('%s %d B' % (k, v['size']) for k, v in sorted(cres['objects'].items())),
                  cres['totals']['models'], cres['totals']['mat_fixed'], cres['totals']['baked'], cres['totals']['textures']))
        print('          MEM0 %s -> %s; room blocks on PC %.2f MiB (was %.2f MiB) of the 17.7 MiB archive heap' % (
            ['0x%x' % x for x in mem['old']], ['0x%x' % x for x in mem['new']],
            sum((x + 0x380) * 2 for x in mem['new']) / 1048576.0, sum((x + 0x380) * 2 for x in mem['old']) / 1048576.0))
        print('          %d data patches (%d deleted records); floors.json %d floors; Tag_ms flows dropped %s' % (
            len(cres['patch_log']), sum(1 for l in cres['patch_log'] if l.endswith('deleted')), len(cres['floors']),
            cres['drop_tagms']))
        ps = cres['particles']['stats']
        print('          Pscene206.jpc %d B (budget %d B): %d particles (%d GC, %d HD-only), %d textures (%d at half size)' % (
            ps['size'], ps['budget'], ps['resources'], ps['from_gc'], ps['from_hd'], ps['textures'], len(ps['halved'])))
    if sres:
        print('  stamps: Obj_Stamp.arc %d B (stamp.bmd resource index %s); %d stamp PNGs; %d frame PNGs; %d item-get icons' % (
            sres['obj_stamp']['size'], sres['obj_stamp']['bmd_index'], len(sres['pngs']), len(sres['frames']),
            sres.get('icons', 0)))
    if bres:
        ar = bres['arc']
        fm = {}
        for i in bres['textures'].values():
            fm[i['fmt']] = fm.get(i['fmt'], 0) + 1
        print('  book  : %s %d B (%.2f MB), %d BLO byte-identical + %d BTI (%s), %d .tga dropped; loose %s' % (
            'stampResR_gc.arc', ar['size'], ar['size'] / 1e6, len(bres['blos']), len(bres['textures']),
            ', '.join('%s x%d' % kv for kv in sorted(fm.items())), len(bres['dropped']),
            ', '.join('%s %s %dx%d' % (k, v['fmt'], v['w'], v['h']) for k, v in sorted(bres['loose'].items()))))
        if bres.get('preview'):
            print('          preview (GC data vs HD source): %s -> %s' % (
                ', '.join('%s %.1f dB' % (k, v['psnr']) for k, v in sorted(bres['preview'].items())), book_preview))
    if cfres:
        fm = {}
        for i in cfres['textures'].values():
            fm[i['fmt']] = fm.get(i['fmt'], 0) + 1
        nd = sum(1 for i in cfres['textures'].values() if not i['drawn_in_mode6'])
        print('  confirm: %s %d B (%.2f MB), %d BLO + 1 BCK byte-identical + %d BTI (%s; %d drawn in mode 6, %d only '
              'referenced)' % (cosconfirm.ARC_NAME, cfres['arc']['size'], cfres['arc']['size'] / 1e6, len(cfres['layouts']),
                               len(cfres['textures']), ', '.join('%s x%d' % kv for kv in sorted(fm.items())),
                               len(cfres['textures']) - nd, nd))
        if cfres.get('preview'):
            print('          preview (GC data vs HD source): %s -> %s' % (
                ', '.join('%s %.1f dB' % (k, v['psnr']) for k, v in sorted(cfres['preview'].items())), confirm_preview))
    if tres:
        parts = []
        if 'cave_text' in hdtext_out:
            parts.append('cave %d messages / %d flows (%d nodes)' % (
                len(tres['cave_msgs']), len(tres['cave_flows']), sum(len(f['nodes']) for f in tres['cave_flows'])))
        if 'stamps_text' in hdtext_out:
            parts.append('stamps %d messages / %d NPC sub-graphs' % (len(tres['stamp_msgs']), len(tres['stamp_flows'])))
        if tres.get('amiibo'):
            parts.append('amiibo %d PNGs + layout.json (%s)' % (
                len(tres['amiibo']['pngs']), ', '.join(sorted(tres['amiibo']['screens']))))
        print('  hdtext: %s' % '; '.join(parts))
    for name, (n, size, dig) in installed:
        print('  %-24s %4d files %11d bytes %s' % (name, n, size, ('sha256 ' + dig[:16]) if dig else ''))
    print('  time  : %.1f s' % log.elapsed())
    rpt = os.path.join(stage, 'build_report.txt')
    try:
        lines = log.lines + [''] + arc_rows + [''] + (cres['report'] + [''] + cres['patch_log'] if cres else []) + \
            ([''] + sres['obj_stamp']['report'] if sres else []) + ['', rep.table()]
        common.write(rpt, '\n'.join(lines).encode('utf-8'))
    except Exception:  # noqa: BLE001
        pass
    if tmp_created and not a.keep_work and not fails:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('  work  : %s (report: %s)' % (work, rpt))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
