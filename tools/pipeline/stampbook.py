"""stampbook.py - HD Stamp Collection screen (dMenu_Stamp_c) assets for the stamps mod.

Builds, from the user's own TP HD dump (read only), the GC-loadable data the native J2D stamp book, the
Collection-screen stamp slot and the file-select stamp plate need (research/stampbook_spec.md section 14,
with the corrections of research/stampbook/check.md: archive size, texture formats, item-window note).

OUTPUTS (folder res/stamps/stampbook/ of the HD Features mod; loaded by the C++ through ResourceService, never through
the DVD/overlay layer - see "Delivery" below)
  stampResR_gc.arc       RARC, root node 'stampresr', retail GC layout (tools/formats/rarc_write.py),
                         uncompressed, every member 32-byte aligned, every name lowercase:
                           scrn/zelda_stamps_{background,menu,buttons,message}.blo   byte-identical to HD
                           timg/<72 HD textures>.bti   full GC BTIs (header 0x20 + pixels at 0x20) converted
                                                       from the GX2 GTX surfaces in stampResR.pack.gz
                           timg/tr_a_btn_00.bti, timg/tr_b_btn_00.bti   from HD LayoutRevo/main2DR (the
                                                       buttons BLO names them; GC main2D has no tr_* glyphs)
                         Dropped: the 51 timg/*.tga (Miiverse post images; only HD's Post path reads them).
  stampicon.bti          Collection slot icon 'stamp' (HD LayoutRevo/clctresR.arc timg/stampicon.bti,
                         128x128 IA8; differs slightly from stampResR's own stampicon, which no BLO uses)
  stampbutton.bti        file-select plate 'stamps_i'   (HD res/Object/fileSel.arc timg/stampbutton.bti)
  stampbuticon.bti       file-select "+" glyph 'stamps_p' (fileSel.arc timg/stampbuticon.bti)
  filesel_stampicon.bti  file-select plate icon 'stamps_1' (fileSel.arc timg/stampicon.bti, 51x51 IA8;
                         renamed because the Collection icon owns the name stampicon.bti)
  book.json              manifest: sources + hashes, per-texture format decision and error, member list,
                         the names the C++ uses, size/heap figures

TEXTURE FORMATS (decided per texture from its pixels; every decision and its max error is in book.json)
  Aurora (extern/aurora/lib/gfx/texture_convert.cpp) decodes every GX format to RGBA8 on upload with bit
  replication (ExpandTo8), and I4/I8 put the intensity in alpha too. The encoders below round to the
  nearest value under exactly those expansions, and the decoders used for validation mirror them.
    greyscale (R==G==B everywhere)  I4/I8 when alpha == intensity, IA4 when 4-bit exact, else IA8: lossless
    flat ink (one visible colour, alpha only 0/255: all 50 stamps)  RGB5A3: alpha exact, the single colour
                                    within 3/255; no gradient exists, so no banding is possible. HD's own GC
                                    headers already say RGB5A3 for 47 of the 50 stamps.
    RGB5A3 / RGB565 exact           used when the round trip is bit-exact (none in this archive)
    anything else                   RGBA8 (lossless). Measured alternatives for the continuous-tone art
                                    (back1, paper, popup, frames, buttons): RGB565 max error 4/255 (PSNR ~42 dB,
                                    contouring in the dark gradients), RGB5A3 3-bit alpha on 11-88 % of the
                                    pixels, CMPR (best-effort BC1) max error 23-51/255 and 1-bit alpha: none of
                                    them is visually lossless, so they stay RGBA8.
  Headers keep HD's wrap/filter/LOD bytes; palettes are removed (numColors 0), mip count 1 (HD has 1).

DELIVERY (VERIFIED in the v2.0.3 source)
  The C++ loads 'stamps/stampbook/stampResR_gc.arc' with svc_resource->load (std::malloc copy, 16-byte aligned), copies
  it into a 32-byte aligned buffer it keeps for the mod's lifetime and mounts it with
  JKR_NEW JKRMemArchive(buf, size, JKRMEMBREAK_FLAG_UNKNOWN0) only between book create() and destroy().
  JKRMemArchive::open reads the archive in place (JKRMemArchive.cpp) and JUTTexture::storeTIMG points at the
  texels in place (JUTTexture.cpp), so no game heap holds the 11-12 MB; the only JKR allocation the mount
  makes is initFileDataPointers' 8 bytes per entry from the system heap. An overlay/DVD mount
  (mDoDvdThd_mountArchive_c) would copy it into a game heap instead, which is why the overlay path is not used.
  Loose BTIs are loaded the same way (32-byte aligned copy) and used as ResTIMG*.

Library: build(hd, out_dir, log, gc=None, preview_dir=None, text=None) -> result dict;
         validate(rep, res) adds rows to a tools/pipeline/validate.Report.
CLI:     python -I stampbook.py --hd <HD content> --out <dir> [--gc <GC files dir>] [--preview <dir>]
"""
import hashlib
import json
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
import hdpack  # noqa: E402
import gtx2img  # noqa: E402

ARC_NAME = 'stampResR_gc.arc'
ARC_ROOT = 'stampresr'
BLOS = ['zelda_stamps_background.blo', 'zelda_stamps_menu.blo', 'zelda_stamps_buttons.blo',
        'zelda_stamps_message.blo']
# (out file, HD folder, archive, member, what it is)
LOOSE = [('stampicon.bti', 'res/LayoutRevo', 'clctresR.arc', 'clctresr/timg/stampicon.bti',
          'Collection slot icon (pane stamp of stam_7_n)'),
         ('stampbutton.bti', 'res/Object', 'fileSel.arc', 'filesel/timg/stampbutton.bti',
          'file-select plate (pane stamps_i)'),
         ('stampbuticon.bti', 'res/Object', 'fileSel.arc', 'filesel/timg/stampbuticon.bti',
          'file-select "+" glyph (pane stamps_p)'),
         ('filesel_stampicon.bti', 'res/Object', 'fileSel.arc', 'filesel/timg/stampicon.bti',
          'file-select plate icon (pane stamps_1)')]
EXTRA_GLYPHS = [('res/LayoutRevo', 'main2DR.arc', 'main2dr/timg/tr_a_btn_00.bti'),
                ('res/LayoutRevo', 'main2DR.arc', 'main2dr/timg/tr_b_btn_00.bti')]
BUTTONS_TWIN = ('res/LayoutRevo', 'clctresR.arc', 'clctresr/scrn/zelda_collect_soubi_do_icon_parts_revo.blo')
# GC-side resources the C++ takes from Dusklight's own GC data (spec 5, 6.1; not shipped)
GC_NEEDS = [('res/Layout/main2D.arc', 'zelda_select_cursor_4parts.blo', 'book select cursor (dSelect_cursor_c)')] + \
    [('res/Layout/main2D.arc', 'im_font_number_32_32_ganshinkyo_%s.bti' % s, 'Collection slot digit %d' % i)
     for i, s in enumerate(['0_02', '1_02', '2_02', '3_02', '4_03', '5_02', '6_02', '7_02', '8_02', '9_02'])] + \
    [('res/Layout/clctres.arc', 'tt_block8x8.bti', 'black full-screen quad (any mounted copy)'),
     ('res/Layout/clctres.arc', 'tt_spot_square3.bti', 'gray_21 empty-slot frame (Collection screen)')]
SIZE_LIMIT = 16 * 1024 * 1024          # spec check_book (f); generous for a process-heap buffer
DIM_LIMIT = 1024                       # GX texture size limit
MAX_FLAT_ERR = 3                       # flat-ink RGB5A3: max per-channel error accepted (out of 255)

I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR = 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14
FMT_NAME = {I4: 'I4', I8: 'I8', IA4: 'IA4', IA8: 'IA8', RGB565: 'RGB565', RGB5A3: 'RGB5A3', RGBA8: 'RGBA8',
            C4: 'C4', C8: 'C8', C14X2: 'C14X2', CMPR: 'CMPR'}
TILE = {I4: (8, 8), I8: (8, 4), IA4: (8, 4), IA8: (4, 4), RGB565: (4, 4), RGB5A3: (4, 4), RGBA8: (4, 4),
        CMPR: (8, 8)}
