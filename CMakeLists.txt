# SwanStationPS5 - desktop (Windows) build, next to the PS5 one (Makefile / build.ps1, which this file never touches).
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   cmake -S . -B build/pc -G Ninja          (from an MSYS2 UCRT64 shell: pacman -S make mingw-w64-ucrt-x86_64-{gcc,SDL2,cmake,ninja})
#   cmake --build build/pc                   -> build/pc/SwanStationPS5.exe
#
# One source tree, two builds: the PS5 defines __PROSPERO__ by itself; this one defines SwanStationPS5_DESKTOP.
# Vulkan is the main renderer (SS5_PC_VULKAN=ON, the default): src/platform/vk/vk_present_desktop.c is the presenter
# (SDL_Vulkan surface + swapchain; the PS5 builds vk_present.c, the same presenter with VK_KHR_display) and
# SwanStation renders through its Vulkan renderer. If Vulkan cannot open, the app falls back to the window's own
# surface and the core's software renderer. SS5_PC_VULKAN=OFF is the software-only build (milestone 1).
cmake_minimum_required(VERSION 3.20)
project(SwanStationPS5 C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(SS5_PC_VULKAN "Vulkan renderer through SDL_Vulkan (needs the Vulkan headers); OFF: software only" ON)

find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(SS5_SDL2_TARGET PkgConfig::SDL2)
else()
  set(SS5_SDL2_TARGET SDL2::SDL2)
endif()

# ---- SwanStation, built for Windows by tools/build-swanstation-pc.sh (needs bash + make from MSYS2) ----
find_program(SS5_BASH bash HINTS "C:/msys64/usr/bin" REQUIRED)
set(SS5_CORE_DIR ${CMAKE_BINARY_DIR}/swanstation)
set(SS5_CORE_LIB ${SS5_CORE_DIR}/libswanstation.a)
add_custom_command(
  OUTPUT ${SS5_CORE_LIB}
  COMMAND ${SS5_BASH} ${CMAKE_SOURCE_DIR}/tools/build-swanstation-pc.sh ${SS5_CORE_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/build-swanstation-pc.sh ${CMAKE_SOURCE_DIR}/tools/swanstation-archive-pc.mk
          ${CMAKE_SOURCE_DIR}/tools/patches/SSPS5.patch
  COMMENT "Building the SwanStation core (first time: several minutes)"
  USES_TERMINAL VERBATIM)
add_custom_target(swanstation_core DEPENDS ${SS5_CORE_LIB})

# ---- rcheevos (MIT) ----
file(GLOB_RECURSE RC_SOURCES CONFIGURE_DEPENDS third_party/rcheevos/src/*.c)
list(FILTER RC_SOURCES EXCLUDE REGEX "rc_client_raintegration\\.c$")
add_library(rcheevos STATIC ${RC_SOURCES})
target_compile_definitions(rcheevos PRIVATE RC_CLIENT_SUPPORTS_HASH)
target_include_directories(rcheevos PUBLIC third_party/rcheevos/include
                                           third_party/swanstation/dep/libretro-common/include)
target_compile_options(rcheevos PRIVATE -w)

# ---- the application ----
file(GLOB UI_SOURCES CONFIGURE_DEPENDS src/ui/*.c)
set(APP_SOURCES
  src/art.c src/bezels.c src/cheats.c src/config.c src/controls.c src/covers.c src/disc.c src/discimport.c
  src/gamedb.c src/i18n.c src/library.c src/main.c src/net.c src/play.c src/profiles.c src/remote.c
  src/state_save.c src/stats.c src/stb_impl.c src/tips.c src/update.c src/util.c src/vendor.c
  src/core/bios_choices.c src/core/host.c src/core/swanstation_options.c
  src/ra/achievements.c src/ra/login.c
  src/platform/plat_sdl.c src/platform/xbr.c src/platform/blit.c
  src/platform/desktop_compat.c src/cdscsi.c src/platform/vk/vk_probe.c
  ${UI_SOURCES})

if(SS5_PC_VULKAN)
  # vk_present.h is the shared interface; the PS5 implements it with VK_KHR_display (vk_present.c), the PC with an
  # SDL_Vulkan surface + swapchain (vk_present_desktop.c, which includes vk_present.c).
  list(APPEND APP_SOURCES src/platform/vk/vk_present_desktop.c)
endif()

add_executable(SwanStationPS5 ${APP_SOURCES})
add_dependencies(SwanStationPS5 swanstation_core)
target_compile_definitions(SwanStationPS5 PRIVATE SwanStationPS5_DESKTOP SDL_MAIN_HANDLED)
if(SS5_PC_VULKAN)
  target_compile_definitions(SwanStationPS5 PRIVATE SwanStationPS5_VULKAN)
endif()
target_include_directories(SwanStationPS5 PRIVATE src src/platform/desktop_shim third_party/stb
  third_party/swanstation/dep/libretro-common/include)
target_compile_options(SwanStationPS5 PRIVATE -w -include ${CMAKE_SOURCE_DIR}/src/platform/desktop_compat.h)
# SwanStation is C++: link with the C++ driver, and with the system libraries its Windows backends use.
set_target_properties(SwanStationPS5 PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(SwanStationPS5 PRIVATE ${SS5_CORE_LIB} rcheevos ${SS5_SDL2_TARGET}
  winmm d3d11 dxgi d3d12 gdi32 user32 ole32 ws2_32 shlwapi)

# Data goes next to the exe unless SwanStationPS5_ROOT says otherwise (default ./SwanStationPS5-data).
add_custom_command(TARGET SwanStationPS5 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/assets)
