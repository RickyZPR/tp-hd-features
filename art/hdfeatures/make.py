#!/usr/bin/env python3
"""make.py - HD Features mod art (one mod = HD Stamps + Cave of Shadows).

Direction "Twilight emblem + stamps": the cave-a look (painted HD Wolf Link on the Mirror of
Twilight rune portal, HD-gold frame and Roman capitals, official 'Twilight Princess HD' logotype)
plus a short row of HD's own sepia stamps on stamp-book tiles.

Builds, next to this script:
  icon.png     512x512 RGBA   Wolf Link on the twilight rune-ring portal inside a gold frame, with a
                              small HD stamp-book tile (Midna stamp, gold 'selected' box) in the
                              lower-right corner
  banner.png  1400x400 RGBA   'HD FEATURES' + official 'Twilight Princess HD' logotype, a fan of
                              five HD stamps rising out of the portal, Wolf Link on the right
  preview.png                 both in a mock of Dusklight 2.0.3's Mods window at 1.5 px/dp (UI
                              scale 1.5) and 1 px/dp, enabled + disabled, icon sizes

Usage:  python -I make.py --hd DIR [--out DIR] [--dusklight DIR] [--font TTF] [--cache DIR] [--no-badge]
                         [--icon-only | --banner-only]
  --hd DIR        the TP HD dump's 'content' folder (or its 'res' folder). Read only.
  --out DIR       where icon.png / banner.png go (default: next to this script).
  --dusklight DIR Dusklight install folder: its res/AlegreyaSC-Bold.ttf is the title font when Perpetua
                  Titling (an Office font) is not installed, and the preview needs its fonts. Optional.
  --font TTF      title font to use instead.
  --cache DIR     keep decoded HD textures as PNGs in DIR (speeds up re-runs; optional).
  --no-badge      icon without the stamp tile (pure cave-a emblem).
  preview.png is only made next to this script (no --out) and only with --dusklight.

Read-only inputs (nothing outside this folder is ever written, except the optional cache dir;
bytecode writing is disabled so tools/formats gets no __pycache__):
  * TP HD dump (Wii U, user's own), decoded straight from the TMPK packs:
      res/Object/LogoUs.pack.gz   Boot_{TL,TR,BL,BR}.bti   HD boot splash -> painted Wolf Link head
      res/Object/Title.pack.gz    titlelogo_r.bmd #0 / #2  TP crest emblem, 'Twilight Princess HD'
      res/LayoutRevo/clctresR.pack.gz  Collection screen: backdrop_{tl,tr,bl,br} (twilight smoke),
          kageri_mirrer_ui #0 (Mirror of Twilight rune ring), md_mask_ui #1/#5/#7 (Fused Shadow
          glyphs), corner_ona00 (filigree corner, tinted gold)
      res/LayoutRevo/stampResR.pack.gz  stamp book: stamp7/30/35/34/29 (Hylian letter H, Link,
          Midna, Wolf Link, Heart Container), box / box2 (stamp boxes, normal + selected), paper
          (page parchment)
  * tools/formats/{hdpack,gtx2img}.py (TMPK + GX2 de-tiling)
  * fonts: PERTIBD.TTF (Perpetua Titling MT Bold, Windows or the user's font folder) for the title, else
    --font, else Dusklight's res/AlegreyaSC-Bold.ttf (the banner then looks slightly different);
    preview only: Dusklight install res/FiraSans-{Regular,Bold}.ttf, MaterialSymbolsRounded-Regular.ttf
  * preview only: mods/hdfeatures/{mod.json.in,CMakeLists.txt} if present (name, version,
    description; built-in fallback text otherwise) and the user's installed Dawnlight .dusk
    (list context, opened read-only).
Nothing is upscaled: every bitmap source ends up at <= 1:1 in the output (vector parts and the
stamp tiles are drawn supersampled; the tiny 1-colour Hylian letter is re-thresholded at 2x and
then shrunk back to exactly 1:1, so its pixel line art stays crisp).
The output contains Nintendo artwork from the user's own dump: it stays on that PC. Never distribute it.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))                  # the project root
sys.dont_write_bytecode = True                                          # read-only on tools/
for _formats in (os.path.join(ROOT, "tools", "formats"),                 # this project: art/hdfeatures/
                 os.path.join(HERE, "..", "tools", "formats")):          # the release: builder/art/
    if os.path.isfile(os.path.join(_formats, "hdpack.py")):
        sys.path.insert(0, os.path.abspath(_formats))
        break
import gtx2img  # noqa: E402
import hdpack  # noqa: E402

HD = None                                             # <content>/res (--hd)
DUSK = None                                           # Dusklight install (--dusklight; optional)
F_TITLE = None                                        # title font (find_title_font)


def find_title_font(explicit=None):
    """Perpetua Titling MT Bold (the look the art was designed with), else --font, else Dusklight's
    Alegreya SC Bold (OFL)."""
    cands = []
    if explicit:
        cands.append(explicit)
    windir = os.environ.get("WINDIR") or "C:/Windows"
    cands.append(os.path.join(windir, "Fonts", "PERTIBD.TTF"))
    local = os.environ.get("LOCALAPPDATA")
    if local:
        cands.append(os.path.join(local, "Microsoft", "Windows", "Fonts", "PERTIBD.TTF"))
    if DUSK:
        cands.append(os.path.join(DUSK, "res", "AlegreyaSC-Bold.ttf"))
    for c in cands:
        if c and os.path.isfile(c):
            return c
    raise SystemExit("no title font: install Perpetua Titling, or pass --font or --dusklight")

CACHE = None
_PACKS = {}


# ----------------------------------------------------------------------------------------------
# source loading
# ----------------------------------------------------------------------------------------------
def tex(pack, member, surface=0):
    """Decode one GX2 surface of a TMPK member from the HD dump -> RGBA PIL image."""
    key = "%s__%s__%d.png" % (pack.replace("/", "_"), member.replace("/", "_"), surface)
    if CACHE and os.path.exists(os.path.join(CACHE, key)):
        return Image.open(os.path.join(CACHE, key)).convert("RGBA")
    path = os.path.join(HD, pack)
    if path not in _PACKS:
        _PACKS[path] = hdpack.Tmpk(hdpack.read_any(path))
    data = _PACKS[path].get(member)
    if data is None:
        raise SystemExit("missing %s in %s" % (member, pack))
    s = hdpack.gtx_surfaces(data)["surfaces"][surface]
    lin = gtx2img.deswizzle(s, data[s["img_off"]:s["img_off"] + s["img"]])
    im = Image.frombytes("RGBA", (s["w"], s["h"]), gtx2img.to_rgba(s, lin))
    if CACHE:
        os.makedirs(CACHE, exist_ok=True)
        im.save(os.path.join(CACHE, key))
    return im


def load_sources():
    S = {}
    boot = Image.new("RGBA", (1920, 1080))
    for q, xy in (("TL", (0, 0)), ("TR", (960, 0)), ("BL", (0, 540)), ("BR", (960, 540))):
        boot.paste(tex("Object/LogoUs.pack.gz", "Boot_%s.bti.gtx" % q), xy)
    S["boot"] = boot.convert("RGB")
    S["crest"] = tex("Object/Title.pack.gz", "titlelogo_r.bmd.gtx", 0)       # 2432x1792 emblem
    S["tphd"] = tex("Object/Title.pack.gz", "titlelogo_r.bmd.gtx", 2)        # 'Twilight Princess HD'
    _PACKS.clear()
    C = "LayoutRevo/clctresR.pack.gz"
    bd = Image.new("RGBA", (1920, 1080))
    for q, xy in (("tl", (0, 0)), ("tr", (960, 0)), ("bl", (0, 540)), ("br", (960, 540))):
        bd.paste(tex(C, "backdrop_%s.bti.gtx" % q), xy)
    S["backdrop"] = bd.convert("RGB")
    S["mirror"] = tex(C, "kageri_mirrer_ui.bmd.gtx", 0)                        # rune ring quadrant
    S["glyphs"] = [tex(C, "md_mask_ui.bmd.gtx", i) for i in (1, 5, 7)]         # Fused Shadow glyphs
    S["corner"] = tex(C, "corner_ona00.bti.gtx")
    _PACKS.clear()
    P = "LayoutRevo/stampResR.pack.gz"
    S["stamps"] = {i: tex(P, "stamp%d.bti.gtx" % i) for i in sorted(set(ROW_IDS) | {ICON_STAMP})}
    S["box"] = tex(P, "box.bti.gtx")                                            # 231x232 stamp box
    S["box2"] = tex(P, "box2.bti.gtx")                                          # selected (gold) box
    S["paper"] = tex(P, "paper.bti.gtx")                                        # 813x478 page quarter
    _PACKS.clear()
    return S


# ----------------------------------------------------------------------------------------------
# small helpers
# ----------------------------------------------------------------------------------------------
def arr(im):
    return np.asarray(im).astype(np.float32) / 255.0


def img(a, mode="RGBA"):
    return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), mode)


def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def mask_img(m):
    return Image.fromarray((np.clip(m, 0, 1) * 255 + 0.5).astype(np.uint8), "L")


def solid(size, rgb, alpha_mask=None):
    im = Image.new("RGBA", size, tuple(rgb) + (255,))
    if alpha_mask is not None:
        im.putalpha(alpha_mask)
    return im


def add_glow(base, mask, rgb, radius, strength):
    """Additive (screen-like) glow from an L mask onto an RGBA image."""
    g = mask.filter(ImageFilter.GaussianBlur(radius))
    ga = arr(g)[..., None] * strength
    b = arr(base)
    col = np.array(rgb, np.float32)[None, None, :] / 255.0
    b[..., :3] = 1 - (1 - b[..., :3]) * (1 - col * ga)
    return img(b)


def radial(size, center, r, inner, outer, gamma=1.0):
    w, h = size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((x - center[0]) ** 2 + (y - center[1]) ** 2) / r
    t = np.clip(d, 0, 1) ** gamma
    inner = np.array(inner, np.float32) / 255.0
    outer = np.array(outer, np.float32) / 255.0
    c = inner[None, None, :] * (1 - t[..., None]) + outer[None, None, :] * t[..., None]
    return c


def ss_draw(size, scale, fn):
    """Supersampled vector drawing: fn(draw, s) draws an L mask at size*scale; returns L at size."""
    w, h = size
    m = Image.new("L", (w * scale, h * scale), 0)
    fn(ImageDraw.Draw(m), scale)
    return m.resize((w, h), Image.LANCZOS)


def gold_map(gray_l, dark=(58, 44, 18), mid=(172, 148, 78), light=(246, 234, 172)):
    """Map an L image (0..255 shading) to a metallic gold RGB ramp."""
    g = arr(gray_l)
    d, m, l_ = (np.array(c, np.float32) / 255.0 for c in (dark, mid, light))
    lo = np.clip(g * 2, 0, 1)[..., None]
    hi = np.clip(g * 2 - 1, 0, 1)[..., None]
    rgb = d * (1 - lo) + m * lo
    rgb = rgb * (1 - hi) + l_ * hi
    return rgb


# ----------------------------------------------------------------------------------------------
# Wolf Link cut-out from the HD boot splash (painted art, ~1:1, never upscaled)
# ----------------------------------------------------------------------------------------------
WOLF_BOX = (30, 160, 620, 760)          # crop of the 1920x1080 boot splash
WOLF_POLY = [(40, 578), (70, 520), (110, 478), (200, 395), (248, 300), (262, 262), (300, 262),
             (335, 290), (372, 215), (398, 178), (440, 186), (480, 205), (570, 200), (600, 260),
             (560, 330), (500, 368), (470, 400), (470, 445), (420, 520), (355, 615), (350, 650),
             (345, 715), (240, 735), (150, 690), (110, 640), (70, 610)]
# region kept at full strength; outside it the neck/mane fades into shadow (hides the logo overlap)
WOLF_KEEP = [(0, 0), (430, 0), (440, 150), (455, 250), (430, 310), (350, 430), (290, 510),
             (240, 565), (150, 600), (0, 600)]


def tuft_mask(size, A, B, n=9, length=30, seed=4):
    """Everything up/left of the line A->B, with fur tufts flowing out to the lower right (L)."""
    rng = np.random.RandomState(seed)
    ax, ay = A
    bx, by = B
    L = math.hypot(bx - ax, by - ay)
    dx, dy = (bx - ax) / L, (by - ay) / L
    nx, ny = dy, -dx                                    # outward normal (right/down of the line)
    if nx < 0:
        nx, ny = -nx, -ny
    pts = [(ax - 2000 * dx + 0, ay - 2000 * dy), (ax, ay)]
    for k in range(n):
        t0, t1 = k / n, (k + 1) / n
        tm = (t0 + t1) / 2 + rng.uniform(-0.1, 0.1) / n
        ln = length * rng.uniform(0.6, 1.25)
        pts.append((ax + dx * L * tm + nx * ln + dx * ln * 0.7, ay + dy * L * tm + ny * ln + dy * ln * 0.7))
        pts.append((ax + dx * L * t1 - nx * rng.uniform(0, 6), ay + dy * L * t1 - ny * rng.uniform(0, 6)))
    pts += [(bx + 2000 * dx, by + 2000 * dy), (bx + 2000 * dx - 4000 * nx, by + 2000 * dy - 4000 * ny),
            (ax - 2000 * dx - 4000 * nx, ay - 2000 * dy - 4000 * ny)]
    sc = 2
    m = Image.new("L", (size[0] * sc, size[1] * sc), 0)
    ImageDraw.Draw(m).polygon([(x * sc, y * sc) for x, y in pts], fill=255)
    return m.resize(size, Image.LANCZOS).filter(ImageFilter.GaussianBlur(1.2))


def wolf_cutout(boot):
    W, H = boot.size
    a = arr(boot)
    L = lum(a) * 255
    sat = (a.max(-1) - a.min(-1)) * 255
    pm = Image.new("L", (W, H), 0)
    ImageDraw.Draw(pm).polygon(WOLF_POLY, fill=255)
    poly = np.asarray(pm) > 0
    key = ((L < 96) & (sat < 70) & poly)
    m = mask_img(key.astype(np.float32)).filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
    m = m.copy()
    ImageDraw.floodfill(m, (230, 560), 128)
    comp = np.asarray(m) == 128
    inv = mask_img((~comp).astype(np.float32)).copy()
    ImageDraw.floodfill(inv, (0, 0), 128)
    filled = (np.asarray(inv) != 128) & poly
    fm = mask_img(filled.astype(np.float32))
    core = arr(fm.filter(ImageFilter.MinFilter(5)))
    near = arr(fm.filter(ImageFilter.MaxFilter(7)))
    soft = np.clip((128 - L) / 48.0, 0, 1)
    alpha = np.maximum(core, soft * near) * poly
    alpha = arr(mask_img(alpha).filter(ImageFilter.GaussianBlur(0.7)))
    km = Image.new("L", (W, H), 0)
    ImageDraw.Draw(km).polygon([(x + WOLF_BOX[0], y + WOLF_BOX[1]) for x, y in WOLF_KEEP], fill=255)
    keep = arr(km.filter(ImageFilter.GaussianBlur(34)))
    xs = np.arange(W, dtype=np.float32)[None, :] - WOLF_BOX[0]
    tufts = arr(tuft_mask((W, H), (WOLF_BOX[0] + 470, WOLF_BOX[1] + 190),
                          (WOLF_BOX[0] + 290, WOLF_BOX[1] + 560)))
    alpha = alpha * tufts                               # ragged fur edge instead of the crop line
    full = mask_img(alpha * np.clip((470 - xs) / 50.0, 0, 1)).crop(WOLF_BOX)   # neck silhouette
    alpha = alpha * np.clip(keep * 1.6 - 0.15, 0, 1)
    a = a * np.array([0.92, 0.99, 1.0], np.float32)       # cool grade toward the twilight palette
    rgba = np.dstack([a, alpha])
    return img(rgba).crop(WOLF_BOX), full


# ----------------------------------------------------------------------------------------------
# twilight elements
# ----------------------------------------------------------------------------------------------
def mirror_ring(src):
    """Full Mirror of Twilight rune ring (1024x1024 L mask) from its 512x512 quadrant (IA8, I=A)."""
    q = arr(src)[..., 3]
    q = np.clip((q - 86 / 255.0) / (1 - 86 / 255.0), 0, 1)      # stored quadrant = bottom-right
    full = np.zeros((1024, 1024), np.float32)
    full[512:, 512:] = q
    full[512:, :512] = q[:, ::-1]
    full[:512, 512:] = q[::-1, :]
    full[:512, :512] = q[::-1, ::-1]
    y, x = np.mgrid[0:1024, 0:1024].astype(np.float32)
    r = np.sqrt((x - 511.5) ** 2 + (y - 511.5) ** 2) / 512.0
    full *= np.clip((0.995 - r) / 0.01, 0, 1)          # drop the corner emblems outside the disc
    return mask_img(full)


def glyph_field(glyphs, size, seed=7):
    """Tile the Fused Shadow glyph masks into a pattern field (L mask)."""
    w, h = size
    out = Image.new("L", size, 0)
    rng = np.random.RandomState(seed)
    tiles = [g.split()[3] if g.mode == "RGBA" else g for g in glyphs]
    tiles = [ImageChops.multiply(t, g.convert("L")) for t, g in zip(tiles, glyphs)]
    y = 0
    while y < h:
        x = -int(rng.randint(0, 200))
        while x < w:
            t = tiles[rng.randint(0, len(tiles))]
            if rng.rand() < 0.5:
                t = t.transpose(Image.FLIP_LEFT_RIGHT)
            out.paste(ImageChops.lighter(out.crop((x, y, x + 256, y + 256)), t), (x, y))
            x += 256
        y += 256
    return out


def rounded_mask(size, inset, radius, scale=4):
    w, h = size
    return ss_draw(size, scale, lambda d, s: d.rounded_rectangle(
        [inset * s, inset * s, (w - inset) * s - 1, (h - inset) * s - 1], radius * s, fill=255))


def tint_ornament(src, light=(255, 238, 170)):
    """HD silver/white ornament -> gold, keeping its embossed shading."""
    a = arr(src)
    sh = lum(a)
    rgb = gold_map(mask_img(sh), dark=(92, 62, 20), mid=(206, 168, 80), light=light)
    return img(np.dstack([rgb, a[..., 3]]))


# ----------------------------------------------------------------------------------------------
# gold frame (vector, supersampled)
# ----------------------------------------------------------------------------------------------
def gold_frame(size, inset, radius, width, light_dir=(-1, -1)):
    """Antique-gold moulding (rounded rect band) with dark keylines and an inner hairline."""
    w, h = size
    outer = arr(rounded_mask(size, inset, radius))
    inner = arr(rounded_mask(size, inset + width, max(2, radius - width)))
    band = np.clip(outer - inner, 0, 1)
    mid_o = arr(rounded_mask(size, inset + width * 0.28, radius - width * 0.28))
    mid_i = arr(rounded_mask(size, inset + width * 0.58, radius - width * 0.58))
    ridge = arr(mask_img(np.clip(mid_o - mid_i, 0, 1)).filter(ImageFilter.GaussianBlur(width * 0.16)))
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    diag = (x / w) * light_dir[0] + (y / h) * light_dir[1]
    diag = (diag - diag.min()) / (diag.max() - diag.min())          # 1 = lit corner
    shade = 0.22 + 0.30 * diag + 0.50 * ridge
    rgb = gold_map(mask_img(np.clip(shade, 0, 1)))
    o_line = np.clip(outer - arr(rounded_mask(size, inset + 1.6, radius - 1.6)), 0, 1)
    i_line = np.clip(arr(rounded_mask(size, inset + width - 1.8, radius - width + 1.8)) - inner, 0, 1)
    key = np.clip(o_line + i_line, 0, 1)[..., None]
    rgb = rgb * (1 - key * 0.8) + np.array([0.09, 0.06, 0.02]) * key * 0.8
    # fine inner hairline, a few px inside the moulding (HD panel style)
    hi_o = arr(rounded_mask(size, inset + width + 5, radius - width - 5))
    hi_i = arr(rounded_mask(size, inset + width + 6.6, radius - width - 6.6))
    hair = np.clip(hi_o - hi_i, 0, 1) * 0.55
    hair_rgb = np.array([0.86, 0.76, 0.46])
    a = np.clip(band + hair, 0, 1)
    rgb = np.where(band[..., None] > 0.001, rgb, hair_rgb)
    return img(np.dstack([rgb, a]))


def twilight_particles(size, boxes, seed=3, n=16, smin=5, smax=15):
    """TP twilight 'pixels': small dark squares drifting up, slightly rotated (L mask, alpha)."""
    rng = np.random.RandomState(seed)
    sc = 4
    m = Image.new("L", (size[0] * sc, size[1] * sc), 0)
    d = ImageDraw.Draw(m)
    for _ in range(n):
        x0, y0, x1, y1 = boxes[rng.randint(0, len(boxes))]
        cx, cy = rng.uniform(x0, x1), rng.uniform(y0, y1)
        s = rng.uniform(smin, smax)
        ang = rng.uniform(-0.5, 0.5)
        val = int(rng.uniform(120, 255))
        pts = []
        for k in range(4):
            t = ang + k * math.pi / 2 + math.pi / 4
            pts.append(((cx + math.cos(t) * s * 0.707) * sc, (cy + math.sin(t) * s * 0.707) * sc))
        d.polygon(pts, fill=val)
    return m.resize(size, Image.LANCZOS)


def map_point(p, size, angle, scale):
    """Where point p of an image lands after Image.rotate(angle, expand=True) and a scale."""
    w, h = size
    t = math.radians(angle)
    c, s = math.cos(t), math.sin(t)
    nw = abs(w * c) + abs(h * s)
    nh = abs(w * s) + abs(h * c)
    dx, dy = p[0] - w / 2, p[1] - h / 2
    x = c * dx + s * dy + nw / 2
    y = -s * dx + c * dy + nh / 2
    return x * scale, y * scale


WOLF_EYE = (280, 307)            # in wolf_cutout() crop coordinates


def place_wolf(S, size, angle, scale, eye_at):
    """Wolf layer (RGBA) + its un-faded silhouette (L), rotated/scaled so the eye lands on eye_at."""
    ex, ey = map_point(WOLF_EYE, S["wolf"].size, angle, scale)
    pos = (round(eye_at[0] - ex), round(eye_at[1] - ey))
    out = []
    for im in (S["wolf"], S["wolf_sil"]):
        r = im.rotate(angle, resample=Image.BICUBIC, expand=True)
        r = r.resize((round(r.width * scale), round(r.height * scale)), Image.LANCZOS)
        if im.mode == "L":
            lay = Image.new("L", size, 0)
            lay.paste(r, pos)
        else:
            lay = Image.new("RGBA", size, (0, 0, 0, 0))
            lay.alpha_composite(r, pos)
        out.append(lay)
    return out[0], out[1]


def light_wolf(canvas, layer, glow, rim_rgb, rim=1.0, halo=0.55, aura=None, sil=None):
    """Composite the wolf: contact shadow, a dark aura where the neck dissolves (aura = 0..1 weight
    array), then a back-light rim scaled by the glow behind each edge (HD title-art look)."""
    wa = layer.split()[3]
    c = arr(canvas)
    sh = arr(wa.filter(ImageFilter.GaussianBlur(14)))
    c[..., :3] *= (1 - sh[..., None] * 0.5)
    if aura is not None:
        big = arr((sil or wa).filter(ImageFilter.MaxFilter(11)).filter(ImageFilter.GaussianBlur(14)))
        c[..., :3] *= (1 - np.clip(big * 1.4, 0, 1)[..., None] * aura[..., None] * 0.9)
    canvas = img(c)
    g = arr(glow)
    if aura is not None:
        g = g * (1 - aura)
    outer = arr(ImageChops.subtract(wa.filter(ImageFilter.MaxFilter(9)), wa)) * g
    canvas = add_glow(canvas, mask_img(outer), rim_rgb, 5, halo)
    canvas.alpha_composite(layer)
    edge = arr(ImageChops.subtract(wa, wa.filter(ImageFilter.MinFilter(5)))) * g
    canvas = add_glow(canvas, mask_img(edge), rim_rgb, 1.0, rim)
    return canvas


# ----------------------------------------------------------------------------------------------
# HD stamps: HD's own stamp-book box (box / box2 = selected) on HD's page parchment, with an HD
# stamp printed in HD's sepia ink. Every HD stamp texel is ink RGB; the drawing is in the alpha.
# ----------------------------------------------------------------------------------------------
INK = (88, 48, 32)
# banner row, left to right: (stamp id, ink size (longer side) as a fraction of the tile, tile
# size, selected)
#   7 = Hylian letter 'H' (for the 26 letter stamps), 30 = Link, 35 = Midna (centre, HD's gold
#   'selected' box), 34 = Wolf Link, 29 = Heart Container. The letter's ink is 27 px tall in HD;
#   0.355 x 76 px keeps it at 1:1 in the banner (its tile is drawn 2x supersampled, see ink_mask).
ROW = [(7, 0.355, 76, False), (30, 0.86, 84, False), (35, 0.80, 94, True), (34, 0.80, 84, False),
       (29, 0.66, 76, False)]
ROW_IDS = [r[0] for r in ROW]
ICON_STAMP = 35                  # icon badge: Midna (her helmet's two horns survive down to 56 px)
BADGE = True                     # --no-badge turns it off
BADGE_PX, BADGE_AT, BADGE_ANGLE, BADGE_WASH = 176, (400, 400), 6, 0.6


def box_silhouette(box):
    """Filled outline of HD's stamp box (its interior is a semi-transparent stipple): L mask."""
    a = box.split()[3].point(lambda v: 255 if v > 96 else 0)
    w, h = a.size
    big = Image.new("L", (w + 2, h + 2), 0)
    big.paste(a, (1, 1))
    ImageDraw.floodfill(big, (0, 0), 128)
    sil = big.crop((1, 1, w + 1, h + 1)).point(lambda v: 0 if v == 128 else 255)
    return sil.filter(ImageFilter.GaussianBlur(0.6))