BPP = {I4: 4, I8: 8, IA4: 8, IA8: 16, RGB565: 16, RGB5A3: 16, RGBA8: 32, CMPR: 4}

# Aurora ExpandTo8<n> (bit replication) and nearest-value lookup tables
E3 = [(q << 5) | (q << 2) | (q >> 1) for q in range(8)]
E4 = [(q << 4) | q for q in range(16)]
E5 = [(q << 3) | (q >> 2) for q in range(32)]
E6 = [(q << 2) | (q >> 4) for q in range(64)]


def _nearest(table):
    return bytes(min(range(len(table)), key=lambda q: (abs(table[q] - v), q)) for v in range(256))


N3, N4, N5, N6 = _nearest(E3), _nearest(E4), _nearest(E5), _nearest(E6)


def tex_bytes(fmt, w, h):
    bw, bh = TILE[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * BPP[fmt] // 8


# ------------------------------------------------------------------------------------------ tiling
def _tile(units, n, w, h, bw, bh):
    """Row-major array of n-byte texel units -> GX tile order (tiles row-major, texels row-major inside).
    Texels outside the image are zero."""
    tw, th = (w + bw - 1) // bw, (h + bh - 1) // bh
    pw = tw * bw
    pad = b'\0' * ((pw - w) * n)
    rows = [bytes(units[y * w * n:(y + 1) * w * n]) + pad for y in range(h)]
    rows += [b'\0' * (pw * n)] * (th * bh - h)
    out = bytearray()
    step = bw * n
    for ty in range(th):
        blk = rows[ty * bh:(ty + 1) * bh]
        for tx in range(tw):
            s = tx * step
            for r in blk:
                out += r[s:s + step]
    return bytes(out)


def _untile(data, n, w, h, bw, bh):
    tw, th = (w + bw - 1) // bw, (h + bh - 1) // bh
    pw = tw * bw
    rows = [bytearray(pw * n) for _ in range(th * bh)]
    step = bw * n
    o = 0
    for ty in range(th):
        for tx in range(tw):
            s = tx * step
            for k in range(bh):
                rows[ty * bh + k][s:s + step] = data[o:o + step]
                o += step
    return b''.join(bytes(r[:w * n]) for r in rows[:h])


# ------------------------------------------------------------------------------------------ encode
def encode(fmt, rgba, w, h):
    """RGBA8 (row-major) -> GX texel data, rounding to the nearest value Aurora expands back."""
    n = w * h
    r, g, b, a = rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4]
    if fmt == RGBA8:
        ar = bytearray(2 * n)
        gb = bytearray(2 * n)
        ar[0::2], ar[1::2], gb[0::2], gb[1::2] = a, r, g, b
        t_ar = _tile(ar, 2, w, h, 4, 4)
        t_gb = _tile(gb, 2, w, h, 4, 4)
        return b''.join(t_ar[i:i + 32] + t_gb[i:i + 32] for i in range(0, len(t_ar), 32))
    if fmt == IA8:
        ia = bytearray(2 * n)
        ia[0::2], ia[1::2] = a, r
        return _tile(ia, 2, w, h, 4, 4)
    if fmt == IA4:
        return _tile(bytes((N4[x] << 4) | N4[y] for x, y in zip(a, r)), 1, w, h, 8, 4)
    if fmt == I8:
        return _tile(bytes(r), 1, w, h, 8, 4)
    if fmt == I4:
        t = _tile(bytes(N4[x] for x in r), 1, w, h, 8, 8)
        return bytes((t[i] << 4) | t[i + 1] for i in range(0, len(t), 2))
    if fmt == RGB565:
        u = bytearray(2 * n)
        for i, (x, y, z) in enumerate(zip(r, g, b)):
            struct.pack_into('>H', u, 2 * i, (N5[x] << 11) | (N6[y] << 5) | N5[z])
        return _tile(u, 2, w, h, 4, 4)
    if fmt == RGB5A3:
        u = bytearray(2 * n)
        for i, (x, y, z, al) in enumerate(zip(r, g, b, a)):
            q = N3[al]
            if q == 7 and abs(255 - al) <= abs(E3[q] - al):
                v = 0x8000 | (N5[x] << 10) | (N5[y] << 5) | N5[z]
            else:
                v = (q << 12) | (N4[x] << 8) | (N4[y] << 4) | N4[z]
            struct.pack_into('>H', u, 2 * i, v)
        return _tile(u, 2, w, h, 4, 4)
    raise NotImplementedError('encode GX format %s' % fmt)


def decode(fmt, data, w, h):
    """GX texel data -> RGBA8 exactly as Aurora's TextureDecoder* produce it (I4/I8 alpha = intensity)."""
    data = bytes(data)
    out = bytearray(w * h * 4)
    if fmt == RGBA8:
        tiles = [data[i:i + 64] for i in range(0, len(data), 64)]
        ar = _untile(b''.join(t[:32] for t in tiles), 2, w, h, 4, 4)
        gb = _untile(b''.join(t[32:] for t in tiles), 2, w, h, 4, 4)
        out[0::4], out[1::4], out[2::4], out[3::4] = ar[1::2], gb[0::2], gb[1::2], ar[0::2]
        return bytes(out)
    if fmt == IA8:
        ia = _untile(data, 2, w, h, 4, 4)
        i = ia[1::2]
        out[0::4], out[1::4], out[2::4], out[3::4] = i, i, i, ia[0::2]
        return bytes(out)
    if fmt == IA4:
        v = _untile(data, 1, w, h, 8, 4)
        i = bytes(E4[x & 15] for x in v)
        out[0::4], out[1::4], out[2::4], out[3::4] = i, i, i, bytes(E4[x >> 4] for x in v)
        return bytes(out)
    if fmt in (I8, I4):
        if fmt == I4:
            nib = bytes(E4[(x >> s) & 15] for x in data for s in (4, 0))
            v = _untile(nib, 1, w, h, 8, 8)
        else:
            v = _untile(data, 1, w, h, 8, 4)
        out[0::4], out[1::4], out[2::4], out[3::4] = v, v, v, v
        return bytes(out)
    if fmt in (RGB565, RGB5A3):
        u = _untile(data, 2, w, h, 4, 4)
        for i in range(w * h):
            t = (u[2 * i] << 8) | u[2 * i + 1]
            if fmt == RGB565:
                px = (E5[t >> 11], E6[(t >> 5) & 63], E5[t & 31], 255)
            elif t & 0x8000:
                px = (E5[(t >> 10) & 31], E5[(t >> 5) & 31], E5[t & 31], 255)
            else:
                px = (E4[(t >> 8) & 15], E4[(t >> 4) & 15], E4[t & 15], E3[(t >> 12) & 7])
            out[4 * i:4 * i + 4] = bytes(px)
        return bytes(out)
    raise NotImplementedError('decode GX format %s' % fmt)


def max_err(a, b):
    return max((abs(x - y) for x, y in zip(a, b)), default=0)


def tex_error(back, src):
    """-> (max error over the texels HD shows (alpha > 0, all 4 channels), max colour error over fully
    transparent texels). The latter only reaches the screen through bilinear filtering at ink edges, scaled
    by w_t * w_v <= 1/4 (straight-alpha blend of a filtered texel)."""
    vis = hid = 0
    for i in range(0, len(src), 4):
        e = max(abs(back[i + k] - src[i + k]) for k in range(4))
        if src[i + 3]:
            vis = max(vis, e)
        else:
            hid = max(hid, e)
    return vis, hid


# ------------------------------------------------------------------------------------------ policy
def choose_format(rgba, hd_fmt):
    """-> (GX format, reason). Lossless unless the texture is flat ink (see module docstring)."""
    r, g, b, a = rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4]
    if r == g == b:
        vals = set(r) | set(a)
        if a == r:
            return (I4, 'greyscale, alpha = intensity, 4-bit exact') if all(v % 17 == 0 for v in vals) else \
                (I8, 'greyscale, alpha = intensity')
        if all(v % 17 == 0 for v in vals):
            return IA4, 'greyscale, 4-bit exact'
        return IA8, 'greyscale: lossless'
    alphas = set(a)
    vis = {(x, y, z) for x, y, z, al in zip(r, g, b, a) if al}
    if alphas <= {0, 255} and len(vis) == 1:
        return RGB5A3, 'flat ink %s, alpha 0/255: one colour, no gradient (HD header %s)' % (
            '#%02X%02X%02X' % next(iter(vis)), FMT_NAME.get(hd_fmt, hd_fmt))
    e5 = set(E5)
    if alphas == {255} and set(r) <= e5 and set(b) <= e5 and set(g) <= set(E6):
        return RGB565, 'opaque and 565-exact'
    return RGBA8, 'continuous tone / soft alpha: lossless'


