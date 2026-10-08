#!/usr/bin/env bash
# PSXS5 - builds SwanStation (libretro's DuckStation fork, GPL-3.0) for the PS5
# as a static archive that lives in the same app as the frontend (and Beetle, if built).
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-swanstation.sh   -> build/swanstation-ps5/libswanstation.a
#
# Source: third_party/swanstation + tools/patches/swanstation-ps5.patch. The
# port is that one patch: the x64 recompiler's code buffer comes from
# ps5platform/exec.h instead of an RWX mmap a title cannot make, the 48 MiB
# in-image code buffer is dropped, and the 4 GiB MMap fastmem scheme (aliased
# views plus a fault handler) is left out so the recompiler uses its LUT path.
#
# Like the other cores, the archive is made self-contained: every object is
# linked into one relocatable object, every symbol but the libretro API is
# made local, and the API is renamed to swanstation_retro_*.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
core="$root/third_party/swanstation"
out="$root/build/swanstation-ps5"
jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}

[[ -f $core/Makefile.libretro ]] || {
    echo "SwanStation source missing: run 'git submodule update --init --recursive third_party/swanstation'" >&2
    exit 2
}
sdk="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
[[ -d $sdk ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null
export PS5_PAYLOAD_SDK=$sdk USE_CCACHE=0
llvm=$(dirname "$(command -v llvm-ar-18 || command -v llvm-ar)")
ar="$llvm/llvm-ar-18"; [[ -x $ar ]] || ar="$llvm/llvm-ar"
objcopy="$llvm/llvm-objcopy-18"; [[ -x $objcopy ]] || objcopy="$llvm/llvm-objcopy"
nm="$llvm/llvm-nm-18"; [[ -x $nm ]] || nm="$llvm/llvm-nm"
lld=$(command -v ld.lld-18 || command -v ld.lld)

mkdir -p "$out"
archive="$out/libswanstation_hw.a"

# PSXS5's changes live as patches (the submodule stays pristine); each is
# applied once.
for patch in "$root"/tools/patches/swanstation-*.patch; do
    [[ -f $patch ]] || continue
    if git -C "$core" apply --ignore-whitespace --reverse --check "$patch" 2>/dev/null; then
        continue # already applied
    fi
    echo "==> [swanstation] applying $(basename "$patch")"
    git -C "$core" apply --ignore-whitespace "$patch"
done

# Rebuild only when the source revision, this script or a patch changed.
stamp="$out/.stamp"
want="$(git -C "$core" rev-parse HEAD) $(cat "$0" "$root/tools/swanstation-archive.mk" "$root"/tools/patches/swanstation-*.patch 2>/dev/null | sha256sum | cut -c1-16)"
if [[ ! -f $out/libswanstation.a || ! -f $stamp || $(cat "$stamp") != "$want" ]]; then
    echo "==> [swanstation] building SwanStation ($jobs jobs)"
    make -C "$core" -f Makefile.libretro clean >/dev/null 2>&1 || true
    # FLAGS through the environment: the Makefile appends its own with +=.
    # -DZSTD_TRACE=0: zstd's weak tracing hooks are not in the title's import
    # table. SwanStation needs exceptions (xbyak throws) and RTTI, which the
    # prospero target leaves off.
    export CFLAGS="-O2 -fPIC -w -ffunction-sections -fdata-sections -march=znver2 -DZSTD_TRACE=0"
    export CXXFLAGS="$CFLAGS -frtti -fexceptions"
    # WITH_MMAP_FASTMEM=0: the mmap fastmem scheme does not survive a PS5
    # title's sandbox; the recompiler stays on with LUT fastmem.
    # CONFIG_HAVE_ALLOCA_H empty: the SDK has no <alloca.h> (alloca is in
    # <stdlib.h>, as on FreeBSD).
    make -s -C "$core" -f Makefile.libretro -f "$root/tools/swanstation-archive.mk" -j"$jobs" \
        platform=unix CPU_ARCH=x64 WITH_MMAP_FASTMEM=0 LINK_STATIC_LIBCPLUSPLUS=0 CONFIG_HAVE_ALLOCA_H= \
        TARGET=libswanstation_hw.a \
        CC="sh $root/tooling/prospero-clang18" CXX="sh $root/tooling/prospero-clang18 -x c++" \
        AR="$ar" psxs5-archive
    cp "$core/libswanstation_hw.a" "$archive"

    echo "==> [swanstation] isolating its symbols"
    work="$out/isolate"
    rm -rf "$work"
    mkdir -p "$work"
    "$lld" -r --whole-archive "$archive" -o "$work/swanstation.o"
    # keep only the libretro API global, then rename it
    "$nm" --defined-only --extern-only "$work/swanstation.o" | awk '{print $NF}' |
        grep -E '^retro_' | sort -u >"$work/api.txt"
    [[ -s $work/api.txt ]] || { echo "no retro_* symbols in SwanStation" >&2; exit 1; }
    sed 's/.*/& swanstation_&/' "$work/api.txt" >"$work/rename.txt"
    "$objcopy" --keep-global-symbols="$work/api.txt" "$work/swanstation.o" "$work/swanstation-local.o"
    "$objcopy" --redefine-syms="$work/rename.txt" "$work/swanstation-local.o" "$work/swanstation-ps5.o"
    rm -f "$out/libswanstation.a"
    "$ar" rcs "$out/libswanstation.a" "$work/swanstation-ps5.o"
    echo "$want" >"$stamp"
    echo "==> [swanstation] $(wc -l <"$work/api.txt") libretro functions as swanstation_retro_*"
fi
echo "==> [swanstation] $out/libswanstation.a"
