#!/usr/bin/env bash
# PSXS5 interface preview: builds the screens for Windows (zig + SDL2 for
# mingw) with a fake core, runs a button script and saves screenshots.
#
#   bash tools/ui-preview/build.sh [script]   -> build/preview/shots/*.png
#
# Needs: python -m ziglang, build/SDL2-<ver>/x86_64-w64-mingw32 (SDL2-devel mingw
# release), Pillow, and a data folder in build/preview-root (library.txt, covers).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
sdl=$(ls -d build/SDL2-*/x86_64-w64-mingw32 | head -1)
out=build/preview
mkdir -p "$out/shots"
export ZIG_GLOBAL_CACHE_DIR="$root/build/zigcache" ZIG_LOCAL_CACHE_DIR="$root/build/zigcache"

sources=(src/cheats.c src/config.c src/covers.c src/disc.c src/i18n.c src/library.c src/main.c
    src/net.c src/stats.c src/tips.c src/controls.c src/bezels.c src/art.c src/profiles.c src/gamedb.c src/stb_impl.c src/util.c src/play.c src/remote.c src/update.c src/vendor.c src/platform/vk/vk_probe.c src/platform/plat_sdl.c src/platform/xbr.c src/platform/blit.c
    src/ui/*.c tools/ui-preview/stubs.c)
python -m ziglang cc -target x86_64-windows-gnu -std=gnu11 -O2 -w \
    -DPSXS5_PREVIEW -DSDL_MAIN_HANDLED -include tools/ui-preview/compat.h \
    -Isrc -Ithird_party/stb -Ithird_party/swanstation/dep/libretro-common/include \
    -Ithird_party/rcheevos/include -I"$sdl/include" \
    "${sources[@]}" "$sdl/lib/libSDL2.dll.a" -o "$out/psxs5-preview.exe"
cp "$sdl/bin/SDL2.dll" "$out/"
echo "built $out/psxs5-preview.exe"

script=${1:-tools/ui-preview/tour.txt}
rm -f "$out"/shots/*.bmp
(
    cd "$out"
    PSXS5_ROOT="$root/build/preview-root" PSXS5_SCRIPT="$root/$script" PSXS5_SHOTS="shots" \
        PSXS5_ASSETS="$root/assets" PSXS5_BANNER="${PSXS5_BANNER:-}" SDL_AUDIODRIVER=dummy \
        ./psxs5-preview.exe
)
python - "$out/shots" <<'PY'
import sys, pathlib
from PIL import Image
for bmp in pathlib.Path(sys.argv[1]).glob("*.bmp"):
    Image.open(bmp).save(bmp.with_suffix(".png"))
    bmp.unlink()
print("screenshots:", ", ".join(sorted(p.name for p in pathlib.Path(sys.argv[1]).glob("*.png"))))
PY
