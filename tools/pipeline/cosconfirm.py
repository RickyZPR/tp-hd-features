"""cosconfirm.py - HD Cave of Shadows save-menu screens (pre-entry "Save and begin" / "Cancel", and the
"Cave of Shadows Record" after an exit portal) assets for the cave.

In TP HD, scanning the Wolf Link amiibo on the Collection screen opens the save menu (dMenu_save_c, use type 6)
before the warp into D_SB11: title "Cave of Shadows", A Confirm / B Back hints, the gold plate with the engraved
Wolf Link emblem, the amiibo's stored hearts and "Level B<n>", and the buttons "Save and begin" / "Cancel"
(from a study of HD's layout files and code). This module builds, from the user's own
TP HD dump (read only), the GC-loadable in-memory archive a native J2D screen in the mod needs to draw it, the
same way tools/pipeline/stampbook.py does it for the stamp book (and with its helpers: Source, make_bti,
encode/decode, Blo, the preview renderer).

OUTPUTS (folder res/cave/confirm/ of the HD Features mod; read by the C++ through ResourceService, never through
the DVD/overlay layer, exactly like res/stamps/stampbook/stampResR_gc.arc)
  cosconfirm_gc.arc   RARC, root node 'cosconfirm', nodes ROOT/TIMG/SCRN, retail GC layout
                      (tools/formats/rarc_write.py), uncompressed, every member 32-byte aligned, names lowercase:
                        scrn/zelda_file_select2.blo                 HD saveres, byte-identical (save menu screen)
                        scrn/zelda_file_select2.bck                 HD saveres, byte-identical (J2DAnmTransformKey)
                        scrn/zelda_file_select_info_amiibo.blo      HD saveres, byte-identical (hearts + Level)
                        scrn/zelda_file_select_icon_parts_revo.blo  HD saveres, byte-identical (A/B hints)
                        scrn/zelda_file_select_los_record.blo       HD saveres, byte-identical (dLos_record_c: the
                              "Cave of Shadows Record" panel on losrpt_n, save menu use type 7; src/cave/record.cpp)
                        timg/<every TEX1 texture of those 4 BLOs>.bti   full GC BTIs converted from the GX2 surfaces
                              (HD's own lookup order: saveres.pack.gz first, else main2DR / clctresR / stampResR)
                        timg/cos_heart_00..03.bti   HD saveres tt_heart_00..03 (full, 1/4, 2/4, 3/4 heart), which
                              HD's dFile_amiibo_c puts on hear_20..39 by name at runtime. Renamed so that, while the
                              archive is mounted (JKRMemArchive prepends itself to the global volume list), GC code
                              that looks hearts up by name (dFile_info_c::changeTexture("tt_heart_00.bti")) never
                              gets these HD hearts.
  confirm.json        manifest: sources + hashes, every member, texture format decisions and errors, which
                      textures mode 6 draws, the panes / frames / messages / placement the C++ uses, the GC-side
                      resources, name shadowing, load contract.
  Nothing else: not the save-slot info screen, not the .bpk/.btk/.brk (they animate materials that are absent
  or hidden here; layout report "Animations"). The Record screen (use type 7) reuses the same three layouts, the
  same bck (frames 4020..4030, 2089..2149) and the hearts, plus zelda_file_select_los_record.blo.

TEXTURE FORMATS: stampbook.choose_format/make_bti (lossless unless flat ink; Aurora ExpandTo8 semantics). Every
TEX1 texture is shipped at full quality, including the ones mode 6 never draws (save slots, popup, record art):
they must resolve for the BLOs to load, and keeping them real leaves the C++ free to show more of the save-menu
screen (e.g. HD's log list after "Save and begin") without a rebuild. confirm.json marks each texture
drawn_in_mode6 true/false and gives the bytes 1x1 placeholders would save.

DELIVERY (same contract as the stamp book; VERIFIED in the v2.0.3 source for stampResR_gc.arc)
  svc_resource->load("cave/confirm/cosconfirm_gc.arc") -> copy into a 32-byte aligned writable buffer ->
  JKR_NEW JKRMemArchive(buf, size, JKRMEMBREAK_FLAG_UNKNOWN0) while the screen exists;
  J2DScreen::setPriority("<blo>", 0x20000, arc) for the 3 layouts (TEX1 refs are type 2 = 'TIMG' lookups in arc);
  J2DAnmLoaderDataBase::load(JKRGetNameResource("zelda_file_select2.bck", arc)); hearts via
  (ResTIMG*)JKRGetTypeResource('TIMG', "cos_heart_0N.bti", arc) + J2DPicture::changeTexture(ResTIMG*, 0).

Library: build(hd, out_dir, log, gc=None, preview_dir=None) -> result dict;
         validate(rep, res) adds rows to a tools/pipeline/validate.Report.
CLI:     python -I cosconfirm.py --hd <HD content> --out <dir> [--gc <GC files dir>] [--preview <dir>]
"""
import hashlib
import math
import os
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402  (puts tools/formats first on sys.path)
import rarc  # noqa: E402
import rarc_write  # noqa: E402
import rarcx  # noqa: E402
import stampbook as sb  # noqa: E402

ARC_NAME = 'cosconfirm_gc.arc'
ARC_ROOT = 'cosconfirm'
RES_PREFIX = 'cave/confirm/'               # ResourceService path of res/cave/confirm/ in the HD Features mod
MANIFEST = 'confirm.json'
OUT_FILES = [ARC_NAME, MANIFEST]
SAVERES = ('res/Layout', 'saveres.arc')
# HD's runtime lookup order for a texture saveres lacks (global volume list; INFERRED order, VERIFIED identical
# pixels wherever several of them have the name - see confirm.json textures.*.also_in)
TEX_FALLBACK = [('res/LayoutRevo', 'main2DR.arc'), ('res/LayoutRevo', 'clctresR.arc'),
                ('res/LayoutRevo', 'stampResR.arc')]
FS2, INFO, ICONS = 'zelda_file_select2.blo', 'zelda_file_select_info_amiibo.blo', 'zelda_file_select_icon_parts_revo.blo'
RECORD = 'zelda_file_select_los_record.blo'
BCK = 'zelda_file_select2.bck'
LAYOUTS = [(FS2, 'dMenu_save_c screen: background, title bar, plate amiibo_n, Yes/No buttons (+ the save menu '
                 'parts mode 6 keeps hidden or off-screen)'),
           (INFO, 'dFile_amiibo_c info screen drawn on the plate amiibo_n: hearts row, "Level  B<n>"'),
           (ICONS, 'A Confirm / B Back hints, top right'),
           (RECORD, 'dLos_record_c panel of the "Cave of Shadows Record" screen (use type 7), drawn on losrpt_n: '
                    'amiibo used / completion time / max combo / damage, the Wolf Link amiibo prompt, Close')]
ANIMS = [(BCK, 'J2DAnmTransformKey for %s (4500 frames)' % FS2)]
# (archive name, HD saveres member, meaning)
CODE_TEXTURES = [('cos_heart_00.bti', 'tt_heart_00.bti', 'full heart (every filled heart but a partial last one)'),
                 ('cos_heart_01.bti', 'tt_heart_01.bti', 'last heart when life % 4 == 1 (1/4)'),
                 ('cos_heart_02.bti', 'tt_heart_02.bti', 'last heart when life % 4 == 2 (2/4)'),
                 ('cos_heart_03.bti', 'tt_heart_03.bti', 'last heart when life % 4 == 3 (3/4)')]
# Messages (HD Msgus bmgres.arc zel_00; the text itself ships through tools/pipeline/hdtext.py)
MESSAGES = [(413, 'title: t_for (w_mg_n header, data case) / t_for2 (a_title, no-data case), 27x27 font, '
                  'line space 20, char space 0 (t_for) / 2 (t_for1, t_for2)'),
            (414, 'Yes text pane f_yes_t'), (415, 'No text pane f_no_t'),
            (1036, 'A hint cont_at + shadow cont_at1'), (1017, 'B hint cont_bt + shadow cont_bt1'),
            (1260, 'info screen level_l (right-aligned)'), (855, 'info screen level_v format (sprintf with the floor)'),
            (7, 'Yes text restored when the save flow starts'), (8, 'No text restored when the save flow starts'),
            (1258, 'Record screen header t_for'), (1061, 'Record screen A hint while the record is revealed'),
            (1254, 'Record: amused_t'), (1255, 'Record: cmtime_t'), (1256, 'Record: mxcomb_t'),
            (1257, 'Record: dmgrec_t'), (1259, 'Record: info t_newrec'), (1261, 'Record: prompt_t (amiibo question)'),
            (1262, 'Record: prompt_t (leave without saving?)'), (1263, 'Record: cl_bt_tx'),
            (854, 'Record: "x%d" (whole hearts, Zelda amiibo count)')]
