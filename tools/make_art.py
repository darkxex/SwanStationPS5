#!/usr/bin/env python3
# SwanStationPS5 - draws the home-screen art (icon0.png and the two 3840x2160 backgrounds).
# SPDX-License-Identifier: GPL-3.0-or-later
"""Original SwanStationPS5 artwork, generated so it can be tweaked and rebuilt:

    python tools/make_art.py            -> sce_sys/icon0.png
                                           sce_sys/background-source.png (selected)
                                           sce_sys/launch-background-source.png (launching)
Convert the two backgrounds to pic0.dds / pic1.dds with tools/make_dds.py.

The look matches the in-app shelf: a deep blue gradient, a glowing CD with a
rainbow sheen and the four face-button shapes in their classic colours.
"""
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONT_BOLD = str(ROOT / "assets/fonts/Inter-600.ttf")
FONT_REGULAR = str(ROOT / "assets/fonts/Inter-400.ttf")

TOP, MID, LOW, BOTTOM = (26, 36, 112), (48, 73, 196), (42, 63, 174), (13, 20, 64)
GREEN, RED, BLUE, PINK = (64, 226, 160), (255, 102, 102), (122, 166, 255), (255, 133, 206)


def gradient(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    stops = [(0.0, TOP), (0.46, MID), (0.62, LOW), (1.0, BOTTOM)]
    for y in range(h):
        t = y / (h - 1)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / (t1 - t0)
                c = tuple(int(c0[i] + (c1[i] - c0[i]) * k) for i in range(3))
                break
        for x in range(w):
            px[x, y] = c
    return img


def vignette(img, strength=0.55, bias_x=0.5):
    w, h = img.size
    mask = Image.new("L", (w // 8, h // 8))
    mp = mask.load()
    for y in range(mask.size[1]):
        for x in range(mask.size[0]):
            dx = (x / mask.size[0] - bias_x) * 1.6
            dy = (y / mask.size[1] - 0.5) * 1.9
            mp[x, y] = int(255 * min(1.0, (dx * dx + dy * dy) ** 0.5 * strength))
    mask = mask.resize((w, h), Image.BICUBIC).filter(ImageFilter.GaussianBlur(w // 200))
    return Image.composite(Image.new("RGB", (w, h), (4, 6, 20)), img, mask)


def glow_layer(size, draw_fn, blur, intensity=1.0):
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    halo = layer.filter(ImageFilter.GaussianBlur(blur))
    if intensity != 1.0:
        a = halo.getchannel("A").point(lambda v: min(255, int(v * intensity)))
        halo.putalpha(a)
    return halo, layer


def symbol(draw, kind, cx, cy, r, width, color):
    if kind == "triangle":
        pts = [(cx, cy - r), (cx + r * 0.95, cy + r * 0.72), (cx - r * 0.95, cy + r * 0.72)]
        draw.line(pts + [pts[0]], fill=color, width=width, joint="curve")
    elif kind == "circle":
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), outline=color, width=width)
    elif kind == "cross":
        d = r * 0.82
        draw.line((cx - d, cy - d, cx + d, cy + d), fill=color, width=width)
        draw.line((cx + d, cy - d, cx - d, cy + d), fill=color, width=width)
    else:
        d = r * 0.82
        draw.rectangle((cx - d, cy - d, cx + d, cy + d), outline=color, width=width)


def disc(size, cx, cy, r):
    """A CD: silver body, rainbow sheen sweeping around, hub and hole."""
    S = 2  # supersample
    w, h = size[0] * S, size[1] * S
    cx, cy, r = cx * S, cy * S, r * S
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    body = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(body)
    bd.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(205, 214, 235, 255))
    # rainbow sheen: conic hue bands, strongest on two opposite lobes
    sheen = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sp = sheen.load()
    x0, x1, y0, y1 = int(cx - r), int(cx + r), int(cy - r), int(cy + r)
    for y in range(max(0, y0), min(h, y1), 2):
        for x in range(max(0, x0), min(w, x1), 2):
            dx, dy = x - cx, y - cy
            dist = math.hypot(dx, dy)
            if dist > r or dist < r * 0.18:
                continue
            ang = math.atan2(dy, dx)
            lobe = (math.cos(2 * (ang - 0.6)) + 1) / 2
            hue = (ang / (2 * math.pi) + dist / r * 0.35) % 1.0
            rr, gg, bb = [int(255 * (0.5 + 0.5 * math.cos(2 * math.pi * (hue + o)))) for o in (0, 1 / 3, 2 / 3)]
            a = int(200 * lobe ** 2)
            for yy in (y, y + 1):
                for xx in (x, x + 1):
                    if xx < w and yy < h:
                        sp[xx, yy] = (rr, gg, bb, a)
    body = Image.alpha_composite(body, sheen)
    mask = Image.new("L", (w, h), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    md.ellipse((cx - r * 0.13, cy - r * 0.13, cx + r * 0.13, cy + r * 0.13), fill=0)
    body.putalpha(ImageChops.multiply(body.getchannel("A"), mask))
    img = Image.alpha_composite(img, body)
    d = ImageDraw.Draw(img)
    d.ellipse((cx - r * 0.34, cy - r * 0.34, cx + r * 0.34, cy + r * 0.34), outline=(255, 255, 255, 140), width=int(r * 0.02) + 1)
    d.ellipse((cx - r * 0.2, cy - r * 0.2, cx + r * 0.2, cy + r * 0.2), fill=(230, 236, 250, 230))
    d.ellipse((cx - r * 0.13, cy - r * 0.13, cx + r * 0.13, cy + r * 0.13), fill=(0, 0, 0, 0))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(255, 255, 255, 170), width=int(r * 0.012) + 1)
    return img.resize(size, Image.LANCZOS)


def add_disc_with_glow(base, cx, cy, r):
    halo = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse((cx - r * 1.08, cy - r * 1.08, cx + r * 1.08, cy + r * 1.08), fill=(140, 170, 255, 150))
    halo = halo.filter(ImageFilter.GaussianBlur(r * 0.25))
    out = Image.alpha_composite(base.convert("RGBA"), halo)
    return Image.alpha_composite(out, disc(base.size, cx, cy, r))


def add_symbols(base, items, width, blur):
    out = base.convert("RGBA")
    for kind, cx, cy, r, color in items:
        halo, layer = glow_layer(out.size, lambda d: symbol(d, kind, cx, cy, r, width, color + (255,)), blur, 1.8)
        out = Image.alpha_composite(Image.alpha_composite(out, halo), layer)
    return out


def text(base, xy, s, size, font, fill=(255, 255, 255, 255), anchor="la", glow=0):
    f = ImageFont.truetype(font, size)
    if glow:
        halo, _ = glow_layer(base.size, lambda d: d.text(xy, s, font=f, fill=(150, 180, 255, 255), anchor=anchor), glow)
        base = Image.alpha_composite(base, halo)
    ImageDraw.Draw(base).text(xy, s, font=f, fill=fill, anchor=anchor)
    return base


def selection_background():
    """Shown behind the home-screen UI while SwanStationPS5 is selected. The Shell draws
    the title and buttons on the left, so the art lives on the right."""
    w, h = 3840, 2160
    img = vignette(gradient(w, h), 0.5, bias_x=0.68)
    img = add_disc_with_glow(img, 2700, 1010, 640)
    img = add_symbols(img, [
        ("triangle", 2700, 230, 120, GREEN),
        ("circle", 3560, 1010, 120, RED),
        ("cross", 2700, 1800, 120, BLUE),
        ("square", 1840, 1010, 120, PINK),
    ], 22, 30)
    img = text(img, (3700, 2010), "SwanStationPS5", 150, FONT_BOLD, anchor="rs", glow=26)
    return img.convert("RGB")


def launch_background():
    """Shown while SwanStationPS5 starts: centred, and it fades into the shelf's colours."""
    w, h = 3840, 2160
    img = vignette(gradient(w, h), 0.45)
    img = add_disc_with_glow(img, 1920, 900, 430)
    img = add_symbols(img, [
        ("triangle", 1920, 280, 70, GREEN),
        ("circle", 2560, 900, 70, RED),
        ("cross", 1920, 1520, 70, BLUE),
        ("square", 1280, 900, 70, PINK),
    ], 14, 20)
    img = text(img, (1920, 1840), "SwanStationPS5", 210, FONT_BOLD, anchor="ms", glow=30)
    img = text(img, (1920, 1960), "PlayStation X Super 5", 72, FONT_REGULAR,
               fill=(201, 210, 255, 255), anchor="ms")
    return img.convert("RGB")


def icon():
    """512x512 home-screen tile: must read at thumbnail size."""
    S = 2
    w = 512 * S
    img = vignette(gradient(w, w), 0.6)
    img = add_disc_with_glow(img, w // 2, int(w * 0.42), int(w * 0.30))
    img = add_symbols(img, [
        ("triangle", w // 2, int(w * 0.085), int(w * 0.045), GREEN),
        ("circle", int(w * 0.84), int(w * 0.42), int(w * 0.045), RED),
        ("square", int(w * 0.16), int(w * 0.42), int(w * 0.045), PINK),
    ], int(w * 0.012), int(w * 0.012))
    img = text(img, (w // 2, int(w * 0.94)), "SwanStationPS5", int(w * 0.2), FONT_BOLD, anchor="ms", glow=int(w * 0.012))
    return img.convert("RGB").resize((512, 512), Image.LANCZOS)


def banner():
    """README header for GitHub: the launch art, wide."""
    S = 2
    w, h = 1600 * S, 520 * S
    img = vignette(gradient(w, h), 0.5, bias_x=0.3)
    img = add_disc_with_glow(img, int(w * 0.24), h // 2, int(h * 0.30))
    img = add_symbols(img, [
        ("triangle", int(w * 0.24), int(h * 0.09), int(h * 0.045), GREEN),
        ("circle", int(w * 0.24) + int(h * 0.44), h // 2, int(h * 0.045), RED),
        ("cross", int(w * 0.24), int(h * 0.91), int(h * 0.045), BLUE),
        ("square", int(w * 0.24) - int(h * 0.44), h // 2, int(h * 0.045), PINK),
    ], int(h * 0.012), int(h * 0.02))
    img = text(img, (int(w * 0.47), int(h * 0.56)), "SwanStationPS5", int(h * 0.30), FONT_BOLD, anchor="ls",
               glow=int(h * 0.025))
    img = text(img, (int(w * 0.475), int(h * 0.70)), "PlayStation X Super 5", int(h * 0.085),
               FONT_REGULAR, fill=(201, 210, 255, 255), anchor="ls")
    img = text(img, (int(w * 0.475), int(h * 0.80)), "PlayStation 1 emulation for jailbroken PS5",
               int(h * 0.05), FONT_REGULAR, fill=(150, 165, 230, 255), anchor="ls")
    return img.convert("RGB").resize((1600, 520), Image.LANCZOS)


if __name__ == "__main__":
    import shutil
    import sys
    media = ROOT / "docs" / "media"
    media.mkdir(parents=True, exist_ok=True)
    banner().save(media / "banner.png", optimize=True)
    if "--banner" in sys.argv:
        print("wrote", media / "banner.png")
        sys.exit(0)
    out = ROOT / "sce_sys"
    icon().save(out / "icon0.png", optimize=True)
    shutil.copyfile(out / "icon0.png", media / "icon.png")
    selection_background().save(out / "background-source.png", optimize=True)
    launch_background().save(out / "launch-background-source.png", optimize=True)
    print("wrote icon0.png, background-source.png, launch-background-source.png in", out)