# ------------------------------------------------------------------------------------------ sources
class Source(object):
    """One HD archive + its TMPK pack (both read only)."""
    _cache = {}

    def __init__(self, hd, folder, arc_name):
        d = os.path.join(hd, *folder.split('/'))
        self.arc_path = common.exists_ci(d, arc_name)
        if self.arc_path is None:
            raise IOError('HD %s/%s missing' % (folder, arc_name))
        self.pack_path = common.exists_ci(d, arc_name[:-4] + '.pack.gz')
        self.rel = '%s/%s' % (folder, os.path.basename(self.arc_path))
        raw = common.read(self.arc_path)
        self.arc_sha1 = hashlib.sha1(raw).hexdigest()
        self.arc = rarc.Rarc(raw, self.rel)
        self.pack = hdpack.Tmpk(hdpack.read_any(self.pack_path)) if self.pack_path else None
        self.pack_sha1 = hashlib.sha1(common.read(self.pack_path)).hexdigest() if self.pack_path else None
        self._gtx = {}
        if self.pack:
            for e in self.pack.entries:
                self._gtx[e['name'].lower()] = e

    @classmethod
    def get(cls, hd, folder, arc_name):
        k = (hd, folder, arc_name.lower())
        if k not in cls._cache:
            cls._cache[k] = Source(hd, folder, arc_name)
        return cls._cache[k]

    def member(self, path):
        m = self.arc.get(path)
        if m is None or m.path.lower() != path.lower():
            raise KeyError('%s: no member %s' % (self.rel, path))
        return m

    def gtx(self, member_path):
        suffix = '/' + member_path.lower() + '.gtx'
        for k, e in self._gtx.items():
            if k.endswith(suffix):
                return e['name'], self.pack.data(e)
        return None, None

    def texture(self, member_path):
        """-> dict(stub, hdr, w, h, rgba, gtx_name, gtx_fmt) from the 32-byte HD stub + its GTX surface."""
        stub = self.member(member_path).data()
        hdr = bti_fields(stub)
        gname, g = self.gtx(member_path)
        if g is None:
            raise KeyError('%s: no GTX for %s in %s' % (self.rel, member_path, os.path.basename(self.pack_path or '?')))
        surfs = hdpack.gtx_surfaces(g)['surfaces']
        s = surfs[0]
        w, h, lin = gtx2img.deswizzle_level(s, g, 0)
        s2 = dict(s)
        s2['w'], s2['h'] = w, h
        return {'stub': stub[:0x20], 'hdr': hdr, 'w': w, 'h': h, 'rgba': gtx2img.to_rgba(s2, lin),
                'gtx_name': gname, 'gtx_fmt': s['fmt_name'], 'gtx_surfaces': len(surfs), 'gtx_mips': s['mips']}


def bti_fields(d, o=0):
    (fmt, alpha, w, h, ws, wt, pal_en, pal_fmt, pal_n, pal_off, mip_en, edge, bias, aniso, minf, magf, minlod,
     maxlod, nimg, unk, lodbias, img_off) = struct.unpack_from('>BBHHBBBBHIBBBBBBbbBBhI', d, o)
    return {'fmt': fmt, 'alpha': alpha, 'w': w, 'h': h, 'wrap': (ws, wt), 'pal_en': pal_en, 'pal_n': pal_n,
            'pal_off': pal_off, 'mip_en': mip_en, 'minf': minf, 'magf': magf, 'minlod': minlod, 'maxlod': maxlod,
            'mips': nimg, 'img_off': img_off}


def make_bti(t):
    """HD stub + RGBA -> (full GC BTI bytes, info dict)."""
    hdr, rgba, w, h = t['hdr'], t['rgba'], t['w'], t['h']
    fmt, why = choose_format(rgba, hdr['fmt'])
    pix = encode(fmt, rgba, w, h)
    back = decode(fmt, pix, w, h)
    vis, hid = tex_error(back, rgba)
    if fmt == RGB5A3 and (vis > MAX_FLAT_ERR or hid > 4 * MAX_FLAT_ERR or back[3::4] != rgba[3::4]):
        why = 'flat ink but RGB5A3 error %d/%d too high: lossless' % (vis, hid)
        fmt = RGBA8
        pix = encode(fmt, rgba, w, h)
        back = decode(fmt, pix, w, h)
        vis, hid = tex_error(back, rgba)
    if len(pix) != tex_bytes(fmt, w, h):
        raise AssertionError('encoder size %d != %d' % (len(pix), tex_bytes(fmt, w, h)))
    err = max(vis, hid)
    a = rgba[3::4]
    if hdr['fmt'] in (C4, C8, C14X2):           # HD palette header: derive the alpha mode from the pixels
        al = set(a)
        alpha = 0 if al == {255} else (1 if al <= {0, 255} else 2)
    else:
        alpha = hdr['alpha']
    b = bytearray(t['stub'])
    b[0], b[1] = fmt, alpha
    struct.pack_into('>HH', b, 2, w, h)
    b[8] = b[9] = 0                               # no palette
    struct.pack_into('>HI', b, 0xA, 0, 0)
    b[0x10] = 0                                   # one image, mipmapping off
    if b[0x14] > 1:
        b[0x14] = 1
    b[0x17] = 0
    b[0x18] = 1
    struct.pack_into('>I', b, 0x1C, 0x20)
    info = {'fmt': FMT_NAME[fmt], 'w': w, 'h': h, 'bytes': 0x20 + len(pix), 'hd_header_fmt': FMT_NAME.get(hdr['fmt'], hdr['fmt']),
            'hd_header_size': [hdr['w'], hdr['h']], 'gtx': t['gtx_fmt'], 'reason': why,
            'max_err_visible': vis, 'max_err_transparent': hid, 'lossless': err == 0, 'wrap': list(hdr['wrap']), 'filter': [hdr['minf'], hdr['magf']]}
    return bytes(b) + pix, info, back


# ------------------------------------------------------------------------------------------ BLO
def _u16(d, o):
    return struct.unpack_from('>H', d, o)[0]


def _u32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def _resrefs(d, o):
    lo = o + _u32(d, o + 12)
    out = []
    for i in range(_u16(d, lo)):
        eo = lo + _u16(d, lo + 2 + 2 * i)
        out.append((d[eo], d[eo + 2:eo + 2 + d[eo + 1]].decode('latin1')))
    return out


def _tag8(b):
    return b.strip(b'\0').decode('latin1')


def _hexc(v):
    return [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]


