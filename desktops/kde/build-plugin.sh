#!/usr/bin/env bash
set -euo pipefail
SELF="$(readlink -f "${BASH_SOURCE[0]}")"
cd "$(dirname "$SELF")"
FORGE_DIR="$(dirname "$(dirname "$(dirname "$SELF")")")"
source "$FORGE_DIR/lib.sh"

init_log plugin

probe_reset
check_cmd cmake cmake
check_cmd g++ gcc
check_cmd qmake6 qt6-base
probe_report

BUILD_DIR=build
rm -rf "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

ARTIFACTS="libframesink.so libframesinkplugin.so qmldir framesink.qmltypes"
for f in $ARTIFACTS; do
	[ -f "$BUILD_DIR/$f" ] || die "build artifact missing: $f"
done

QML_DEST="${QML_INSTALL_DIR:-$(qmake6 -query QT_INSTALL_QML)/org/forge/framesink}"
PKG_DEST="$HOME/.local/share/plasma/wallpapers/org.forge.framesink"

STAGE="$OUTPUT/integration/kde"
mkdir -p "$STAGE/qml/org/forge/framesink" "$STAGE/plasma/wallpapers"
for f in $ARTIFACTS; do cp -f "$BUILD_DIR/$f" "$STAGE/qml/org/forge/framesink/"; done
rm -rf "$STAGE/plasma/wallpapers/org.forge.framesink"
cp -r wallpaper-package "$STAGE/plasma/wallpapers/org.forge.framesink"

mkdir -p "$(dirname "$PKG_DEST")"
rm -rf "$PKG_DEST"
cp -r wallpaper-package "$PKG_DEST"

mkdir -p "$QML_DEST" 2>/dev/null || sudo mkdir -p "$QML_DEST"
for f in $ARTIFACTS; do cp -f "$BUILD_DIR/$f" "$QML_DEST/" 2>/dev/null || sudo cp -f "$BUILD_DIR/$f" "$QML_DEST/"; done

log "qml module installed: $QML_DEST"
log "wallpaper package installed: $PKG_DEST"
log "module updates need a shell restart: systemctl --user restart plasma-plasmashell"
