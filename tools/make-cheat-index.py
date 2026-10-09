#!/usr/bin/env python3
"""SwanStationPS5 - writes assets/cheats-index.txt: the names of libretro-database's
PlayStation cheat files (cht/Sony - PlayStation). SwanStationPS5 matches a game against
this list and downloads just that one file, so cheats work with no library on
the console and without listing folders (which a sandboxed SwanStationPS5 can't).

    python tools/make-cheat-index.py        (rerun now and then for new files)

SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import pathlib
import urllib.request

REPO = "libretro/libretro-database"
FOLDER = "cht/Sony - PlayStation/"


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "SwanStationPS5", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def main():
    branch = get(f"https://api.github.com/repos/{REPO}")["default_branch"]
    tree = get(f"https://api.github.com/repos/{REPO}/git/trees/{branch}?recursive=1")
    if tree.get("truncated"):
        raise SystemExit("the tree listing was truncated; list the folder another way")
    names = sorted(e["path"][len(FOLDER):] for e in tree["tree"]
                   if e["type"] == "blob" and e["path"].startswith(FOLDER) and e["path"].endswith(".cht")
                   and "/" not in e["path"][len(FOLDER):])
    out = pathlib.Path(__file__).resolve().parent.parent / "assets" / "cheats-index.txt"
    out.write_text(f"# {REPO} {branch}: {FOLDER} ({len(names)} files)\n" + "\n".join(names) + "\n",
                   encoding="utf-8", newline="\n")
    print(f"{len(names)} cheat files -> {out}")


if __name__ == "__main__":
    main()
