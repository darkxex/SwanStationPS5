#!/usr/bin/env bash
# SwanStationPS5 v2 - builds Beetle PSX HW (Mednafen PSX, libretro, GPL-2.0) for the PS5
# as a static archive that can live in the same app as PCSX-ReARMed.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-beetle.sh   -> build/beetle-ps5/libbeetle_psx.a
#
# Source: third_party/beetle-psx, Mihawk-99's PS5 fork (PS5_BeetlePSX) at the
# revision PS5 RetroArch ships: its ps5 platform turns on the Vulkan renderer
# and turns off lightrec, the CD-ROM driver and OpenGL.
#
# Both cores are libretro cores: both define retro_init, retro_run... and carry
# their own copies of libretro-common, zlib, libchdr. So the archive is made
# self-contained: every object is linked into one relocatable object, every
# symbol but the libretro API is made local, and the API is renamed to
# beetle_retro_*. src/core/host.c calls the core it runs through a table.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
core="$root/third_party/beetle-psx"
out="$root/build/beetle-ps5"
jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}

[[ -f $core/Makefile ]] || {
    echo "Beetle source missing: run 'git submodule update --init third_party/beetle-psx'" >&2
    exit 2
}
sdk="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
[[ -d $sdk ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null
export PS5_PAYLOAD_SDK=$sdk USE_CCACHE=0
llvm=$(dirname "$(command -v llvm-ar-18 || command -v llvm-ar)")
ar="$llvm/llvm-ar-18"; [[ -x $ar ]] || ar="$llvm/llvm-ar"
objcopy="$llvm/llvm-objcopy-18"; [[ -x $objcopy ]] || objcopy="$llvm/llvm-objcopy"
lld=$(command -v ld.lld-18 || command -v ld.lld)

mkdir -p "$out"
archive="$out/libmednafen_psx_hw.a"

# SwanStationPS5's small changes to the core live as patches (the submodule stays
# pristine); each is applied once.
for patch in "$root"/tools/patches/beetle-psx-*.patch; do
    [[ -f $patch ]] || continue
    if git -C "$core" apply --ignore-whitespace --reverse --check "$patch" 2>/dev/null; then
        continue # already applied
    fi
    echo "==> [beetle] applying $(basename "$patch")"
    git -C "$core" apply --ignore-whitespace "$patch"
done

# Rebuild only when the source revision, this script or a patch changed.
stamp="$out/.stamp"
want="$(git -C "$core" rev-parse HEAD) $(cat "$0" "$root/tools/beetle-archive.mk" "$root"/tools/patches/beetle-psx-*.patch 2>/dev/null | sha256sum | cut -c1-16)"
if [[ ! -f $out/libbeetle_psx.a || ! -f $stamp || $(cat "$stamp") != "$want" ]]; then
    echo "==> [beetle] building Beetle PSX HW ($jobs jobs)"
    make -C "$core" clean >/dev/null 2>&1 || true
    # FLAGS through the environment: the Makefile appends its own with +=.
    export CFLAGS="-O2 -fPIC -ffunction-sections -fdata-sections -march=znver2"
    export CXXFLAGS="$CFLAGS"
    # Not STATIC_LINKING: that build leaves libretro-common to the frontend,
    # and SwanStationPS5's copy belongs to PCSX-ReARMed. The shared-library source list
    # carries Beetle's own; beetle-archive.mk archives it instead of linking.
    make -C "$core" -f Makefile -f "$root/tools/beetle-archive.mk" -j"$jobs" platform=ps5 \
        TARGET=libmednafen_psx_hw.a \
        CC="sh $root/tooling/prospero-clang18" CXX="sh $root/tooling/prospero-clang18 -x c++" \
        AR="$ar" SwanStationPS5-archive
    cp "$core/libmednafen_psx_hw.a" "$archive"

    echo "==> [beetle] isolating its symbols"
    work="$out/isolate"
    rm -rf "$work"
    mkdir -p "$work"
    "$lld" -r --whole-archive "$archive" -o "$work/beetle.o"
    # keep only the libretro API global, then rename it
    nm="$llvm/llvm-nm-18"; [[ -x $nm ]] || nm="$llvm/llvm-nm"
    "$nm" --defined-only --extern-only "$work/beetle.o" | awk '{print $NF}' |
        grep -E '^retro_' | sort -u >"$work/api.txt"
    [[ -s $work/api.txt ]] || { echo "no retro_* symbols in Beetle" >&2; exit 1; }
    sed 's/.*/& beetle_&/' "$work/api.txt" >"$work/rename.txt"
    "$objcopy" --keep-global-symbols="$work/api.txt" "$work/beetle.o" "$work/beetle-local.o"
    "$objcopy" --redefine-syms="$work/rename.txt" "$work/beetle-local.o" "$work/beetle-psx.o"
    rm -f "$out/libbeetle_psx.a"
    "$ar" rcs "$out/libbeetle_psx.a" "$work/beetle-psx.o"
    echo "$want" >"$stamp"
    echo "==> [beetle] $(wc -l <"$work/api.txt") libretro functions as beetle_retro_*"
fi
echo "==> [beetle] $out/libbeetle_psx.a"
