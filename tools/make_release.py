#!/usr/bin/env python3
"""Compiles SwanStationPS5 and packs the release asset, PPSA98510.zip, in the project root.

    python3 tools/make_release.py                 # make, then PPSA98510.zip
    python3 tools/make_release.py --skip-build    # only pack what dist/ already holds
    python3 tools/make_release.py --jobs 12 --no-nice

The zip holds just the app folder, PPSA98510/, as the releases have since 1.0.0: that is what the
in-app updater installs, and what you copy to /data/homebrew. The release tag is the version from
PSXS5_VERSION in src/psxs5.h, without a "v" (1.0.1).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = "PPSA98510"


def read_version():
    text = (ROOT / "src/psxs5.h").read_text()
    m = re.search(r'#define\s+PSXS5_VERSION\s+"([^"]+)"', text)
    if not m:
        sys.exit("error: PSXS5_VERSION not found in src/psxs5.h")
    return m.group(1)


def build(jobs, use_nice):
    env = dict(os.environ, BUILD_JOBS=str(jobs))
    cmd = ["make"]
    if use_nice:
        cmd = ["nice", "-n", "19"] + cmd
    print("==> " + " ".join(cmd) + f"   (BUILD_JOBS={jobs})", flush=True)
    result = subprocess.run(cmd, cwd=ROOT, env=env)
    if result.returncode != 0:
        sys.exit(f"error: the build failed (make exit code {result.returncode}); no zip was made")


def pack():
    app_dir = ROOT / "dist" / APP
    if not (app_dir / "eboot.bin").is_file():
        sys.exit(f"error: {app_dir / 'eboot.bin'} is missing: build first (drop --skip-build)")
    out = ROOT / f"{APP}.zip"
    if out.exists():
        out.unlink()
    count = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(app_dir.rglob("*")):
            if path.is_file():
                zf.write(path, f"{APP}/{path.relative_to(app_dir).as_posix()}")
                count += 1
    return out, count


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--skip-build", action="store_true", help="do not run make; pack dist/ as it is")
    ap.add_argument("--jobs", type=int, default=6, help="parallel compile jobs (default 6)")
    ap.add_argument("--no-nice", action="store_true", help="run make at normal priority (faster, hogs the PC)")
    args = ap.parse_args()

    version = read_version()
    print(f"SwanStationPS5 {version}")
    if not args.skip_build:
        build(args.jobs, not args.no_nice)
    out, count = pack()
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    print(f"==> {out}  ({count} files, {out.stat().st_size / 1e6:.1f} MB)")
    print(f"    sha256 {digest}")
    print(f"    publish: gh release create {version} {out.name} --repo darkxex/SwanStationPS5 "
          f"--title \"SwanStationPS5 {version}\" --generate-notes --latest")


main()
