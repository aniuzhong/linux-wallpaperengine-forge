#!/usr/bin/env bash
#
# build-gui.sh — build linux-wallpaperengine-gui (Electron frontend + Go backend)
#
# Source:  upstream clone pinned at GUI_REF (third_party/), no patches, no overlay
# Output:  out/linux-unpacked/ (electron-builder --dir, unpacked directory form)
# Log:     out/build-gui.log
#
# Notes:   toolchain entirely from apt (nodejs/npm/golang-go); steamworks.js
#          uses the npm prebuilt binary (glibc 2.43 backward compatible, no
#          local cargo build). Upstream's bun-based build script is replayed
#          in the same order, three steps. npm deps and the electron binary
#          via npmmirror, go modules via goproxy.cn. Independent of
#          build-engine.sh, can run on its own.
#
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log gui
GUI_REF="${GUI_REF:-8855fad673932991dde0241530941a520b0e34a2}"
GUI_SRC="$THIRD_PARTY/linux-wallpaperengine-gui"

# ---- 1. System dependency probe ----
log "Probing system dependencies..."
probe_reset
check_cmd git git
check_cmd node nodejs
check_cmd npm npm
check_cmd go golang-go
check_cmd pkg-config pkg-config
# GTK and libayatana are link-time deps of the Go backend (CGO tray/notifications)
for mod in glib-2.0 gtk+-3.0 ayatana-appindicator3-0.1; do
	pkg-config --exists "$mod" || { warn "Missing dev library: $mod"; PROBE_MISSING+=("$mod"); }
done
probe_report

# ---- 2. Source ----
ensure_repo "https://github.com/AzPepoze/linux-wallpaperengine-gui" "$GUI_SRC" "$GUI_REF"

# ---- 3. Dependency install (npmmirror + electron mirrors) ----
NPM_REGISTRY="https://registry.npmmirror.com"
export npm_config_registry="$NPM_REGISTRY"
export ELECTRON_MIRROR="https://npmmirror.com/mirrors/electron/"
export ELECTRON_BUILDER_BINARIES_MIRROR="https://npmmirror.com/mirrors/electron-builder-binaries/"
if [ ! -f "$GUI_SRC/.npmrc" ]; then
	echo "registry=$NPM_REGISTRY" > "$GUI_SRC/.npmrc"
fi
log "Installing frontend dependencies (npm install) ..."
# --legacy-peer-deps: upstream declares deps under bun's loose resolution
# (vite 7 vs a plugin peer-requiring vite 8); npm's strict check would refuse
(cd "$GUI_SRC" && npm install --no-audit --no-fund --legacy-peer-deps)

# ---- 4. Go backend environment ----
# go.mod wants >= 1.25.5; system golang-go (1.26) satisfies it — GOTOOLCHAIN=local
# forbids a network toolchain download
export GOPROXY="https://goproxy.cn,direct"
export GOTOOLCHAIN=local

# ---- 5. Build (same order as upstream's bun run build: backend -> vite -> electron-builder) ----
log "Building Go backend ..."
(cd "$GUI_SRC" && npm run build:backend)
log "Building frontend (vite) ..."
(cd "$GUI_SRC" && ./node_modules/.bin/vite build)
log "Packaging (electron-builder --dir) ..."
(cd "$GUI_SRC" && ./node_modules/.bin/electron-builder --linux --dir)

# ---- 6. Collect artifacts ----
[ -d "$GUI_SRC/dist/linux-unpacked" ] || die "Build finished but dist/linux-unpacked not found"
rm -rf "$OUTPUT/linux-unpacked"
cp -a "$GUI_SRC/dist/linux-unpacked" "$OUTPUT/linux-unpacked"

log "=========================================="
log "GUI build complete: $OUTPUT/linux-unpacked"
log "Run: $OUTPUT/linux-unpacked/linux-wallpaperengine-gui"
log "=========================================="
