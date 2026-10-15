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
check_cmd patchelf patchelf
probe_report

BUILD_DIR=build
rm -rf "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

ARTIFACTS="libframesink.so libframesinkplugin.so qmldir framesink.qmltypes"
for f in $ARTIFACTS; do
	[ -f "$BUILD_DIR/$f" ] || die "build artifact missing: $f"
done

QML_DEST="${QML_INSTALL_DIR:-$HOME/.local/lib/qt6/qml/org/forge/framesink}"
PKG_DEST="$HOME/.local/share/plasma/wallpapers/org.forge.framesink"
UNIT_DIR="$HOME/.config/systemd/user/plasma-plasmashell.service.d"
UNIT_DROPIN="$UNIT_DIR/10-framesink.conf"

STAGE="$OUTPUT/integration/kde"
mkdir -p "$STAGE/qml/org/forge/framesink" "$STAGE/plasma/wallpapers"
for f in $ARTIFACTS; do cp -f "$BUILD_DIR/$f" "$STAGE/qml/org/forge/framesink/"; done
rm -rf "$STAGE/plasma/wallpapers/org.forge.framesink"
cp -r wallpaper-package "$STAGE/plasma/wallpapers/org.forge.framesink"

mkdir -p "$(dirname "$PKG_DEST")"
rm -rf "$PKG_DEST"
cp -r wallpaper-package "$PKG_DEST"

rm -rf "$QML_DEST"
mkdir -p "$QML_DEST"
for f in $ARTIFACTS; do cp -f "$BUILD_DIR/$f" "$QML_DEST/"; done
patchelf --set-rpath '$ORIGIN' "$QML_DEST/libframesinkplugin.so"

if [ ! -f "$UNIT_DROPIN" ]; then
	mkdir -p "$UNIT_DIR"
	cat > "$UNIT_DROPIN" <<EOF
[Service]
Environment=QML_IMPORT_PATH=$HOME/.local/lib/qt6/qml
Environment=QML2_IMPORT_PATH=$HOME/.local/lib/qt6/qml
EOF
	systemctl --user daemon-reload
	log "created plasmashell drop-in: $UNIT_DROPIN"
fi

log "qml module installed: $QML_DEST"
log "wallpaper package installed: $PKG_DEST"
log "module updates need a shell restart: systemctl --user restart plasma-plasmashell"
