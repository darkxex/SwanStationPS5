#!/usr/bin/env python3
# SwanStationPS5 - retro pixel-art logo for the GitHub page (not the app's own art).
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Draws everything on a small pixel canvas, then scales it up with nearest
neighbour so every pixel stays a crisp square, and adds CRT scanlines.

  python tools/make_logo.py   -> docs/media/logo.png (square, 512 px)
                                 docs/media/social-preview.png (1280x640)
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent

# 7x9 chunky glyphs, '#' = pixel
GLYPHS = {
    "P": ["######.", "##...##", "##...##", "##...##", "######.", "##.....", "##.....", "##.....", "##....."],
    "S": [".#####.", "##...##", "##.....", "##.....", ".#####.", ".....##", ".....##", "##...##", ".#####."],
    "X": ["##...##", "##...##", ".##.##.", "..###..", "..###..", "..###..", ".##.##.", "##...##", "##...##"],
    "5": ["#######", "##.....", "##.....", "######.", ".....##", ".....##", ".....##", "##...##", ".#####."],
}
# 3x5 small font for the tagline
SMALL = {
    "A": ["###", "#.#", "###", "#.#", "#.#"], "C": ["###", "#..", "#..", "#..", "###"],
    "E": ["###", "#..", "##.", "#..", "###"], "I": ["###", ".#.", ".#.", ".#.", "###"],
    "L": ["#..", "#..", "#..", "#..", "###"], "N": ["##.", "#.#", "#.#", "#.#", "#.#"],
    "O": ["###", "#.#", "#.#", "#.#", "###"], "P": ["###", "#.#", "###", "#..", "#.."],
    "R": ["##.", "#.#", "##.", "#.#", "#.#"], "S": ["###", "#..", "###", "..#", "###"],
    "T": ["###", ".#.", ".#.", ".#.", ".#."], "U": ["#.#", "#.#", "#.#", "#.#", "###"],
    "X": ["#.#", "#.#", ".#.", "#.#", "#.#"], "Y": ["#.#", "#.#", ".#.", ".#.", ".#."],
    "1": [".#.", "##.", ".#.", ".#.", "###"], "5": ["###", "#..", "###", "..#", "###"],
    "M": ["#.#", "###", "###", "#.#", "#.#"], "H": ["#.#", "#.#", "###", "#.#", "#.#"],
    "D": ["##.", "#.#", "#.#", "#.#", "##."], "G": ["###", "#..", "#.#", "#.#", "###"],
    "W": ["#.#", "#.#", "###", "###", "#.#"], "B": ["##.", "#.#", "##.", "#.#", "##."],
    "K": ["#.#", "#.#", "##.", "#.#", "#.#"], "J": ["..#", "..#", "..#", "#.#", "###"], "F": ["###", "#..", "##.", "#..", "#.."],
    " ": ["...", "...", "...", "...", "..."], "-": ["...", "...", "###", "...", "..."],
}

NAVY = (14, 16, 48)
INK = (8, 8, 24)
GREEN, RED, BLUE, PINK = (64, 224, 160), (255, 92, 92), (120, 150, 255), (255, 120, 200)
GOLD = (255, 214, 92)
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def dithered_gradient(w, h, top, bottom):
    img = Image.new("RGB", (w, h))
    px = img.load()
    steps = 6
    for y in range(h):
        for x in range(w):
            t = y / (h - 1) * steps
            band = int(t)
            frac = t - band
            if frac * 16 > BAYER[y % 4][x % 4]:
                band += 1
            band = min(band, steps)
            k = band / steps
            px[x, y] = tuple(int(top[i] + (bottom[i] - top[i]) * k) for i in range(3))
    return img


def blit(draw, glyph, x, y, color, scale=1):
    for r, row in enumerate(glyph):
        for c, ch in enumerate(row):
            if ch == "#":
                draw.rectangle([x + c * scale, y + r * scale, x + (c + 1) * scale - 1, y + (r + 1) * scale - 1], fill=color)


def wordmark(draw, text, x, y, scale=1, depth=3):
    """Chunky letters with a stepped 3D extrusion, like an old title screen."""
    adv = 8 * scale
    shades = [(70, 40, 140), (110, 60, 190)]
    for d in range(depth, 0, -1):
        for i, ch in enumerate(text):
            blit(draw, GLYPHS[ch], x + i * adv + d * scale, y + d * scale, shades[d % 2], scale)
    for i, ch in enumerate(text):
        g = GLYPHS[ch]
        # top half light, bottom half gold: the classic two-tone chrome
        top = [row if r < 4 else "." * len(row) for r, row in enumerate(g)]
        bot = [row if r >= 4 else "." * len(row) for r, row in enumerate(g)]
        blit(draw, top, x + i * adv, y, (236, 240, 255), scale)
        blit(draw, bot, x + i * adv, y, GOLD, scale)
        blit(draw, [row[:1] if r < 4 else "" for r, row in enumerate(g)], x + i * adv, y, (255, 255, 255), scale)