def ink_mask(stamp, scale, threshold=0.5):
    """Alpha of an ink stamp at `scale` (float HxW). Downscales are plain Lanczos; an upscale (only
    the tiny Hylian letter, inside its 2x-supersampled tile) re-thresholds the edge so the 1-colour
    pixel line art stays crisp once the tile is shrunk back to 1:1."""
    a = stamp.split()[3]
    w, h = max(1, round(stamp.width * scale)), max(1, round(stamp.height * scale))
    b = arr(a.resize((w, h), Image.LANCZOS))
    if scale > 1.0:
        gain = max(scale * 0.9, 0.55 / threshold, 0.55 / (1 - threshold))
        b = np.clip((b - threshold) * gain + 0.5, 0, 1)
    return b


def ink_wash(stamp, amount, close=3):
    """A light ink wash inside the stamp's outline (holes filled), so the figure still reads as a
    solid shape once the icon is mipmapped down to 32-84 px. The line art itself is unchanged."""
    a = stamp.split()[3].point(lambda v: 255 if v > 60 else 0)
    a = a.filter(ImageFilter.MaxFilter(close * 2 + 1))
    w, h = a.size
    big = Image.new("L", (w + 2, h + 2), 255)
    big.paste(a.point(lambda v: 0 if v else 255), (1, 1))
    ImageDraw.floodfill(big, (0, 0), 128)
    holes = big.crop((1, 1, w + 1, h + 1)).point(lambda v: 255 if v == 255 else 0)
    sil = ImageChops.lighter(a, holes).filter(ImageFilter.MinFilter(close * 2 + 1))
    sil = arr(sil.filter(ImageFilter.GaussianBlur(0.7)))
    out = stamp.copy()
    out.putalpha(mask_img(np.maximum(arr(stamp.split()[3]), sil * amount)))
    return out