# bck frames HD uses for mode 6 (layout report "Animations"; HD advances 2 frames per tick)
FRAMES = {
    'setup_Nm_02': {'pane': 'Nm_02', 'frames': [1, 1], 'note': 'applied to the Nm_02 subtree at screen setup: '
                    'rescales Nm_02 to 0.95 and moves background + title bar into place (required)'},
    'yesno_in': {'pane': 'w_yes_n + w_no_n', 'frames': [2109, 2119]},
    'yesno_out': {'pane': 'w_yes_n + w_no_n', 'frames': [2099, 2089]},
    'select_cancel': {'pane': 'w_yes_n + w_no_n', 'frames': [2119, 2137],
                      'note': 'key values VERIFIED, HD frame table INFERRED from GC'},
    'select_save': {'pane': 'w_yes_n + w_no_n', 'frames': [2139, 2149],
                    'note': 'key values VERIFIED, HD frame table INFERRED from GC'},
    'plate_in': {'pane': 'amiibo_n (data) / a_title (no data)', 'frames': [4000, 4010]},
    'plate_out': {'pane': 'amiibo_n / a_title', 'frames': [4010, 4000]},
    'record_state': {'pane': 'amiibo_n + losrpt_n', 'frames': [4020, 4030],
                     'note': 'mode 7 (Record screen, record.cpp): plate up to y 27.7, panel scale 0 -> 1'},
    'record_yesno_in': {'pane': 'w_yes_n + w_no_n', 'frames': [2089, 2099],
                        'note': 'mode 7 prompt (HD 0x02a05a70(2089, 2099)): ends with No enlarged (cursor No)'},
}
# Panes the C++ drives (layout report "Panes the C++ drives"); validated to exist with this type.
PANES = {
    FS2: {'Nm_02': 'PAN2', 'w_dmbas0': 'PIC2', 'w_dmhibi': 'PIC2', 'w_mg_n': 'PAN2', 'w_mg_w07': 'PIC2',
          'w_mgw03': 'PIC2', 'w_mgn': 'PAN2', 't_for': 'TBX2', 't_for1': 'TBX2', 'w_tabi_s': 'TBX2',
          'w_tabi_x': 'TBX2', 'amiibo_n': 'PAN2', 'aminf_bg': 'PIC2', 'w_yes_n': 'PAN2', 'w_yes_02': 'PIC2',
          'w_yes_m': 'PIC2', 'f_yes_t': 'TBX2', 'w_yes_t': 'TBX2', 'w_no_n': 'PAN2', 'w_no_02': 'PIC2',
          'w_no_m': 'PIC2', 'f_no_t': 'TBX2', 'w_no_t': 'TBX2', 'a_title': 'PAN2', 'w_mg_w01': 'PIC2',
          'w_mgw02': 'PIC2', 'w_tabi_1': 'TBX2', 't_for2': 'TBX2', 'losrpt_n': 'PAN2', 'w_btn_n': 'PAN2',
          'N_pop': 'PAN2', 'w_er_n': 'PAN2', 't_gmov_1': 'TBX2', 'w_yes_g': 'PIC2', 'w_no_g': 'PIC2'},
    INFO: dict([('Nm_02', 'PAN2'), ('w_dat_i1', 'PAN2'), ('w_n_ht01', 'PAN2'), ('level_l', 'TBX2'),
                ('level_v', 'TBX2'), ('w_htdiff', 'TBX2'), ('t_newrec', 'TBX2'), ('w_htdif1', 'TBX2'),
                ('t_w_01', 'PIC2'), ('t_w_02', 'PIC2'), ('t_w_04', 'PIC2'), ('w_hm', 'TBX2'), ('hear_01', 'PIC2'),
                ('hear_02', 'PIC2'), ('heab_02', 'PIC2')] +
               [('hear_%02d' % i, 'PIC2') for i in range(20, 40)] + [('heab_%02d' % i, 'PIC2') for i in range(20, 40)]),
    ICONS: {'abtn_n': 'PAN2', 'a_btn_cp': 'PIC2', 'a_btn_c1': 'PIC2', 'a_btn_c2': 'PIC2', 'cont_at': 'TBX2',
            'cont_at1': 'TBX2', 'bbtn_n': 'PAN2', 'b_btn_cp': 'PIC2', 'cont_bt': 'TBX2', 'cont_bt1': 'TBX2'},
    RECORD: {'Nm_02': 'PAN2', 'submenu': 'PAN2', 'data_n': 'PAN2', 'amused_t': 'TBX2', 'cmtime_t': 'TBX2',
             'cmtime_v': 'TBX2', 'mxcomb_t': 'TBX2', 'mxcomb_v': 'TBX2', 'dmgrec_t': 'TBX2', 'dmgrec_v': 'TBX2',
             'dmghea_1': 'PIC2', 'dmghea_2': 'PIC2', 'dmghea_3': 'PIC2', 'promp_i1': 'PIC2', 'promp_i2': 'PIC2',
             'prompt_t': 'TBX2', 'cl_bt_im': 'PIC2', 'cl_bt_tx': 'TBX2', 'amwolf_n': 'PAN2', 'amwolf_i': 'PIC2',
             'amwolf_l': 'TBX2', 'amwolf_v': 'TBX2', 'amganf_n': 'PAN2', 'amganf_i': 'PIC2', 'amganf_l': 'TBX2',
             'amganf_v': 'TBX2', 'amzeld_n': 'PAN2', 'amzeld_i': 'PIC2', 'amzeld_v': 'TBX2'},
}
INFO_HIDDEN = ['t_w_02', 't_w_01', 'w_hm', 'hear_01', 'hear_02', 'heab_02', 't_w_04']
INFO_EMPTY = ['level_v', 'w_htdiff', 't_newrec', 'w_htdif1']
INFO_REF = 'w_dat_i1'                      # info screen pane fitted into amiibo_n (HD 0x0294a108)
# GC-side resources the C++ takes from Dusklight's own GC data (not shipped)
GC_NEEDS = [('res/Layout/main2D.arc', 'zelda_select_cursor_4parts.blo', 'dSelect_cursor_c (gold 4-corner cursor)'),
            ('res/Layout/main2D.arc', 'zelda_select_cursor_4parts.bpk', 'dSelect_cursor_c colour animation'),
            ('res/Layout/main2D.arc', 'zelda_select_cursor_4parts.btk', 'dSelect_cursor_c texture animation'),
            ('res/Fontus/fontres.arc', 'rodan_b_24_22.bfn', 'message font for setFont (mDoExt_getMesgFont) and the '
             'hints\' FNT1')]
# Textures the layout report's composite draws (its drawn_panes.json + the 4 code hearts): regression check
EXPECTED_DRAWN = ['ab_backdrop2', 'back1', 'button1', 'button2', 'buttonfull3', 'cos_heart_00', 'cos_heart_01',
                  'cos_heart_02', 'cos_heart_03', 'titlebar', 'tr_a_btn_00', 'tr_b_btn_00', 'tt_hearb_00']
SHADOW_ARCS = ['res/Layout/main2D.arc', 'res/Layout/clctres.arc', 'res/Layout/saveres.arc', 'res/Object/fileSel.arc',
               'res/Layout/ringres.arc', 'res/Layout/optres.arc']
J2D_BLOCKS = {'INF1', 'TEX1', 'FNT1', 'MAT1', 'BGN1', 'END1', 'EXT1', 'PAN1', 'PAN2', 'PIC1', 'PIC2', 'TBX1',
              'TBX2', 'WIN1', 'WIN2'}       # v2.0.3 J2DScreen::makeHierarchyPanes / createPane
SIZE_LIMIT = 16 * 1024 * 1024


def _u16(d, o):
    return struct.unpack_from('>H', d, o)[0]


def _u32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def _sha1(d):
    return hashlib.sha1(d).hexdigest()


