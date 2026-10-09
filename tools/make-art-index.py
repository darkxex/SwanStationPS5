#!/usr/bin/env python3
"""SwanStationPS5 - writes the lists of downloadable game art SwanStationPS5 matches a game
against (as it does for cheats), so it downloads just that one picture:
  assets/bezels-index.txt   The Bezel Project's PlayStation bezels
  assets/snaps-index.txt    libretro-thumbnails: gameplay pictures (Named_Snaps)
  assets/titles-index.txt   libretro-thumbnails: title screens (Named_Titles)
  assets/boxarts-index.txt  libretro-thumbnails: covers by name (Named_Boxarts), for discs
                            whose serial can't be read

    python tools/make-art-index.py        (rerun now and then for new art)

SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import pathlib
import urllib.parse
import urllib.request

LISTS = [
    ("thebezelproject/bezelproject-PSX", "retroarch/overlay/GameBezels/PSX/", "bezels-index.txt"),
    ("libretro-thumbnails/Sony_-_PlayStation", "Named_Snaps/", "snaps-index.txt"),
    ("libretro-thumbnails/Sony_-_PlayStation", "Named_Titles/", "titles-index.txt"),
    ("libretro-thumbnails/Sony_-_PlayStation", "Named_Boxarts/", "boxarts-index.txt"),
]


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "SwanStationPS5", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def write(REPO, FOLDER, file):
    branch = get(f"https://api.github.com/repos/{REPO}")["default_branch"]
    # the folder alone (the whole repository's listing is too long to come back whole)
    folder = urllib.parse.quote(f"{branch}:{FOLDER.rstrip('/')}", safe="")
    tree = get(f"https://api.github.com/repos/{REPO}/git/trees/{folder}")
    if tree.get("truncated"):
        raise SystemExit("the folder listing was truncated")
    names = sorted(e["path"] for e in tree["tree"] if e["type"] == "blob" and e["path"].endswith(".png"))
    out = pathlib.Path(__file__).resolve().parent.parent / "assets" / file
    out.write_text(f"# {REPO} {branch}: {FOLDER} ({len(names)} files)\n" + "\n".join(names) + "\n",
                   encoding="utf-8", newline="\n")
    print(f"{len(names)} pictures -> {out}")


def main():
    for repo, folder, file in LISTS:
        write(repo, folder, file)


if __name__ == "__main__":
    main()