def small_text(draw, text, x, y, color, center=False):
    w = len(text) * 4 - 1
    if center:
        x -= w // 2
    for i, ch in enumerate(text.upper()):
        blit(draw, SMALL.get(ch, SMALL[" "]), x + i * 4, y, color)


def pixel_disc(draw, cx, cy, r):
    rainbow = [(255, 110, 90), (255, 190, 80), (140, 230, 100), (90, 200, 255), (150, 120, 255), (255, 120, 220)]
    for y in range(-r, r + 1):
        for x in range(-r, r + 1):
            d2 = x * x + y * y
            if d2 > r * r:
                continue
            if d2 <= (r * 0.22) ** 2:
                col = NAVY if d2 <= (r * 0.12) ** 2 else (220, 225, 240)
            else:
                import math
                a = (math.atan2(y, x) + math.pi) / (2 * math.pi)
                # silver disc with two rainbow sheens, banded like a low-colour palette
                sheen = abs(((a * 2) % 1.0) - 0.5) * 2
                if sheen < 0.45:
                    col = rainbow[int(a * len(rainbow) * 2) % len(rainbow)]
                else:
                    col = (200, 205, 225) if (x + y) % 2 else (180, 186, 212)
            if d2 > (r - 1) ** 2:
                col = (240, 242, 255)
            draw.point((cx + x, cy + y), fill=col)


def symbols(draw, cx, cy, spread):
    tri = ["..#..", ".#.#.", ".#.#.", "#...#", "#####"]
    cir = [".###.", "#...#", "#...#", "#...#", ".###."]
    crs = ["#...#", ".#.#.", "..#..", ".#.#.", "#...#"]
    sq = ["#####", "#...#", "#...#", "#...#", "#####"]
    blit(draw, tri, cx - 5, cy - spread - 5, GREEN, 2)
    blit(draw, cir, cx + spread - 5, cy - 5, RED, 2)
    blit(draw, crs, cx - 5, cy + spread - 5, BLUE, 2)
    blit(draw, sq, cx - spread - 5, cy - 5, PINK, 2)


def scanlines(img, scale):
    px = img.load()
    for y in range(img.height):
        if y % scale == scale - 1:
            for x in range(img.width):
                r, g, b = px[x, y]
                px[x, y] = (r * 3 // 4, g * 3 // 4, b * 3 // 4)
    return img


def frame(draw, w, h):
    draw.rectangle([0, 0, w - 1, h - 1], outline=(60, 70, 160))
    draw.rectangle([1, 1, w - 2, h - 2], outline=INK)


def square_logo():
    w = h = 128
    img = dithered_gradient(w, h, (40, 40, 120), (10, 10, 36))
    d = ImageDraw.Draw(img)
    pixel_disc(d, 64, 44, 24)
    symbols(d, 64, 44, 34)
    # wordmark at 2x inside the 128 canvas: 5 letters * 16 px = 80 px wide
    wordmark(d, "SwanStationPS5", 22, 84, scale=2, depth=2)
    small_text(d, "PLAYSTATION X SUPER 5", 64, 112, (150, 160, 230), center=True)
    frame(d, w, h)
    s = 4
    return scanlines(img.resize((w * s, h * s), Image.NEAREST), s)


def social_preview():
    w, h = 320, 160
    img = dithered_gradient(w, h, (44, 40, 128), (8, 8, 30))
    d = ImageDraw.Draw(img)
    # horizon grid, a nod to 90s title screens
    for i in range(0, 7):
        y = 118 + i * i * 1
        if y < h:
            d.line([(0, y), (w, y)], fill=(70, 50, 150))
    for x in range(-200, w + 200, 24):
        d.line([(w // 2 + (x - w // 2) // 4, 118), (x, h)], fill=(70, 50, 150))
    pixel_disc(d, 72, 66, 34)
    symbols(d, 72, 66, 46)
    wordmark(d, "SwanStationPS5", 140, 40, scale=3, depth=3)
    small_text(d, "PLAYSTATION X SUPER 5", 140 + 60, 82, (220, 225, 255), center=True)
    small_text(d, "PS1 EMULATION FOR JAILBROKEN PS5", 140 + 60, 92, (140, 150, 220), center=True)
    frame(d, w, h)
    s = 4
    return scanlines(img.resize((w * s, h * s), Image.NEAREST), s)


if __name__ == "__main__":
    out = ROOT / "docs" / "media"
    out.mkdir(parents=True, exist_ok=True)
    square_logo().save(out / "logo.png", optimize=True)
    social_preview().save(out / "social-preview.png", optimize=True)
    print("wrote logo.png and social-preview.png in", out)