class Blo(object):
    """Minimal SCRNblo2 reader (format: Dusklight libs/JSystem/src/J2DGraph): TEX1/FNT1 references, the
    material fields the preview renderer needs, and the pane tree with PIC2/TBX2 parameters."""

    def __init__(self, d):
        self.d = d
        self.magic = d[:8]
        self.tex, self.fnt, self.mats = [], [], []
        self.counts = {}
        self.root = {'type': 'SCREEN', 'name': '<screen>', 'children': [], 'x': 0.0, 'y': 0.0, 'w': 0.0, 'h': 0.0,
                     'sx': 1.0, 'sy': 1.0, 'rz': 0.0, 'anchor': (0.0, 0.0), 'visible': True}
        o = 0x20
        self.blocks = []
        stack, last = [self.root], self.root
        while o + 8 <= len(d):
            t = d[o:o + 4].decode('latin1')
            sz = _u32(d, o + 4)
            self.blocks.append(t)
            if t == 'INF1':
                self.root['w'], self.root['h'] = _u16(d, o + 8), _u16(d, o + 10)
            elif t == 'TEX1':
                self.tex = _resrefs(d, o)
            elif t == 'FNT1':
                self.fnt = _resrefs(d, o)
            elif t == 'MAT1':
                self._mat1(o)
            elif t == 'BGN1':
                stack.append(last)
            elif t == 'END1':
                stack.pop()
            elif t in ('PAN2', 'PIC2', 'TBX2', 'WIN2'):
                p = self._pane(o, t)
                stack[-1]['children'].append(p)
                last = p
                self.counts[t] = self.counts.get(t, 0) + 1
            elif t == 'EXT1' or sz == 0:
                break
            o += sz
        _place(self.root, [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]])

    def _mat1(self, o):
        d = self.d
        n = _u16(d, o + 8)
        offs = struct.unpack_from('>23I', d, o + 0x0C)
        o_init, o_id, o_matcol, o_texno, o_tsn, o_tevcol = offs[0], offs[1], offs[5], offs[11], offs[16], offs[14]
        for i in range(n):
            ib = o + o_init + _u16(d, o + o_id + 2 * i) * 0xE8
            h = lambda k: _u16(d, ib + k)  # noqa: E731
            texs = [self.tex[_u16(d, o + o_texno + 2 * h(0x38 + 2 * k))][1] if h(0x38 + 2 * k) != 0xFFFF else None
                    for k in range(8)]
            tc = []
            for k in range(4):
                x = h(0x92 + 2 * k)
                tc.append(list(struct.unpack_from('>4h', d, o + o_tevcol + 8 * x)) if x != 0xFFFF else None)
            mc = h(8)
            self.mats.append({'textures': [t for t in texs if t], 'tevstages': d[o + o_tsn + d[ib + 4]] if d[ib + 4] != 0xFF else 0,
                              'tevcolor': tc, 'matcolor': _hexc(_u32(d, o + o_matcol + 4 * mc)) if mc != 0xFFFF else None})

    def _pane(self, o, t):
        d = self.d
        q = o if t == 'PAN2' else o + 8
        size = _u32(d, q + 4)
        vis, bp = d[q + 0xC], d[q + 0xD]
        w, h, sx, sy, rx, ry, rz, tx, ty = struct.unpack_from('>9f', d, q + 0x20)
        p = {'type': t, 'name': _tag8(d[q + 0x10:q + 0x18]), 'user': _tag8(d[q + 0x18:q + 0x20]),
             'visible': bool(vis), 'w': w, 'h': h, 'sx': sx, 'sy': sy, 'rz': rz, 'x': tx, 'y': ty,
             'anchor': ((0.0, w / 2, w)[bp % 3], (0.0, h / 2, h)[bp // 3]) if bp < 9 else (0.0, 0.0),
             'children': []}
        e = q + size
        if t == 'PIC2':
            p['material'] = _u16(d, e + 4)
            p['uv'] = [list(struct.unpack_from('>hh', d, e + 0x10 + 4 * k)) for k in range(4)]
            p['corners'] = [_hexc(_u32(d, e + 0x20 + 4 * k)) for k in range(4)]
        elif t == 'TBX2':
            (_, _, mat, cs, ls, fw, fh) = struct.unpack_from('>HHHhhHH', d, e)
            slen = _u16(d, e + 0x1E)
            p.update({'material': mat, 'char_space': cs, 'line_space': ls, 'font_w': fw, 'font_h': fh,
                      'hbind': d[e + 0xE], 'vbind': d[e + 0xF], 'char_color': _hexc(_u32(d, e + 0x10)),
                      'grad_color': _hexc(_u32(d, e + 0x14)),
                      'text': d[e + 0x20:e + 0x20 + slen].split(b'\0')[0].decode('latin1')})
        elif t == 'WIN2':
            p['material'] = _u16(d, e + 0x24)
        return p

    def walk(self, p=None):
        p = p or self.root
        yield p
        for c in p['children']:
            for x in self.walk(c):
                yield x

    def find(self, name):
        for p in self.walk():
            if p['name'] == name:
                return p
        return None


def _place(p, parent):
    """Absolute 2x3 matrix per pane (J2DPane::makeMatrix: parent * T(x,y) * R(-rz) * S(sx,sy))."""
    if p['type'] == 'SCREEN':
        m = parent
    else:
        th = math.radians(-p['rz'])
        c, s = math.cos(th), math.sin(th)
        loc = [[c * p['sx'], -s * p['sy'], p['x']], [s * p['sx'], c * p['sy'], p['y']]]
        m = [[parent[i][0] * loc[0][j] + parent[i][1] * loc[1][j] + (parent[i][2] if j == 2 else 0.0)
              for j in range(3)] for i in range(2)]
    p['m'] = m
    for ch in p['children']:
        _place(ch, m)


# ------------------------------------------------------------------------------------------ build
def build(hd, out_dir, log, gc=None, preview_dir=None, text=None):
    """Write the outputs listed in the module docstring into out_dir (created; existing files overwritten)."""
    os.makedirs(out_dir, exist_ok=True)
    src = Source.get(hd, 'res/LayoutRevo', 'stampResR.arc')
    res = {'out_dir': out_dir, 'textures': {}, 'blos': {}, 'dropped': [], 'loose': {}, 'gc': gc,
           'preview_dir': preview_dir, 'hd_rgba': {}, 'gc_rgba': {}, 'sources': {}}
    res['sources'][src.rel] = {'sha1': src.arc_sha1, 'pack': os.path.basename(src.pack_path), 'pack_sha1': src.pack_sha1}
    entries = []
    for m in src.arc.files():
        rel = m.path.split('/', 1)[1]
        low = rel.lower()
        if low.endswith('.tga'):
            res['dropped'].append(rel)
            continue
        if low.endswith('.blo'):
            d = m.data()
            res['blos'][low.split('/')[-1]] = {'sha1': hashlib.sha1(d).hexdigest(), 'size': len(d)}
            entries.append((low, d))
        elif low.endswith('.bti'):
            t = src.texture(m.path)
            data, info, back = make_bti(t)
            info.update({'src': '%s:%s' % (src.rel, m.path), 'gtx_member': t['gtx_name']})
            res['textures'][low.split('/')[-1][:-4]] = info
            res['hd_rgba'][low.split('/')[-1][:-4]] = (t['w'], t['h'], t['rgba'])
            entries.append((low, data))
        else:
            entries.append((low, m.data()))
    for folder, arc_name, member in EXTRA_GLYPHS:
        s2 = Source.get(hd, folder, arc_name)
        res['sources'][s2.rel] = {'sha1': s2.arc_sha1, 'pack': os.path.basename(s2.pack_path), 'pack_sha1': s2.pack_sha1}
        t = s2.texture(member)
        data, info, back = make_bti(t)
        name = member.split('/')[-1][:-4].lower()
        info.update({'src': '%s:%s' % (s2.rel, member), 'gtx_member': t['gtx_name']})
        res['textures'][name] = info
        res['hd_rgba'][name] = (t['w'], t['h'], t['rgba'])
        entries.append(('timg/%s.bti' % name, data))
    # timg first, then scrn: same node order as HD's stampResR (ROOT, TIMG, SCRN)
    entries.sort(key=lambda e: 0 if e[0].startswith('timg/') else 1)
    arc_bytes = rarc_write.build(ARC_ROOT, entries)
    common.write(os.path.join(out_dir, ARC_NAME), arc_bytes)
    res['arc'] = {'size': len(arc_bytes), 'sha256': common.sha256(arc_bytes), 'members': len(entries)}
    log('stampbook: %s %d bytes (%.2f MB / %.2f MiB), %d members (%d BLO, %d BTI), %d .tga dropped', ARC_NAME,
        len(arc_bytes), len(arc_bytes) / 1e6, len(arc_bytes) / 1048576.0, len(entries), len(res['blos']),
        len(res['textures']), len(res['dropped']))
    fmts = {}
    for n, i in res['textures'].items():
        fmts.setdefault(i['fmt'], []).append(n)
    for f, ns in sorted(fmts.items()):
        log('stampbook:   %-6s x%2d  %s', f, len(ns), ', '.join(sorted(ns, key=_natkey)[:8]) + (' ...' if len(ns) > 8 else ''))
    # loose BTIs
    for fn, folder, arc_name, member, what in LOOSE:
        s2 = Source.get(hd, folder, arc_name)
        res['sources'][s2.rel] = {'sha1': s2.arc_sha1, 'pack': os.path.basename(s2.pack_path), 'pack_sha1': s2.pack_sha1}
        t = s2.texture(member)
        data, info, back = make_bti(t)
        info.update({'src': '%s:%s' % (s2.rel, member), 'use': what, 'sha256': common.sha256(data),
                     'resource_path': 'stamps/stampbook/' + fn,
                     'load': 'svc_resource->load, copy to a 32-byte aligned buffer, use as ResTIMG* (texels at +0x20)'})
        common.write(os.path.join(out_dir, fn), data)
        res['loose'][fn] = info
        res['hd_rgba']['loose:' + fn] = (t['w'], t['h'], t['rgba'])
        log('stampbook: %-22s %6d bytes %-5s %dx%d  (%s)', fn, len(data), info['fmt'], info['w'], info['h'], what)
    twin = Source.get(hd, BUTTONS_TWIN[0], BUTTONS_TWIN[1]).member(BUTTONS_TWIN[2]).data()
    res['buttons_twin_sha1'] = hashlib.sha1(twin).hexdigest()
    res['text'] = text or {}
    res['manifest'] = _manifest(res)
    common.write_json(os.path.join(out_dir, 'book.json'), res['manifest'])
    return res


def _natkey(s):
    import re
    return [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', s)]


def _manifest(res):
    tex = {k: {kk: v for kk, v in i.items()} for k, i in sorted(res['textures'].items(), key=lambda kv: _natkey(kv[0]))}
    return {
        'version': 1,
        'note': 'Generated by tools/build_assets.py (tools/pipeline/stampbook.py) from the user\'s own TP HD dump; '
                'Nintendo data, local use only, never commit or distribute.',
        'sources': res['sources'],
        'archive': {
            'file': ARC_NAME, 'resource_path': 'stamps/stampbook/' + ARC_NAME, 'root': ARC_ROOT,
            'size': res['arc']['size'], 'sha256': res['arc']['sha256'], 'members': res['arc']['members'],
            'blo': {k: dict(v, member='scrn/' + k) for k, v in res['blos'].items()},
            'dropped': sorted(res['dropped'], key=_natkey),
            'load': 'svc_resource->load("stamps/stampbook/%s") -> copy into a 32-byte aligned buffer kept for the mod '
                    'lifetime -> JKR_NEW JKRMemArchive(buf, size, JKRMEMBREAK_FLAG_UNKNOWN0) between book create() '
                    'and destroy(); J2DScreen::setPriority("<blo name>", 0x20000, arc) for the 4 BLOs; textures '
                    'resolve by name inside the archive (TEX1 refs, TIMG dir).' % ARC_NAME,
        },
        'textures': tex,
        'loose': {k: v for k, v in sorted(res['loose'].items())},
        'gc_side': [{'archive': a, 'member': m, 'use': u} for a, m, u in GC_NEEDS],
    }


# ------------------------------------------------------------------------------------------ validate
def _jkr_hash(s):
    h = 0
    for c in s.lower().encode('shift_jis'):
        h = (h * 3 + c) & 0xFFFF
    return h


def validate(rep, res):
    """(a) BLOs byte-identical + SCRNblo2, (b) every TEX1 ref resolves inside the archive with JKR's lookup
    rules, (c) BTI headers/sizes, (d) no .tga, (e) RARC structure + 32-byte alignment + lowercase names,
    (f) size limit, (g) loose BTIs, (h) decoded GC texels vs HD source, GC-side resources, heap/VRAM figures,
    composite preview GC vs HD."""
    g = 'stampbook'
    out = res['out_dir']
    buf = common.read(os.path.join(out, ARC_NAME))
    probs = rarcx.check(buf)
    a = rarc.Rarc(buf, ARC_NAME)
    names_ok = all(e['name'] == e['name'].lower() and e['hash'] == _jkr_hash(e['name']) for e in a.entries) and \
        a.nodes[0]['name'] == ARC_ROOT and all(n['name'] == n['name'].lower() for n in a.nodes)
    types = sorted(n['type'] for n in a.nodes)
    rep.add(g, 'archive structure', not probs and names_ok and types == ['ROOT', 'SCRN', 'TIMG'],
            '%d members, nodes %s, %s; names lowercase + JKR hashes %s' % (
                len(a.files()), types, '; '.join(probs[:3]) or 'RARC ok, data 32-byte aligned, uncompressed',
                'ok' if names_ok else 'WRONG'))
    rep.add(g, 'size limit', len(buf) <= SIZE_LIMIT, '%d bytes = %.2f MB (%.2f MiB) <= %d MiB; was ~13.8 MB with '
            'all-RGBA8' % (len(buf), len(buf) / 1e6, len(buf) / 1048576.0, SIZE_LIMIT // 1048576))
    tga = [m.path for m in a.files() if m.name.lower().endswith('.tga')]
    rep.add(g, 'no .tga', not tga and len(res['dropped']) == 51, '%d Miiverse .tga dropped, %d left' % (len(res['dropped']), len(tga)))
    # (a) BLOs
    hd_src = Source.get(*_src_key(res, 'stampResR.arc'))
    bad = []
    for b in BLOS:
        m = a.get('scrn/' + b)
        hm = hd_src.member(ARC_ROOT + '/scrn/' + b)
        if m is None:
            bad.append(b + ' missing')
            continue
        d = m.data()
        if d != hm.data():
            bad.append(b + ' differs from HD')
        if d[:8] != b'SCRNblo2':
            bad.append(b + ' magic %r' % d[:8])
    btw = res['buttons_twin_sha1'] == res['blos'].get('zelda_stamps_buttons.blo', {}).get('sha1')
    rep.add(g, 'BLOs byte-identical', not bad and btw, '%s; buttons == clctresR do_icon_parts_revo %s' % (
        '; '.join(bad) or '4 SCRNblo2 layouts = HD bytes (sha1 checked)', 'yes' if btw else 'NO'))
    # (b) TEX1 refs
    timg = {m.name: m for m in a.files() if m.path.lower().startswith(ARC_ROOT + '/timg/')}
    unresolved, kinds, fonts, refs = [], set(), set(), 0
    blos = {}
    for b in BLOS:
        bl = Blo(a.get('scrn/' + b).data())
        blos[b] = bl
        for kind, nm in bl.tex:
            refs += 1
            kinds.add(kind)
            if kind not in (2, 3) or nm.lower() not in timg:
                unresolved.append('%s:%s(type %d)' % (b, nm, kind))
        for kind, nm in bl.fnt:
            fonts.add(nm)
    rep.add(g, 'TEX1 refs resolve', not unresolved, '%d refs in 4 BLOs, resref types %s, all found in timg/ '
            '(lowercase lookup)%s' % (refs, sorted(kinds), (': MISSING ' + ', '.join(unresolved)) if unresolved else ''))
    rep.add(g, 'FNT1 refs (runtime fonts)', True, '%s not shipped: the C++ sets mDoExt_getSubFont()/getMesgFont() '
            '(spec 6.1)' % ', '.join(sorted(fonts)))
    # (c)+(h) BTIs
    bad, worst, gc_rgba = [], {}, {}
    for name, m in sorted(timg.items()):
        d = m.data()
        f = bti_fields(d)
        stem = name[:-4]
        info = res['textures'].get(stem)
        p = _bti_problems(d, f)
        if info is None:
            p.append('not in manifest')
        if p:
            bad.append('%s: %s' % (name, ', '.join(p)))
            continue
        rgba = decode(f['fmt'], d[0x20:0x20 + tex_bytes(f['fmt'], f['w'], f['h'])], f['w'], f['h'])
        gc_rgba[stem] = (f['w'], f['h'], rgba)
        w0, h0, src = res['hd_rgba'][stem]
        vis, hid = tex_error(rgba, src) if (w0, h0) == (f['w'], f['h']) else (999, 999)
        lim = MAX_FLAT_ERR if FMT_NAME[f['fmt']] == 'RGB5A3' else 0
        if vis > lim or hid > 4 * lim:
            bad.append('%s: decoded error %d/%d > %d/%d' % (name, vis, hid, lim, 4 * lim))
        k = FMT_NAME[f['fmt']]
        wv, wh = worst.get(k, (0, 0))
        worst[k] = (max(wv, vis), max(wh, hid))
    res['gc_rgba'] = gc_rgba
    rep.add(g, 'BTI headers + pixels', not bad and len(timg) == 74, '%d BTIs: img_off 0x20, size = format x dims, '
            '1 image, no palette, <= %d px, wrap/filter valid%s' % (len(timg), DIM_LIMIT, (': ' + '; '.join(bad[:4])) if bad else ''))
    flat_alpha = all(res['hd_rgba'][k][2][3::4] == gc_rgba[k][2][3::4] for k in gc_rgba)
    rep.add(g, 'texels vs HD source', not bad and flat_alpha, 'decoded with Aurora semantics, max error visible/'
            'transparent texels: %s (0 = bit-exact; RGB5A3 = the 50 flat-ink stamps: visible <= %d/255, transparent '
            'texels <= %d/255 = <= %d/255 on screen via edge filtering); alpha bit-exact in all %d: %s' % (
                ', '.join('%s %d/%d' % (k, v[0], v[1]) for k, v in sorted(worst.items())), MAX_FLAT_ERR,
                4 * MAX_FLAT_ERR, MAX_FLAT_ERR, len(gc_rgba), flat_alpha))
    s0 = gc_rgba.get('stamp0')
    if s0:
        a0 = s0[2][3::4]
        ink = sum(1 for x in a0 if x)
        rep.add(g, 'stamp0 = Hylian A', ink > 0 and
                hashlib.sha1(a0).hexdigest() == hashlib.sha1(res['hd_rgba']['stamp0'][2][3::4]).hexdigest(),
                '%dx%d, %d ink texels, alpha mask identical to HD GTX stamp0 (sha1 %s)' % (
                    s0[0], s0[1], ink, hashlib.sha1(a0).hexdigest()[:12]))
    # (g) loose
    bad = []
    for fn, info in sorted(res['loose'].items()):
        d = common.read(os.path.join(out, fn))
        f = bti_fields(d)
        p = _bti_problems(d, f)
        if not p:
            rgba = decode(f['fmt'], d[0x20:], f['w'], f['h'])
            if max_err(rgba, res['hd_rgba']['loose:' + fn][2]) != 0:
                p.append('decoded pixels differ from HD')
        if p:
            bad.append('%s: %s' % (fn, ', '.join(p)))
    rep.add(g, 'loose BTIs', not bad, '%s%s' % (', '.join('%s %s %dx%d' % (k, v['fmt'], v['w'], v['h'])
                                                           for k, v in sorted(res['loose'].items())),
                                                 (': ' + '; '.join(bad)) if bad else '; lossless'))
    # GC-side resources + name shadowing
    if res.get('gc'):
        miss, where, shadow = [], [], set()
        gcarc = {}
        for arcp, member, use in GC_NEEDS:
            if arcp not in gcarc:
                p = os.path.join(res['gc'], *arcp.split('/'))
                gcarc[arcp] = rarc.Rarc.from_file(p) if os.path.exists(p) else None
            ga = gcarc[arcp]
            if ga is None or not any(m.name.lower() == member.lower() for m in ga.files()):
                miss.append('%s:%s' % (arcp, member))
        for arcp in ('res/Layout/main2D.arc', 'res/Layout/clctres.arc'):
            ga = gcarc.get(arcp)
            if ga:
                shadow |= {m.name.lower() for m in ga.files()} & set(timg)
        rep.add(g, 'GC-side resources', not miss, '%d needed by the C++ (cursor BLO, 10 digit BTIs, tt_block8x8, '
                'tt_spot_square3) present in the GC dump%s' % (len(GC_NEEDS), (': MISSING ' + ', '.join(miss)) if miss else ''))
        rep.add(g, 'name shadowing', True, 'book names also in GC main2D/clctres: %s (harmless: the archive is '
                'mounted only between book create/destroy, spec 16)' % (', '.join(sorted(shadow)) or 'none'))
    # heap / VRAM figures (INFERRED proxy: the Collection screen the book replaces lives in the same heap)
    panes = sum(sum(b.counts.values()) for b in blos.values())
    mats = sum(len(b.mats) for b in blos.values())
    texrefs = sum(sum(len(m['textures']) for m in b.mats) for b in blos.values())
    vram = sum(i['w'] * i['h'] * 4 for i in res['textures'].values())
    col = _collect_counts(res.get('gc'))
    ok = col is None or (panes + mats) <= col[0] + col[1]
    rep.add(g, 'heap headroom', ok, 'archive in a mod-owned process buffer (no game heap; JKRMemArchive/JUTTexture '
            'use it in place). J2D objects in dMw ExpHeap2D (0x177000): book %d panes + %d materials + %d texture '
            'slots%s. VRAM after Aurora RGBA8 upload <= %.1f MB' % (
                panes, mats, texrefs, (' vs GC Collection screen (freed before the book is built) %d panes + %d '
                                       'materials' % col) if col else '', vram / 1e6), warn=True)
    if res.get('preview_dir'):
        _preview_rows(rep, res, blos)


def _src_key(res, arc_name):
    hd = None
    for k in Source._cache:
        if k[2] == arc_name.lower():
            return k
    raise KeyError(arc_name)


def _bti_problems(d, f):
    p = []
    if f['fmt'] not in TILE:
        p.append('format %s' % f['fmt'])
        return p
    if f['img_off'] != 0x20:
        p.append('img_off 0x%x' % f['img_off'])
    if len(d) != 0x20 + tex_bytes(f['fmt'], f['w'], f['h']):
        p.append('size %d != %d' % (len(d), 0x20 + tex_bytes(f['fmt'], f['w'], f['h'])))
    if f['mips'] != 1 or f['mip_en'] or f['maxlod']:
        p.append('mips %d/%d/%d' % (f['mips'], f['mip_en'], f['maxlod']))
    if f['pal_en'] or f['pal_n'] or f['pal_off']:
        p.append('palette fields set')
    if not (0 < f['w'] <= DIM_LIMIT and 0 < f['h'] <= DIM_LIMIT):
        p.append('dims %dx%d' % (f['w'], f['h']))
    if any(x > 2 for x in f['wrap']) or f['minf'] > 5 or f['magf'] > 1:
        p.append('wrap/filter %s %d %d' % (f['wrap'], f['minf'], f['magf']))
    return p


def _collect_counts(gc):
    if not gc:
        return None
    p = os.path.join(gc, 'res', 'Layout', 'clctres.arc')
    if not os.path.exists(p):
        return None
    a = rarc.Rarc.from_file(p)
    panes = mats = 0
    for n in ('zelda_collect_soubi_screen.blo', 'zelda_collect_soubi_do_icon_parts.blo'):
        m = a.get('scrn/' + n)
        if m is None:
            return None
        b = Blo(m.data())
        panes += sum(b.counts.values())
        mats += len(b.mats)
    return panes, mats


# ------------------------------------------------------------------------------------------ preview
OWNED_DEMO = {0, 1, 2, 4, 7, 9, 12, 13, 18, 20, 26, 27, 28, 31, 35, 40, 44, 46, 47}     # = hdlayout preview state
SEL_FRAMES = ['it_i0010', 'it_i0011', 'it_i0012', 'it_i0013', 'it_i0014',
              'it_i0019', 'it_i0018', 'it_i0017', 'it_i0016', 'it_i0015']


def _preview_rows(rep, res, blos):
    g = 'stampbook'
    try:
        import numpy  # noqa: F401
        from PIL import Image  # noqa: F401
    except ImportError:
        rep.add(g, 'composite preview', False, 'skipped: numpy/Pillow not installed', warn=True)
        return
    od = res['preview_dir']
    os.makedirs(od, exist_ok=True)
    fonts = _gc_fonts(res.get('gc'))
    rows = []
    for tag, page, cursor, msg in (('page1', 0, 2, False), ('page5', 4, 4, False), ('reward', 4, 9, True)):
        imgs = {}
        for which in ('gc', 'hd'):
            tex = res['gc_rgba'] if which == 'gc' else {k: v for k, v in res['hd_rgba'].items() if ':' not in k}
            r = _Renderer(tex, fonts)
            _draw_book(r, blos, res.get('text') or {}, page, cursor, msg)
            imgs[which] = r.image()
            r.save(os.path.join(od, 'book_%s_%s.png' % (tag, which)))
        import numpy as np
        d = np.abs(imgs['gc'].astype(np.int32) - imgs['hd'].astype(np.int32))[:, :, :3]
        mse = float((d.astype(np.float64) ** 2).mean())
        psnr = 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)
        rows.append((tag, psnr, int(d.max()), float((d > 2).mean() * 100)))
        missing = sorted(r.missing)
        res.setdefault('preview', {})[tag] = {'psnr': psnr, 'max': int(d.max()), 'missing': missing}
    from PIL import Image
    for fn in sorted(res['loose']):
        d = common.read(os.path.join(res['out_dir'], fn))
        f = bti_fields(d)
        Image.frombytes('RGBA', (f['w'], f['h']), decode(f['fmt'], d[0x20:], f['w'], f['h'])).save(
            os.path.join(od, 'loose_%s.png' % fn[:-4]))
    ok = all(p >= 45 and m <= 8 for _, p, m, _ in rows) and not any(res['preview'][t]['missing'] for t in res['preview'])
    rep.add(g, 'composite GC vs HD', ok, '%s (book_<state>_gc.png vs _hd.png + loose_*.png in %s, 1216x896)%s' % (
        '; '.join('%s PSNR %.1f dB, max %d, %.2f%% px > 2' % r for r in rows), od,
        '' if fonts else '; no GC fonts: text skipped'))


def _gc_fonts(gc):
    fonts = {}
    if not gc:
        return fonts
    for key, arcp, member in (('mesg', 'res/Fontus/fontres.arc', 'rodan_b_24_22.bfn'),
                              ('sub', 'res/Fontus/rubyres.arc', 'reishotai_24_22.bfn')):
        p = os.path.join(gc, *arcp.split('/'))
        if os.path.exists(p):
            m = rarc.Rarc.from_file(p).get(member)
            if m is not None:
                fonts[key] = _Font(m.data())
    return fonts


class _Font(object):
    """GC BFN (INF1/WID1/MAP1/GLY1) with the glyph sheets decoded (Aurora semantics) as coverage arrays."""

    def __init__(self, d):
        import numpy as np
        o = 0x20
        self.maps, self.widths, self.sheets = [], {}, []
        while o + 8 <= len(d):
            t, sz = d[o:o + 4], _u32(d, o + 4)
            if t == b'INF1':
                (self.ftype, self.ascent, self.descent, self.width, self.leading, self.default) = \
                    struct.unpack_from('>6H', d, o + 8)
            elif t == b'WID1':
                s, e = struct.unpack_from('>HH', d, o + 8)
                for i in range(s, e + 1):
                    self.widths[i] = (d[o + 0xC + 2 * (i - s)], d[o + 0xD + 2 * (i - s)])
            elif t == b'MAP1':
                mt, s, e, n = struct.unpack_from('>HHHH', d, o + 8)
                cnt = max(n * (2 if mt == 3 else 1), 1)
                self.maps.append((mt, s, e, n, [_u16(d, o + 0x10 + 2 * k) for k in range(cnt)]))
            elif t == b'GLY1':
                (gs, ge, self.cw, self.ch, ssz, tf, self.cols, self.rows, tw, th) = \
                    struct.unpack_from('>HHHHIHHHHH', d, o + 8)
                k = o + 0x20
                while k + ssz <= o + sz:
                    rgba = decode(tf, d[k:k + ssz], tw, th)
                    self.sheets.append(np.frombuffer(rgba, np.uint8).reshape(th, tw, 4)[:, :, 3].astype(np.float32) / 255.0)
                    k += ssz
            if sz == 0:
                break
            o += sz

    def glyph(self, code):
        for mt, s, e, n, data in self.maps:
            if s <= code <= e:
                if mt == 0:
                    return code - s + data[0]
                if mt == 2:
                    return data[code - s]
                if mt == 3:
                    for k in range(n):
                        if data[2 * k] == code:
                            return data[2 * k + 1]
        return self.default

    def cell(self, gi):
        per = self.rows * self.cols
        sh = self.sheets[min(gi // per, len(self.sheets) - 1)]
        r = gi % per
        x, y = (r % self.cols) * self.cw, (r // self.cols) * self.ch
        return sh[y:y + self.ch, x:x + self.cw]


class _Renderer(object):
    """Software J2D compositor (port of the hdlayout research renderer): 1-stage TEV C=TEXC*RASC,
    2-stage C=lerp(C0,C1,TEXC)*RASC, vertex corner colours, material alpha, bilinear sampling, J2DPrint-like
    text. 608x448 at 2x."""
    W, H, S = 608, 448, 2

    def __init__(self, tex, fonts):
        import numpy as np
        self.np = np
        self.tex = {}
        for k, (w, h, rgba) in tex.items():
            self.tex[k.lower()] = np.frombuffer(rgba, np.uint8).reshape(h, w, 4).astype(np.float32) / 255.0
        self.fonts = fonts
        self.canvas = np.zeros((self.H * self.S, self.W * self.S, 4), np.float32)
        self.canvas[:, :, 3] = 1.0
        self.missing = set()

    def image(self):
        return (self.np.clip(self.canvas, 0, 1) * 255 + 0.5).astype(self.np.uint8)

    def save(self, path):
        from PIL import Image
        Image.fromarray(self.image(), 'RGBA').save(path)

    def _quad(self, p, img):
        np = self.np
        (a, b, c), (d, e, f) = p['m']
        ax, ay = p['anchor']
        corners = [(a * x + b * y + c, d * x + e * y + f) for x, y in
                   ((-ax, -ay), (p['w'] - ax, -ay), (-ax, p['h'] - ay), (p['w'] - ax, p['h'] - ay))]
        xs, ys = [q[0] for q in corners], [q[1] for q in corners]
        s = self.S
        X0, Y0 = max(0, int(math.floor(min(xs) * s))), max(0, int(math.floor(min(ys) * s)))
        X1, Y1 = min(self.W * s, int(math.ceil(max(xs) * s))), min(self.H * s, int(math.ceil(max(ys) * s)))
        det = a * e - b * d
        if X1 <= X0 or Y1 <= Y0 or abs(det) < 1e-9:
            return
        yy, xx = np.mgrid[Y0:Y1, X0:X1].astype(np.float32)
        sx, sy = (xx + 0.5) / s, (yy + 0.5) / s
        lx = (e * (sx - c) - b * (sy - f)) / det + ax
        ly = (-d * (sx - c) + a * (sy - f)) / det + ay
        inside = (lx >= 0) & (lx < p['w']) & (ly >= 0) & (ly < p['h'])
        ih, iw = img.shape[:2]
        u = np.clip((lx / p['w'] * iw).astype(np.int32), 0, iw - 1)
        v = np.clip((ly / p['h'] * ih).astype(np.int32), 0, ih - 1)
        src = img[v, u].copy()
        src[:, :, 3] *= inside
        dst = self.canvas[Y0:Y1, X0:X1]
        al = src[:, :, 3:4]
        dst[:, :, :3] = src[:, :, :3] * al + dst[:, :, :3] * (1 - al)

    def _sample(self, t, U, V):
        np = self.np
        th, tw = t.shape[:2]
        x, y = U * tw - 0.5, V * th - 0.5
        x0, y0 = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
        fx, fy = (x - x0)[:, :, None], (y - y0)[:, :, None]
        xa, xb = np.clip(x0, 0, tw - 1), np.clip(x0 + 1, 0, tw - 1)
        ya, yb = np.clip(y0, 0, th - 1), np.clip(y0 + 1, 0, th - 1)
        return (t[ya, xa] * (1 - fx) * (1 - fy) + t[ya, xb] * fx * (1 - fy) + t[yb, xa] * (1 - fx) * fy +
                t[yb, xb] * fx * fy)

    def picture(self, p, mat, alpha_mul=1.0):
        np = self.np
        if not mat['textures']:
            return
        key = mat['textures'][0].lower()
        key = key[:-4] if key.endswith('.bti') else key
        t = self.tex.get(key)
        if t is None:
            self.missing.add(key)
            return
        s = self.S
        oh, ow = max(2, int(round(p['h'] * s))), max(2, int(round(p['w'] * s)))
        uv = np.array(p['uv'], np.float32) / 256.0
        ys = np.linspace(0, 1, oh, dtype=np.float32)[:, None]
        xs = np.linspace(0, 1, ow, dtype=np.float32)[None, :]
        U = (uv[0, 0] * (1 - xs) + uv[1, 0] * xs) * (1 - ys) + (uv[2, 0] * (1 - xs) + uv[3, 0] * xs) * ys
        V = (uv[0, 1] * (1 - xs) + uv[1, 1] * xs) * (1 - ys) + (uv[2, 1] * (1 - xs) + uv[3, 1] * xs) * ys
        tex = self._sample(t, U, V)
        cc = [np.array(c, np.float32) / 255.0 for c in p['corners']]
        yy, xx = ys[:, :, None], xs[:, :, None]
        ras = (cc[0] * (1 - xx) + cc[1] * xx) * (1 - yy) + (cc[2] * (1 - xx) + cc[3] * xx) * yy
        if mat['tevstages'] >= 2 and mat['tevcolor'][0] is not None:
            c0 = np.array(mat['tevcolor'][0], np.float32) / 255.0
            c1 = np.array(mat['tevcolor'][1], np.float32) / 255.0
            out = np.concatenate([c0[:3] + (c1[:3] - c0[:3]) * tex[:, :, :3],
                                  c0[3] + (c1[3] - c0[3]) * tex[:, :, 3:4]], axis=2)
        else:
            out = tex.copy()
        out = out * ras
        out[:, :, 3] *= (mat['matcolor'][3] / 255.0 if mat['matcolor'] else 1.0) * alpha_mul
        self._quad(p, np.clip(out, 0, 1))

    def text(self, p, mat, text, font):
        np = self.np
        F = self.fonts.get(font)
        if F is None or not text:
            return
        s = self.S
        fw, fh = p['font_w'], p['font_h']
        kx = fw / float(F.cw)
        lines = text.split('\n')
        widths = []
        for ln in lines:
            widths.append(sum(F.widths.get(F.glyph(ord(ch)), (0, F.cw))[1] * kx + p['char_space'] for ch in ln))
        asc = F.ascent * (fh / float(F.ch))
        desc = F.descent * (fh / float(F.ch))
        text_h = (len(lines) - 1) * p['line_space'] + asc + desc
        bw, bh = p['w'], p['h']
        voff = {0: int(bh - text_h - 0.5) // 2, 1: int(bh - text_h - 0.5)}.get(p['vbind'], 0)
        ow, oh = max(2, int(round(bw * s))), max(2, int(round(bh * s)))
        img = np.zeros((oh, ow, 4), np.float32)
        c0 = np.array(mat['tevcolor'][0] or [0, 0, 0, 0], np.float32) / 255.0
        c1 = np.array(mat['tevcolor'][1] or [255, 255, 255, 255], np.float32) / 255.0
        ctop, cbot = np.array(p['char_color'], np.float32) / 255.0, np.array(p['grad_color'], np.float32) / 255.0
        from PIL import Image
        for li, ln in enumerate(lines):
            x = {2: 0.0, 1: bw - widths[li]}.get(p['hbind'], (bw - widths[li]) / 2.0)
            base = voff + asc + li * p['line_space']
            for ch in ln:
                gi = F.glyph(ord(ch))
                kern, wid = F.widths.get(gi, (0, F.cw))
                cell = F.cell(gi)
                tw_, th_ = max(1, int(round(fw * s))), max(1, int(round(fh * s)))
                I = np.asarray(Image.fromarray((cell * 255).astype(np.uint8)).resize((tw_, th_), Image.BILINEAR),
                               np.float32)[:, :, None] / 255.0
                gy = np.linspace(0, 1, th_, dtype=np.float32)[:, None, None]
                vc = ctop * (1 - gy) + cbot * gy
                glyph = np.concatenate([(c0[:3] + (c1[:3] - c0[:3]) * I) * vc[:, :, :3],
                                        (c0[3] + (c1[3] - c0[3]) * I) * vc[:, :, 3:4]], axis=2)
                X, Y = int(round((x - kern * kx) * s)), int(round((base - asc) * s))
                hh, ww = glyph.shape[:2]
                cx0, cy0, cx1, cy1 = max(0, X), max(0, Y), min(ow, X + ww), min(oh, Y + hh)
                if cx1 > cx0 and cy1 > cy0:
                    src = glyph[cy0 - Y:cy1 - Y, cx0 - X:cx1 - X]
                    dst = img[cy0:cy1, cx0:cx1]
                    al = src[:, :, 3:4]
                    dst[:, :, :3] = np.where(dst[:, :, 3:4] > 0, src[:, :, :3] * al + dst[:, :, :3] * (1 - al), src[:, :, :3])
                    dst[:, :, 3:4] = al + dst[:, :, 3:4] * (1 - al)
                x += wid * kx + p['char_space']
        img[:, :, 3] *= mat['matcolor'][3] / 255.0 if mat['matcolor'] else 1.0
        self._quad(p, img)

    def screen(self, blo, vis=None, txt=None, alpha=None, fonts=None):
        vis, txt, alpha, fonts = vis or {}, txt or {}, alpha or {}, fonts or {}

        def rec(p, pv):
            v = vis.get(p['name'], p['visible']) and pv
            if v and p['type'] in ('PIC2', 'TBX2') and p.get('material', 0xFFFF) < len(blo.mats):
                mat = blo.mats[p['material']]
                if p['type'] == 'PIC2':
                    self.picture(p, mat, alpha.get(p['name'], 1.0))
                else:
                    self.text(p, mat, txt.get(p['name'], p['text']), fonts.get(p['name'], 'mesg'))
            for ch in p['children']:
                rec(ch, v)
        rec(blo.root, True)


def _draw_book(r, blos, text, page, cursor, message):
    """Book state as dMenu_Stamp_c::refreshStampPanes draws it (spec 7.4); same demo ownership as the
    developer's reference preview of HD's stamp screen, so the images can be compared with it."""
    vis, txt = {}, {}
    for slot in range(10):
        for pg in range(5):
            vis['it_s%04d' % (pg * 10 + slot)] = (pg == page and page * 10 + slot in OWNED_DEMO)
        vis['it_e%02d' % (slot + 1)] = page * 10 + slot not in OWNED_DEMO
        vis['it_i%04d' % slot] = slot != cursor
        vis[SEL_FRAMES[slot]] = slot == cursor
        vis['it_t%04d' % slot] = False
    sid = page * 10 + cursor
    txt['info_txt'] = text.get(2890 + sid, 'Stamp %d' % sid) if sid in OWNED_DEMO else ''
    txt['page_txt'] = (text.get(2802) or '%d / %d').replace('%d', str(page + 1), 1).replace('%d', '5', 1)
    txt['post_txt'] = text.get(2803, 'Post')
    txt['title_tx'] = text.get(2800, 'Stamps')
    r.screen(blos['zelda_stamps_background.blo'], txt=txt, fonts={'title_tx': 'sub'})
    r.screen(blos['zelda_stamps_menu.blo'], vis=vis, txt=txt)
    r.screen(blos['zelda_stamps_buttons.blo'])
    if message:
        r.screen(blos['zelda_stamps_message.blo'], vis={'n_all': True},
                 txt={'msg_text': text.get(2806, '(message 2806)')})


def hdtext_previews(path):
    """{id: plain text} from a staged/installed mods/hdfeatures/res/stamps/hdtext/messages.json (optional)."""
    out = {}
    try:
        with open(path, encoding='utf-8') as f:
            for m in json.load(f):
                if m.get('file') == 'zel_00' and m.get('preview') is not None:
                    out[m['id']] = m['preview']
    except (OSError, ValueError):
        pass
    return out


# ------------------------------------------------------------------------------------------ CLI
def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hd', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--gc')
    ap.add_argument('--preview')
    ap.add_argument('--text', help='hdtext messages.json for the preview strings')
    a = ap.parse_args(argv)
    import validate as _validate
    log = common.Log()
    gc = a.gc
    if gc and os.path.isdir(os.path.join(gc, 'files')):
        gc = os.path.join(gc, 'files')
    res = build(os.path.abspath(a.hd), os.path.abspath(a.out), log, gc=gc, preview_dir=a.preview,
                text=hdtext_previews(a.text) if a.text else None)
    rep = _validate.Report()
    validate(rep, res)
    print(rep.table())
    return 1 if rep.failed() else 0


if __name__ == '__main__':
    sys.exit(main())
