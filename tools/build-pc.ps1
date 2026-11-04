# SwanStationPS5 - builds the Windows (PC) version with MSYS2 UCRT64 + CMake + Ninja.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools\build-pc.ps1                       builds in this checkout:   build\pc\SwanStationPS5.exe
#   tools\build-pc.ps1 -Work C:\ss5pc        copies the sources to C:\ss5pc first (for a checkout on a WSL / network
#                                            path, which MSYS2's make cannot build from), then builds there
#   tools\build-pc.ps1 -Run -Rom game.cue    and runs it (SwanStationPS5_ROOT=<work>\data unless set)
#
# Needs MSYS2 (C:\msys64) with: pacman -S mingw-w64-ucrt-x86_64-{gcc,SDL2,vulkan-headers} make
# and CMake + Ninja on PATH. The PS5 build (Makefile / build.ps1) is not touched.
param(
    [string]$Work = "",
    [string]$Msys = "C:\msys64",
    [switch]$Run,
    [string]$Rom = ""
)
$ErrorActionPreference = "Stop"
$source = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath
$dir = $source
if ($Work) {
    $dir = $Work
    New-Item -ItemType Directory -Force $dir | Out-Null
    # Everything but .git and build output. The core's sources (third_party\swanstation) are copied once: the
    # build patches them in place (tools\patches\SSPS5.patch), so a later copy must not overwrite them.
    $skip = @(".git", "build")
    if (Test-Path (Join-Path $dir "third_party\swanstation\Makefile.libretro")) { $skip += "swanstation" }
    robocopy $source $dir /E /XD @skip /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    # a submodule's .git file points back into the checkout's .git, which is not copied
    Remove-Item (Join-Path $dir "third_party\swanstation\.git"), (Join-Path $dir "third_party\rcheevos\.git") -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path (Join-Path $dir "third_party\swanstation\Makefile.libretro"))) {
    throw "third_party\swanstation is empty: run 'git submodule update --init' in the checkout"
}

$env:PATH = "$Msys\ucrt64\bin;$Msys\usr\bin;$env:PATH;C:\Program Files\Git\cmd"
$env:TMP = $env:TEMP = [System.IO.Path]::GetTempPath().TrimEnd('\')
Push-Location $dir
try {
    cmake -S . -B build/pc -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DSS5_PC_VULKAN=ON
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    cmake --build build/pc
    if ($LASTEXITCODE) { throw "build failed" }
    foreach ($dll in "SDL2.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll") {
        Copy-Item "$Msys\ucrt64\bin\$dll" build\pc -Force -ErrorAction SilentlyContinue
    }
    Write-Host "built $dir\build\pc\SwanStationPS5.exe"
    if ($Run) {
        if (-not $env:SwanStationPS5_ROOT) { $env:SwanStationPS5_ROOT = Join-Path $dir "data" }
        $exe = @("build\pc\SwanStationPS5.exe")
        $argv = @(); if ($Rom) { $argv = @("--rom", $Rom) }
        & $exe[0] @argv
    }
} finally { Pop-Location }
