#!/usr/bin/env python3
# PSXS5 - prepares a PlayStation library on a PC and uploads it to the PS5.
# SPDX-License-Identifier: GPL-3.0-or-later
"""
psxs5_sync.py - get your PS1 games onto PSXS5.

  python tools/psxs5_sync.py plan    --source "D:\\Games\\PS1"
  python tools/psxs5_sync.py prepare --source "D:\\Games\\PS1"   (into PSXS5_ready in your user folder)
  python tools/psxs5_sync.py upload  --host 192.168.1.50
  python tools/psxs5_sync.py cheats  --host 192.168.1.50      (libretro .cht library)
  python tools/psxs5_sync.py bios    scph5501.bin --host 192.168.1.50
  python tools/psxs5_sync.py index   --host 192.168.1.50 [--fix-cues]
                                      (games copied another way: list them for PSXS5)
  python tools/psxs5_sync.py ra-login --host 192.168.1.50    (RetroAchievements)

prepare builds one clean folder per game:
  * already-extracted .cue/.bin, .chd, .pbp are used as-is (hardlinked, no copy)
  * archives (.7z/.rar/.zip) are only extracted when no extracted copy exists
  * missing .cue sheets are generated, multi-disc games get an .m3u
  * duplicates (the same game as a folder and as a loose archive) are skipped
  * the largest image beside the game becomes fallback-cover.png (used only
    when the cover database has nothing for that serial)
upload mirrors the staging folder to /data/PSXS5/games over FTP and skips
files that are already there with the same size, so it can be re-run.
"""
from __future__ import annotations

import argparse
import ftplib
import io
import os
import re
import shutil
import subprocess
import sys
import time
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
import urllib.error
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
from psx_disc import disc_serial  # noqa: E402

DISC_EXTS = {".cue", ".bin", ".chd", ".pbp", ".iso", ".img", ".m3u", ".ccd", ".sub", ".mdf", ".mds"}
LOADABLE = {".cue", ".chd", ".pbp", ".iso", ".m3u", ".ccd", ".mds"}
ARCHIVE_EXTS = {".7z", ".rar", ".zip"}
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp"}
REMOTE_ROOT = "/data/PSXS5"
SEVEN_ZIP_CANDIDATES = [
    r"C:\Program Files\7-Zip\7z.exe",
    r"C:\Program Files (x86)\7-Zip\7z.exe",
    "7z",
    "7zz",
]
CHEATS_REPO = "https://github.com/libretro/libretro-database.git"
CHEATS_DIR = "cht/Sony - PlayStation"
COVERS_REPO = "https://github.com/xlenore/psx-covers.git"
COVER_URL = "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/{style}/{serial}.{ext}"
COVER_EXT = {"default": "jpg", "3d": "png"}
REGION_PREFIXES = {
    "usa": ["SLUS", "SCUS"],
    "europe": ["SLES", "SCES", "SCED"],
    "japan": ["SLPS", "SCPS", "SLPM", "SIPS", "PAPX", "SCPM"],
}


# --------------------------------------------------------------------- names

def normalize(name: str) -> str:
    """'Final Fantasy Tactics [NTSC-U] [SCUS-94221]' -> 'finalfantasytactics'."""
    name = re.sub(r"\.[A-Za-z0-9]{1,4}$", "", name)
    name = re.sub(r"\[[^\]]*\]|\([^)]*\)", "", name)
    name = re.sub(r"^disney-?pixar'?s\s+", "", name, flags=re.I)
    romans = {"ii": "2", "iii": "3", "iv": "4", "v": "5", "vi": "6", "vii": "7", "viii": "8", "ix": "9", "x": "10"}
    name = re.sub(r"\b(i{2,3}|iv|vi{0,3}|ix|x)\b", lambda m: romans.get(m.group(1).lower(), m.group(1)),
                  name, flags=re.I)
    key = re.sub(r"[^a-z0-9]", "", name.lower())
    if key.startswith("the") and len(key) > 6:
        key = key[3:]
    if key.endswith("the"):
        key = key[:-3]
    return key


def disc_number(name: str) -> int:
    m = re.search(r"\((?:Disc|CD)\s*(\d+)\)", name, re.I)
    return int(m.group(1)) if m else 0


def game_title(name: str) -> str:
    """Folder name for the PS5: 'Chrono Cross (USA) (Disc 1).cue' -> 'Chrono Cross (USA)'."""
    name = re.sub(r"\.[A-Za-z0-9]{1,4}$", "", name)
    # "RE2 (USA) (Disc 1) (Leon)" and "(Disc 2) (Claire)" are one game: cut at the disc tag
    name = re.split(r"\s*\((?:Disc|CD)\s*\d+\)", name, flags=re.I)[0]
    name = re.sub(r"\s*\(Track\s*\d+\)", "", name, flags=re.I)
    name = re.sub(r"\s*\[[^\]]*\]", "", name)
    name = name.replace("_", " ")
    name = re.sub(r"\s+", " ", name).strip(" .-")
    return re.sub(r'[<>:"/\\|?*]', "", name) or "Game"


