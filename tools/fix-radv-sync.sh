#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source="$root/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
out="$root/build/radv-sync"
expected=39921c4e1c19fecc5c7914e022cd10be01e0325ca990cc027960a1777cf85b96
actual=$(sha256sum "$source" | cut -d' ' -f1)
[[ $actual == "$expected" ]] || { echo "Unrecognized RADV archive for the timed-wait fix" >&2; exit 1; }
stamp="$actual $(sha256sum "$0" | cut -d' ' -f1)"
if [[ -f $out/.stamp && $(cat "$out/.stamp") == "$stamp" && -f $out/libvulkan_radeon.ps5.a ]]; then
    exit 0
fi

ar=${LLVM_AR:-$(command -v llvm-ar-18 || command -v llvm-ar)}
objcopy=${LLVM_OBJCOPY:-$(command -v llvm-objcopy-18 || command -v llvm-objcopy)}
nm=${LLVM_NM:-$(command -v llvm-nm-18 || command -v llvm-nm)}
mkdir -p "$out"
members=(wsi_common_videoout.c.o winsys_ps5_radv_ps5_sync.c.o)
# Leave the driver's other C11 waits on their original clock.
(
    cd "$out"
    "$ar" x "$source" "${members[@]}"
    for member in "${members[@]}"; do
        symbols=$("$nm" --undefined-only "$member")
        grep -q ' U cnd_timedwait$' <<< "$symbols"
        "$objcopy" --redefine-sym cnd_timedwait=ss_radv_cnd_timedwait "$member"
    done
    cp "$source" libvulkan_radeon.ps5.a.tmp
    "$ar" rcs libvulkan_radeon.ps5.a.tmp "${members[@]}"
    mv libvulkan_radeon.ps5.a.tmp libvulkan_radeon.ps5.a
    printf '%s\n' "$stamp" > .stamp
)