def stamp_tile(S, sid, T, ink_frac, selected=False, wash=0.0, ink=INK, paper_xy=(150, 130)):
    """One stamp-book box at T x T px (RGBA): parchment (native-scale crop of HD's page, toned to
    the book's cream), HD's box / box2 texture (231 px -> T, downscale only for T <= 231) and the
    stamp, centred on its ink bounding box, printed by multiplying HD's ink into the paper."""
    box = S["box2" if selected else "box"]
    sil = box_silhouette(box).resize((T, T), Image.LANCZOS)
    px, py = paper_xy
    p = arr(S["paper"].crop((px, py, px + T, py + T)))[..., :3]
    cream = np.array((246, 232, 196), np.float32) / 255
    p = p * 0.55 + cream * 0.45
    tile = img(np.dstack([p, arr(sil)]))
    tile.alpha_composite(box.resize((T, T), Image.LANCZOS))
    st = S["stamps"][sid]
    if wash:
        st = ink_wash(st, wash)
    bb = st.split()[3].getbbox()
    sc = ink_frac * T / max(bb[2] - bb[0], bb[3] - bb[1])         # fit the longer side
    m = ink_mask(st, sc, 0.45)
    cx, cy = (bb[0] + bb[2]) / 2 * sc, (bb[1] + bb[3]) / 2 * sc
    ox, oy = round(T / 2 - cx), round(T / 2 - cy)
    full = np.zeros((T, T), np.float32)                  # ink mask placed in tile space (clipped)
    h, w = m.shape
    x0, y0 = max(0, ox), max(0, oy)
    x1, y1 = min(T, ox + w), min(T, oy + h)
    full[y0:y1, x0:x1] = m[y0 - oy:y1 - oy, x0 - ox:x1 - ox]
    t = arr(tile)
    a = full[..., None]
    c = np.array(ink, np.float32) / 255
    t[..., :3] = np.minimum(t[..., :3] * (1 - a + a * c * 1.25), t[..., :3])   # ink soaks into paper
    return img(t)


