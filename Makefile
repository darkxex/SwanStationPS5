# PSXS5 - Linux/WSL build entry points (based on ps5-native-app-boilerplate).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

SHELL := /bin/bash
.DEFAULT_GOAL := app

-include .env

# v2: Vulkan through the linked RADV driver (APP_VULKAN=0 builds without it).
APP_VULKAN ?= 1
ifeq ($(APP_VULKAN),1)
APP_DEFINITIONS ?= PSXS5_HAVE_CURL=1 PSXS5_VULKAN=1
PSXS5_VULKAN_INCLUDE := .deps/native/radv-release/include
else
APP_DEFINITIONS ?= PSXS5_HAVE_CURL=1
endif
# PSXS5: the PCSX-ReARMed core is linked as a static libretro archive.
PSXS5_CORE := build/core-ps5/libpcsx_rearmed.a
# RetroAchievements: rcheevos (MIT), built by tools/build-rcheevos.sh.
PSXS5_RCHEEVOS := build/rcheevos-ps5/librcheevos.a
APP_INCLUDE_PATHS ?= third_party/pcsx_rearmed/deps/libretro-common/include third_party/stb examples/update-check third_party/rcheevos/include $(PSXS5_VULKAN_INCLUDE)
ifeq ($(APP_VULKAN),1)
# v2: Beetle PSX HW (Vulkan renderer), built by tools/build-beetle.sh
PSXS5_BEETLE := build/beetle-ps5/libbeetle_psx.a
# SwanStation (Vulkan renderer), built by tools/build-swanstation.sh
PSXS5_SWANSTATION := build/swanstation-ps5/libswanstation.a
endif
APP_STATIC_ARCHIVES ?= $(PSXS5_CORE) $(PSXS5_RCHEEVOS) $(PSXS5_BEETLE) $(PSXS5_SWANSTATION)
APP_RUNTIME_MODULES ?=
# fcntl: console_curl. The allocator family: ps5_heap.c (the C heap is too small).
APP_WRAP_SYMBOLS ?= fcntl malloc free calloc realloc reallocf memalign posix_memalign aligned_alloc malloc_usable_size
APP_SOURCE_DIR ?=
APP_PARAM ?=
APP_SCE_SYS ?=
APP_ASSETS ?= assets
APP_ROOT_FILES ?=
# Lapy elevation: needed to list /data/PSXS5 under ShadowMountPlus.
APP_LAPY_HELPER ?= 1
PACBREW_PACKAGES ?= sdl2 libcurl
PACBREW_INCLUDE_PATHS ?=
PACBREW_STATIC_ARCHIVES ?=
PS5_HOST ?=
FTP_PORT ?= 2121
DEPLOY_FORMAT ?= folder
PS5_FTP_USER ?= anonymous
PS5_FTP_PASSWORD ?= codex
DEPLOY_DRY_RUN ?= 0
TITLE_ID ?=
APP_NAME ?=
APP_CATEGORY ?= game
CONTENT_SUFFIX ?=
HOST_CC ?= clang
HOST_CXX ?= clang++
HOST_CC ?= clang
HOST_TEST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
	-ffunction-sections -fdata-sections
HOST_TEST_CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
	-ffunction-sections -fdata-sections
HOST_TEST_LDFLAGS ?= -Wl,--gc-sections
GTEST_ARGS ?=
BUILD_JOBS ?= $(shell nproc 2>/dev/null || echo 2)
USE_CCACHE ?= 1
export BUILD_JOBS USE_CCACHE
export HOST_CC HOST_CXX HOST_TEST_CFLAGS HOST_TEST_CXXFLAGS HOST_TEST_LDFLAGS
export APP_VULKAN APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_RUNTIME_MODULES APP_WRAP_SYMBOLS
export APP_SOURCE_DIR APP_PARAM APP_SCE_SYS APP_ASSETS APP_ROOT_FILES
export APP_LAPY_HELPER
export PACBREW_PACKAGES PACBREW_INCLUDE_PATHS PACBREW_STATIC_ARCHIVES
export PS5_HOST FTP_PORT DEPLOY_FORMAT PS5_FTP_USER PS5_FTP_PASSWORD DEPLOY_DRY_RUN
export TITLE_ID APP_NAME APP_CATEGORY CONTENT_SUFFIX

