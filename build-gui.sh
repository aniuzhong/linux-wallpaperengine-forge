#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log gui
GUI_SRC="$THIRD_PARTY/linux-wallpaperengine-gui"

log "Probing system dependencies..."
probe_reset
check_cmd git git
check_cmd curl curl
check_cmd unzip unzip
check_cmd cc build-essential
check_cmd pkg-config pkg-config
check_lib glib-2.0 libglib2.0-dev
check_lib gtk+-3.0 libgtk-3-dev
check_lib ayatana-appindicator3-0.1 libayatana-appindicator3-dev
probe_report

ensure_repo "https://github.com/AzPepoze/linux-wallpaperengine-gui" "$GUI_SRC" "$GUI_REF"
apply_patches "$GUI_SRC" "${GUI_PATCH_DIRS[@]}"

for f in "$OVERLAY_DIR"/*.ts; do
	name=$(basename "$f")
	cp -f "$f" "$GUI_SRC/src/frontend/main/services/$name"
	log "Overlay: src/frontend/main/services/$name <- src/gui-workshop/"
done
while read -r hash path; do
	case "$hash" in \#*|"") continue ;; esac
	[ -n "$path" ] || continue
	cur=$(git -C "$GUI_SRC" rev-parse "HEAD:$path" 2>/dev/null || echo "")
	if [ -n "$cur" ] && [ "$cur" != "$hash" ]; then
		warn "Upstream file evolved since overlay was authored: $path"
		warn "  overlay base: $hash / pinned HEAD: $cur"
		warn "  review and merge upstream changes into src/gui-workshop/"
	fi
done < "$OVERLAY_DIR/UPSTREAM_BASE"

GO_VERSION="${GO_VERSION:-$(sed -n 's/^go \([0-9][0-9.]*\)$/\1/p' "$GUI_SRC/src/backend/go.mod" | head -n1)}"
[ -n "$GO_VERSION" ] || die "Cannot parse Go version from go.mod; set GO_VERSION env explicitly"
ensure_go "$GO_VERSION"
ensure_bun "$BUN_VERSION"
ensure_rust

SWJS_SRC="$THIRD_PARTY/steamworks.js"
if [ ! -d "$SWJS_SRC/.git" ]; then
	log "Cloning steamworks.js ..."
	git clone https://github.com/ceifa/steamworks.js "$SWJS_SRC"
fi
log "Building steamworks.js native module (cargo build --release) ..."
(cd "$SWJS_SRC" && cargo build --release)
SWJS_DIST="$SWJS_SRC/dist/linux64"
mkdir -p "$SWJS_DIST"
cp "$SWJS_SRC/target/release/libsteamworksjs.so" "$SWJS_DIST/steamworksjs.linux-x64-gnu.node"
cp "$SWJS_SRC/sdk/redistributable_bin/linux64/libsteam_api.so" "$SWJS_DIST/"

export npm_config_registry="$NPM_REGISTRY"
export ELECTRON_MIRROR
export ELECTRON_BUILDER_BINARIES_MIRROR
if [ ! -f "$GUI_SRC/bunfig.toml" ]; then
	cat > "$GUI_SRC/bunfig.toml" <<EOF
[install]
registry = "$NPM_REGISTRY"
EOF
fi
log "Installing frontend dependencies (bun install) ..."
(cd "$GUI_SRC" && bun install)

NODE_SWJS="$GUI_SRC/node_modules/steamworks.js/dist/linux64"
[ -d "$NODE_SWJS" ] || die "node_modules/steamworks.js not found; bun install may have failed"
log "Overriding steamworks native module with local build ..."
cp "$SWJS_DIST/steamworksjs.linux-x64-gnu.node" "$NODE_SWJS/"
cp "$SWJS_DIST/libsteam_api.so" "$NODE_SWJS/"

log "Wiring $GO_REPLACE_PKG module into GUI go.mod ..."
(cd "$GUI_SRC/src/backend" && go mod edit \
	-require="$GO_REPLACE_PKG@v0.0.0" \
	-replace="$GO_REPLACE_PKG=$GO_REPLACE_DIR")

log "Type-checking frontend (tsc --noEmit) ..."
(cd "$GUI_SRC" && ./node_modules/.bin/tsc --noEmit)
log "Building GUI (bun run build) ..."
(cd "$GUI_SRC" && bun run build)

[ -d "$GUI_SRC/dist/linux-unpacked" ] || die "Build finished but dist/linux-unpacked not found"
rm -rf "$OUTPUT/linux-unpacked"
cp -a "$GUI_SRC/dist/linux-unpacked" "$OUTPUT/linux-unpacked"

log "=========================================="
log "GUI build complete: $OUTPUT/linux-unpacked"
log "Run: $OUTPUT/linux-unpacked/linux-wallpaperengine-gui"
log "=========================================="
