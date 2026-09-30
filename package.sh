#!/usr/bin/env bash
#
# package.sh — assemble the release suite (ubuntu/26.04)
#
# Prereq: artifacts from all three build entries present:
#           out/engine/                       engine (build-engine.sh, pruned)
#           out/linux-unpacked/               GUI (build-gui.sh)
#           out/integration/gnome-extension/  extension (build-extension.sh)
# Output: out/lwe-forge-<gui-ver>+<engine-hash9>-ubuntu26-x64.tar.gz
# Log:    out/build-package.log
#
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log package

GUI_PAYLOAD="$OUTPUT/linux-unpacked"
ENG_PAYLOAD="$OUTPUT/engine"
GUI_SRC="$THIRD_PARTY/linux-wallpaperengine-gui"
GUI_REF="${GUI_REF:-8855fad673932991dde0241530941a520b0e34a2}"

# ---- 1. Preconditions ----
[ -d "$GUI_PAYLOAD" ] || die "out/linux-unpacked does not exist, please run build-gui.sh first"
[ -x "$ENG_PAYLOAD/linux-wallpaperengine" ] || die "out/engine is incomplete, please run build-engine.sh first"
[ -d "$OUTPUT/integration/gnome-extension/wallpaper-sink@lwe-forge" ] || die "out/integration/gnome-extension is incomplete, please run build-extension.sh first"

# ---- 2. Version and naming ----
# GUI version read from upstream package.json; the engine short hash in the
# suite name makes the build baseline traceable at a glance
GUI_VER=$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$GUI_SRC/package.json" | head -n1)
[ -n "$GUI_VER" ] || die "Cannot read GUI version number"
ENG_REF9="${ENGINE_REF:0:9}"
SUITE_NAME="lwe-forge-${GUI_VER}+${ENG_REF9}-ubuntu26-x64"
STAGE="$OUTPUT/$SUITE_NAME"

# ---- 3. Assemble ----
log "Assembling $SUITE_NAME ..."
rm -rf "$STAGE"
mkdir -p "$STAGE/bin"

# GUI payload
cp -a "$GUI_PAYLOAD" "$STAGE/gui"
# Engine payload (flat)
cp -a "$ENG_PAYLOAD/." "$STAGE/engine/"
# In-suite engine entry: wrapper forces the engine onto Xwayland on Wayland
# sessions (see script header)
install -m 755 "$FORGE_DIR/packaging/engine-wrapper.sh" "$STAGE/bin/linux-wallpaperengine"
# GNOME desktop-integration extension (staged by build-extension.sh)
mkdir -p "$STAGE/gnome-extension"
cp -a "$OUTPUT/integration/gnome-extension/." "$STAGE/gnome-extension/"

# Launcher and README
install -m 755 "$FORGE_DIR/packaging/run-gui.sh" "$STAGE/run-gui.sh"
install -m 644 "$FORGE_DIR/packaging/README.md"  "$STAGE/README.md"

# VERSION manifest: component versions and patch list, shipped in the suite
{
	echo "suite  : lwe-forge (ubuntu/26.04)"
	echo "gui    : $GUI_VER (ref $GUI_REF, upstream unpatched)"
	echo "engine : ref $ENGINE_REF"
	echo "patches:"
	for p in "$FORGE_DIR"/patches/engine/*.patch; do echo "  engine/$(basename "$p")"; done
	echo "extension: wallpaper-sink (GNOME 50 Wayland icon-layer arbitration)"
	echo "built  : $(date '+%F %T %Z')"
} > "$STAGE/VERSION"

# ---- 4. Compress ----
# Payload ~1.5 GB (mostly CEF); takes a few minutes
log "Compressing $SUITE_NAME.tar.gz (payload ~1.5 GB, takes a few minutes) ..."
tar -C "$OUTPUT" -czf "$OUTPUT/$SUITE_NAME.tar.gz" "$SUITE_NAME"
rm -rf "$STAGE"

log "=========================================="
log "Suite ready: $OUTPUT/$SUITE_NAME.tar.gz ($(du -h "$OUTPUT/$SUITE_NAME.tar.gz" | cut -f1))"
log "Usage: tar xzf $SUITE_NAME.tar.gz && cd $SUITE_NAME && ./run-gui.sh"
log "=========================================="