RUNTIME := runtime/libc.prx
RUNTIME_INPUTS := tools/rebuild-libc.sh tools/build-host-tools.sh tools/ninja-build.sh \
	$(wildcard tooling/native/*.cpp tooling/native/*.hpp) \
	$(wildcard tooling/native/runtime/*.txt)
HOST_UNIT_TEST := build/tests/demo_renderer_tests

.PHONY: all app build init doctor test test-deps test-unit test-integration libc deps pacbrew pacbrew-list assets-check format format-check tidy lint check ffpkg ffpfsc packages sandbox-elevation-ffpfsc update-check-example test-update-check self-update-helper self-update-example test-self-update deploy undeploy clean distclean help

all: app
build: app

init:
	@printf '%s\n' '==> [init] Configuring the application identity in sce_sys/param.json'
	@bash tools/init-project.sh sce_sys/param.json

doctor:
	@printf '%s\n' '==> [doctor] Checking the Linux/WSL host without changing it'
	@bash tools/doctor.sh

test: test-unit test-integration test-elevation test-update-check test-self-update

.PHONY: test-elevation
test-elevation:
	@bash tools/setup-native-dependencies.sh >/dev/null
	@mkdir -p build/tests
	@$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -idirafter .deps/native/ps5-payload-sdk/target/include \
		tests/test_elevation.cpp $(HOST_TEST_LDFLAGS) -o build/tests/test_elevation
	@build/tests/test_elevation
	@printf '%s\n' 'Resident/one-shot Lapy client, exchange, and proof checks passed.'

test-update-check:
	@mkdir -p build/tests
	@$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		tests/test_update_check.cpp $(HOST_TEST_LDFLAGS) -o build/tests/test_update_check
	@build/tests/test_update_check
	@printf '%s\n' 'Update-check version, parsing and decision checks passed.'
test-self-update:
	@mkdir -p build/tests/self-update
	@for name in miniz miniz_tinfl miniz_tdef miniz_zip; do \
		$(HOST_CC) -std=c11 -O2 -w -g -fsanitize=address,undefined \
			-c third_party/miniz/$$name.c -o build/tests/self-update/$$name.o || exit 1; \
	done
	@$(HOST_CXX) $(HOST_TEST_CXXFLAGS) -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Ithird_party -Iexamples/self-update -Iexamples/update-check \
		tests/test_self_update.cpp examples/self-update-helper/updater.cpp \
		examples/self-update-helper/archive.cpp examples/self-update-helper/files.cpp \
		build/tests/self-update/*.o -pthread $(HOST_TEST_LDFLAGS) -o build/tests/test_self_update
	@build/tests/test_self_update
	@printf '%s\n' 'Self-update check, download, staging, apply and refusal checks passed.'

test-deps:
	@printf '%s\n' '==> [test-deps] Fetching the pinned host-only GoogleTest source'
	@bash tools/setup-test-dependencies.sh >/dev/null

test-unit:
	@bash tools/build-tests.sh
	@printf '%s\n' '==> [test-unit] Running host-native GoogleTest application tests'
	@$(HOST_UNIT_TEST) $(GTEST_ARGS)

test-integration:
	@printf '%s\n' '==> [test-integration] Running host tooling integration tests'
	@python3 -m unittest discover -s tests -p 'test_*.py' -v

deps: test-deps
	@printf '%s\n' '==> [deps] Fetching declared native dependencies'
	@bash tools/setup-native-dependencies.sh
	@bash tools/setup-pacbrew-dependencies.sh --environment

pacbrew:
	@printf '%s\n' '==> [pacbrew] Fetching the pinned prebuilt ports sysroot'
	@bash tools/setup-pacbrew-dependencies.sh --all

pacbrew-list:
	@printf '%s\n' '==> [pacbrew] Listing available pkg-config modules'
	@bash tools/setup-pacbrew-dependencies.sh --list

assets-check:
	@printf '%s\n' '==> [assets] Validating icon, backgrounds, and selection audio'
	@bash tools/validate-assets.sh

libc:
	@printf '%s\n' '==> [libc] Rebuilding and verifying the clean-room runtime'
	@bash tools/rebuild-libc.sh

$(RUNTIME): $(RUNTIME_INPUTS)
	@printf '%s\n' '==> [libc] Generating the missing or outdated runtime'
	@bash tools/rebuild-libc.sh

app: $(RUNTIME) core radv
	@printf '%s\n' '==> [app] Compiling, linking, signing, and assembling the app folder'
	@bash tools/build.sh Folder

ffpkg: $(RUNTIME) core
	@printf '%s\n' '==> [ffpkg] Building the app folder and UFS2 image'
	@bash tools/build.sh Ffpkg

ffpfsc: $(RUNTIME) core
	@printf '%s\n' '==> [ffpfsc] Building the app folder and compressed image'
	@bash tools/build.sh Ffpfsc

packages: $(RUNTIME)
	@printf '%s\n' '==> [packages] Building the app folder and both package formats'
	@bash tools/build.sh All

sandbox-elevation-ffpfsc: $(RUNTIME)
	@printf '%s\n' '==> [sandbox-elevation] Building the embedded upstream-Lapy proof image'
	@APP_SOURCE_DIR=examples/sandbox-elevation/src \
		APP_PARAM=examples/sandbox-elevation/sce_sys/param.json \
		APP_SCE_SYS=sce_sys APP_ASSETS= APP_LAPY_HELPER=1 \
		bash tools/build.sh Ffpfsc

update-check-example: $(RUNTIME)
	@printf '%s\n' '==> [update-check] Building the catalog update-check example title'
	@APP_SOURCE_DIR=examples/update-check \
		APP_PARAM=examples/update-check/sce_sys/param.json \
		APP_SCE_SYS=sce_sys APP_ASSETS=examples/update-check/assets \
		PACBREW_PACKAGES="libcurl $(PACBREW_PACKAGES)" \
		APP_WRAP_SYMBOLS="fcntl $(APP_WRAP_SYMBOLS)" \
		bash tools/build.sh Folder

SELF_UPDATE_HELPER := build/self-update/self-updater.elf

self-update-helper:
	@printf '%s\n' '==> [self-update] Building the helper for the payload loader'
	@bash tools/setup-native-dependencies.sh >/dev/null
	@$(MAKE) --no-print-directory -s -C examples/self-update-helper \
		PS5_PAYLOAD_SDK="$(CURDIR)/.deps/native/ps5-payload-sdk" \
		OUTPUT="$(CURDIR)/$(SELF_UPDATE_HELPER)"
	@python3 tools/validate-loader-elf.py "$(SELF_UPDATE_HELPER)"

self-update-example: $(RUNTIME) self-update-helper
	@printf '%s\n' '==> [self-update] Building the self-update example title'
	@APP_SOURCE_DIR=examples/self-update \
		APP_PARAM=examples/self-update/sce_sys/param.json \
		APP_SCE_SYS=sce_sys APP_ASSETS=examples/self-update/assets \
		APP_INCLUDE_PATHS="examples/update-check $(APP_INCLUDE_PATHS)" \
		PACBREW_PACKAGES="libcurl $(PACBREW_PACKAGES)" \
		APP_WRAP_SYMBOLS="fcntl $(APP_WRAP_SYMBOLS)" \
		APP_ROOT_FILES="$(SELF_UPDATE_HELPER) $(APP_ROOT_FILES)" \
		bash tools/build.sh Folder

deploy:
	@printf '%s\n' '==> [deploy] Building and publishing the selected app output over FTP'
	@bash tools/deploy.sh

undeploy:
	@printf '%s\n' '==> [undeploy] Removing staged development files for this title over FTP'
	@bash tools/deploy.sh undeploy

format:
	@printf '%s\n' '==> [format] Formatting C and C++ sources'
	@bash tools/run_clang_format.sh

format-check:
	@printf '%s\n' '==> [format] Checking C and C++ formatting'
	@bash tools/run_clang_format.sh --check

tidy:
	@printf '%s\n' '==> [tidy] Running Clang static analysis'
	@bash tools/run_clang_tidy.sh

lint:
	@printf '%s\n' '==> [lint] Running source, metadata, and shell checks'
	@bash tools/lint.sh

check: lint test app

clean:
	@printf '%s\n' '==> [clean] Removing generated build outputs'
	@rm -rf -- build dist
	@rm -f -- $(RUNTIME)

distclean: clean
	@printf '%s\n' '==> [distclean] Removing downloaded dependency caches'
	@rm -rf -- .deps

.PHONY: core core-desktop desktop run-desktop
# The core is rebuilt by its own Makefile; this only forwards to it.
.PHONY: radv
# v2: the RADV driver build and the SDK platform layer (tools/fetch-radv.sh).
radv:
ifeq ($(APP_VULKAN),1)
	@bash tools/fetch-radv.sh
endif

core:
	@bash tools/build-core.sh ps5
	@bash tools/build-rcheevos.sh ps5
ifeq ($(APP_VULKAN),1)
	@bash tools/build-beetle.sh
	@bash tools/build-swanstation.sh
endif

core-desktop:
	@bash tools/build-core.sh desktop
	@bash tools/build-rcheevos.sh desktop

# Linux/WSL desktop build of the same frontend, for testing games on a PC.
desktop: core-desktop
	@$(MAKE) --no-print-directory -f Makefile.desktop

run-desktop: desktop
	@./build/desktop/psxs5

help:
	@printf '%s\n' \
	  'PSXS5:' \
	  'make                 Build the PS5 app folder dist/PPSA97510 (core + frontend)' \
	  'make deploy PS5_HOST=<ip>  Build and upload to /data/homebrew over FTP' \
	  'make desktop         Build the desktop test build (needs libsdl2-dev)' \
	  'make run-desktop     Run it; set PSXS5_ROOT to a folder with games/' \
	  '' \
	  'Boilerplate:' \
	  'make                 Generate libc.prx and build the app folder' \
	  'make init TITLE_ID=PPSA12345 APP_NAME="My App"  Configure app identity' \
	  'make doctor          Check required and optional Linux/WSL tools' \
	  'make test            Run all host unit and integration tests' \
	  'make test-deps       Fetch verified host-only GoogleTest source' \
	  'make test-unit       Run host-native GoogleTest application tests' \
	  'make test-integration  Run host tooling integration tests' \
	  'make deps            Fetch native dependencies into .deps/' \
	  'make pacbrew         Fetch the pinned PacBrew ports sysroot' \
	  'make pacbrew-list    List PacBrew pkg-config module names' \
	  'make assets-check    Validate the current presentation assets' \
	  'make libc            Force a deterministic runtime/libc.prx rebuild' \
	  'make format          Apply the shared Clang formatting policy' \
	  'make format-check    Check formatting without modifying files' \
	  'make tidy            Run the shared Clang static-analysis policy' \
	  'make lint            Run format, tidy, metadata, and shell checks' \
	  'make check           Run lint and build the skeleton app' \
	  'make ffpkg           Build the folder and UFS2 .ffpkg image' \
	  'make ffpfsc          Build the folder and compressed .ffpfsc image' \
	  'make packages        Build folder, .ffpkg, and .ffpfsc outputs' \
	  'make sandbox-elevation-ffpfsc  Build the official-Lapy client proof image' \
	  'make update-check-example  Build the catalog update-check example title' \
	  'make test-update-check     Run the update-check host tests' \
	  'make self-update-example   Build the self-update example title and its helper' \
	  'make test-self-update      Run the self-update host tests' \
	  'make deploy PS5_HOST=<address>  Build and FTP-deploy the app folder' \
	  'make undeploy PS5_HOST=<address>  Remove this title from /data/homebrew' \
	  'Build variables:     APP_DEFINITIONS, APP_INCLUDE_PATHS, APP_STATIC_ARCHIVES, APP_RUNTIME_MODULES, APP_WRAP_SYMBOLS, APP_LAPY_HELPER' \
	  'PacBrew variables:   PACBREW_PACKAGES, PACBREW_INCLUDE_PATHS, PACBREW_STATIC_ARCHIVES' \
	  'Deploy variables:    FTP_PORT=2121, DEPLOY_FORMAT=folder|ffpfsc|ffpkg, DEPLOY_DRY_RUN=0|1' \
	  'Local defaults:      Copy .env.example to the ignored .env file' \
	  'Build speed:         BUILD_JOBS defaults to all CPUs; USE_CCACHE=0 disables ccache' \
	  'make clean           Remove build/, dist/, and generated libc.prx' \
	  'make distclean       Also remove the ignored .deps/ cache'
