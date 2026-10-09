#!/usr/bin/env bash
# SwanStationPS5 v2 - fetches the RADV Vulkan driver build (made by the "Build RADV"
# workflow, .github/workflows/radv.yml) and installs it for tools/build.sh:
#   .deps/native/radv-release/                 the driver archive and Vulkan headers
#   .deps/native/ps5-payload-sdk/target/...    + the SDK fork's platform layer
# Needs gh (GH_TOKEN in CI) because the repository may be private.
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tag=${RADV_RELEASE_TAG:-deps-radv-7b59ef27}
native="$root/.deps/native"
sdk="$native/ps5-payload-sdk"

if [[ -f $native/radv-release/.tag && $(cat "$native/radv-release/.tag") == "$tag" &&
      -f $sdk/target/lib/libps5platform.a ]]; then
    exit 0
fi
[[ -d $sdk/target/lib ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo "==> [vulkan] downloading $tag"
gh release download "$tag" -R "${GITHUB_REPOSITORY:-SynoPiia/SwanStationPS5}" -p '*.tar.zst' -D "$work"

rm -rf "$native/radv-release"
tar --zstd -xf "$work/radv-release.tar.zst" -C "$native"
echo "$tag" > "$native/radv-release/.tag"

# Only the platform layer is taken from the fork's SDK: the rest is the same
# v0.42 release tools/setup-native-dependencies.sh installed.
tar --zstd -xf "$work/ps5-payload-sdk.tar.zst" -C "$work" \
    ps5-payload-sdk/target/lib/libps5platform.a ps5-payload-sdk/target/include/ps5platform
cp "$work/ps5-payload-sdk/target/lib/libps5platform.a" "$sdk/target/lib/"
rm -rf "$sdk/target/include/ps5platform"
cp -r "$work/ps5-payload-sdk/target/include/ps5platform" "$sdk/target/include/"
echo "==> [vulkan] RADV and the platform layer are in $native"
