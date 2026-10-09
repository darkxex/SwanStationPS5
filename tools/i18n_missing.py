#!/usr/bin/env python3
# SwanStationPS5 - lists interface strings that src/i18n.c has no translation row for.
# SPDX-License-Identifier: GPL-3.0-or-later
"""python tools/i18n_missing.py

Looks at every tr("...") and at the string tables of the screens (rows, labels,
hints), which are drawn through tr()."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STRING = r'"((?:[^"\\]|\\.)*)"'
TABLE_FILES = {"settings_screen.c", "menu_screen.c", "coverflow.c", "main.c"}
IGNORE = {"SwanStationPS5", "4:3", "16:9", "16:10", "2x", "3x", "4x", "25%", "50%", "75%", "100%",
          "L1", "R1", "L2", "R2", "L3", "R3", "OPTIONS", "SELECT", "START", "Pop", "BIOS",
          "Format", "Europe", "RetroAchievements"}


def main() -> None:
    i18n = (ROOT / "src" / "i18n.c").read_text(encoding="utf-8")
    keys = set(re.findall(r"\{\{" + STRING, i18n))
    used = set()
    for f in (ROOT / "src").rglob("*.c"):
        text = f.read_text(encoding="utf-8")
        used |= set(re.findall(r"\btr\(" + STRING, text))
        if f.name in TABLE_FILES:
            for s in re.findall(STRING, text):
                if re.match(r"^[A-Z][a-z]", s) and "/" not in s and "%s:" not in s and "\\" not in s:
                    used.add(s)
    all_keys = re.findall(r"\{\{" + STRING, i18n)
    dupes = sorted({k for k in all_keys if all_keys.count(k) > 1})
    for d in dupes:
        print("duplicate row:", d)
    missing = sorted(u for u in used if u not in keys and u not in IGNORE)
    for m in missing:
        print(m)
    print(f"-- {len(missing)} missing")


if __name__ == "__main__":
    main()