def place_tile(canvas, tile, center, angle, ss, shadow_a=0.8, glow=None):
    """Rotate a tile drawn at ss x (supersampled), shrink it to 1x, drop it on the canvas centred at
    `center` with a soft contact shadow and an optional twilight back-glow (rgb, radius, strength)."""
    r = tile.rotate(angle, resample=Image.BICUBIC, expand=True)
    r = r.resize((round(r.width / ss), round(r.height / ss)), Image.LANCZOS)
    pos = (round(center[0] - r.width / 2), round(center[1] - r.height / 2))
    a = r.split()[3]
    W, H = canvas.size
    if glow is not None:
        g = Image.new("L", (W, H), 0)
        g.paste(a, pos)
        canvas = add_glow(canvas, g, glow[0], glow[1], glow[2])
    s = Image.new("L", (W, H), 0)
    s.paste(a.filter(ImageFilter.MaxFilter(3)), (pos[0] + 2, pos[1] + 6))
    s = arr(s.filter(ImageFilter.GaussianBlur(7)))
    c = arr(canvas)
    c[..., :3] *= (1 - s[..., None] * shadow_a)
    canvas = img(c)
    canvas.alpha_composite(r, pos)
    return canvas


# ----------------------------------------------------------------------------------------------
# icon
# ----------------------------------------------------------------------------------------------
def build_icon(S):
    N = 512
    R = 72                       # 8dp corner radius of a 56dp manager tile -> 73 px at 512
    size = (N, N)
    P = (306, 176)               # twilight portal centre (behind the ears)
    # 1. dark twilight base, lit from the portal, painted smoke from the HD Collection backdrop
    base = radial(size, P, 430, (18, 60, 62), (2, 5, 7), gamma=0.7)
    smoke = S["backdrop"].crop((690, 150, 690 + 600, 150 + 600)).resize(size, Image.LANCZOS)
    sl = lum(arr(smoke))
    base = base * (0.55 + 0.9 * sl[..., None])
    canvas = img(np.dstack([base, np.ones((N, N))]))
    # 2. Fused Shadow glyphs, faint, towards the edges
    gf = arr(glyph_field(S["glyphs"], size))
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    edge = np.clip((np.sqrt((xx - P[0]) ** 2 + (yy - P[1]) ** 2) - 150) / 200.0, 0, 1)
    ga = gf * (0.05 + 0.17 * edge)
    c = arr(canvas)
    c[..., :3] = c[..., :3] * (1 - ga[..., None]) + np.array([0.35, 0.85, 0.80]) * ga[..., None]
    canvas = img(c)
    # 3. portal glow + Mirror of Twilight rune ring
    glow = radial(size, P, 300, (255, 255, 255), (0, 0, 0), gamma=1.25)[..., 0]
    canvas = add_glow(canvas, mask_img(glow), (52, 196, 182), 0, 0.95)
    core = radial(size, P, 170, (255, 255, 255), (0, 0, 0), gamma=1.9)[..., 0]
    canvas = add_glow(canvas, mask_img(core), (190, 255, 236), 0, 0.75)
    ring = mirror_ring(S["mirror"]).resize((440, 440), Image.LANCZOS)
    rl = Image.new("L", size, 0)
    rl.paste(ring, (P[0] - 220, P[1] - 220))
    canvas = add_glow(canvas, rl, (60, 230, 210), 7, 0.75)
    c = arr(canvas)
    ra = arr(rl) * 0.62
    c[..., :3] = c[..., :3] * (1 - ra[..., None]) + np.array([0.78, 1.0, 0.94]) * ra[..., None]
    canvas = img(c)
    # 4. shadow smoke rolling in from the lower right: the wolf emerges from it
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    diag = ((xx - 330) * 0.62 + (yy - 300) * 0.78) / 150.0
    shade = np.clip(diag, 0, 1) ** 1.2 * (0.80 + 0.35 * (sl - sl.mean()))
    c = arr(canvas)
    c[..., :3] *= (1 - np.clip(shade, 0, 0.93)[..., None])
    canvas = img(c)
    # 5. Wolf Link (HD boot splash painting; rotated slightly, scaled 0.84 -> downscale only)
    layer, sil = place_wolf(S, size, -12, 0.84, (262, 246))
    aura = np.clip((xx - 330) / 110.0, 0, 1) * np.clip((420 - yy) / 120.0 + 0.6, 0, 1)
    canvas = light_wolf(canvas, layer, mask_img(glow), (120, 255, 236), rim=1.0, halo=0.7, aura=aura,
                        sil=sil)
    # 6. twilight particles: glowing squares drifting up off the wolf's shadowed back
    pm = twilight_particles(size, [(400, 60, 480, 140), (420, 150, 490, 250), (370, 30, 440, 80)],
                            seed=11, n=11, smin=5, smax=15)
    canvas = add_glow(canvas, pm, (90, 240, 220), 4, 0.8)
    c = arr(canvas)
    pa = arr(pm)[..., None] * 0.9
    c[..., :3] = c[..., :3] * (1 - pa) + np.array([0.70, 1.0, 0.93]) * pa
    canvas = img(c)
    # 7. vignette
    vig = radial(size, (N * 0.55, N * 0.42), N * 0.78, (255, 255, 255), (0, 0, 0), gamma=2.0)[..., 0]
    c = arr(canvas)
    c[..., :3] *= (0.45 + 0.55 * vig[..., None])
    canvas = img(c)
    # 8. clip + gold frame
    inset, fw = 3, 15
    clip = rounded_mask(size, inset + 3, R - inset - 3)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    canvas.putalpha(clip)
    out.alpha_composite(canvas)
    frame = gold_frame(size, inset, R - inset, fw)
    fsh = frame.split()[3].filter(ImageFilter.GaussianBlur(5))
    o = arr(out)
    o[..., :3] *= (1 - arr(fsh)[..., None] * 0.55)
    out = img(o)
    out.alpha_composite(frame)
    # HD filigree (Collection-screen corner ornament, tinted gold) on two opposite corners
    orn = tint_ornament(S["corner"])
    osz, oin = 80, 14
    orn = orn.resize((osz, osz), Image.LANCZOS)
    corners = [(orn.transpose(Image.ROTATE_180), (oin, oin))]
    if not BADGE:
        corners.insert(0, (orn, (N - osz - oin, N - osz - oin)))
    for o_img, pos in corners:
        oa = o_img.split()[3]
        s_ = Image.new("L", size, 0)
        s_.paste(oa.filter(ImageFilter.MaxFilter(3)), (pos[0] + 1, pos[1] + 2))
        s_ = arr(s_.filter(ImageFilter.GaussianBlur(2)))
        o = arr(out)
        o[..., :3] *= (1 - s_[..., None] * 0.8)
        out = img(o)
        out.alpha_composite(o_img, pos)
    if BADGE:
        # HD Stamps half of the mod: one stamp-book tile (selected gold box, Midna) in the lower
        # right, where cave-a had its second filigree, over the shadow the wolf emerges from
        ss = 1.6
        T = round(BADGE_PX * ss)
        tile = stamp_tile(S, ICON_STAMP, T, 0.80, selected=True, wash=BADGE_WASH, ink=(70, 36, 22))
        out = place_tile(out, tile, BADGE_AT, BADGE_ANGLE, ss, shadow_a=0.95,
                         glow=((60, 220, 200), 10, 0.55))
    # the manager clips the tile to its 8dp/56dp corner radius: apply the same clip here
    ui_clip = rounded_mask(size, 0, R + 1)
    out.putalpha(ImageChops.multiply(out.split()[3], ui_clip))
    return out