# ------------------------------------------------------------------------------------------ BLO + BCK
class Blo(sb.Blo):
    """stampbook.Blo + the pane animation index (J2DPane field_0x4, PAN2 +0xA) and the BLO transform kept for
    re-applying animations."""

    def _pane(self, o, t):
        p = sb.Blo._pane(self, o, t)
        q = o if t == 'PAN2' else o + 8
        p['aid'] = _u16(self.d, q + 0xA)
        p['base'] = (p['x'], p['y'], p['sx'], p['sy'], p['rz'])
        return p

    def reset(self):
        for p in self.walk():
            if 'base' in p:
                p['x'], p['y'], p['sx'], p['sy'], p['rz'] = p['base']
        self.place()

    def place(self):
        sb._place(self.root, [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]])


def _hermite(p, t0, v0, s0, t1, v1, s1):
    dt = t1 - t0
    if dt == 0:
        return v0
    t = (p - t0) / dt
    t2, t3 = t * t, t * t * t
    return (2 * t3 - 3 * t2 + 1) * v0 + (t3 - 2 * t2 + t) * s0 * dt + (-2 * t3 + 3 * t2) * v1 + (t3 - t2) * s1 * dt


def _key(frame, maxf, typ, vals):
    """J2DGetKeyFrameInterpolation (v2.0.3 J2DAnimation.h): type 0 keys (t, v, s), type 1 keys (t, v, in, out)."""
    if maxf == 1:
        return vals[0]
    st = 3 if typ == 0 else 4
    if frame < vals[0]:
        return vals[1]
    last = (maxf - 1) * st
    if vals[last] <= frame:
        return vals[last + 1]
    base, n = 0, maxf
    while n > 1:
        half = n // 2
        if frame >= vals[base + half * st]:
            base += half * st
            n -= half
        else:
            n = half
    v = vals[base:]
    if typ == 0:
        return _hermite(frame, v[0], v[1], v[2], v[3], v[4], v[5])
    return _hermite(frame, v[0], v[1], v[3], v[4], v[5], v[6])