# --------------------------------------------------------------------- discovery

@dataclass
class Source:
    """One game found in the source library."""
    title: str
    key: str
    kind: str                      # "folder" or "archive"
    path: Path
    loadables: list[Path] = field(default_factory=list)   # cue/chd/pbp...
    loose_bins: list[Path] = field(default_factory=list)  # bins without a cue
    archives: list[Path] = field(default_factory=list)    # still to extract
    cover: Path | None = None
    duplicate_of: str | None = None


def cue_files(cue: Path) -> list[str]:
    try:
        text = cue.read_text(errors="replace")
    except OSError:
        return []
    return re.findall(r'FILE\s+"([^"]+)"', text, re.I)


def scan_folder(folder: Path) -> Source:
    files = [p for p in folder.rglob("*") if p.is_file()]
    loadables = sorted(p for p in files if p.suffix.lower() in LOADABLE)
    covered = set()
    complete = []
    for p in loadables:
        if p.suffix.lower() == ".cue":
            refs = cue_files(p)
            if refs and all((p.parent / r).exists() for r in refs):
                complete.append(p)
                covered.update((p.parent / r).resolve() for r in refs)
        else:
            complete.append(p)
    # A PSP-style EBOOT.PBP next to real disc images is just another copy.
    if any(p.suffix.lower() in {".cue", ".chd", ".m3u"} for p in complete):
        complete = [p for p in complete if p.suffix.lower() not in {".pbp", ".iso"}]
    bins = sorted(p for p in files if p.suffix.lower() == ".bin" and p.resolve() not in covered)
    archives = sorted(p for p in files if p.suffix.lower() in ARCHIVE_EXTS)

    # Archives only matter for discs that are not already extracted.
    have = {normalize(p.name) + str(disc_number(p.name)) for p in complete}
    have |= {normalize(p.name) + str(disc_number(p.name)) for p in bins}
    needed = [a for a in archives if normalize(a.name) + str(disc_number(a.name)) not in have]
    if complete or bins:
        # e.g. "Dino Crisis (v1.0).7z" next to an extracted v1.1: keep the extracted one
        needed = [a for a in needed if disc_number(a.name) > 0]

    covers = sorted((p for p in files if p.suffix.lower() in IMAGE_EXTS), key=lambda p: p.stat().st_size, reverse=True)
    names = [p.name for p in complete] or [p.name for p in bins] or [a.name for a in needed] or [folder.name]
    title = game_title(names[0])
    if title.upper() == "EBOOT":
        title = game_title(folder.name)
    return Source(title, normalize(title), "folder", folder, complete, bins, needed,
                  covers[0] if covers else None)


def discover(source: Path) -> list[Source]:
    games: list[Source] = []
    loose: dict[str, Source] = {}  # discs lying in the source folder itself, by game
    # one folder or archive per disc ("FF VII (USA) (Disc 1)", "(Disc 2)"...): one game
    per_disc: dict[tuple[str, str], Source] = {}
    for entry in sorted(source.iterdir(), key=lambda p: p.name.lower()):
        disc_named = disc_number(entry.name) > 0
        if entry.is_dir():
            g = scan_folder(entry)
            if not (g.loadables or g.loose_bins or g.archives):
                continue
            if disc_named and (g.key, "folder") in per_disc:
                first = per_disc[(g.key, "folder")]
                first.loadables += g.loadables
                first.loose_bins += g.loose_bins
                first.archives += g.archives
                first.cover = first.cover or g.cover
                continue
            if disc_named:
                per_disc[(g.key, "folder")] = g
            games.append(g)
        elif entry.suffix.lower() in ARCHIVE_EXTS:
            title = game_title(entry.name)
            key = normalize(title)
            if disc_named and (key, "archive") in per_disc:
                per_disc[(key, "archive")].archives.append(entry)
                continue
            g = Source(title, key, "archive", entry, archives=[entry])
            if disc_named:
                per_disc[(key, "archive")] = g
            games.append(g)
        elif entry.suffix.lower() == ".m3u":
            continue  # PSXS5 writes the playlist itself (one made by VLC may hold this PC's paths)
        elif entry.suffix.lower() in LOADABLE:
            # "FF IX (USA) (Disc 1).chd", "(Disc 2).chd"...: one game with every disc
            title = game_title(entry.name)
            key = normalize(title)
            if key in loose:
                loose[key].loadables.append(entry)
            else:
                loose[key] = Source(title, key, "folder", entry.parent, loadables=[entry])
                games.append(loose[key])

    # Folders win over loose archives of the same game.
    seen: dict[str, Source] = {}
    for g in sorted(games, key=lambda g: g.kind != "folder"):
        if g.key in seen:
            g.duplicate_of = seen[g.key].title
        else:
            seen[g.key] = g
    return games


