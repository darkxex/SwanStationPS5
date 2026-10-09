#!/usr/bin/env bash
# SwanStationPS5 - builds rcheevos (RetroAchievements, MIT) as a static library.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-rcheevos.sh ps5       -> build/rcheevos-ps5/librcheevos.a
#   tools/build-rcheevos.sh desktop   -> build/rcheevos-desktop/librcheevos.a

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-ps5}
src="$root/third_party/rcheevos"
out="$root/build/rcheevos-$target"
[[ -f $src/include/rc_client.h ]] || {
    echo "rcheevos missing: run 'git submodule update --init --recursive'" >&2
    exit 2
}

case $target in
ps5)
    sdk="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
    [[ -d $sdk ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null
    export PS5_PAYLOAD_SDK=$sdk USE_CCACHE=0
    cc=(sh "$root/tooling/prospero-clang18")
    ar=$(command -v llvm-ar-18 || command -v llvm-ar)
    flags=(-O2 -fPIC -ffunction-sections -fdata-sections -DNDEBUG) # no __assert in the PS5 libc
    ;;
desktop)
    cc=("${CC:-gcc}")
    ar=${AR:-ar}
    flags=(-O2 -fPIC -g)
    ;;
*)
    echo "usage: tools/build-rcheevos.sh [ps5|desktop]" >&2
    exit 2
    ;;
esac

# Everything except RAIntegration (a Windows-only toolkit DLL bridge).
mapfile -t sources < <(cd "$src" && find src -name '*.c' ! -name 'rc_client_raintegration.c' | sort)
mkdir -p "$out/obj"
objects=()
for s in "${sources[@]}"; do
    o="$out/obj/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    if [[ ! -f $o || $src/$s -nt $o ]]; then
        "${cc[@]}" "${flags[@]}" -std=gnu99 -DRC_CLIENT_SUPPORTS_HASH \
            -I"$src/include" -I"$root/third_party/swanstation/dep/libretro-common/include" \
            -c "$src/$s" -o "$o"
    fi
    objects+=("$o")
done
rm -f "$out/librcheevos.a"
"$ar" rcs "$out/librcheevos.a" "${objects[@]}"
echo "==> [rcheevos] $out/librcheevos.a (${#objects[@]} objects)"
