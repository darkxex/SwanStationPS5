#!/usr/bin/env python3
"""SwanStationPS5 - writes assets/chtdb-index.txt: which file of DuckStation's cheat
database (github.com/duckstation/chtdb, cheats/) holds the codes for each disc
serial. The second source of cheats, for games libretro-database has nothing
for (Persona, Diablo...); SwanStationPS5 downloads only that game's file. Also
assets/chtdb-patches-index.txt, the same for patches/ (widescreen, 60 fps,
NTSC mode, fixes: changes that don't make a game easier).

    python tools/make-chtdb-index.py        (rerun now and then for new files)

Each file starts with a header naming every serial it covers
("{SLUS-01158, SLUS-01339}"), so disc 2 finds disc 1's file. Only files with
GameShark codes are listed (that's what both emulators take).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import io
import pathlib
import re
import urllib.request
import zipfile

ZIP = "https://codeload.github.com/duckstation/chtdb/zip/refs/heads/master"


def main():
    req = urllib.request.Request(ZIP, headers={"User-Agent": "SwanStationPS5"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    write(data, "cheats", "chtdb-index.txt")
    write(data, "patches", "chtdb-patches-index.txt")


def write(data, folder, out_name):
    index = {}
    files = 0
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info in sorted(z.infolist(), key=lambda i: i.filename):
            parts = info.filename.split("/")
            if len(parts) != 3 or parts[1] != folder or not parts[2].endswith(".cht"):
                continue
            name = parts[2]
            text = z.read(info).decode("utf-8", "replace")
            if not re.search(r"^\s*Type\s*=\s*Gameshark", text, re.M | re.I):
                continue
            files += 1
            stem = name[:-4]
            serials = []
            header = re.search(r"\{([^}]*)\}", text.split("\n", 1)[0])
            if header:
                serials = [s.strip().upper() for s in re.split(r"[,|]", header.group(1)) if s.strip()]
            base = re.match(r"^([A-Z]{4}-\d{5})", stem.upper())
            if base and base.group(1) not in serials:
                serials.insert(0, base.group(1))
            for s in serials:
                # a plain SERIAL.cht beats a SERIAL-HASH.cht variant
                if s not in index or (stem.upper() == s and index[s].upper() != s + ".CHT"):
                    index[s] = name
    out = pathlib.Path(__file__).resolve().parent.parent / "assets" / out_name
    lines = [f"{s}\t{index[s]}" for s in sorted(index)]
    out.write_text(f"# duckstation/chtdb {folder}/: serial<TAB>file (" + str(files) + " files)\n" + "\n".join(lines) + "\n",
                   encoding="utf-8", newline="\n")
    print(f"{len(lines)} serials in {files} files -> {out}")


if __name__ == "__main__":
    main()