# --------------------------------------------------------------------- prepare

def seven_zip() -> str:
    for c in SEVEN_ZIP_CANDIDATES:
        if Path(c).exists() or shutil.which(c):
            return c
    sys.exit("7-Zip not found. Install it from https://www.7-zip.org/")


def place(src: Path, dst: Path) -> None:
    """Hardlink when possible (same drive, no extra space), else copy."""
    if dst.exists() and dst.stat().st_size == src.stat().st_size:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def write_cue_for_bins(bins: list[Path], out_dir: Path) -> list[Path]:
    """Creates cue sheets for bins that arrived without one.
    'Game (Track 1).bin' .. 'Game (Track N).bin' become one multi-track cue."""
    groups: dict[str, list[Path]] = {}
    for b in bins:
        base = re.sub(r"\s*\(Track\s*\d+\)", "", b.stem, flags=re.I)
        groups.setdefault(base, []).append(b)
    cues = []
    for base, members in groups.items():
        members.sort(key=lambda p: int((re.search(r"Track\s*(\d+)", p.stem, re.I) or [0, 0])[1]))
        lines = []
        for i, b in enumerate(members, 1):
            lines.append(f'FILE "{b.name}" BINARY')
            if i == 1:
                lines += [f"  TRACK {i:02d} MODE2/2352", "    INDEX 01 00:00:00"]
            else:
                lines += [f"  TRACK {i:02d} AUDIO", "    INDEX 00 00:00:00", "    INDEX 01 00:02:00"]
        cue = out_dir / f"{base}.cue"
        cue.write_text("\n".join(lines) + "\n")
        cues.append(cue)
    return cues


