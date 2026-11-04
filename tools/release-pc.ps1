# SwanStationPS5 - builds the Windows (PC) version and packs it in a zip that runs on any Windows PC.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools\release-pc.ps1                 build (tools\build-pc.ps1 in C:\ss5pc), app folder to dist\SwanStationPS5forWindows
#   tools\release-pc.ps1 -SkipBuild      the same with what the last build left
#   make releasepc                       the same from the WSL checkout, then dist/SwanStationPS5forWindows.zip
#
# The folder holds the exe, the DLLs it needs, the assets and a short README. No user data (BIOS, games, saves).
param(
    [string]$Work = "C:\ss5pc",
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$source = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build-pc.ps1") -Work $Work
}
$out = Join-Path $Work "build\pc"
if (-not (Test-Path (Join-Path $out "SwanStationPS5.exe"))) { throw "no $out\SwanStationPS5.exe: build first" }

$header = Get-Content (Join-Path $source "src\SwanStationPS5.h") | Select-String 'define SwanStationPS5_VERSION "([^"]+)"'
$version = $header.Matches[0].Groups[1].Value
$name = "SwanStationPS5forWindows"
$stage = Join-Path $Work "release\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $out "SwanStationPS5.exe") $stage
Copy-Item (Join-Path $out "*.dll") $stage
Copy-Item (Join-Path $out "assets") (Join-Path $stage "assets") -Recurse
New-Item -ItemType Directory -Force (Join-Path $stage "SwanStationPS5-data\bios"), (Join-Path $stage "SwanStationPS5-data\games") | Out-Null
Set-Content -Encoding UTF8 (Join-Path $stage "README-PC.txt") @"
SwanStationPS5 $version for Windows

1. Put a PlayStation BIOS (.bin) in SwanStationPS5-data\bios
2. Put your games (.cue/.bin, .chd, .pbp ...) in SwanStationPS5-data\games
3. Run SwanStationPS5.exe

F11: full screen / window.
Needs a GPU with Vulkan 1.1 drivers (NVIDIA, AMD or Intel; up to date).
Settings, saves and covers are kept in SwanStationPS5-data next to the exe.
"@

$dist = Join-Path $source "dist"
New-Item -ItemType Directory -Force $dist | Out-Null
# the app folder goes to dist\ (as the PS5 build leaves dist\PPSA98510); the Makefile zips it the way make deploy does
$target = Join-Path $dist $name
if (Test-Path $target) { Remove-Item $target -Recurse -Force }
Copy-Item $stage $target -Recurse
Write-Host "app folder: $target"
