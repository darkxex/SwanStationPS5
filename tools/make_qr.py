#!/usr/bin/env python3
# PSXS5 - the project page as a QR code for Settings > About.
# SPDX-License-Identifier: GPL-3.0-or-later
"""python -m pip install segno pillow; python tools/make_qr.py -> assets/qr-github.png
One pixel per module with a 2-module white margin: PSXS5 scales it up crisply."""
from pathlib import Path

import segno
from PIL import Image

URL = "https://github.com/darkxex/SwanStationPS5"
ROOT = Path(__file__).resolve().parent.parent

matrix = segno.make(URL, error="m").matrix
n, quiet = len(matrix), 2
img = Image.new("RGBA", (n + 2 * quiet, n + 2 * quiet), (255, 255, 255, 255))
for y, row in enumerate(matrix):
    for x, dark in enumerate(row):
        if dark:
            img.putpixel((x + quiet, y + quiet), (15, 19, 48, 255))
img.save(ROOT / "assets" / "qr-github.png", optimize=True)
print("wrote assets/qr-github.png for", URL)