# ----------------------------------------------------------------------------------------------
# gold title lettering
# ----------------------------------------------------------------------------------------------
def text_mask(text, font_path, size, tracking=0.0, ss=4):
    """Supersampled text mask (L) with letter-spacing (in px at 1x), tightly cropped + padding."""
    f = ImageFont.truetype(font_path, size * ss)
    adv = [f.getlength(ch) + tracking * ss for ch in text]
    w = 0.0
    for a in adv:                                     # plain sum (Python 3.12+ sum() of floats is compensated)
        w += a
    w = int(w + size * ss)
    asc, desc = f.getmetrics()
    h = asc + desc + size * ss // 2
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    x = size * ss // 2
    for ch, a in zip(text, adv):
        d.text((x, size * ss // 4), ch, font=f, fill=255)
        x += a
    bb = m.getbbox()
    pad = 24 * ss
    m = m.crop((bb[0] - pad, bb[1] - pad, bb[2] + pad, bb[3] + pad))
    return m.resize((m.width // ss, m.height // ss), Image.LANCZOS), pad // ss


def gold_lettering(mask, light=(-0.55, -0.85), outline=2.2):
    """Metallic HD-gold fill with bevel + dark keyline. Returns (RGBA letters, L shadow mask)."""
    m = arr(mask)
    h, w = m.shape
    ys = np.where(m.max(1) > 0.5)[0]
    y0, y1 = (ys.min(), ys.max()) if len(ys) else (0, h)
    t = np.clip((np.arange(h, dtype=np.float32) - y0) / max(1, y1 - y0), 0, 1)[:, None]
    # vertical metal ramp: bright upper half, darker band below the middle, warm bounce at the foot
    ramp = 0.86 - 0.42 * np.clip((t - 0.25) / 0.45, 0, 1) + 0.18 * np.clip((t - 0.78) / 0.22, 0, 1)
    hm = arr(mask.filter(ImageFilter.GaussianBlur(1.6)))
    gy, gx = np.gradient(hm)
    bevel = -(gx * light[0] + gy * light[1]) * 2.8
    shade = np.clip(ramp + bevel, 0, 1)
    rgb = gold_map(mask_img(shade), dark=(84, 56, 18), mid=(206, 168, 82), light=(255, 246, 196))
    letters = img(np.dstack([rgb, m]))
    ol = mask.filter(ImageFilter.MaxFilter(3))
    if outline > 1.5:
        ol = ol.filter(ImageFilter.MaxFilter(3))
    ol = ol.filter(ImageFilter.GaussianBlur(0.6))
    out = solid(mask.size, (24, 16, 6), ol)
    out.alpha_composite(letters)
    return out, ol


def compose(canvas, layer, xy, shadow=None, shadow_off=(0, 4), shadow_blur=6, shadow_a=0.75,
          glow=None, glow_rgb=(70, 220, 200), glow_r=14, glow_a=0.45):
    """Composite a layer with optional soft drop shadow and teal glow (both from L masks)."""
    W, H = canvas.size
    if glow is not None:
        g = Image.new("L", (W, H), 0)
        g.paste(glow, xy)
        canvas = add_glow(canvas, g, glow_rgb, glow_r, glow_a)
    if shadow is not None:
        s = Image.new("L", (W, H), 0)
        s.paste(shadow, (xy[0] + shadow_off[0], xy[1] + shadow_off[1]))
        s = arr(s.filter(ImageFilter.GaussianBlur(shadow_blur)))
        c = arr(canvas)
        c[..., :3] *= (1 - s[..., None] * shadow_a)
        canvas = img(c)
    canvas.alpha_composite(layer, xy)
    return canvas


def gold_rule(length, thick=2.0, diamond=9, gap=0):
    """HD-style divider: tapered gold rule with a centre diamond (vector, supersampled). With a
    `gap` the middle is left open (for the logotype) and each half ends in its own diamond."""
    ss = 4
    W, H = length, diamond * 2 + 6
    m = Image.new("L", (W * ss, H * ss), 0)
    d = ImageDraw.Draw(m)
    cy = H * ss / 2
    half = W * ss / 2
    r = diamond * ss
    for side in (-1, 1):
        dc = half + side * (gap * ss / 2 + (r * 0.62 if gap else 0))     # this half's diamond
        pts = [(dc + side * r * 1.4 * (0.62 if gap else 1), cy - thick * ss / 2),
               (half + side * (half - 2), cy), (dc + side * r * 1.4 * (0.62 if gap else 1), cy + thick * ss / 2)]
        d.polygon(pts, fill=255)
        if gap or side == 1:
            d.polygon([(dc, cy - r), (dc + r * 0.62, cy), (dc, cy + r), (dc - r * 0.62, cy)], fill=255)
    mask = m.resize((W, H), Image.LANCZOS)
    y = np.linspace(0, 1, H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    rgb = gold_map(mask_img(0.85 - 0.45 * y))
    return img(np.dstack([rgb, arr(mask)])), mask


# ----------------------------------------------------------------------------------------------
# banner
# ----------------------------------------------------------------------------------------------
def ellipse_mask(size, cx, cy, rx, ry, ss=4):
    return ss_draw(size, ss, lambda d, s: d.ellipse([(cx - rx) * s, (cy - ry) * s,
                                                     (cx + rx) * s, (cy + ry) * s], fill=255))


def ring_band(ring, inner=0.80):
    """Keep only the outer rune band of the Mirror ring (radius >= inner)."""
    a = arr(ring)
    n = a.shape[0]
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    r = np.sqrt((x - n / 2 + 0.5) ** 2 + (y - n / 2 + 0.5) ** 2) / (n / 2)
    return mask_img(a * np.clip((r - inner) / 0.03, 0, 1))


# banner layout (1400x400 px). Dusklight shows it in a 724x180dp header: cover fit crops ~26 px
# off the top and bottom, it is opaque down to y ~165 and fades out by y ~374. Over it sit the five
# see-through action buttons (x 46-534, y 72-158) and the close X (x ~1276-1369, y 57-150).
BANNER_CX = 828                  # title + stamp-row axis
TITLE, TITLE_PX, TITLE_TRACK, TITLE_Y = "HD FEATURES", 66, 4, 46     # TITLE_Y = cap top
LOGO_W, LOGO_GAP, RULE_W = 272, 10, 540
ROW_GAP, ROW_SPACING, ROW_ARC, ROW_TILT = 10, 12, 3.0, 4.0
PORTAL = (296, 980, 140)         # rune-ring mouth: centre y, width, height
WOLF_SCALE, WOLF_EYE_AT = 0.72, (1200, 186)


def build_banner(S):
    W, H = 1400, 400
    size = (W, H)
    CX = BANNER_CX
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    # 1. base: near-black twilight teal, painted smoke from the HD Collection backdrop at 1:1
    smoke = S["backdrop"].crop((260, 330, 260 + W, 330 + H))
    sl = lum(arr(smoke))
    v = (yy / H)[..., None]
    base = np.array([9, 26, 29], np.float32) / 255 * (1 - v) + np.array([2, 5, 7], np.float32) / 255 * v
    base = base * (0.70 + 0.75 * sl[..., None])
    canvas = img(np.dstack([base, np.ones((H, W))]))
    # 2. Fused Shadow glyphs, faint teal, on the flanks
    gf = arr(glyph_field(S["glyphs"], size, seed=21))
    flank = np.clip((np.abs(xx - CX) - 250) / 450.0, 0, 1)
    ga = gf * (0.03 + 0.12 * flank) * (1 - 0.6 * (yy / H))
    crest = S["crest"].crop(S["crest"].split()[3].getbbox())
    cs = 330.0 / crest.height
    crest = crest.resize((round(crest.width * cs), 330), Image.LANCZOS)
    cpos = (270 - crest.width // 2, 205 - crest.height // 2)
    cfoot = Image.new("L", size, 0)
    cfoot.paste(crest.split()[3], cpos)
    ga = ga * (1 - np.clip(arr(cfoot.filter(ImageFilter.GaussianBlur(10))) * 1.6, 0, 0.85))
    c = arr(canvas)
    c[..., :3] = c[..., :3] * (1 - ga[..., None]) + np.array([0.35, 0.85, 0.80]) * ga[..., None]
    canvas = img(c)
    # 2b. the Twilight Princess emblem (Fused Shadow helmet + tribal wolf), faint, behind the buttons
    cm = arr(crest)
    cmask = cm[..., 3] * (0.35 + 0.65 * lum(cm))
    lay = Image.new("L", size, 0)
    lay.paste(mask_img(cmask * 0.22), cpos)
    canvas = add_glow(canvas, lay, (70, 220, 200), 3, 0.5)
    c = arr(canvas)
    la = arr(lay)[..., None]
    c[..., :3] = c[..., :3] * (1 - la) + np.array([0.45, 0.85, 0.80]) * la
    canvas = img(c)
    # 3. the twilight portal: Mirror of Twilight rune-ring mouth seen from above, the far-wall
    #    ledges of the cave floors below it, then the abyss (cave-a's shaft, lower and flatter so
    #    the stamp row can rise out of it)
    RY, RW, RH = PORTAL
    halo = np.exp(-(((xx - CX) / 560.0) ** 2 + ((yy - RY + 70) / 130.0) ** 2) * 1.3)
    canvas = add_glow(canvas, mask_img(halo), (34, 150, 142), 0, 0.75)
    ring = mirror_ring(S["mirror"])
    OPEN = 0.60                                       # inner edge of the mouth band (x radius)
    mouth_band = ring_band(ring, OPEN)
    floor_band = ring_band(ring, 0.86)
    inside = ellipse_mask(size, CX, RY, RW / 2 * OPEN, RH / 2 * OPEN).filter(ImageFilter.GaussianBlur(4))
    inside = arr(inside)
    depth = np.clip(1 - (((xx - CX) / (RW / 2 * OPEN)) ** 2 + ((yy - RY - 12) / (RH / 2 * OPEN)) ** 2),
                    0, 1) ** 0.6
    c = arr(canvas)
    c[..., :3] *= (1 - inside[..., None] * (0.45 + 0.50 * depth[..., None]))      # dark well
    canvas = img(c)
    lay = Image.new("L", size, 0)
    ow, oh = RW * OPEN, RH * OPEN
    floors = [(0.96, 9, 0.70), (0.80, 17, 0.50), (0.66, 24, 0.34), (0.54, 30, 0.22), (0.44, 35, 0.13)]
    for f, dy, a in floors:
        rw, rh = round(ow * f), round(oh * f)
        r = floor_band.resize((rw, rh), Image.LANCZOS)
        t = Image.new("L", size, 0)
        t.paste(r, (CX - rw // 2, RY + dy - rh // 2))
        far = np.clip((RY + dy - yy) / (rh * 0.22) + 0.45, 0, 1)    # far wall seen, near wall hidden
        lay = ImageChops.lighter(lay, mask_img(arr(t) * far * a))
    canvas = add_glow(canvas, lay, (60, 220, 200), 3, 0.6)
    c = arr(canvas)
    la = arr(lay)[..., None]
    c[..., :3] = c[..., :3] * (1 - la) + np.array([0.55, 0.92, 0.85]) * la
    canvas = img(c)
    mouth = Image.new("L", size, 0)
    mouth.paste(mouth_band.resize((RW, RH), Image.LANCZOS), (CX - RW // 2, RY - RH // 2))
    mouth = mask_img(arr(mouth) * 0.85)
    canvas = add_glow(canvas, mouth, (60, 230, 210), 7, 0.95)
    c = arr(canvas)
    ma = arr(mouth)[..., None]
    c[..., :3] = c[..., :3] * (1 - ma) + np.array([0.80, 1.0, 0.95]) * ma
    canvas = img(c)
    # shadow rolling in from the lower right, where the wolf's body would be
    sh = np.clip(((xx - 1150) * 0.5 + (yy - 290) * 0.87) / 100.0, 0, 1)
    sh = np.maximum(sh, np.clip((xx - 1290) / 100.0, 0, 1) * 0.9)
    c = arr(canvas)
    c[..., :3] *= (1 - 0.85 * sh[..., None])
    canvas = img(c)
    # 4. Wolf Link on the right, looking toward the title, back-lit by the twilight
    glow = np.exp(-(((xx - 1090) / 240.0) ** 2 + ((yy - 110) / 180.0) ** 2))
    canvas = add_glow(canvas, mask_img(glow * (1 - sh)), (40, 176, 164), 0, 0.85)
    layer, sil = place_wolf(S, size, -8, WOLF_SCALE, WOLF_EYE_AT)
    aura = np.clip((xx - 1265) / 80.0, 0, 1) * np.clip((340 - yy) / 120.0 + 0.6, 0, 1)
    canvas = light_wolf(canvas, layer, mask_img(glow), (120, 255, 236), rim=1.3, halo=0.75,
                        aura=aura, sil=sil)
    # 5. twilight particles drifting up off the wolf
    pm = twilight_particles(size, [(1300, 20, 1390, 150), (1240, 70, 1300, 140), (330, 215, 420, 260),
                                   (1180, 250, 1240, 290)], seed=17, n=11, smin=5, smax=12)
    canvas = add_glow(canvas, pm, (90, 240, 220), 4, 0.7)
    c = arr(canvas)
    pa = arr(pm)[..., None] * 0.85
    c[..., :3] = c[..., :3] * (1 - pa) + np.array([0.70, 1.0, 0.93]) * pa
    vig = np.clip(1 - ((xx - CX) / 950.0) ** 2 - ((yy - 160) / 430.0) ** 2, 0, 1)
    c[..., :3] *= (0.50 + 0.50 * vig[..., None])
    canvas = img(c)
    # 6. title: "HD FEATURES" (Perpetua Titling MT Bold, HD gold), gold rule, official logotype
    m1, p1 = text_mask(TITLE, F_TITLE, TITLE_PX, tracking=TITLE_TRACK)
    l1, o1 = gold_lettering(m1)
    y_title = TITLE_Y
    canvas = compose(canvas, l1, (CX - l1.width // 2, y_title - p1), shadow=o1, glow=m1, glow_a=0.42,
                     glow_r=18)
    tp = S["tphd"]
    tp = tp.crop(tp.split()[3].getbbox())
    sc = float(LOGO_W) / tp.width
    tp = tp.resize((LOGO_W, round(tp.height * sc)), Image.LANCZOS)
    y_tp = y_title + m1.height - 2 * p1 + LOGO_GAP
    # gold rule split around the logotype: --<> Twilight Princess HD <>--
    rule, rmask = gold_rule(RULE_W, thick=2.0, diamond=6, gap=LOGO_W + 26)
    y_rule = y_tp + tp.height // 2 - rule.height // 2 + 2
    canvas = compose(canvas, rule, (CX - rule.width // 2, y_rule), shadow=rmask, shadow_off=(0, 2),
                     shadow_blur=3)
    canvas = compose(canvas, tp, (CX - tp.width // 2 + 4, y_tp), shadow=tp.split()[3],
                     shadow_off=(0, 3), shadow_blur=5, shadow_a=0.8)
    # 7. the HD Stamps half: a fan of five stamp-book tiles rising out of the portal light,
    #    Hylian H, Link, Midna (in HD's gold 'selected' box), Wolf Link, Heart Container
    ss = 2
    top = y_tp + tp.height + ROW_GAP
    big = max(t for _, _, t, _ in ROW)
    widths = [t for _, _, t, _ in ROW]
    total = sum(widths) + ROW_SPACING * (len(ROW) - 1)
    x = CX - total / 2.0
    n = len(ROW)
    for i, (sid, frac, T, sel) in enumerate(ROW):
        k = i - (n - 1) / 2.0                          # -2 .. 2
        cx = x + T / 2.0
        cy = top + big / 2.0 + ROW_ARC * k * k
        tile = stamp_tile(S, sid, T * ss, frac, selected=sel, paper_xy=(120 + 37 * i, 110 + 23 * i))
        canvas = place_tile(canvas, tile, (cx, cy), -ROW_TILT * k, ss, shadow_a=0.85,
                            glow=((60, 220, 200), 12, 0.65 if sel else 0.45))
        x += T + ROW_SPACING
    # a few twilight motes rising past the stamps
    pm = twilight_particles(size, [(CX - total / 2 - 50, top + 30, CX - total / 2 - 10, top + 90),
                                   (CX + total / 2 + 10, top + 20, CX + total / 2 + 45, top + 80)],
                            seed=29, n=6, smin=5, smax=10)
    canvas = add_glow(canvas, pm, (90, 240, 220), 4, 0.7)
    c = arr(canvas)
    pa = arr(pm)[..., None] * 0.85
    c[..., :3] = c[..., :3] * (1 - pa) + np.array([0.70, 1.0, 0.93]) * pa
    canvas = img(c)
    return canvas


# ----------------------------------------------------------------------------------------------
# preview: the Dusklight v2.0.3 Mods window, laid out from res/rml/{window,mods,controls,theme}.rcss
#   window max 1088x768dp, border 2dp #928761, radius 14dp, bg rgba(21,22,16,.9), no tab bar
#   mod-list pane flex 0 0 360dp (border-box, 16dp padding); entries: 56dp icon (radius 8dp,
#   fill, 20 % control-colour backing), 10dp padding, 12dp gap; selected = accent gradient 16->64
#   detail pane 724dp: mod-header 180dp, banner 'cover' + image-fade-start 0.4 (desaturated when
#   the mod is disabled); mod-actions: 44dp icon buttons at (24dp, 24dp), gap 8dp, radius 14dp,
#   fill rgba(17,16,10,.2), 1dp rgba(border,.25) ring, opacity .9 -> Reload, Disable, Logs, Open
#   folder, Uninstall for an enabled installed .dusk (Enable, Logs, Open folder, Uninstall when
#   disabled); window close: 48dp at 16dp from the top-right, glyph 24dp, 2dp backdrop blur
#   rendered at 1.5 px/dp (UI scale 1.5) and 1 px/dp
# ----------------------------------------------------------------------------------------------
TXT = (224, 219, 200)
ACCENT = (194, 164, 45)
BORDER = (146, 135, 91)
WIN_BG = (23, 24, 18)               # rgba(21,22,16,.9) over the blurred game
GAME_BG = (38, 42, 40)
GREEN = (68, 204, 85)
G_REFRESH, G_PAUSE, G_PLAY, G_LOGS, G_FOLDER, G_DELETE, G_CLOSE, G_PUZZLE, G_CLOUD = (
    "", "", "", "", "", "", "", "", "")
FALLBACK_DESC = ("Adds two Twilight Princess HD features: the 50 HD stamps with HD's stamp book, "
                 "and the Cave of Shadows, the 41-floor Wolf Link amiibo dungeon. Turn each one on "
                 "or off in this mod's settings.")


def _font(name, px):
    return ImageFont.truetype(os.path.join(DUSK, "res", name), max(1, round(px)))


def _text(canvas, xy, s, font, rgb, a=1.0, anchor="la"):
    lay = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(lay).text(xy, s, font=font, fill=tuple(rgb) + (round(255 * a),), anchor=anchor)
    canvas.alpha_composite(lay)


def _wrap(s, font, width, max_lines=None):
    words, lines, cur = s.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if font.getlength(t) <= width:
            cur = t
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    if max_lines and len(lines) > max_lines:
        lines = lines[:max_lines]
        while font.getlength(lines[-1] + "...") > width:
            lines[-1] = lines[-1][:-1]
        lines[-1] = lines[-1].rstrip() + "..."
    return lines


def _rrect(size, box, r, fill, outline=None, width=1, ss=3):
    m = Image.new("RGBA", (size[0] * ss, size[1] * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([v * ss for v in box], r * ss, fill=fill, outline=outline,
                        width=max(1, round(width * ss)) if outline else 0)
    return m.resize(size, Image.LANCZOS)


def _this_mod():
    """(name, author, version, description) of the HD Features mod, from mods/hdfeatures if the
    sources exist yet (mod.json.in + CMakeLists.txt), else a built-in stand-in."""
    import json
    import re
    base = os.path.join(ROOT, "mods", "hdfeatures")
    name, author, ver, desc = "HD Features", "Ricky", "1.0.0", FALLBACK_DESC
    try:
        with open(os.path.join(base, "mod.json.in"), encoding="utf-8") as f:
            d = json.loads(re.sub(r"@[A-Za-z0-9_]+@", "0", f.read()))
        name, author = d.get("name", name), d.get("author", author)
        desc = d.get("description", desc) or desc
    except (OSError, ValueError):
        pass
    try:
        with open(os.path.join(base, "CMakeLists.txt"), encoding="utf-8") as f:
            m = re.search(r'set\(\s*\w*VERSION\w*\s+"([0-9][^"]*)"', f.read())
        ver = m.group(1) if m else ver
    except OSError:
        pass
    return name, author, ver, desc


def _installed_mod(filename):
    """Metadata of one of the user's installed .dusk mods (opened read-only), for list context."""
    import json
    import zipfile
    path = os.path.join(os.environ.get("APPDATA", ""), "TwilitRealm", "Dusklight", "mods", filename)
    try:
        with zipfile.ZipFile(path) as z:
            d = json.loads(z.read("mod.json"))
        return d.get("name", filename), d.get("author", ""), d.get("version", "?"), d.get("description", "")
    except (OSError, KeyError, ValueError, zipfile.BadZipFile):
        return None


def mip_resize(im, size):
    """Approximates the engine's mipmapped sampling: box-filtered mip chain + bilinear."""
    cur = im.convert("RGBa")
    while cur.width // 2 >= size[0] and cur.height // 2 >= size[1]:
        cur = cur.reduce(2)
    return cur.resize(size, Image.BILINEAR).convert("RGBA")


def _gray(im):
    a = arr(im)
    a[..., :3] = lum(a)[..., None]
    return img(a)


def _icon_tile(icon, px, radius, desat=False):
    t = mip_resize(icon, (px, px))
    if desat:
        t = _gray(t)
    m = _rrect((px, px), (0, 0, px - 1, px - 1), radius, (255, 255, 255, 255)).split()[3]
    bg = Image.new("RGBA", (px, px), (17, 16, 10, 51))
    bg.alpha_composite(t)
    bg.putalpha(ImageChops.multiply(bg.split()[3], m))
    return bg


def _placeholder_tile(px, radius, K, glyph=G_PUZZLE, rgb=TXT, a=0.45):
    t = _rrect((px, px), (0, 0, px - 1, px - 1), radius, (17, 16, 10, 51))
    _text(t, (px / 2, px / 2), glyph, _font("MaterialSymbolsRounded-Regular.ttf", 36 * K), rgb, a, "mm")
    return t


def mod_header(banner, K, pw, under, inactive=False, close_dx=None):
    """The detail pane's mod-header (pw x 180dp) drawn over `under` (the window surface behind it):
    cover-fit banner with the 40 % fade, the action buttons and (if close_dx is given: x of the
    close button's centre inside the header) the window's close X."""
    dp = lambda v: round(v * K)                                           # noqa: E731
    hh = dp(180)
    hdr = under.copy()
    s = max(pw / banner.width, hh / banner.height)
    b = mip_resize(banner, (round(banner.width * s), round(banner.height * s)))
    b = b.crop(((b.width - pw) // 2, (b.height - hh) // 2, (b.width - pw) // 2 + pw, (b.height - hh) // 2 + hh))
    a = arr(b)
    fade = np.clip((hh - (np.arange(hh, dtype=np.float32) + 0.5)) / (hh - 0.4 * hh), 0, 1)[:, None]
    a[..., 3] *= fade
    if inactive:
        a[..., :3] = lum(a)[..., None]
    hdr.alpha_composite(img(a))
    ficon = _font("MaterialSymbolsRounded-Regular.ttf", 28 * K)
    glyphs = ((G_PLAY,) if inactive else (G_REFRESH, G_PAUSE)) + (G_LOGS, G_FOLDER, G_DELETE)
    bx, by = dp(24), dp(24)
    for gl in glyphs:
        lay = Image.new("RGBA", hdr.size, (0, 0, 0, 0))
        btn = _rrect((dp(44), dp(44)), (0, 0, dp(44) - 1, dp(44) - 1), dp(14), (17, 16, 10, 51),
                     BORDER + (64,), K)
        lay.alpha_composite(btn, (bx, by))
        _text(lay, (bx + dp(22), by + dp(22)), gl, ficon, TXT, 1.0, "mm")
        la = arr(lay)
        la[..., 3] *= 0.9                                                 # button opacity 0.9
        hdr.alpha_composite(img(la))
        bx += dp(44 + 8)
    if close_dx is not None:
        cx, cy, r = close_dx, dp(16 + 24), dp(24)
        box = (cx - r, cy - r, cx + r, cy + r)
        hdr.paste(hdr.crop(box).filter(ImageFilter.GaussianBlur(dp(2))), box[:2])
        _text(hdr, (cx, cy), G_CLOSE, _font("MaterialSymbolsRounded-Regular.ttf", 24 * K), TXT, 0.9, "mm")
    return hdr


def mods_window(icon, banner, K=1.5, inactive=False):
    name, author, ver, desc = _this_mod()
    dp = lambda v: round(v * K)                                           # noqa: E731
    WW, WH = dp(1088), dp(768)
    pad = dp(28)
    canvas = Image.new("RGBA", (WW + 2 * pad, WH + 2 * pad), GAME_BG + (255,))
    win = _rrect((WW, WH), (0, 0, WW - 1, WH - 1), dp(14), WIN_BG + (255,), BORDER + (255,), 2 * K)
    canvas.alpha_composite(win, (pad, pad))
    ox, oy = pad + dp(2), pad + dp(2)
    list_w = dp(360)
    # --- list pane ---------------------------------------------------------------------------
    d = ImageDraw.Draw(canvas)
    d.line([(ox + list_w, oy), (ox + list_w, oy + WH - dp(4))], fill=BORDER, width=max(1, round(K)))
    fb = _font("FiraSans-Bold.ttf", 20 * K)
    fr13 = _font("FiraSans-Regular.ttf", 13 * K)
    fr14 = _font("FiraSans-Regular.ttf", 14 * K)
    x, y = ox + dp(16), oy + dp(16)
    ew = list_w - dp(32)
    canvas.alpha_composite(_placeholder_tile(dp(56), dp(8), K, G_CLOUD, ACCENT, 1.0), (x + dp(10), y + dp(10)))
    _text(canvas, (x + dp(78), y + dp(18)), "Online mods", fb, TXT)
    for i, line in enumerate(_wrap("Download community mods and check for updates.", fr13, ew - dp(90))):
        _text(canvas, (x + dp(78), y + dp(44) + i * dp(17)), line, fr13, TXT, 0.5)
    y += dp(76) + dp(4)
    d.line([(x + dp(10), y + dp(8)), (x + ew - dp(10), y + dp(8))], fill=(56, 54, 40), width=max(1, round(K)))
    y += dp(17) + dp(4)
    entries = [(name, author, ver, desc, "Disabled" if inactive else "Enabled", icon, True)]
    dawn = _installed_mod("dev.bezide.dawnlight.dusk")
    if dawn:
        entries.append(dawn + ("Enabled", None, False))
    for nm, au, v, dsc, status, ic, sel in entries:
        eh = dp(10) + max(dp(56), dp(26 + 18 + 2 * 18.2)) + dp(10)
        if sel:
            ga = np.linspace(16, 64, eh, dtype=np.float32)[:, None] * np.ones((1, ew), np.float32)
            g = img(np.dstack([np.ones((eh, ew, 3)) * np.array(ACCENT) / 255, ga / 255]))
            m = _rrect((ew, eh), (0, 0, ew - 1, eh - 1), dp(10), (255, 255, 255, 255)).split()[3]
            g.putalpha(ImageChops.multiply(g.split()[3], m))
            canvas.alpha_composite(g, (x, y))
            canvas.alpha_composite(_rrect((ew, eh), (0, 0, ew - 1, eh - 1), dp(10), None,
                                          BORDER + (102,), K), (x, y))
        tile = _icon_tile(ic, dp(56), dp(8), desat=inactive) if ic is not None else \
            _placeholder_tile(dp(56), dp(8), K)
        canvas.alpha_composite(tile, (x + dp(10), y + dp(10)))
        tx = x + dp(10 + 56 + 12)
        dim = 0.5 if (inactive and sel) else 1.0
        _text(canvas, (tx, y + dp(8)), nm, fb, TXT, dim)
        meta = "%s  ·  v%s  ·  " % (au, v)
        _text(canvas, (tx, y + dp(36)), meta, fr13, TXT, 0.5 * dim)
        col = TXT if status == "Disabled" else GREEN
        _text(canvas, (tx + fr13.getlength(meta), y + dp(36)), status, fr13, col, dim)
        for i, line in enumerate(_wrap(dsc, fr14, ew - dp(10 + 56 + 12 + 10), 2)):
            _text(canvas, (tx, y + dp(56) + i * dp(18.2)), line, fr14, TXT, 0.65 * dim)
        y += eh + dp(4)
    # --- detail pane: banner header ----------------------------------------------------------
    px0 = ox + list_w + max(1, round(K))
    pw = pad + WW - dp(2) - px0
    hh = dp(180)
    close_dx = (pad + WW - dp(16) - dp(24)) - px0
    hdr = mod_header(banner, K, pw, canvas.crop((px0, oy, px0 + pw, oy + hh)), inactive, close_dx)
    # round the window's top-right corner
    wm = Image.new("L", canvas.size, 0)
    wm.paste(win.split()[3], (pad, pad))
    lay = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    lay.paste(hdr, (px0, oy))
    lay.putalpha(ImageChops.multiply(lay.split()[3], wm))
    canvas.alpha_composite(lay)
    # title, author, description
    tx, ty = px0 + dp(24), oy + hh + dp(12)
    ft = _font("FiraSans-Bold.ttf", 28 * K)
    _text(canvas, (tx, ty), name + " ", ft, TXT)
    _text(canvas, (tx + ft.getlength(name + " "), ty + dp(10)), "v" + ver,
          _font("FiraSans-Regular.ttf", 16 * K), TXT)
    ty += dp(28 * 1.3 + 8)
    by = "by %s · " % author
    _text(canvas, (tx, ty), by, fr14, TXT)
    _text(canvas, (tx + fr14.getlength(by), ty), "Disabled" if inactive else "Enabled",
          fr14, TXT if inactive else GREEN)
    ty += dp(14 * 1.4 + 8)
    f20 = _font("FiraSans-Regular.ttf", 20 * K)
    for i, line in enumerate(_wrap(desc, f20, pw - dp(48))[:8]):
        _text(canvas, (tx, ty + i * dp(20 * 1.4)), line, f20, TXT, 0.85)
    return canvas


def header_only(banner, K, inactive=False):
    """Just the detail header at its real size (724dp wide) with the buttons and the close X,
    cropped out of the window mock so every piece lines up exactly."""
    w = mods_window(Image.new("RGBA", (8, 8)), banner, K, inactive)
    pad = round(28 * K)
    x0 = pad + round(2 * K) + round(360 * K) + max(1, round(K))
    return w.crop((x0, pad, pad + round(1088 * K), pad + round(2 * K) + round(180 * K)))


def icon_strip(icon, K=1.5):
    """Icon size checks on the window colour: the list tile at 1.5 px/dp (84 px) and 1 px/dp
    (56 px), the 42dp install toast, 32 px, the disabled (desaturated) look, and 32 px zoomed."""
    big = round(56 * K)
    items = [(big, "84 px: list tile at 150 %", False), (56, "56 px: list tile at 100 %", False),
             (42, "42 px: install toast", False), (32, "32 px", False),
             (big, "84 px, mod disabled (desaturated)", True), (56, "56 px, mod disabled", True)]
    W, H = 470, sum(px + 22 for px, _, _ in items) + 2 * big + 60
    st = Image.new("RGBA", (W, H), WIN_BG + (255,))
    f = _font("FiraSans-Regular.ttf", 16)
    y = 24
    for px, lab, des in items:
        st.alpha_composite(_icon_tile(icon, px, max(2, round(px * 8 / 56)), desat=des), (24 + (big - px) // 2, y))
        _text(st, (big + 48, y + px // 2), lab, f, TXT, 0.7, "lm")
        y += px + 22
    z = Image.new("RGBA", (32, 32), WIN_BG + (255,))
    z.alpha_composite(_icon_tile(icon, 32, 5))
    st.alpha_composite(z.resize((big * 2, big * 2), Image.NEAREST), (24, y))
    _text(st, (2 * big + 48, y + big), "32 px, zoomed", f, TXT, 0.7, "lm")
    return st


def build_preview(icon, banner):
    K = 1.5
    w1 = mods_window(icon, banner, K)
    st = icon_strip(icon, K)
    w2 = mods_window(icon, banner, 1.0)
    h15 = header_only(banner, K, inactive=True)
    h10 = header_only(banner, 1.0, inactive=True)
    W = max(w1.width, st.width + w2.width + 20)
    f = _font("FiraSans-Bold.ttf", 22)
    top = w1.height + 50
    mid = top + max(st.height, w2.height) + 20
    H = mid + 44 + h15.height + 24 + 44 + h10.height + 30
    out = Image.new("RGBA", (W, H), (14, 15, 12, 255))
    out.alpha_composite(w1, (0, 0))
    _text(out, (20, w1.height + 12), "Above: Mods window at 1.5 px/dp (UI scale 1.5), enabled.   "
          "Below: icon sizes  |  the same window at 1 px/dp.", f, TXT, 0.7)
    out.alpha_composite(st, (0, top))
    out.alpha_composite(w2, (W - w2.width, top))
    _text(out, (20, mid + 10), "Mod disabled: header at 1.5 px/dp (Enable, Logs, Open folder, Uninstall; "
          "the manager desaturates the banner)", f, TXT, 0.7)
    out.alpha_composite(h15, (20, mid + 44))
    y = mid + 44 + h15.height + 24
    _text(out, (20, y + 10), "Mod disabled: header at 1 px/dp", f, TXT, 0.7)
    out.alpha_composite(h10, (20, y + 44))
    return out


# ----------------------------------------------------------------------------------------------
def _arg(argv, name):
    if name in argv:
        i = argv.index(name)
        if i + 1 >= len(argv):
            raise SystemExit("%s needs a value" % name)
        return argv[i + 1]
    return None


def main(argv):
    global CACHE, BADGE, HD, DUSK, F_TITLE
    if "--cache" in argv:
        CACHE = _arg(argv, "--cache")
    if "--no-badge" in argv:
        BADGE = False
    hd = _arg(argv, "--hd")
    if not hd:
        raise SystemExit("usage: make.py --hd <TP HD content folder> [--out DIR] [--dusklight DIR] [--font TTF]")
    hd = os.path.abspath(hd)
    HD = hd if os.path.basename(hd.rstrip("/\\")).lower() == "res" else os.path.join(hd, "res")
    if not os.path.isdir(HD):
        raise SystemExit("--hd: %s has no res folder" % hd)
    DUSK = _arg(argv, "--dusklight")
    out_dir = _arg(argv, "--out")
    F_TITLE = find_title_font(_arg(argv, "--font"))
    print("title font", F_TITLE)
    target = os.path.abspath(out_dir) if out_dir else HERE
    os.makedirs(target, exist_ok=True)
    S = load_sources()
    S["wolf"], S["wolf_sil"] = wolf_cutout(S["boot"])
    only = "icon" if "--icon-only" in argv else "banner" if "--banner-only" in argv else None
    if only in (None, "icon"):
        icon = build_icon(S)
        icon.save(os.path.join(target, "icon.png"))
        print("icon.png", icon.size)
    if only in (None, "banner"):
        banner = build_banner(S)
        banner.save(os.path.join(target, "banner.png"))
        print("banner.png", banner.size)
    if only is None and not out_dir and DUSK:
        prev = build_preview(icon, banner).convert("RGB")
        prev.save(os.path.join(HERE, "preview.png"))
        print("preview.png", prev.size)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