class Bck(object):
    """J3D1bck1 / ANK1 (J2DAnmKeyLoader) evaluator; transform() maps to J2DPane::updateTransform:
    scale.x/z, rotation x/z/y -> rotX/rotY/rotZ, translate.x/z."""

    def __init__(self, d):
        if d[:8] != b'J3D1bck1' or d[0x20:0x24] != b'ANK1':
            raise ValueError('not a J3D1bck1/ANK1 animation: %r %r' % (d[:8], d[0x20:0x24]))
        o = 0x20
        self.loop, self.rdec = d[o + 8], d[o + 9]
        self.frame_max, self.count, sc, rc, tc = struct.unpack_from('>hHHHH', d, o + 0xA)
        to, so, ro, tro = struct.unpack_from('>IIII', d, o + 0x14)
        self.S = list(struct.unpack_from('>%df' % sc, d, o + so))
        self.R = list(struct.unpack_from('>%dh' % rc, d, o + ro))
        self.T = list(struct.unpack_from('>%df' % tc, d, o + tro))
        self.tab = [struct.unpack_from('>9H', d, o + to + 0x12 * i) for i in range(self.count * 3)]
        self.sizes = (sc, rc, tc)

    def _comp(self, idx, axis, kind, frame):
        maxf, off, typ = self.tab[idx * 3 + axis][kind * 3:kind * 3 + 3]
        if maxf == 0:
            return (1.0, 0.0, 0.0)[kind]
        v = _key(frame, maxf, typ, (self.S, self.R, self.T)[kind][off:])
        if kind == 1:
            return ((int(v) << self.rdec) & 0xFFFF) * 360.0 / 65535.0
        return v

    def transform(self, idx, frame):
        return {'sx': self._comp(idx, 0, 0, frame), 'sy': self._comp(idx, 2, 0, frame),
                'rx': self._comp(idx, 0, 1, frame), 'ry': self._comp(idx, 2, 1, frame),
                'rz': self._comp(idx, 1, 1, frame), 'x': self._comp(idx, 0, 2, frame), 'y': self._comp(idx, 2, 2, frame)}

    def table_ok(self):
        """Every key table slice lies inside its value array."""
        bad = []
        for i, e in enumerate(self.tab):
            for kind in range(3):
                maxf, off, typ = e[kind * 3:kind * 3 + 3]
                if maxf == 0:
                    continue
                n = 1 if maxf == 1 else maxf * (3 if typ == 0 else 4)
                if typ not in (0, 1) or off + n > self.sizes[kind]:
                    bad.append((i // 3, i % 3, kind))
        return bad


def _apply(blo, bck, name, frame):
    """J2DPane::animationTransform on the subtree of `name` (pane values only; caller re-places)."""
    root = blo.find(name)
    if root is None:
        return False
    for p in blo.walk(root):
        if p['type'] != 'SCREEN' and p.get('aid', 0xFFFF) < bck.count:
            t = bck.transform(p['aid'], frame)
            p['x'], p['y'], p['sx'], p['sy'], p['rz'] = t['x'], t['y'], t['sx'], t['sy'], t['rz']
    return True


def _place_info(ia, plate):
    """dDlst_FileInfo_c::draw / HD dFile_amiibo_c draw (0x0294a108): Nm_02 position matrix =
    amiibo_n global matrix x Scale(amiibo_n.w / w_dat_i1.w, amiibo_n.h / w_dat_i1.h); children follow."""
    ref, nm = ia.find(INFO_REF), ia.find('Nm_02')
    kx, ky = plate['w'] / ref['w'], plate['h'] / ref['h']
    (a, b, c), (d, e, f) = plate['m']
    nm['m'] = [[a * kx, b * ky, c], [d * kx, e * ky, f]]
    for ch in nm['children']:
        sb._place(ch, nm['m'])
    return kx, ky


def hearts(life, max_life):
    """HD 0x0294a718: {pane: (visible, archive texture or None)} for hear_20..39 / heab_20..39.
    life in quarter hearts, max_life in fifths (TP units)."""
    out = {}
    filled = (life + 3) // 4 if life > 0 else 0
    part = life % 4 if life > 0 else 0
    for i in range(20):
        out['heab_%02d' % (20 + i)] = (i < max_life // 5, None)
        tex = None
        if i < filled:
            tex = CODE_TEXTURES[part][0] if (part and i == filled - 1) else CODE_TEXTURES[0][0]
        out['hear_%02d' % (20 + i)] = (i < filled, tex)
    return out


# ------------------------------------------------------------------------------------------ build
def _source_info(s):
    return {'sha1': s.arc_sha1, 'pack': os.path.basename(s.pack_path) if s.pack_path else None,
            'pack_sha1': s.pack_sha1}


def _timg(src, name):
    """-> member path of timg/<name> in an HD Source, or None."""
    p = '%s/timg/%s' % (src.arc.nodes[0]['name'], name.lower())
    try:
        return src.member(p).path
    except KeyError:
        return None


def _hd_messages(hd):
    """{id: text} for MESSAGES from HD Msgus/bmgres.arc zel_00 (plain characters; none of these has tags)."""
    import hdtext
    p = common.exists_ci(os.path.join(hd, 'res', 'Msgus'), 'bmgres.arc')
    out = {}
    if p is None:
        return out
    bm = hdtext.load_bmg(p, 'zel_00')
    for mid, _ in MESSAGES:
        e = bm.by_id.get(mid)
        if e is None:
            continue
        toks = hdtext.tokens(e.text)
        out[mid] = {'text': ''.join(bytes((t[1],)).decode('cp1252', 'replace') for t in toks if t[0] == 'c'),
                    'tags': sum(1 for t in toks if t[0] != 'c')}
    return out


def build(hd, out_dir, log, gc=None, preview_dir=None):
    """Write cosconfirm_gc.arc + confirm.json into out_dir (created; existing files overwritten)."""
    os.makedirs(out_dir, exist_ok=True)
    sv = sb.Source.get(hd, *SAVERES)
    falls = [sb.Source.get(hd, f, a) for f, a in TEX_FALLBACK]
    res = {'out_dir': out_dir, 'gc': gc, 'preview_dir': preview_dir, 'hd': hd, 'sources': {}, 'layouts': {},
           'anims': {}, 'textures': {}, 'hd_rgba': {}, 'entries': []}
    res['sources'][sv.rel] = _source_info(sv)
    entries = []
    blos = {}
    for name, role in LAYOUTS:
        d = sv.member('saveres/scrn/' + name).data()
        blos[name] = Blo(d)
        res['layouts'][name] = {'member': 'scrn/' + name, 'hd': '%s:saveres/scrn/%s' % (sv.rel, name), 'size': len(d),
                                'sha1': _sha1(d), 'role': role}
        entries.append(('scrn/' + name, d))
    for name, role in ANIMS:
        d = sv.member('saveres/scrn/' + name).data()
        res['anims'][name] = {'member': 'scrn/' + name, 'hd': '%s:saveres/scrn/%s' % (sv.rel, name), 'size': len(d),
                              'sha1': _sha1(d), 'role': role}
        entries.append(('scrn/' + name, d))
    # TEX1 textures (lowercased like JKR's name lookup), first come first served across the 3 layouts
    refs = {}
    for name, _ in LAYOUTS:
        for kind, tn in blos[name].tex:
            refs.setdefault(tn.lower(), {'tex1_name': tn, 'kind': kind, 'layouts': []})['layouts'].append(name)
    for tn, r in sorted(refs.items()):
        cands = [s for s in [sv] + falls if _timg(s, tn)]
        if not cands:
            raise KeyError('no HD source for TEX1 texture %s (%s)' % (tn, ', '.join(r['layouts'])))
        s0 = cands[0]
        t = s0.texture(_timg(s0, tn))
        data, info, back = sb.make_bti(t)
        stem = tn[:-4]
        also = {}
        for s in cands[1:]:
            t2 = s.texture(_timg(s, tn))
            also[s.rel] = 'identical pixels' if (t2['w'], t2['h'], t2['rgba']) == (t['w'], t['h'], t['rgba']) else \
                'differs (%dx%d)' % (t2['w'], t2['h'])
            res['sources'].setdefault(s.rel, _source_info(s))
        res['sources'].setdefault(s0.rel, _source_info(s0))
        info.update({'member': 'timg/' + tn, 'src': '%s:%s' % (s0.rel, _timg(s0, tn)), 'gtx_member': t['gtx_name'],
                     'tex1_name': r['tex1_name'], 'resref_type': r['kind'], 'referenced_by': r['layouts'],
                     'also_in': also, 'sha256': common.sha256(data)})
        res['textures'][stem] = info
        res['hd_rgba'][stem] = (t['w'], t['h'], t['rgba'])
        entries.append(('timg/' + tn, data))
    for an, hn, what in CODE_TEXTURES:
        t = sv.texture(_timg(sv, hn))
        data, info, back = sb.make_bti(t)
        stem = an[:-4]
        also = {}
        for s in falls:
            if _timg(s, hn):
                t2 = s.texture(_timg(s, hn))
                also[s.rel] = 'identical pixels' if (t2['w'], t2['h'], t2['rgba']) == (t['w'], t['h'], t['rgba']) else \
                    'differs (%dx%d, not used: HD resolves the name to saveres, which is mounted last)' % (t2['w'], t2['h'])
        info.update({'member': 'timg/' + an, 'src': '%s:%s' % (sv.rel, _timg(sv, hn)), 'gtx_member': t['gtx_name'],
                     'hd_name': hn, 'use': what, 'referenced_by': ['code (hearts row)'], 'also_in': also,
                     'sha256': common.sha256(data)})
        res['textures'][stem] = info
        res['hd_rgba'][stem] = (t['w'], t['h'], t['rgba'])
        entries.append(('timg/' + an, data))
    # timg first, then scrn (node order ROOT, TIMG, SCRN like the stamp book archive)
    entries.sort(key=lambda e: (0 if e[0].startswith('timg/') else 1, e[0]))
    arc_bytes = rarc_write.build(ARC_ROOT, entries)
    common.write(os.path.join(out_dir, ARC_NAME), arc_bytes)
    res['entries'] = [e[0] for e in entries]
    res['arc'] = {'size': len(arc_bytes), 'sha256': common.sha256(arc_bytes), 'members': len(entries)}
    res['messages'] = _hd_messages(hd)
    res['drawn'] = drawn_textures(blos, Bck(dict(entries)['scrn/' + BCK]))
    for stem, info in res['textures'].items():
        info['drawn_in_mode6'] = stem in res['drawn']
        info['drawn_by'] = sorted(res['drawn'].get(stem, []))[:6]
    res['blo_stats'] = {n: {'panes': sum(b.counts.values()), 'materials': len(b.mats)} for n, b in blos.items()}
    log('cosconfirm: %s %d bytes (%.2f MB / %.2f MiB), %d members (%d BLO, 1 BCK, %d BTI)', ARC_NAME, len(arc_bytes),
        len(arc_bytes) / 1e6, len(arc_bytes) / 1048576.0, len(entries), len(LAYOUTS), len(res['textures']))
    fmts = {}
    for n, i in res['textures'].items():
        fmts.setdefault(i['fmt'], []).append(n)
    for f, ns in sorted(fmts.items()):
        log('cosconfirm:   %-6s x%2d  %s', f, len(ns), ', '.join(sorted(ns)[:8]) + (' ...' if len(ns) > 8 else ''))
    nd = [k for k, v in res['textures'].items() if not v['drawn_in_mode6']]
    log('cosconfirm:   %d textures drawn in mode 6, %d only referenced (%d bytes as full BTIs)',
        len(res['textures']) - len(nd), len(nd), sum(res['textures'][k]['bytes'] for k in nd))
    res['manifest'] = _manifest(res)
    common.write_json(os.path.join(out_dir, MANIFEST), res['manifest'])
    return res


# ------------------------------------------------------------------------------------------ mode 6 states
STATES = {
    # name: (life quarter-hearts, max life fifths, level, cursor on Yes, yes/no frame, plate frame)
    'entry': (10, 20, 6, True, 2119, 4010),       # the HD video frame: 2 full + 1 half + 1 empty, Level B6
    'cancel': (10, 20, 6, False, 2137, 4010),     # Cancel selected (end of the select_cancel swap)
    'nodata': (0, 0, 0, True, 2119, 4010),        # fresh amiibo (life <= 0): centred a_title, no plate
    'full': (79, 100, 50, True, 2119, 4010),      # both heart rows: 19 full + 3/4, 20 containers, Level B50
}


def configure(blos, bck, life, max_life, level, yes, yn, am, text=None, header_dy=0.0):
    """Pane edits HD's dMenu_save_c (use type 6) + dFile_amiibo_c make, as (screen, overrides) in draw order.
    Frame/visibility facts: layout + code reports (HD Zelda.rpx addresses there)."""
    text = text or {}
    msg = lambda i, d: text.get(i, d)  # noqa: E731
    fs, ia, ip = blos[FS2], blos[INFO], blos[ICONS]
    for b in (fs, ia, ip):
        b.reset()
    data = life > 0
    _apply(fs, bck, 'Nm_02', 1)
    for n in ('w_yes_n', 'w_no_n'):
        _apply(fs, bck, n, yn)
    for n in ('amiibo_n', 'a_title', 'losrpt_n'):
        _apply(fs, bck, n, am)
    if header_dy:
        fs.find('w_mg_n')['y'] += header_dy
    fs.place()
    vis = {'w_tabi_s': False, 'w_tabi_x': False, 'Wi_btn_n': False, 'GC_btn_n': True, 'w_yes_t': False,
           'w_no_t': False, 'w_kete': False, 'w_modo': False, 'w_er_msg': False, 'w_er_msR': False,
           'w_tabi_1': False, 'a_title': not data, 'amiibo_n': data, 'w_mg_n': data}
    for i in range(10):
        vis['tmoyou%02d' % i] = False
    alpha = {'w_yes_m': 1.0 if yes else 0.0, 'w_no_m': 0.0 if yes else 1.0, 'w_yes_g': 0.0, 'w_no_g': 0.0,
             'w_yes_gr': 0.0, 'w_no_gr': 0.0, 't_for1': 0.0, 'w_bk_l00': 0.0, 'w_bk_l01': 0.0, 'w_bk_l02': 0.0}
    title = msg(413, 'Cave of Shadows')
    txt = {'t_for': title, 't_for2': title, 'f_yes_t': msg(414, 'Save and enter'), 'f_no_t': msg(415, 'Cancel'),
           't_gmov_1': '', 'er_for0': '', 'er_for1': '', 'er_for2': ''}
    sel, other = (255, 255, 255, 255), (150, 150, 150, 255)
    white = {'f_yes_t': sel if yes else other, 'f_no_t': other if yes else sel}
    fsize = {'t_for': (27, 27, 20, 0), 't_for1': (27, 27, 20, 2), 't_for2': (27, 27, 20, 2)}
    fonted = {'t_for', 't_for1', 't_for2', 'f_yes_t', 'f_no_t', 'f_kete', 'f_modo', 'er_for0', 'er_for1', 'er_for2',
              't_gmov_1'}
    out = [(fs, {'vis': vis, 'text': txt, 'alpha': alpha, 'white': white, 'fsize': fsize, 'fonted': fonted})]
    if data:
        ia.place()
        _place_info(ia, fs.find('amiibo_n'))
        ivis = dict((n, False) for n in INFO_HIDDEN)
        itex = {}
        for pn, (v, t) in hearts(life, max_life).items():
            ivis[pn] = v
            if t:
                itex[pn] = t
        lv = msg(855, 'B%d')
        itext = {'level_l': msg(1260, 'Level'), 'level_v': lv.replace('%d', str(level), 1) if level > 0 else '',
                 'w_htdiff': '', 't_newrec': '', 'w_htdif1': ''}
        if level <= 0:
            ivis['level_l'] = False
        out.append((ia, {'vis': ivis, 'text': itext, 'tex': itex,
                         'fonted': {'level_l', 'level_v', 'w_htdiff', 't_newrec', 'w_htdif1', 'w_hm'}}))
    ip.place()
    out.append((ip, {'text': {'cont_at': msg(1036, 'Confirm'), 'cont_at1': msg(1036, 'Confirm'),
                              'cont_bt': msg(1017, 'Back'), 'cont_bt1': msg(1017, 'Back')}}))
    return out


def draw_list(blo, cfg):
    """Panes J2DScreen::draw would draw with these overrides: [(kind, pane, material, alpha, text)]."""
    vis, text, tex = cfg.get('vis', {}), cfg.get('text', {}), cfg.get('tex', {})
    alpha, white, fsize, fonted = cfg.get('alpha', {}), cfg.get('white', {}), cfg.get('fsize', {}), cfg.get('fonted')
    out = []

    def rec(p, pv, pa):
        v = vis.get(p['name'], p['visible']) and pv
        a = pa * alpha.get(p['name'], 1.0)
        if v and p['type'] in ('PIC2', 'TBX2') and p.get('material', 0xFFFF) < len(blo.mats):
            m0 = blo.mats[p['material']]
            mat = dict(m0, textures=list(m0['textures']), tevcolor=[list(c) if c else c for c in m0['tevcolor']],
                       matcolor=list(m0['matcolor']) if m0['matcolor'] else None)
            if p['name'] in tex:
                mat['textures'] = [tex[p['name']]]
            if p['name'] in white:
                mat['tevcolor'][1] = list(white[p['name']])
            if p['type'] == 'PIC2':
                out.append(('pic', p, mat, a, None))
            elif fonted is None or p['name'] in fonted:
                s = text.get(p['name'], p['text'])
                if s:
                    q = dict(p)
                    if p['name'] in fsize:
                        q['font_w'], q['font_h'], q['line_space'], q['char_space'] = fsize[p['name']]
                    if mat['matcolor']:
                        mat['matcolor'] = mat['matcolor'][:3] + [int(round(mat['matcolor'][3] * a))]
                    out.append(('txt', q, mat, a, s))
        for c in p['children']:
            rec(c, v, a)
    rec(blo.root, True, 1.0)
    return out


def _on_screen(p):
    (a, b, c), (d, e, f) = p['m']
    ax, ay = p['anchor']
    pts = [(a * x + b * y + c, d * x + e * y + f) for x, y in
           ((-ax, -ay), (p['w'] - ax, -ay), (-ax, p['h'] - ay), (p['w'] - ax, p['h'] - ay))]
    xs, ys = [q[0] for q in pts], [q[1] for q in pts]
    area = abs((pts[1][0] - pts[0][0]) * (pts[2][1] - pts[0][1]) - (pts[1][1] - pts[0][1]) * (pts[2][0] - pts[0][0]))
    return area > 0.25 and min(xs) < 608 and max(xs) > 0 and min(ys) < 448 and max(ys) > 0


def drawn_textures(blos, bck):
    """{texture stem: {pane, ...}} over every mode-6 state and animation frame: both variants (data with all 40
    heart panes / no data), both selections, yes/no frames 2089..2149, plate frames 4000..4010, plus the 1/4, 2/4
    and full last heart. Pure Python (deterministic)."""
    out = {}
    runs = []
    for life, mx in ((79, 100), (0, 0)):              # all 40 heart panes (last one 3/4) / no data
        for yes in (True, False):
            for yn in range(2089, 2150):
                runs.append((life, mx, yes, yn, 4010))
            for am in range(4000, 4011):
                runs.append((life, mx, yes, 2119, am))
    for life in (77, 78, 80):                         # the other last-heart textures (1/4, 2/4, full)
        runs.append((life, 100, True, 2119, 4010))
    for life, mx, yes, yn, am in runs:
        for blo, cfg in configure(blos, bck, life, mx, 6, yes, yn, am):
            for kind, p, mat, a, s in draw_list(blo, cfg):
                ma = (mat['matcolor'][3] / 255.0) if (mat['matcolor'] and kind == 'pic') else 1.0
                if kind != 'pic' or a * ma <= 0 or not _on_screen(p):
                    continue
                for t in mat['textures'][:1]:
                    stem = t.lower()[:-4] if t.lower().endswith('.bti') else t.lower()
                    out.setdefault(stem, set()).add(p['name'])
    return out


# ------------------------------------------------------------------------------------------ manifest
def _manifest(res):
    tex = {}
    for k in sorted(res['textures']):
        tex[k] = dict(res['textures'][k])
    nd = sorted(k for k, v in tex.items() if not v['drawn_in_mode6'])
    return {
        'version': 1,
        'note': 'Generated by tools/build_assets.py (tools/pipeline/cosconfirm.py) from the user\'s own TP HD dump; '
                'Nintendo data, local use only, never commit or distribute.',
        'screen': 'TP HD Cave of Shadows pre-entry screen = dMenu_save_c use type 6 (layout + code research in '
                  'scratchpad cosconfirm/hdlayout, cosconfirm/hdcode). Draw order (INFERRED): ' + FS2 + ' -> ' + INFO +
                  ' (on amiibo_n, data case only) -> ' + ICONS + ' -> dSelect_cursor_c.',
        'sources': {k: res['sources'][k] for k in sorted(res['sources'])},
        'archive': {
            'file': ARC_NAME, 'resource_path': RES_PREFIX + ARC_NAME, 'root': ARC_ROOT,
            'size': res['arc']['size'], 'sha256': res['arc']['sha256'], 'members': res['entries'],
            'load': 'svc_resource->load("%s%s") -> copy into a 32-byte aligned writable buffer -> JKR_NEW '
                    'JKRMemArchive(buf, size, JKRMEMBREAK_FLAG_UNKNOWN0) while the screen exists (it prepends itself '
                    'to the global volume list: build GC-side pieces first, unmount before GC screens are built) -> '
                    'J2DScreen::setPriority("<blo>", 0x20000, arc) for the layouts (the Record screen adds %s); '
                    'TEX1 refs are type 2 (TIMG lookups inside arc).' % (RES_PREFIX, ARC_NAME, RECORD),
        },
        'layouts': {k: dict(v, panes=res['blo_stats'][k]['panes'], materials=res['blo_stats'][k]['materials'])
                    for k, v in res['layouts'].items()},
        'animations': {k: dict(v, load='J2DAnmLoaderDataBase::load(JKRGetNameResource("%s", arc)); '
                                     'pane->setAnimation(anm); anm->setFrame(f); pane->animationTransform()' % k,
                               frames=FRAMES)
                       for k, v in res['anims'].items()},
        'textures': tex,
        'textures_not_drawn_in_mode6': {
            'names': nd, 'bytes': sum(tex[k]['bytes'] for k in nd),
            'note': 'referenced by the BLOs (must resolve for them to load) but never visible in mode 6; shipped as '
                    'full lossless BTIs anyway (see module docstring); 1x1 placeholders would save about these bytes.'},
        'code_textures': {an: {'hd_name': hn, 'use': what,
                               'get': '(ResTIMG*)JKRGetTypeResource(\'TIMG\', "%s", arc) -> J2DPicture::changeTexture'
                                      '(timg, 0) on hear_20..39 (never by name: the name lookup is global)' % an}
                          for an, hn, what in CODE_TEXTURES},
        'hearts': 'heab_(20+i) visible when i < maxLife/5; hear_(20+i) visible when i < ceil(life/4); the last one '
                  'gets cos_heart_01/02/03 when life%4 is 1/2/3, every other visible one cos_heart_00 (life in '
                  'quarter hearts, maxLife in fifths; amiibo data +0x12 / +0x13; HD 0x0294a718). 2 rows of 10.',
        'level': 'level <= 0: level_l hidden, level_v ""; else level_l = msg 1260, level_v = sprintf(msg 855, level) '
                 '(amiibo data +0x11; HD 0x0294aa8c)',
        'info_screen': {'hidden': INFO_HIDDEN, 'empty_text': INFO_EMPTY, 'fonted': ['level_l', 'level_v'],
                        'placement': '%s Nm_02 position matrix = amiibo_n global matrix x Scale(amiibo_n.w / %s.w, '
                                     'amiibo_n.h / %s.h) = Scale(420/474, 85/86) (HD 0x0294a108, same as v2.0.3 '
                                     'dDlst_FileInfo_c::draw)' % (INFO, INFO_REF, INFO_REF)},
        'save_screen': {
            'data_case': 'life > 0: a_title hidden, w_mg_n (header t_for = msg 413) visible and slides up from +190 '
                         'over the last 20 of 50 intro frames; then A/B hints set, Yes/No in, info set, amiibo_n shown '
                         'and plate_in played',
            'nodata_case': 'life <= 0: w_mg_n hidden, a_title (t_for2 = msg 413) scaled in with plate_in frames, '
                           'no plate',
            'hidden': ['w_tabi_s', 'w_tabi_x', 'w_tabi_1', 'w_yes_t', 'w_no_t', 'tmoyou00..09', 'Wi_btn_n'],
            'alpha_zero': ['t_for1', 'w_yes_g', 'w_no_g', 'w_yes_gr', 'w_no_gr'],
            'selection': 'selected button: text white FFFFFFFF + w_yes_m/w_no_m (button2) alpha 255; the other: '
                         'text 969696FF + highlight alpha 0 (HD 0x02a05a70)',
            'cursor': 'dSelect_cursor_c (GC main2D) on the selected button: centre of w_yes_n/w_no_n, +- half its '
                      'scaled size (INFERRED geometry)',
            'fonts': 'every TBX2 here names rodan_24_22.bfn (absent in HD and GC) so only panes the code setFont()s '
                     'draw: t_for, t_for1, t_for2, f_yes_t, f_no_t, level_l, level_v; the hints name '
                     'rodan_b_24_22.bfn (GC fontres)'},
        'panes': {k: dict(sorted(v.items())) for k, v in PANES.items()},
        'messages': {str(mid): dict(res['messages'].get(mid, {'text': None}), use=use) for mid, use in MESSAGES},
        'gc_side': [{'archive': a, 'member': m, 'use': u} for a, m, u in GC_NEEDS],
    }


# ------------------------------------------------------------------------------------------ validate
def validate(rep, res):
    """Archive structure, layouts/animation byte-identical + loadable by v2.0.3's J2D, driven panes present,
    TEX1 refs resolve, BTI headers + texels vs HD, GC-side resources, name shadowing, heap proxy, composite
    preview (converted data vs HD source)."""
    g = 'cosconfirm'
    out = res['out_dir']
    buf = common.read(os.path.join(out, ARC_NAME))
    probs = rarcx.check(buf)
    a = rarc.Rarc(buf, ARC_NAME)
    names_ok = all(e['name'] == e['name'].lower() and e['hash'] == sb._jkr_hash(e['name']) for e in a.entries) and \
        a.nodes[0]['name'] == ARC_ROOT and all(n['name'] == n['name'].lower() for n in a.nodes)
    types = sorted(n['type'] for n in a.nodes)
    rep.add(g, 'archive structure', not probs and names_ok and types == ['ROOT', 'SCRN', 'TIMG'],
            '%d members, nodes %s, %s; names lowercase + JKR hashes %s' % (
                len(a.files()), types, '; '.join(probs[:3]) or 'RARC ok, data 32-byte aligned, uncompressed',
                'ok' if names_ok else 'WRONG'))
    want = sorted(['scrn/' + n for n, _ in LAYOUTS + ANIMS] + ['timg/' + i['member'][5:] for i in res['textures'].values()])
    have = sorted(m.path.split('/', 1)[1] for m in a.files())
    rep.add(g, 'members exactly as planned', have == want and len(res['manifest']['archive']['members']) == len(have),
            '%d = %d layouts + 1 bck + %d TEX1 textures + %d code textures%s' % (
                len(have), len(LAYOUTS), len(res['textures']) - len(CODE_TEXTURES), len(CODE_TEXTURES),
                '' if have == want else '; extra %s missing %s' % (sorted(set(have) - set(want))[:3],
                                                                    sorted(set(want) - set(have))[:3])))
    rep.add(g, 'size limit', len(buf) <= SIZE_LIMIT, '%d bytes = %.2f MB (%.2f MiB) <= %d MiB' % (
        len(buf), len(buf) / 1e6, len(buf) / 1048576.0, SIZE_LIMIT // 1048576))
    # layouts + animation: bytes, magic, structure v2.0.3 parses
    sv = sb.Source.get(res['hd'], *SAVERES)
    bad, blos, notes = [], {}, []
    for name, _ in LAYOUTS + ANIMS:
        m = a.get('scrn/' + name)
        if m is None:
            bad.append(name + ' missing')
            continue
        d = m.data()
        if d != sv.member('saveres/scrn/' + name).data():
            bad.append(name + ' differs from HD')
        if name.endswith('.blo'):
            p = _blo_problems(d)
            bad += ['%s: %s' % (name, x) for x in p]
            blos[name] = Blo(d)
    bck = None
    try:
        bck = Bck(a.get('scrn/' + BCK).data())
    except Exception as e:  # noqa: BLE001
        bad.append('%s: %r' % (BCK, e))
    if bck is not None:
        tb = bck.table_ok()
        if tb:
            bad.append('%s: %d key tables out of range' % (BCK, len(tb)))
        aids = [p['aid'] for p in blos[FS2].walk() if p['type'] != 'SCREEN' and p['aid'] != 0xFFFF]
        over = [x for x in aids if x >= bck.count]
        need = max(max(v['frames']) for v in FRAMES.values())
        if over:
            bad.append('%s: %d pane anim indices >= %d (J2DAnmTransformKey::calcTransform range over)' % (FS2, len(over), bck.count))
        if bck.frame_max < need:
            bad.append('%s frame_max %d < %d' % (BCK, bck.frame_max, need))
        notes.append('%s: ANK1 %d frames, %d anim entries >= %d pane indices of %s (max %d), all key tables in range' % (
            BCK, bck.frame_max, bck.count, len(aids), FS2, max(aids)))
    rep.add(g, 'layouts + bck byte-identical, J2D-loadable', not bad, '; '.join(bad[:4]) or (
        '%d SCRNblo2 + 1 J3D1bck1 = HD saveres bytes (sha1); blocks only %s; header size/block count, MAT1 stride '
        '0xE8, material / texture / font indices in range; %s' % (len(LAYOUTS), '/'.join(sorted({
            'INF1', 'TEX1', 'FNT1', 'MAT1', 'PAN2', 'PIC2', 'TBX2', 'BGN1', 'END1', 'EXT1'})), '; '.join(notes))))
    # panes the C++ drives
    miss = []
    for name, want_p in PANES.items():
        b = blos.get(name)
        for pn, typ in sorted(want_p.items()):
            p = b.find(pn) if b else None
            if p is None or p['type'] != typ:
                miss.append('%s:%s' % (name.split('_', 2)[-1][:12], pn))
    rep.add(g, 'driven panes present', not miss, '%d panes over %d layouts with the expected PAN2/PIC2/TBX2 types%s' % (
        sum(len(v) for v in PANES.values()), len(PANES), (': MISSING ' + ', '.join(miss[:6])) if miss else ''))
    # TEX1 / FNT1
    timg = {m.name: m for m in a.files() if m.path.lower().startswith(ARC_ROOT + '/timg/')}
    unresolved, kinds, fonts, nref = [], set(), set(), 0
    for name, b in blos.items():
        for kind, nm in b.tex:
            nref += 1
            kinds.add(kind)
            if kind not in (2, 3) or nm.lower() not in timg:
                unresolved.append('%s:%s(type %d)' % (name, nm, kind))
        for kind, nm in b.fnt:
            fonts.add('%s(type %d)' % (nm, kind))
    code_ok = all(an in timg for an, _, _ in CODE_TEXTURES)
    rep.add(g, 'TEX1 refs + code textures resolve', not unresolved and code_ok,
            '%d TEX1 refs in %d BLOs, resref types %s, all in timg/ (lowercase JKR lookup); code textures %s %s%s' % (
                nref, len(blos), sorted(kinds), ', '.join(an for an, _, _ in CODE_TEXTURES), 'present' if code_ok else 'MISSING',
                (': UNRESOLVED ' + ', '.join(unresolved[:4])) if unresolved else ''))
    rep.add(g, 'FNT1 refs (runtime fonts)', True, '%s not shipped: rodan_24_22 exists in neither HD nor GC, so only '
            'panes the C++ setFont()s draw (as in HD); rodan_b_24_22 is GC fontres' % ', '.join(sorted(fonts)))
    # BTIs
    bad, worst, gc_rgba = [], {}, {}
    for name, m in sorted(timg.items()):
        d = m.data()
        f = sb.bti_fields(d)
        stem = name[:-4]
        info = res['textures'].get(stem)
        p = sb._bti_problems(d, f)
        if info is None:
            p.append('not in manifest')
        if p:
            bad.append('%s: %s' % (name, ', '.join(p)))
            continue
        rgba = sb.decode(f['fmt'], d[0x20:0x20 + sb.tex_bytes(f['fmt'], f['w'], f['h'])], f['w'], f['h'])
        gc_rgba[stem] = (f['w'], f['h'], rgba)
        w0, h0, src = res['hd_rgba'][stem]
        vis, hid = sb.tex_error(rgba, src) if (w0, h0) == (f['w'], f['h']) else (999, 999)
        lim = sb.MAX_FLAT_ERR if sb.FMT_NAME[f['fmt']] == 'RGB5A3' else 0
        if vis > lim or hid > 4 * lim:
            bad.append('%s: decoded error %d/%d > %d/%d' % (name, vis, hid, lim, 4 * lim))
        k = sb.FMT_NAME[f['fmt']]
        wv, wh = worst.get(k, (0, 0))
        worst[k] = (max(wv, vis), max(wh, hid))
    res['gc_rgba'] = gc_rgba
    rep.add(g, 'BTI headers + pixels', not bad and len(timg) == len(res['textures']),
            '%d BTIs: img_off 0x20, size = format x dims, 1 image, no palette, <= %d px, wrap/filter valid%s' % (
                len(timg), sb.DIM_LIMIT, (': ' + '; '.join(bad[:4])) if bad else ''))
    alpha_ok = all(res['hd_rgba'][k][2][3::4] == gc_rgba[k][2][3::4] for k in gc_rgba)
    drawn = sorted(k for k, v in res['textures'].items() if v['drawn_in_mode6'])
    rep.add(g, 'texels vs HD source', not bad and alpha_ok,
            'decoded with Aurora semantics, max error visible/transparent texels by format: %s (0 = bit-exact); alpha '
            'bit-exact in all %d; the %d drawn in mode 6 (%s) all lossless: %s' % (
                ', '.join('%s %d/%d' % (k, v[0], v[1]) for k, v in sorted(worst.items())), len(gc_rgba), len(drawn),
                ', '.join(drawn), all(res['textures'][k]['lossless'] for k in drawn)))
    rep.add(g, 'mode-6 drawn set', drawn == EXPECTED_DRAWN,
            '%d textures visible over both variants, both selections and every yes/no (2089..2149) and plate '
            '(4000..4010) frame%s; %d only referenced (%d bytes)' % (
                len(drawn), '' if drawn == EXPECTED_DRAWN else ': expected %s' % EXPECTED_DRAWN,
                len(res['textures']) - len(drawn), res['manifest']['textures_not_drawn_in_mode6']['bytes']))
    diff = sorted('%s (%s)' % (k, ', '.join('%s %s' % (s.split('/')[-1], w) for s, w in v['also_in'].items()))
                  for k, v in res['textures'].items() if any(not w.startswith('identical') for w in v['also_in'].values()))
    rep.add(g, 'texture source choice', True, 'saveres first (its own archive, HD setPriority), else main2DR/clctresR/'
            'stampResR (identical wherever several have it); same name but different pixels elsewhere: %s' % (
                '; '.join(diff) or 'none'))
    # heart logic sanity (2 full + 1 half + 1 empty = life 10, max 20)
    hs = hearts(10, 20)
    shown = [k for k in sorted(hs) if k.startswith('hear_') and hs[k][0]]
    ok = shown == ['hear_20', 'hear_21', 'hear_22'] and hs['hear_22'][1] == 'cos_heart_02.bti' and \
        hs['hear_20'][1] == 'cos_heart_00.bti' and sum(1 for k in hs if k.startswith('heab_') and hs[k][0]) == 4
    rep.add(g, 'hearts mapping', ok, 'life 10 / max 20 -> containers heab_20..23, hearts hear_20 + hear_21 '
            'cos_heart_00, hear_22 cos_heart_02 (the HD video frame)')
    if res.get('gc'):
        _gc_rows(rep, res, timg)
    _heap_row(rep, res, blos)
    if res.get('preview_dir'):
        _preview_rows(rep, res, blos, bck)


def _blo_problems(d):
    """Structure checks mirroring what v2.0.3's J2DScreen/J2DMaterialFactory read."""
    p = []
    if d[:8] != b'SCRNblo2':
        return ['magic %r' % d[:8]]
    fsize, nblk = _u32(d, 8), _u32(d, 12)
    if fsize != len(d):
        p.append('header size %d != %d' % (fsize, len(d)))
    o, n, tags = 0x20, 0, []
    ntex = nfnt = nmat = 0
    while o + 8 <= len(d):
        t, sz = d[o:o + 4].decode('latin1'), _u32(d, o + 4)
        tags.append(t)
        n += 1
        if t not in J2D_BLOCKS:
            p.append('unknown block %s' % t)
        if t == 'TEX1':
            ntex = _u16(d, o + 8)
        elif t == 'FNT1':
            nfnt = _u16(d, o + 8)
        elif t == 'MAT1':
            nmat = _u16(d, o + 8)
            offs = struct.unpack_from('>23I', d, o + 0x0C)
            ninit = (offs[1] - offs[0]) // 0xE8
            if (offs[1] - offs[0]) % 0xE8 or any(_u16(d, o + offs[1] + 2 * i) >= ninit for i in range(nmat)):
                p.append('MAT1 init data stride/index')
        if t == 'EXT1' or sz == 0:
            break
        if sz % 4 or o + sz > len(d):
            p.append('block %s size %d' % (t, sz))
            break
        o += sz
    if n != nblk:
        p.append('block count %d != header %d' % (n, nblk))
    b = Blo(d)
    if len(b.tex) != ntex or len(b.fnt) != nfnt or len(b.mats) != nmat:
        p.append('resref counts')
    for q in b.walk():
        if q['type'] in ('PIC2', 'TBX2', 'WIN2') and q.get('material', 0xFFFF) != 0xFFFF and q['material'] >= nmat:
            p.append('pane %s material %d >= %d' % (q['name'], q['material'], nmat))
    return p


def _gc_rows(rep, res, timg):
    g = 'cosconfirm'
    gc = res['gc']
    miss, cache = [], {}

    def arc(p):
        if p not in cache:
            fp = os.path.join(gc, *p.split('/'))
            cache[p] = rarc.Rarc.from_file(fp) if os.path.exists(fp) else None
        return cache[p]
    for ap, member, _ in GC_NEEDS:
        ga = arc(ap)
        if ga is None or not any(m.name.lower() == member.lower() for m in ga.files()):
            miss.append('%s:%s' % (ap, member))
    cur = arc('res/Layout/main2D.arc')
    ctex = []
    if cur is not None and cur.get('zelda_select_cursor_4parts.blo') is not None:
        cb = sb.Blo(cur.get('zelda_select_cursor_4parts.blo').data())
        names = {m.name.lower() for m in cur.files()}
        ctex = [n for _, n in cb.tex]
        miss += ['main2D:%s' % n for n in ctex if n.lower() not in names]
    rep.add(g, 'GC-side resources', not miss, '%d needed by the C++ (cursor BLO/BPK/BTK + its %d textures in main2D, '
            'rodan_b_24_22 in fontres) present in the GC dump%s' % (len(GC_NEEDS), len(ctex),
                                                                    (': MISSING ' + ', '.join(miss)) if miss else ''))
    shadow = {}
    for ap in SHADOW_ARCS:
        ga = arc(ap)
        if ga is None:
            continue
        for m in ga.files():
            n = m.name.lower()
            if n in timg or n in [x for x, _ in LAYOUTS + ANIMS]:
                shadow.setdefault(n, []).append(ap.split('/')[-1][:-4])
    heart_clash = [n for n in shadow if n.startswith('cos_heart')]
    res['shadow'] = shadow
    rep.add(g, 'name shadowing', not heart_clash, 'names also in GC %s: %d (%s); harmless: this archive\'s screens '
            'resolve TEX1 in it first and GC screens pass their own archive (d_menu_save.cpp), it only matters for '
            'global name lookups while mounted; the hearts GC looks up by name (dFile_info_c tt_heart_00) are '
            'shipped as cos_heart_* so they never shadow' % (
                '/'.join(x.split('/')[-1][:-4] for x in SHADOW_ARCS), len(shadow),
                ', '.join(sorted(shadow)[:12]) + (' ...' if len(shadow) > 12 else '')))


def _heap_row(rep, res, blos):
    """INFERRED proxy: the J2D objects of this screen vs the GC save menu dMenu_save_c builds in the same place
    (zelda_file_select2 + 3 x zelda_file_select_info_text + warning window)."""
    g = 'cosconfirm'
    panes = sum(sum(b.counts.values()) for b in blos.values())
    mats = sum(len(b.mats) for b in blos.values())
    gcn = None
    if res.get('gc'):
        p = os.path.join(res['gc'], 'res', 'Layout', 'saveres.arc')
        if os.path.exists(p):
            ga = rarc.Rarc.from_file(p)
            parts = [('zelda_file_select2.blo', 1), ('zelda_file_select_info_text.blo', 3),
                     ('zelda_file_select_warning_window.blo', 1)]
            if all(ga.get(n) is not None for n, _ in parts):
                gp = gm = 0
                for n, k in parts:
                    b = sb.Blo(ga.get(n).data())
                    gp += k * sum(b.counts.values())
                    gm += k * len(b.mats)
                gcn = (gp, gm)
    vram = sum(i['w'] * i['h'] * 4 for i in res['textures'].values())
    vdrawn = sum(i['w'] * i['h'] * 4 for i in res['textures'].values() if i['drawn_in_mode6'])
    rep.add(g, 'heap headroom', gcn is None or panes + mats <= 1.25 * (gcn[0] + gcn[1]),
            'archive in a mod-owned process buffer (no game heap). J2D: %d panes + %d materials%s. VRAM after Aurora '
            'RGBA8 upload <= %.1f MB (%.1f MB for the textures mode 6 draws)' % (
                panes, mats, (' vs GC dMenu_save_c (fs2 + 3 info_text + warning) %d panes + %d materials' % gcn)
                if gcn else '', vram / 1e6, vdrawn / 1e6), warn=True)


# ------------------------------------------------------------------------------------------ preview
def _cursor(gc):
    """GC main2D zelda_select_cursor_4parts.blo + its decoded textures (what v2.0.3's dSelect_cursor_c draws)."""
    p = os.path.join(gc, 'res', 'Layout', 'main2D.arc') if gc else None
    if not p or not os.path.exists(p):
        return None, {}
    ga = rarc.Rarc.from_file(p)
    m = ga.get('zelda_select_cursor_4parts.blo')
    if m is None:
        return None, {}
    cb = Blo(m.data())
    tex = {}
    for _, n in cb.tex:
        tm = ga.get(n)
        if tm is None:
            continue
        d = tm.data()
        f = sb.bti_fields(d)
        if f['fmt'] in sb.TILE and f['fmt'] != sb.CMPR:
            tex[n.lower()[:-4]] = (f['w'], f['h'], sb.decode(f['fmt'], d[f['img_off']:f['img_off'] + sb.tex_bytes(
                f['fmt'], f['w'], f['h'])], f['w'], f['h']))
    return cb, tex


def _place_cursor(cb, fs, yes):
    tgt = fs.find('w_yes_n' if yes else 'w_no_n')
    (a, b, c), (d, e, f) = tgt['m']
    ax, ay = tgt['anchor']
    pts = [(a * x + b * y + c, d * x + e * y + f) for x, y in
           ((-ax, -ay), (tgt['w'] - ax, -ay), (-ax, tgt['h'] - ay), (tgt['w'] - ax, tgt['h'] - ay))]
    cx, cy = sum(q[0] for q in pts) / 4.0, sum(q[1] for q in pts) / 4.0
    hw, hh = tgt['w'] * tgt['sx'] * 0.5, tgt['h'] * tgt['sy'] * 0.5
    cb.reset()
    n = cb.find('n_all')
    n['x'], n['y'] = cx, cy
    for nm, sx, sy in (('l_u_null', -1, -1), ('r_u_null', 1, -1), ('l_d_null', -1, 1), ('r_d_null', 1, 1)):
        q = cb.find(nm)
        q['x'], q['y'] = sx * hw, sy * hh
    cb.place()


def render_state(blos, bck, tex, fonts, state, text, cursor=None):
    import numpy as np  # noqa: F401
    life, mx, level, yes, yn, am = STATES[state]
    r = sb._Renderer(tex, fonts)
    for blo, cfg in configure(blos, bck, life, mx, level, yes, yn, am, text):
        for kind, p, mat, a, s in draw_list(blo, cfg):
            if kind == 'pic':
                r.picture(p, mat, a)
            else:
                r.text(p, mat, s, 'mesg')
    if cursor is not None and cursor[0] is not None and life > 0:
        cb, ctex = cursor
        for k, v in ctex.items():
            r.tex[k] = np.frombuffer(v[2], np.uint8).reshape(v[1], v[0], 4).astype(np.float32) / 255.0
        _place_cursor(cb, blos[FS2], yes)
        hide = dict((n % c, False) for c in ('ld', 'lu', 'rd', 'ru') for n in ('l_%s', 'i_c_%s1', 'i_c_%s2'))
        for kind, p, mat, a, s in draw_list(cb, {'vis': hide}):     # glow + indirect-warped layers: not modelled
            if kind == 'pic':
                r.picture(p, mat, a)
    return r


def _preview_rows(rep, res, blos, bck):
    g = 'cosconfirm'
    try:
        import numpy as np
        from PIL import Image  # noqa: F401
    except ImportError:
        rep.add(g, 'composite preview', False, 'skipped: numpy/Pillow not installed', warn=True)
        return
    od = res['preview_dir']
    os.makedirs(od, exist_ok=True)
    fonts = sb._gc_fonts(res.get('gc'))
    cursor = _cursor(res.get('gc'))
    text = {k: v['text'] for k, v in res['messages'].items()}
    rows, missing = [], set()
    for state in STATES:
        imgs = {}
        for which in ('gc', 'hd'):
            tex = res['gc_rgba'] if which == 'gc' else res['hd_rgba']
            r = render_state(blos, bck, tex, fonts, state, text, cursor)
            imgs[which] = r.image()
            missing |= r.missing
            r.save(os.path.join(od, 'confirm_%s_%s.png' % (state, which)))
        d = np.abs(imgs['gc'].astype(np.int32) - imgs['hd'].astype(np.int32))[:, :, :3]
        mse = float((d.astype(np.float64) ** 2).mean())
        psnr = 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)
        rows.append((state, psnr, int(d.max()), float((d > 2).mean() * 100)))
        res.setdefault('preview', {})[state] = {'psnr': psnr, 'max': int(d.max())}
    ok = all(p >= 45 and m <= 8 for _, p, m, _ in rows) and not missing
    rep.add(g, 'composite GC vs HD', ok, '%s (confirm_<state>_gc.png vs _hd.png in %s, 1216x896; GC fonts + GC cursor)%s%s' % (
        '; '.join('%s PSNR %.1f dB, max %d, %.2f%% px > 2' % x for x in rows), od,
        '' if fonts else '; no GC fonts: text skipped', ('; MISSING textures ' + ', '.join(sorted(missing))) if missing else ''))


# ------------------------------------------------------------------------------------------ CLI
def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hd', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--gc')
    ap.add_argument('--preview')
    a = ap.parse_args(argv)
    import validate as _validate
    log = common.Log()
    gc = a.gc
    if gc and os.path.isdir(os.path.join(gc, 'files')):
        gc = os.path.join(gc, 'files')
    res = build(os.path.abspath(a.hd), os.path.abspath(a.out), log, gc=gc, preview_dir=a.preview)
    rep = _validate.Report()
    validate(rep, res)
    print(rep.table())
    return 1 if rep.failed() else 0


if __name__ == '__main__':
    sys.exit(main())