def extract(archive: Path, out_dir: Path) -> None:
    tmp = Path(tempfile.mkdtemp(prefix="psxs5_", dir=out_dir.parent))
    try:
        print(f"    extracting {archive.name} ...", flush=True)
        r = subprocess.run([seven_zip(), "x", "-y", f"-o{tmp}", str(archive)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if r.returncode != 0:
            print(f"    ! 7-Zip failed on {archive.name}: {r.stderr.strip()[:200]}")
            return
        # an archive inside the archive (e.g. a split "S3.7z.001"...): unpack it too
        if not any(f.suffix.lower() in DISC_EXTS for f in tmp.rglob("*") if f.is_file()):
            inner = sorted(f for f in tmp.rglob("*") if f.is_file() and
                           (f.suffix.lower() in ARCHIVE_EXTS or re.search(r"\.(7z|zip|rar)\.0*1$", f.name, re.I)))
            for nested in inner[:1]:
                print(f"    extracting nested {nested.name} ...", flush=True)
                subprocess.run([seven_zip(), "x", "-y", f"-o{tmp / 'nested'}", str(nested)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for f in tmp.rglob("*"):
            if f.is_file() and f.suffix.lower() in DISC_EXTS:
                dst = out_dir / f.name
                if not dst.exists():
                    shutil.move(str(f), dst)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def save_cover(img: Path, out_dir: Path) -> None:
    try:
        from PIL import Image
        with Image.open(img) as im:
            im = im.convert("RGB")
            im.thumbnail((512, 512))
            im.save(out_dir / "fallback-cover.png")
    except Exception:  # Pillow missing or unreadable image: covers are optional
        if img.suffix.lower() in {".png", ".jpg", ".jpeg"}:
            shutil.copy2(img, out_dir / f"fallback-cover{img.suffix.lower()}")


def write_m3u(out_dir: Path, title: str) -> None:
    discs = sorted((p for p in out_dir.iterdir() if p.suffix.lower() in {".cue", ".chd", ".pbp"}
                    and disc_number(p.name) > 0), key=lambda p: disc_number(p.name))
    if len(discs) > 1:
        m3u = out_dir / f"{title}.m3u"
        if m3u.exists():
            m3u.unlink()  # it may be a hard link to the user's own file: never write through it
        m3u.write_text("\n".join(p.name for p in discs) + "\n")


def write_serial(out_dir: Path) -> str | None:
    """serial.txt lets PSXS5 match covers/saves even for .chd, which it can't read."""
    for pattern in ("*.m3u", "*.cue", "*.pbp", "*.iso", "*.bin"):
        for p in sorted(out_dir.glob(pattern)):
            serial = disc_serial(p)
            if serial:
                (out_dir / "serial.txt").write_text(serial + "\n")
                return serial
    return None


def prepare(games: list[Source], staging: Path, only: str | None) -> None:
    staging.mkdir(parents=True, exist_ok=True)
    for g in games:
        if g.duplicate_of or (only and only.lower() not in g.title.lower()):
            continue
        prepare_one(g, staging)


DISC_DATA = {".bin", ".img", ".iso", ".chd", ".pbp", ".mdf", ".sub"}


def sync(games: list[Source], staging: Path, host: str, port: int, only: str | None) -> None:
    """Prepare, upload and clean up one game at a time, so the PC never holds
    more than one extracted game. Small files (cue, m3u, serial.txt, covers)
    stay in staging so the library index can still be written afterwards."""
    staging.mkdir(parents=True, exist_ok=True)
    todo = [g for g in games if not g.duplicate_of and (not only or only.lower() in g.title.lower())]
    for n, g in enumerate(todo, 1):
        print(f"[{n}/{len(todo)}] {g.title}", flush=True)
        out = prepare_one(g, staging)
        upload_tree(out, f"{REMOTE_ROOT}/games/{out.name}", host, port)
        # the disc images are on the PS5 now: drop them here (copies and extracted
        # archives take real space; removing a hard link leaves the original alone)
        entry = game_entry(out)
        if entry:  # remember what was uploaded, so the index still lists it
            (out / "uploaded.txt").write_text("\t".join(map(str, entry)) + "\n", encoding="utf-8")
        for p in out.iterdir():
            if p.suffix.lower() in DISC_DATA:
                p.unlink()
    index = write_index(staging)
    tmp = Path(tempfile.mkdtemp())
    shutil.copyfile(index, tmp / "library.txt")
    upload_tree(tmp, REMOTE_ROOT, host, port, force=True)
    shutil.rmtree(tmp)


def prepare_one(g: Source, staging: Path) -> Path:
    out = staging / g.title
    out.mkdir(parents=True, exist_ok=True)
    print(f"  {g.title}")
    for cue in g.loadables:
        place(cue, out / cue.name)
        if cue.suffix.lower() == ".cue":
            for ref in cue_files(cue):
                place(cue.parent / ref, out / Path(ref).name)
    for b in g.loose_bins:
        place(b, out / b.name)
    if g.loose_bins:
        write_cue_for_bins([out / b.name for b in g.loose_bins], out)
    for a in g.archives:
        extract(a, out)
    # archives may also bring bins without sheets
    sheets = {r.lower() for c in out.glob("*.cue") for r in cue_files(c)}
    orphans = [b for b in out.glob("*.bin") if b.name.lower() not in sheets]
    if orphans:
        write_cue_for_bins(orphans, out)
    write_m3u(out, g.title)
    write_serial(out)
    if g.cover and not any(out.glob("fallback-cover.*")):
        save_cover(g.cover, out)
    return out


def print_plan(games: list[Source]) -> None:
    total = dups = 0
    for g in games:
        if g.duplicate_of:
            dups += 1
            print(f"  skip  {g.path.name}  (same game as '{g.duplicate_of}')")
            continue
        total += 1
        what = []
        if g.loadables:
            what.append(f"{len(g.loadables)} ready")
        if g.loose_bins:
            what.append(f"{len(g.loose_bins)} bin(s) need a cue")
        if g.archives:
            what.append("extract " + ", ".join(a.name for a in g.archives))
        print(f"  game  {g.title:<55} {'; '.join(what)}")
    print(f"\n{total} games, {dups} duplicates skipped")


# --------------------------------------------------------------------- FTP

def ftp_connect(host: str, port: int) -> ftplib.FTP:
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=30)
    ftp.login()
    ftp.voidcmd("TYPE I")
    return ftp


def ftp_chmod(ftp: ftplib.FTP, path: str) -> None:
    """Files uploaded over FTP arrive as 0644/0755; a PS5 title whose eboot.bin
    isn't executable fails with "Can't start the game or app" (CE-107750-0)."""
    try:
        ftp.sendcmd(f"SITE CHMOD 777 {path}")
    except ftplib.all_errors:
        pass


def ftp_mkdirs(ftp: ftplib.FTP, path: str) -> None:
    cur = ""
    for part in path.strip("/").split("/"):
        cur += "/" + part
        try:
            ftp.mkd(cur)
        except ftplib.error_perm:
            pass
        ftp_chmod(ftp, cur)


def ftp_size(ftp: ftplib.FTP, path: str) -> int:
    try:
        return int(ftp.size(path) or -1)
    except (ftplib.error_perm, ftplib.error_reply, ValueError):
        return -1


def upload_tree(local: Path, remote: str, host: str, port: int, force: bool = False) -> None:
    ftp = ftp_connect(host, port)
    files = sorted(p for p in local.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in files)
    done = 0
    made: set[str] = set()
    started = time.monotonic()
    for p in files:
        rel = p.relative_to(local).as_posix()
        dst = f"{remote}/{rel}"
        rdir = dst.rsplit("/", 1)[0]
        size = p.stat().st_size
        if rdir not in made:
            ftp_mkdirs(ftp, rdir)
            made.add(rdir)
        if not force and ftp_size(ftp, dst) == size:
            done += size
            continue
        # the line updates as the file goes: overall %, this file's MB, speed
        sent = 0
        file_start = time.monotonic()
        last_shown = 0.0

        def show(final: bool = False) -> None:
            elapsed = max(time.monotonic() - file_start, 0.001)
            speed = f"{sent / elapsed / (1 << 20):.1f} MB/s" if elapsed >= 0.5 else ""  # too early to tell
            line = (f"  [{(done + sent) * 100 // max(total, 1):3d}%] {rel}  "
                    f"{sent >> 20}/{size >> 20} MB  {speed}")
            print("\r" + line.ljust(100), end="\n" if final else "", flush=True)

        def block(data: bytes) -> None:
            nonlocal sent, last_shown
            sent += len(data)
            now = time.monotonic()
            if now - last_shown >= 0.25:
                last_shown = now
                show()

        with p.open("rb") as fh:
            ftp.storbinary(f"STOR {dst}", fh, blocksize=1 << 20, callback=block)
        show(final=True)
        ftp_chmod(ftp, dst)
        done += size
    ftp.quit()
    elapsed = max(time.monotonic() - started, 0.001)
    print(f"  [100%] {len(files)} files in {remote}  ({done / elapsed / (1 << 20):.1f} MB/s on average)")


# --------------------------------------------------------------------- cheats

def library_serials(staging: Path) -> list[str]:
    serials = []
    for game in sorted(p for p in staging.iterdir() if p.is_dir()):
        txt = game / "serial.txt"
        serial = txt.read_text().strip() if txt.exists() else write_serial(game)
        if serial:
            serials.append(serial)
        else:
            print(f"  ? no serial for {game.name} (add cover.png to its folder)")
    return serials


def fetch_my_covers(serials: list[str], dest: Path, styles: list[str]) -> None:
    got = missing = 0
    for style in styles:
        folder = dest / style
        folder.mkdir(parents=True, exist_ok=True)
        for serial in serials:
            target = folder / f"{serial}.{COVER_EXT[style]}"
            if target.exists():
                got += 1
                continue
            url = COVER_URL.format(style=style, serial=serial, ext=COVER_EXT[style])
            try:
                with urllib.request.urlopen(url, timeout=20) as r:
                    target.write_bytes(r.read())
                got += 1
            except urllib.error.HTTPError as e:
                missing += 1
                print(f"  - no {style} cover for {serial} ({e.code})")
    print(f"  {got} covers ready, {missing} not in the database, in {dest}")


def fetch_all_covers(dest: Path, styles: list[str], regions: list[str]) -> Path:
    """Sparse-clones the cover database, limited to the chosen styles and regions."""
    repo = dest / "psx-covers"
    if not repo.exists():
        subprocess.run(["git", "clone", "--depth", "1", "--filter=blob:none", "--no-checkout",
                        COVERS_REPO, str(repo)], check=True)
    prefixes = [p for r in regions for p in REGION_PREFIXES[r]]
    patterns = [f"/covers/{s}/{p}-*" for s in styles for p in prefixes]
    subprocess.run(["git", "-C", str(repo), "sparse-checkout", "init", "--no-cone"], check=True)
    subprocess.run(["git", "-C", str(repo), "sparse-checkout", "set", "--no-cone", *patterns], check=True)
    subprocess.run(["git", "-C", str(repo), "checkout"], check=True)
    for s in styles:
        n = len(list((repo / "covers" / s).glob("*")))
        print(f"  {n} {s} covers in {repo / 'covers' / s}")
    return repo / "covers"


LOAD_ORDER = (".m3u", ".pbp", ".chd", ".cue", ".ccd", ".iso", ".img")


def game_entry(folder: Path) -> tuple[str, str, int, str, str] | None:
    """(title, serial, discs, file name, first disc name) for one staged game folder."""
    files = sorted(p for p in folder.iterdir() if p.is_file())
    for ext in LOAD_ORDER:
        hits = [p for p in files if p.suffix.lower() == ext]
        if not hits:
            continue
        chosen = hits[0]
        discs, first = 1, chosen.stem
        if ext == ".m3u":
            lines = [l.strip() for l in chosen.read_text(errors="replace").splitlines()
                     if l.strip() and not l.startswith("#")]
            discs = max(1, len(lines))
            if lines:
                first = Path(lines[0]).stem
        serial_file = folder / "serial.txt"
        serial = serial_file.read_text().strip() if serial_file.exists() else (write_serial(folder) or "")
        return folder.name, serial, discs, chosen.name, first
    # disc images already uploaded and removed by `sync`
    marker = folder / "uploaded.txt"
    if marker.exists():
        title, serial, discs, name, first = marker.read_text(encoding="utf-8").rstrip("\n").split("\t")
        return title, serial, int(discs), name, first
    return None


def write_index(staging: Path) -> Path:
    """library.txt: what a sandboxed PSXS5 (which can't list folders) loads."""
    lines = ["# PSXS5 library index - written by tools/psxs5_sync.py; one game per line:",
             "# title<TAB>serial<TAB>discs<TAB>path<TAB>first disc name"]
    for folder in sorted((p for p in staging.iterdir() if p.is_dir()), key=lambda p: p.name.lower()):
        entry = game_entry(folder)
        if entry:
            title, serial, discs, name, first = entry
            lines.append("\t".join([title, serial, str(discs),
                                    f"{REMOTE_ROOT}/games/{folder.name}/{name}", first]))
    index = staging.parent / "PSXS5_library.txt"  # beside, not inside, the uploaded tree
    index.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"  index: {len(lines) - 2} games in {index}")
    return index


def best_cheat(cheat_dir: Path, title: str, serial: str, disc_name: str) -> Path | None:
    """Same idea as PSXS5's own matcher: exact disc name, else same title, then region."""
    want = {normalize(title), normalize(disc_name)}
    region = ("(USA)" if serial[:4] in ("SLUS", "SCUS") else
              "(Europe)" if serial[:4] in ("SLES", "SCES", "SCED") else
              "(Japan)" if serial else None)
    best, best_score = None, 0
    for cht in cheat_dir.glob("*.cht"):
        if cht.stem.lower() == disc_name.lower():
            return cht
        if normalize(cht.stem) not in want:
            continue
        score = 500
        if region and region in cht.stem:
            score += 100
        elif "(World)" in cht.stem:
            score += 90
        if "(GameShark)" in cht.stem:
            score += 5
        if "(Disc 1)" in cht.stem or "(Disc" not in cht.stem:
            score += 10
        if score > best_score:
            best, best_score = cht, score
    return best


def place_game_cheats(staging: Path, cheat_dir: Path) -> int:
    """Copies each game's best .cht into its folder as cheats.cht."""
    placed = 0
    for folder in (p for p in staging.iterdir() if p.is_dir()):
        entry = game_entry(folder)
        if not entry:
            continue
        cht = best_cheat(cheat_dir, entry[0], entry[1], entry[4])
        if cht:
            shutil.copyfile(cht, folder / "cheats.cht")
            placed += 1
    print(f"  cheats.cht placed for {placed} games")
    return placed


def fetch_cheats(dest: Path) -> Path:
    """Sparse-clones only the PlayStation folder of libretro-database."""
    repo = dest / "libretro-database"
    if not repo.exists():
        subprocess.run(["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
                        CHEATS_REPO, str(repo)], check=True)
    subprocess.run(["git", "-C", str(repo), "sparse-checkout", "set", CHEATS_DIR], check=True)
    subprocess.run(["git", "-C", str(repo), "pull", "--ff-only"], check=False)
    folder = repo / CHEATS_DIR
    print(f"  {len(list(folder.glob('*.cht')))} PlayStation cheat files in {folder}")
    return folder


# --------------------------------------------------------------------- main

# --------------------------------------------------------------------- RetroAchievements

def ra_login(host: str, port: int, user: str | None, hardcore: bool, profile: str | None = None) -> None:
    """Exchanges the password for a login token once, on this PC. Only the
    token goes to the PS5; the password is never written anywhere."""
    import getpass
    import json
    import urllib.parse
    user = user or input("RetroAchievements user name: ").strip()
    password = getpass.getpass("RetroAchievements password (not stored): ")
    data = urllib.parse.urlencode({"r": "login2", "u": user, "p": password}).encode()
    del password
    req = urllib.request.Request("https://retroachievements.org/dorequest.php", data=data,
                                 headers={"User-Agent": "PSXS5-sync/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            reply = json.load(r)
    except urllib.error.HTTPError as e:
        reply = json.load(e) if e.headers.get_content_type() == "application/json" else {}
    if not reply.get("Success") or not reply.get("Token"):
        sys.exit(f"sign-in failed: {reply.get('Error') or 'unknown error'}")
    tmp = Path(tempfile.mkdtemp())
    try:
        (tmp / "retroachievements.ini").write_text(
            "# RetroAchievements sign-in (token only, never the password)\n"
            f"user={reply.get('User') or user}\ntoken={reply['Token']}\nhardcore={int(hardcore)}\n",
            encoding="utf-8", newline="\n")
        # a profile's sign-in goes in its folder (the same name rule as PSXS5's)
        remote = REMOTE_ROOT
        if profile:
            folder = "".join(c if c.isalnum() or c in " -_" else "_" for c in profile)
            remote = f"{REMOTE_ROOT}/profiles/{folder}"
        upload_tree(tmp, remote, host, port, force=True)
    finally:
        shutil.rmtree(tmp)
    who = f" for the profile {profile}" if profile else ""
    print(f"Signed in as {reply.get('User') or user}{who}. Restart PSXS5 (or switch profiles) to use it.")


# Beetle PSX HW looks for one file name per region (scph5500/5501/5502.bin);
# SwanStation takes any of them. The region is the letter that ends the
# version text inside the dump: "System ROM Version 4.1 12/16/97 E".
BIOS_REGION_NAMES = {"J": "scph5500.bin", "A": "scph5501.bin", "E": "scph5502.bin"}


def bios_upload_name(path):
    data = path.read_bytes()
    if len(data) != 512 * 1024:
        sys.exit(f"{path.name}: {len(data)} bytes; a PS1 BIOS dump is 512 KB")
    at = data.rfind(b"System ROM Version")  # the last one: the first is a template
    if at >= 0:
        text = data[at:at + 48].split(b"\0")[0].decode("ascii", "replace").strip()
        region = text[-1:]
        if region in BIOS_REGION_NAMES:
            return BIOS_REGION_NAMES[region]
    print(f"  {path.name}: region not recognised, uploading it under its own name")
    return path.name.lower()


# What PSXS5 loads from a game folder, best first.
INDEX_PICK = [".m3u", ".cue", ".chd", ".pbp", ".iso", ".img", ".mdf", ".ccd", ".bin"]


def ftp_list(ftp, path):
    """(name, is_dir) of a folder. LIST, not NLST: the PS5's servers lack NLST."""
    lines = []
    ftp.retrlines(f"LIST {path}", lines.append)
    out = []
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) == 9 and parts[8] not in (".", ".."):
            out.append((parts[8], line.startswith("d")))
    return out


def ftp_read_text(ftp, path):
    chunks = []
    ftp.retrbinary(f"RETR {path}", chunks.append)
    return b"".join(chunks).decode("utf-8", "replace")


def check_cue(ftp, folder, cue, files, fix):
    """A .cue whose FILE lines name a missing .bin (shortened names such as
    BREATH~1.BIN after a FAT32 copy) can't boot. With fix, a sheet with one
    FILE line is pointed at the folder's only .bin."""
    text = ftp_read_text(ftp, f"{folder}/{cue}")
    named = re.findall(r'^\s*FILE\s+"?([^"\r\n]+?)"?\s+\w+\s*$', text, re.M | re.I)
    missing = [n for n in named if n not in files]
    if not missing:
        return True
    bins = [f for f in files if f.lower().endswith(".bin")]
    if fix and len(named) == 1 and len(bins) == 1:
        fixed = re.sub(r'^(\s*FILE\s+)"?[^"\r\n]+?"?(\s+\w+\s*)$', lambda m: f'{m.group(1)}"{bins[0]}"{m.group(2)}',
                       text, count=1, flags=re.M | re.I)
        ftp.storbinary(f"STOR {folder}/{cue}", io.BytesIO(fixed.encode("utf-8")))
        print(f"    fixed {cue}: now points at {bins[0]}")
        return True
    print(f"    {cue} names {', '.join(missing)}, which isn't in the folder"
          + (" (run again with --fix-cues)" if len(named) == 1 and len(bins) == 1 else ""))
    return False


def index_remote(host, port, fix_cues):
    """library.txt for games already on the PS5, however they got there."""
    ftp = ftp_connect(host, port)
    games_dir = f"{REMOTE_ROOT}/games"
    lines = ["# PSXS5 library index - written by tools/psxs5_sync.py index; one game per line:",
             "# title<TAB>serial<TAB>discs<TAB>path<TAB>first disc name"]
    for name, is_dir in sorted(ftp_list(ftp, games_dir), key=lambda e: e[0].lower()):
        if not is_dir:
            continue
        folder = f"{games_dir}/{name}"
        files = [f for f, d in ftp_list(ftp, folder) if not d]
        by_ext = {}
        for f in sorted(files, key=str.lower):
            by_ext.setdefault(Path(f).suffix.lower(), []).append(f)
        pick = next((by_ext[e][0] for e in INDEX_PICK if e in by_ext), None)
        if not pick:
            print(f"  {name}: no game file, skipped")
            continue
        print(f"  {name}: {pick}")
        for cue in by_ext.get(".cue", []):
            check_cue(ftp, folder, cue, files, fix_cues)
        discs = 1
        if pick.lower().endswith(".m3u"):
            entries = [l.strip() for l in ftp_read_text(ftp, f"{folder}/{pick}").splitlines()
                       if l.strip() and not l.startswith("#")]
            discs = max(1, len(entries))
            first = Path(entries[0]).stem if entries else Path(pick).stem
        else:
            first = Path(pick).stem
        # serial left empty: PSXS5 reads it from the disc (or serial.txt)
        lines.append("\t".join([name, "", str(discs), f"{folder}/{pick}", first]))
    ftp.storbinary(f"STOR {REMOTE_ROOT}/library.txt", io.BytesIO(("\n".join(lines) + "\n").encode("utf-8")))
    ftp.quit()
    print(f"  library.txt: {len(lines) - 2} games. Restart PSXS5 to see them.")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["plan", "prepare", "upload", "sync", "covers", "cheats", "bios", "app",
                                        "ra-login", "index"])
    ap.add_argument("--fix-cues", action="store_true",
                    help="index: point .cue sheets at the folder's .bin when the name they give is missing")
    ap.add_argument("--user", help="ra-login: RetroAchievements user name")
    ap.add_argument("--hardcore", action="store_true", help="ra-login: start in hardcore mode")
    ap.add_argument("--profile", help="ra-login: sign in a PSXS5 profile (its name, as on the console)")
    ap.add_argument("--app-dir", type=Path, default=Path(__file__).resolve().parent.parent / "dist" / "PPSA98510",
                    help="app: the built title folder to install")
    ap.add_argument("--all", action="store_true", help="covers: the whole database, not just your games")
    ap.add_argument("--style", choices=["default", "3d", "both"], default="default",
                    help="covers: flat front art (default), 3D boxes, or both")
    ap.add_argument("--regions", default="usa,europe,japan",
                    help="covers --all: comma-separated subset of usa,europe,japan")
    ap.add_argument("files", nargs="*", help="BIOS file(s) for the bios command")
    ap.add_argument("--source", type=Path, help="the folder with your PS1 games")
    ap.add_argument("--staging", type=Path,
                    help="where prepared games go before the upload (default: PSXS5_ready next to --source, "
                         "on the same drive, so games are linked there rather than copied)")
    ap.add_argument("--host", help="PS5 IP address")
    ap.add_argument("--port", type=int, default=2121, help="etaHEN FTP port")
    ap.add_argument("--only", help="limit prepare to titles containing this text")
    args = ap.parse_args()
    if args.staging is None:
        # beside the games: same drive, so place() hard-links instead of copying
        args.staging = (args.source.resolve().parent if args.source else Path.home()) / "PSXS5_ready"

    if args.command == "index":
        if not args.host:
            sys.exit("--host is required (your PS5's IP address)")
        index_remote(args.host, args.port, args.fix_cues)
        return

    if args.command in {"sync", "plan", "prepare"}:
        if not args.source:
            sys.exit("--source is required: the folder with your PS1 games, e.g. --source \"D:\\Games\\PS1\"")
        if not args.source.is_dir():
            sys.exit(f"{args.source} isn't a folder")
    if args.command == "sync":
        if not args.host:
            sys.exit("--host is required (your PS5's IP address)")
        sync(discover(args.source), args.staging, args.host, args.port, args.only)
        return

    if args.command in {"plan", "prepare"}:
        games = discover(args.source)
        if args.command == "plan":
            print_plan(games)
        else:
            prepare(games, args.staging, args.only)
            print(f"\nReady in {args.staging}. Next: upload --host <PS5 IP>")
        return

    if args.command == "covers":
        styles = ["default", "3d"] if args.style == "both" else [args.style]
        dest = args.staging.parent / "PSXS5_covers"
        if args.all:
            regions = [r.strip().lower() for r in args.regions.split(",") if r.strip()]
            bad = [r for r in regions if r not in REGION_PREFIXES]
            if bad:
                sys.exit(f"unknown region(s): {', '.join(bad)}")
            folder = fetch_all_covers(dest, styles, regions)
        else:
            fetch_my_covers(library_serials(args.staging), dest, styles)
            folder = dest
        if args.host:
            for s in styles:
                upload_tree(folder / s, f"{REMOTE_ROOT}/covers/{s}", args.host, args.port)
        return

    if args.command == "cheats":
        folder = fetch_cheats(args.staging.parent / "PSXS5_cheats")
        if args.staging.exists():
            place_game_cheats(args.staging, folder)
        if args.host:
            upload_tree(folder, f"{REMOTE_ROOT}/cheats", args.host, args.port)
            if args.staging.exists():  # the per-game cheats.cht files
                upload_tree(args.staging, f"{REMOTE_ROOT}/games", args.host, args.port)
        return

    if not args.host:
        sys.exit("--host is required (your PS5's IP address)")
    if args.command == "ra-login":
        ra_login(args.host, args.port, args.user, args.hardcore, args.profile)
        return
    if args.command == "app":
        if not (args.app_dir / "eboot.bin").exists():
            sys.exit(f"no eboot.bin in {args.app_dir}; download the build first")
        upload_tree(args.app_dir, f"/data/homebrew/{args.app_dir.name}", args.host, args.port, force=True)
        return
    if args.command == "upload":
        index = write_index(args.staging)
        upload_tree(args.staging, f"{REMOTE_ROOT}/games", args.host, args.port)
        tmp = Path(tempfile.mkdtemp())
        shutil.copyfile(index, tmp / "library.txt")
        upload_tree(tmp, REMOTE_ROOT, args.host, args.port, force=True)
        shutil.rmtree(tmp)
    elif args.command == "bios":
        tmp = Path(tempfile.mkdtemp())
        for f in args.files:
            name = bios_upload_name(Path(f))
            print(f"  {Path(f).name} -> bios/{name}")
            shutil.copy2(f, tmp / name)
        upload_tree(tmp, f"{REMOTE_ROOT}/bios", args.host, args.port)
        shutil.rmtree(tmp)


if __name__ == "__main__":
    main()
