#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log engine
ENGINE_SRC="$THIRD_PARTY/linux-wallpaperengine"

log "Probing system dependencies..."
probe_reset
check_cmd git git
check_cmd curl curl
check_cmd cc build-essential
check_cmd "$CC" "$CC"
check_cmd "$CXX" "$CXX"
check_cmd pkg-config pkg-config
check_lib gl libgl-dev
check_lib xrandr libxrandr-dev
check_lib xinerama libxinerama-dev
check_lib xcursor libxcursor-dev
check_lib xi libxi-dev
check_lib xxf86vm libxxf86vm-dev
check_lib xcb-randr libxcb-randr0-dev
check_lib glew libglew-dev
check_header /usr/include/GL/glut.h freeglut3-dev
check_lib sdl2 libsdl2-dev
check_lib liblz4 liblz4-dev
check_lib libavcodec libavcodec-dev
check_lib libavformat libavformat-dev
check_lib libavutil libavutil-dev
check_lib libswscale libswscale-dev
check_lib mpv libmpv-dev
check_lib glfw3 libglfw3-dev
check_lib libpulse libpulse-dev
check_lib fftw3 libfftw3-dev
check_lib freetype2 libfreetype-dev
check_lib dbus-1 libdbus-1-dev
check_lib zlib zlib1g-dev
check_lib libpng libpng-dev
check_lib gmp libgmp-dev
check_header /usr/include/glm/glm.hpp libglm-dev
probe_report

ensure_repo "https://github.com/Almamu/linux-wallpaperengine.git" "$ENGINE_SRC" "$ENGINE_REF"
log "Syncing submodules (slow on first run) ..."
git -C "$ENGINE_SRC" submodule update --init --recursive
apply_patches "$ENGINE_SRC" "${ENGINE_PATCH_DIRS[@]}"

ensure_cmake 3.22

BUILD_DIR="$ENGINE_SRC/build"
PAYLOAD="$OUTPUT/engine"

CEF_CACHE="$TOOLCHAINS/cef"
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$ENGINE_SRC" "$BUILD_DIR/CMakeCache.txt"; then
	warn "Build tree was configured under a different path; recreating it (CEF download cache preserved at $CEF_CACHE)"
	if [ -d "$BUILD_DIR/cef" ]; then
		rm -rf "$CEF_CACHE"
		mkdir -p "$CEF_CACHE"
		cp -al "$BUILD_DIR/cef/." "$CEF_CACHE/" 2>/dev/null || cp -a "$BUILD_DIR/cef/." "$CEF_CACHE/"
	fi
	rm -rf "$BUILD_DIR"
fi
if [ -n "$(ls -A "$CEF_CACHE" 2>/dev/null)" ]; then
	mkdir -p "$BUILD_DIR/cef"
	cp -al "$CEF_CACHE/." "$BUILD_DIR/cef/" 2>/dev/null || cp -a "$CEF_CACHE/." "$BUILD_DIR/cef/"
fi

log "Configuring CMake ..."
cmake -S "$ENGINE_SRC" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_INSTALL_PREFIX="$PAYLOAD" \
	-DBUILD_TESTING=OFF
log "Building ($(nproc) jobs) ..."
cmake --build "$BUILD_DIR" -j"$(nproc)"
log "Installing into $PAYLOAD ..."
cmake --install "$BUILD_DIR"

[ -x "$PAYLOAD/linux-wallpaperengine" ] || die "Install finished but linux-wallpaperengine not found in payload"

log "Pruning non-runtime files from payload ..."
(
	cd "$PAYLOAD"
	rm -rf bin include share
	rm -f api-test \
		bm_fftw-float bm_kiss-float fastconv-float fastconvr-float fastfilt-float \
		ffr-float fft-float psdpng-float st-float testcpp-float tkfc-float tr-float \
		function_source run-test262 qjs qjsc \
		glslang glslangValidator spirv-cross spirv-remap
	find lib -mindepth 1 ! -name 'libkissfft-float.so*' -delete
)

log "=========================================="
log "Engine build complete: $PAYLOAD"
log "Run: $PAYLOAD/linux-wallpaperengine --help"
log "=========================================="
