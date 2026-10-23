#!/usr/bin/env python3
"""base_art.py - the plain, original Mods-menu art of the HD Features base package, drawn entirely by code.

  icon.png    512x512   a dusk-coloured tile with a cream postage stamp; on the stamp, a stepped cave arch
                        glowing from inside and a small four-point sparkle
  banner.png 1400x400   the same dusk gradient, three plain stamps (sparkle, cave arch, crescent) and the title
                        "HD FEATURES" / "Stamps + Cave of Shadows"

No game textures, logos, characters or symbols from any game are used: only gradients, rectangles, circles and
polygons, and two free fonts (SIL Open Font License) that ship with Dusklight: Alegreya SC Bold (title) and
Fira Sans (subtitle). Needs Pillow. It drew mods/hdfeatures/package/res/icon.png and banner.png (pinned in
tools/dist/make_release.py PINNED_RES).

usage: python -I base_art.py --fonts <folder with AlegreyaSC-Bold.ttf and FiraSans-Regular.ttf> --out <folder>
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 3                                        # supersampling factor
TEAL = (16, 74, 84)
INDIGO = (30, 22, 66)
DEEP = (12, 10, 30)
CREAM = (244, 234, 211)
INK = (110, 78, 44)
GLOW = (110, 236, 214)
GOLD = (240, 214, 150)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def gradient(w, h, top, bottom, diagonal=0.0):
    """Vertical (or slanted) two-colour gradient."""
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = (y / max(1, h - 1)) * (1 - diagonal) + (x / max(1, w - 1)) * diagonal
            px[x, y] = lerp(top, bottom, max(0.0, min(1.0, t)))
    return im


def radial_glow(w, h, cx, cy, r, color, strength):
    """RGBA layer: a soft round glow."""
    layer = Image.new('RGBA', (w, h), color + (0,))
    m = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(m)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=int(255 * strength))
    m = m.filter(ImageFilter.GaussianBlur(r * 0.45))
    layer.putalpha(m)
    return layer


def sparkle(d, cx, cy, r, fill):
    """Four-point star."""
    k = r * 0.22
    pts = [(cx, cy - r), (cx + k, cy - k), (cx + r, cy), (cx + k, cy + k), (cx, cy + r), (cx - k, cy + k),
           (cx - r, cy), (cx - k, cy - k)]
    d.polygon(pts, fill=fill)


def stamp(size_w, size_h, motif, s):
    """A cream postage stamp (perforated edge, inner frame, dusk field, motif) as RGBA, at scale s."""
    W, H = int(size_w * s), int(size_h * s)
    pad = int(14 * s)
    im = Image.new('RGBA', (W + 2 * pad, H + 2 * pad), (0, 0, 0, 0))
    body = Image.new('L', im.size, 0)
    d = ImageDraw.Draw(body)
    d.rectangle((pad, pad, pad + W, pad + H), fill=255)
    hole = 6.5 * s
    step = 19 * s
    nx = max(2, int(W / step))
    ny = max(2, int(H / step))
    for i in range(nx + 1):                    # perforation: round bites along every edge
        x = pad + W * i / nx
        for y in (pad, pad + H):
            d.ellipse((x - hole, y - hole, x + hole, y + hole), fill=0)
    for j in range(ny + 1):
        y = pad + H * j / ny
        for x in (pad, pad + W):
            d.ellipse((x - hole, y - hole, x + hole, y + hole), fill=0)
    paper = Image.new('RGBA', im.size, CREAM + (255,))
    im.paste(paper, (0, 0), body)
    # inner field
    m = int(22 * s)
    fx0, fy0, fx1, fy1 = pad + m, pad + m, pad + W - m, pad + H - m
    field = gradient(fx1 - fx0, fy1 - fy0, (44, 120, 128), (52, 40, 104))
    im.paste(field, (fx0, fy0))
    d2 = ImageDraw.Draw(im)
    d2.rectangle((fx0 - 4 * s, fy0 - 4 * s, fx1 + 4 * s, fy1 + 4 * s), outline=INK + (255,), width=max(1, int(3 * s)))
    cx, cy = (fx0 + fx1) / 2, (fy0 + fy1) / 2
    fw, fh = fx1 - fx0, fy1 - fy0
    if motif == 'arch':
        _arch(im, cx, fy1, fw, fh, s)
        sparkle(d2, cx + fw * 0.26, fy0 + fh * 0.2, fh * 0.1, GOLD + (255,))
    elif motif == 'sparkle':
        glow = radial_glow(im.width, im.height, int(cx), int(cy), int(fh * 0.34), GLOW, 0.55)
        im.alpha_composite(glow)
        sparkle(ImageDraw.Draw(im), cx, cy, fh * 0.3, GOLD + (255,))
        sparkle(ImageDraw.Draw(im), cx + fw * 0.28, cy - fh * 0.26, fh * 0.08, CREAM + (255,))
    elif motif == 'moon':
        r = fh * 0.26
        off = r * 0.55
        mask = Image.new('L', im.size, 0)
        md = ImageDraw.Draw(mask)
        md.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
        md.ellipse((cx - r + off, cy - r - off * 0.35, cx + r + off, cy + r - off * 0.35), fill=0)
        im.paste(Image.new('RGBA', im.size, GOLD + (255,)), (0, 0), mask)
        dd = ImageDraw.Draw(im)
        sparkle(dd, cx - fw * 0.25, cy + fh * 0.22, fh * 0.06, CREAM + (255,))
        sparkle(dd, cx + fw * 0.22, cy + fh * 0.12, fh * 0.045, CREAM + (255,))
    # clip everything to the stamp body
    a = im.getchannel('A')
    im.putalpha(Image.composite(a, Image.new('L', im.size, 0), body))
    return im


def _arch(im, cx, base_y, fw, fh, s):
    """A stepped rock mass with a round-topped doorway glowing from inside."""
    d = ImageDraw.Draw(im)
    steps = 4
    top = base_y - fh * 0.78
    half = fw * 0.46
    pts = [(cx - half, base_y)]
    for i in range(steps):                     # left side: steps going up
        x = cx - half + (half * 0.62) * (i + 1) / steps
        y_hi = base_y - (base_y - top) * (i + 1) / steps
        pts += [(cx - half + (half * 0.62) * i / steps, y_hi), (x, y_hi)]
    pts += [(cx + half * 0.38, top)]
    for i in range(steps):                     # right side: steps going down
        x = cx + half * 0.38 + (half * 0.62) * i / steps
        y_lo = top + (base_y - top) * i / steps
        pts += [(x, y_lo), (cx + half * 0.38 + (half * 0.62) * (i + 1) / steps, y_lo)]
    pts += [(cx + half, base_y)]
    d.polygon(pts, fill=DEEP + (255,))
    # doorway
    dw, dh = fw * 0.2, fh * 0.36
    glow = radial_glow(im.width, im.height, int(cx), int(base_y - dh * 0.45), int(dw * 1.6), GLOW, 0.7)
    im.alpha_composite(glow)
    d = ImageDraw.Draw(im)
    d.rectangle((cx - dw / 2, base_y - dh + dw / 2, cx + dw / 2, base_y), fill=(150, 248, 230, 255))
    d.ellipse((cx - dw / 2, base_y - dh, cx + dw / 2, base_y - dh + dw), fill=(150, 248, 230, 255))
    d.rectangle((cx - dw * 0.3, base_y - dh * 0.55, cx + dw * 0.3, base_y), fill=(214, 255, 246, 255))
    d.ellipse((cx - dw * 0.3, base_y - dh * 0.55 - dw * 0.3, cx + dw * 0.3, base_y - dh * 0.55 + dw * 0.3),
              fill=(214, 255, 246, 255))


def shadowed(layer, blur, offset, alpha):
    sh = Image.new('RGBA', layer.size, (0, 0, 0, 0))
    a = layer.getchannel('A').point(lambda v: int(v * alpha))
    sh.putalpha(a)
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new('RGBA', (layer.width + offset[0] * 2, layer.height + offset[1] * 2), (0, 0, 0, 0))
    out.alpha_composite(sh, (offset[0] * 2, offset[1] * 2))
    out.alpha_composite(layer, (offset[0], offset[1]))
    return out


def make_icon():
    s = SS
    W = 512 * s
    bg = gradient(W, W, TEAL, INDIGO).convert('RGBA')
    bg.alpha_composite(radial_glow(W, W, W // 2, int(W * 0.42), int(W * 0.42), GLOW, 0.22))
    st = stamp(300, 352, 'arch', s).rotate(-4, resample=Image.BICUBIC, expand=True)
    st = shadowed(st, 10 * s, (6 * s, 8 * s), 0.55)
    bg.alpha_composite(st, ((W - st.width) // 2 + 2 * s, (W - st.height) // 2))
    d = ImageDraw.Draw(bg)
    for (x, y, r) in ((70, 80, 10), (440, 110, 7), (455, 420, 9), (58, 400, 6)):
        sparkle(d, x * s, y * s, r * s, (200, 245, 236, 200))
    return bg.resize((512, 512), Image.LANCZOS)


def make_banner(fonts):
    s = SS
    W, H = 1400 * s, 400 * s
    bg = gradient(W, H, TEAL, INDIGO, diagonal=0.65).convert('RGBA')
    bg.alpha_composite(radial_glow(W, H, int(W * 0.27), int(H * 0.55), int(H * 0.62), GLOW, 0.2))
    # rays
    rays = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(rays)
    for i in range(7):
        a = math.radians(-60 + i * 20)
        x0, y0 = W * 0.27, H * 1.05
        d.polygon([(x0, y0), (x0 + math.cos(a - 0.05) * W, y0 - math.sin(a - 0.05 + math.pi / 2) * W),
                   (x0 + math.cos(a + 0.05) * W, y0 - math.sin(a + 0.05 + math.pi / 2) * W)], fill=26)
    rays = rays.filter(ImageFilter.GaussianBlur(14 * s))
    layer = Image.new('RGBA', (W, H), (190, 255, 240, 0))
    layer.putalpha(rays)
    bg.alpha_composite(layer)
    # three stamps
    for motif, cx, cy, ang, sw, sh in (('sparkle', 190, 205, 9, 190, 214), ('arch', 380, 196, -3, 214, 244),
                                       ('moon', 575, 210, -10, 190, 214)):
        st = stamp(sw, sh, motif, s).rotate(ang, resample=Image.BICUBIC, expand=True)
        st = shadowed(st, 9 * s, (5 * s, 7 * s), 0.5)
        bg.alpha_composite(st, (int(cx * s - st.width / 2), int(cy * s - st.height / 2)))
    # title
    tf = ImageFont.truetype(os.path.join(fonts, 'AlegreyaSC-Bold.ttf'), 92 * s)
    sf = ImageFont.truetype(os.path.join(fonts, 'FiraSans-Regular.ttf'), 38 * s)
    tx, ty = 728 * s, 116 * s
    text = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    td = ImageDraw.Draw(text)
    td.text((tx, ty), 'HD FEATURES', font=tf, fill=GOLD + (255,))
    shadow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    shadow.putalpha(text.getchannel('A').point(lambda v: int(v * 0.6)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(6 * s))
    bg.alpha_composite(shadow, (3 * s, 5 * s))
    bg.alpha_composite(text)
    d = ImageDraw.Draw(bg)
    bb = d.textbbox((tx, ty), 'HD FEATURES', font=tf)
    y_rule = bb[3] + 22 * s
    d.line((tx + 4 * s, y_rule, bb[2], y_rule), fill=GOLD + (170,), width=2 * s)
    sparkle(d, bb[2] + 16 * s, y_rule, 9 * s, GOLD + (220,))
    d.text((tx + 2 * s, y_rule + 20 * s), 'Stamps + Cave of Shadows', font=sf, fill=(198, 238, 232, 255))
    return bg.resize((1400, 400), Image.LANCZOS)


def main(argv=None):
    ap = argparse.ArgumentParser(description='HD Features base art')
    ap.add_argument('--fonts', required=True, help='folder with AlegreyaSC-Bold.ttf and FiraSans-Regular.ttf')
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    for f in ('AlegreyaSC-Bold.ttf', 'FiraSans-Regular.ttf'):
        if not os.path.isfile(os.path.join(a.fonts, f)):
            print('error: %s not in %s' % (f, a.fonts), file=sys.stderr)
            return 2
    os.makedirs(a.out, exist_ok=True)
    make_icon().save(os.path.join(a.out, 'icon.png'), optimize=True)
    make_banner(a.fonts).save(os.path.join(a.out, 'banner.png'), optimize=True)
    print('icon.png 512x512, banner.png 1400x400 ->', a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
