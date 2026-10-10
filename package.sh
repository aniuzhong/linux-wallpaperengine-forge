#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log package

GUI_PAYLOAD="$OUTPUT/linux-unpacked"
ENG_PAYLOAD="$OUTPUT/engine"
GUI_SRC="$THIRD_PARTY/linux-wallpaperengine-gui"

[ -d "$GUI_PAYLOAD" ] || die "out/linux-unpacked does not exist, please run build-gui.sh first"
[ -x "$ENG_PAYLOAD/linux-wallpaperengine" ] || die "out/engine is incomplete, please run build-engine.sh first"
[ -d "$TARGET_DIR/packaging" ] || die "missing $TARGET_DIR/packaging"

GUI_VER=$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$GUI_SRC/package.json" | head -n1)
[ -n "$GUI_VER" ] || die "Cannot read GUI version number"
ENG_REF9="${ENGINE_REF:0:9}"
TARGET_TAG="$(printf '%s' "${TARGET_DIR#"$FORGE_DIR"/}" | tr '/' '-')-$(forge_arch)"
SUITE_NAME="lwe-forge-${GUI_VER}+${ENG_REF9}-${TARGET_TAG}"
STAGE="$OUTPUT/$SUITE_NAME"

log "Assembling $SUITE_NAME ..."
rm -rf "$STAGE"
mkdir -p "$STAGE/bin"

cp -a "$GUI_PAYLOAD" "$STAGE/gui"
cp -a "$ENG_PAYLOAD/." "$STAGE/engine/"
cp -a "$TARGET_DIR/packaging/." "$STAGE/"
if [ ! -e "$STAGE/bin/linux-wallpaperengine" ]; then
	ln -s ../engine/linux-wallpaperengine "$STAGE/bin/linux-wallpaperengine"
fi
if [ -n "${INTEGRATION_STAGE:-}" ] && [ -d "$OUTPUT/integration" ]; then
	cp -a "$OUTPUT/integration/." "$STAGE/"
fi

{
	echo "suite  : lwe-forge"
	echo "gui    : $GUI_VER (ref $GUI_REF)"
	echo "engine : ref $ENGINE_REF"
	for p in "$STAGE"/*; do
		b=$(basename "$p")
		case "$b" in
			gui|engine|bin|run-gui.sh|VERSION|README.md) continue ;;
		esac
		if [ -f "$p" ]; then
			echo "integration: $b"
		elif [ -d "$p" ]; then
			for f in "$p"/*; do
				[ -e "$f" ] || continue
				echo "integration: $(basename "$f")"
			done
		fi
	done
	echo "patches:"
	[ -f "${GUI_PATCH_LIST:-}" ] && sed "s|^|  gui/|" "$GUI_PATCH_LIST" | sort
	[ -f "${ENGINE_PATCH_LIST:-}" ] && sed "s|^|  engine/|" "$ENGINE_PATCH_LIST" | sort
	echo "built  : $(date '+%F %T %Z')"
} > "$STAGE/VERSION"

log "Compressing $SUITE_NAME.tar.gz (payload ~1.5 GB, takes a few minutes) ..."
tar -C "$OUTPUT" -czf "$OUTPUT/$SUITE_NAME.tar.gz" "$SUITE_NAME"
rm -rf "$STAGE"

log "=========================================="
log "Suite ready: $OUTPUT/$SUITE_NAME.tar.gz ($(du -h "$OUTPUT/$SUITE_NAME.tar.gz" | cut -f1))"
log "Usage: tar xzf $SUITE_NAME.tar.gz && cd $SUITE_NAME && ./run-gui.sh"
log "=========================================="
