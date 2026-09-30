#!/usr/bin/env bash
#
# build-engine.sh — build the linux-wallpaperengine engine (Ubuntu 26.04 / amd64)
#
# Source:  upstream clone pinned at ENGINE_REF (third_party/), with
#          patches/engine/ applied on top (0002 desktop-layer window, the only
#          default patch)
# Output:  out/engine/ (flat payload: install prefix == payload root, engine
#          binary next to the CEF runtime, shipped with the suite)
# Log:     out/build-engine.log
#
# Notes:   the first run downloads ~1GB from the official CEF build source
#          (spotifycdn, no CN mirror); the download persists in toolchains/cef
#          and is pre-seeded into rebuilt build trees. System toolchain
#          (gcc 15 / cmake 4.x) used as-is.
#
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log engine
ENGINE_SRC="$THIRD_PARTY/linux-wallpaperengine"

# ---- 1. System dependency probe (minimal set) ----
# Only gate-keeping commands; missing dev libs surface at the cmake
# configure/build stage — install as reported and rerun
log "Probing system dependencies..."
probe_reset
check_cmd git git
check_cmd cc build-essential
check_cmd 'g++' g++
check_cmd pkg-config pkg-config
check_cmd cmake cmake
probe_report

# ---- 2. Source + submodules + patches ----
ensure_repo "https://github.com/Almamu/linux-wallpaperengine.git" "$ENGINE_SRC" "$ENGINE_REF"
log "Syncing submodules (slow on first run) ..."
git -C "$ENGINE_SRC" submodule update --init --recursive
apply_patches "$ENGINE_SRC" "$FORGE_DIR/patches/engine"

# ---- 3. Configure + build + install ----
BUILD_DIR="$ENGINE_SRC/build"
PAYLOAD="$OUTPUT/engine"

# Hermetic build: launcher environments (AppImage hosts, sandboxed terminals)
# inject an LD_LIBRARY_PATH pointing at bundled libs; cmake bakes those absolute
# paths into the link graph and the tree breaks when a launcher upgrade changes
# the mount hash (observed: libEGL.so under /tmp/.mount_* then "No rule to make
# target"). Always resolve against system paths.
unset LD_LIBRARY_PATH

# CMake build trees bind absolute paths and die on an environment move;
# rescue the ~1GB CEF download into toolchains/cef before recreating the tree.
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
# Pre-seed the CEF cache: DownloadCEF skips its download when the unpacked dir exists
if [ -n "$(ls -A "$CEF_CACHE" 2>/dev/null)" ]; then
	mkdir -p "$BUILD_DIR/cef"
	cp -al "$CEF_CACHE/." "$BUILD_DIR/cef/" 2>/dev/null || cp -a "$BUILD_DIR/cef/." "$BUILD_DIR/cef/"
fi

log "Configuring CMake ..."
# CMAKE_POLICY_VERSION_MINIMUM=3.5: cmake 4.x rejects old submodules declaring
# minimum<3.5; harmless for compliant projects, saves a full rerun on a hit
cmake -S "$ENGINE_SRC" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PAYLOAD" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DBUILD_TESTING=OFF
log "Building ($(nproc) jobs) ..."
cmake --build "$BUILD_DIR" -j"$(nproc)"
log "Installing into $PAYLOAD ..."
cmake --install "$BUILD_DIR"

# install(TARGETS) puts the main binary at the prefix root, next to the CEF runtime
[ -x "$PAYLOAD/linux-wallpaperengine" ] || die "Install finished but linux-wallpaperengine not found in payload"

# ---- 4. Payload pruning: drop non-runtime install artifacts ----
# install() also lands submodule dev/test outputs into the prefix. Safety
# verified with readelf: none of the engine's 23 NEEDED entries touch
# glslang/spirv (statically linked); the only submodule shared dep, kissfft,
# loads via RUNPATH $ORIGIN/lib. Pruned:
#   - test/benchmark binaries (api-test and the kissfft bm_*/fastconv*/fft family)
#   - QuickJS CLI tools (qjs/qjsc/run-test262/function_source)
#   - shader CLIs (glslang/glslangValidator/spirv-cross/spirv-remap)
#   - dev files (include/ share/ bin/, plus everything under lib/ except the
#     kissfft runtime — static libs and cmake/pkgconfig exports)
# CEF runtime files (libcef.so/pak/icudtl/locales) are never touched.
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
