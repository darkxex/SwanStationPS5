#!/usr/bin/env bash
# SwanStationPS5 - builds SwanStation (libretro's DuckStation fork, GPL-3.0) for Windows (MSYS2 UCRT64)
# as a static archive linked into the desktop build. The PC twin of tools/build-swanstation.sh.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-swanstation-pc.sh   -> build/pc/swanstation/libswanstation.a
#
# Same source (third_party/swanstation + tools/patches/SSPS5.patch; the patch's PS5 parts are behind
# __PROSPERO__ and do nothing here), one relocatable object: the libretro API is renamed to
# swanstation_retro_*. Run it from an MSYS2 UCRT64 shell
# (pacman -S make mingw-w64-ucrt-x86_64-gcc); CMake does it for you.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
core="$root/third_party/swanstation"
out="${1:-$root/build/pc/swanstation}"
jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}

[[ -f $core/Makefile.libretro ]] || {
    echo "SwanStation source missing: run 'git submodule update --init third_party/swanstation'" >&2
    exit 2
}
mkdir -p "$out"
archive="$out/libswanstation_hw.a"

for patch in "$root"/tools/patches/SSPS5.patch; do
    [[ -f $patch ]] || continue
    if git -C "$core" apply --ignore-whitespace --reverse --check "$patch" 2>/dev/null; then
        continue # already applied
    fi
    echo "==> [swanstation-pc] applying $(basename "$patch")"
    git -C "$core" apply --ignore-whitespace "$patch"
done

stamp="$out/.stamp"
want="$(git -C "$core" rev-parse HEAD 2>/dev/null || echo nogit) $(cat "$0" "$root/tools/swanstation-archive-pc.mk" "$root/tools/patches/SSPS5.patch" | sha256sum | cut -c1-16)"
if [[ ! -f $out/libswanstation.a || ! -f $stamp || $(cat "$stamp") != "$want" ]]; then
    echo "==> [swanstation-pc] building SwanStation ($jobs jobs)"
    export CFLAGS="-O2 -w -ffunction-sections -fdata-sections"
    export CXXFLAGS="$CFLAGS -frtti -fexceptions"
    # WITH_MMAP_FASTMEM=0: the recompiler stays on with LUT fastmem, as on the PS5 (no fault handler to debug).
    make -s -C "$core" -f Makefile.libretro -f "$root/tools/swanstation-archive-pc.mk" -j"$jobs" \
        platform=win CPU_ARCH=x64 WITH_MMAP_FASTMEM=0 \
        TARGET=libswanstation_hw.a SwanStationPS5-archive
    cp "$core/libswanstation_hw.a" "$archive"

    echo "==> [swanstation-pc] isolating its symbols"
    work="$out/isolate"
    rm -rf "$work"
    mkdir -p "$work"
    ld -r --whole-archive "$archive" -o "$work/swanstation.o"
    nm --defined-only --extern-only "$work/swanstation.o" | awk '{print $NF}' |
        grep -E '^retro_' | sort -u >"$work/api.txt"
    [[ -s $work/api.txt ]] || { echo "no retro_* symbols in SwanStation" >&2; exit 1; }
    sed 's/.*/& swanstation_&/' "$work/api.txt" >"$work/rename.txt"
    # Only the API is renamed: localizing the rest (as the PS5 script does) breaks the COMDAT sections of COFF
    # objects, and the app's own C code shares no symbol names with the core.
    # The libraries the core bundles (stb_image, miniz) are also in the app: keep the core's copies private.
    # The .drectve section carries the core's -export: directives for the old retro_* names.
    nm --defined-only --extern-only "$work/swanstation.o" | awk '{print $NF}' |
        grep -E '^(stbi_|stbir_|stbiw_|mz_|tinfl_|tdefl_|miniz_)' | sort -u >"$work/private.txt" || true
    objcopy --redefine-syms="$work/rename.txt" --remove-section=.drectve         $(sed 's/.*/--localize-symbol=&/' "$work/private.txt") "$work/swanstation.o" "$work/swanstation-pc.o"
    rm -f "$out/libswanstation.a"
    ar rcs "$out/libswanstation.a" "$work/swanstation-pc.o"
    echo "$want" >"$stamp"
    echo "==> [swanstation-pc] $(wc -l <"$work/api.txt") libretro functions as swanstation_retro_*"
fi
echo "==> [swanstation-pc] $out/libswanstation.a"
