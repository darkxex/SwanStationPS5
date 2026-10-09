#!/usr/bin/env python3
# SwanStationPS5 - builds the Japanese interface font: a small Noto Sans JP subset.
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Noto Sans JP is ~9 MB; SwanStationPS5 only needs kana, punctuation and the kanji its
translations use. This keeps those (plus all kana, so short new strings work)
and writes assets/fonts/NotoSansJP-SwanStationPS5.ttf (SIL Open Font License 1.1).

  pip install fonttools
  python tools/make_jp_font.py      (run again after adding Japanese text to src/i18n.c)
"""
import io
import re
import urllib.request
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = Path(__file__).resolve().parent.parent
SOURCE = "https://github.com/google/fonts/raw/main/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf"
CACHE = ROOT / "build" / "NotoSansJP-wght.ttf"
OUT = ROOT / "assets" / "fonts" / "NotoSansJP-SwanStationPS5.ttf"


def wanted_codepoints() -> set[int]:
    text = (ROOT / "src" / "i18n.c").read_text(encoding="utf-8")
    cps = {ord(c) for c in text if ord(c) > 0xFF}
    cps |= set(range(0x3000, 0x3040))  # CJK punctuation
    cps |= set(range(0x3040, 0x3100))  # hiragana, katakana
    cps |= set(range(0xFF01, 0xFF5F))  # full-width forms
    cps |= {0x30FB, 0x30FC}            # middle dot, long vowel mark
    return cps


def main() -> None:
    if not CACHE.exists():
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        print("downloading Noto Sans JP...")
        with urllib.request.urlopen(SOURCE, timeout=120) as r:
            CACHE.write_bytes(r.read())
    font = TTFont(CACHE)
    # one static weight between Inter 400 and 600
    font = instancer.instantiateVariableFont(font, {"wght": 500})
    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.hinting = False
    sub = subset.Subsetter(options)
    sub.populate(unicodes=wanted_codepoints())
    sub.subset(font)
    buf = io.BytesIO()
    font.save(buf)
    OUT.write_bytes(buf.getvalue())
    kanji = len([c for c in wanted_codepoints() if 0x4E00 <= c <= 0x9FFF])
    print(f"wrote {OUT} ({OUT.stat().st_size // 1024} KB, {kanji} kanji)")


if __name__ == "__main__":
    main()
