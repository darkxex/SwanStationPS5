#!/usr/bin/env bash
# SwanStationPS5 - builds frame generation (third_party/framegen: AMD FSR 3's frame
# interpolation as PS5SX2 runs it, GPL-3.0-or-later / MIT) for the PS5 as a
# static archive, behind the C interface in src/platform/vk/fg_bridge.h.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build-framegen.sh   -> build/framegen-ps5/libframegen.a
#
# Like the cores, the archive is self-contained: everything is linked into one
# relocatable object and every symbol but ssfg_* is made local (the Vulkan
# command pointers included).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src="$root/third_party/framegen"
out="$root/build/framegen-ps5"
sdk="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
[[ -d $sdk ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null
vulkan="$root/.deps/native/radv-release/include"
[[ -d $vulkan ]] || bash "$root/tools/fetch-radv.sh"
export PS5_PAYLOAD_SDK=$sdk USE_CCACHE=0
llvm=$(dirname "$(command -v llvm-ar-18 || command -v llvm-ar)")
ar="$llvm/llvm-ar-18"; [[ -x $ar ]] || ar="$llvm/llvm-ar"
objcopy="$llvm/llvm-objcopy-18"; [[ -x $objcopy ]] || objcopy="$llvm/llvm-objcopy"
nm="$llvm/llvm-nm-18"; [[ -x $nm ]] || nm="$llvm/llvm-nm"
lld=$(command -v ld.lld-18 || command -v ld.lld)

stamp="$out/.stamp"
want=$(cat "$0" "$src"/*.cpp "$src"/*.h "$src"/bridge/* "$src"/framegen/*.inc "$root/src/platform/vk/fg_bridge.h" |
    sha256sum | cut -c1-16)
if [[ -f $out/libframegen.a && -f $stamp && $(cat "$stamp") == "$want" ]]; then
    echo "==> [framegen] $out/libframegen.a"
    exit 0
fi

echo "==> [framegen] building"
rm -rf "$out"
mkdir -p "$out"
flags=(-x c++ -std=c++20 -O2 -fPIC -w -ffunction-sections -fdata-sections -march=znver2 -frtti -fexceptions
    -I"$src" -I"$src/bridge" -I"$vulkan" -include "$src/bridge/fg_vk.h")
for file in "$src/ps5_framegen.cpp" "$src/bridge/fg_bridge.cpp"; do
    sh "$root/tooling/prospero-clang18" "${flags[@]}" -c "$file" -o "$out/$(basename "${file%.cpp}").o"
done
"$lld" -r "$out/ps5_framegen.o" "$out/fg_bridge.o" -o "$out/framegen.o"
"$nm" --defined-only --extern-only "$out/framegen.o" | awk '{print $NF}' | grep -E '^ssfg_' | sort -u >"$out/api.txt"
[[ -s $out/api.txt ]] || { echo "no ssfg_* symbols" >&2; exit 1; }
"$objcopy" --keep-global-symbols="$out/api.txt" "$out/framegen.o" "$out/framegen-local.o"
"$ar" rcs "$out/libframegen.a" "$out/framegen-local.o"
echo "$want" >"$stamp"
echo "==> [framegen] $(wc -l <"$out/api.txt") functions, $out/libframegen.a"
