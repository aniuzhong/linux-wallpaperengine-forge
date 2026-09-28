#!/usr/bin/env bash
#
# build-gui.sh — 编译 linux-wallpaperengine-gui (Electron 前端 + Go 后端)
#
# 源码:   上游克隆钉在 GUI_REF, 构建前套用 patches/gui/ 全系列补丁;
#         forge 自有模块 pkg/peony (桌面透明注入) 由 go mod edit 现场
#         接入, 不进补丁
# 产物:   out/linux-unpacked/ (electron-builder --dir, 目录形态)
# 日志:   out/build-gui.log
#
# 说明:   steamworks.js 的 npm 预编译产物对麒麟不可用, 本脚本用用户态
#         rust 工具链本地重编并覆盖之; 工具链与前端依赖全部走国内镜像,
#         首次构建较慢。与 build-engine.sh / build-shim.sh 无先后依赖,
#         可在独立容器中单独运行。
#
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log gui
GUI_SRC="$SOURCES/linux-wallpaperengine-gui"

# ---- 1. 系统依赖探测 ----
# GTK 与 libayatana 是托盘/通知的链接期依赖
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

# ---- 2. 源码 + 补丁 ----
ensure_repo "https://github.com/AzPepoze/linux-wallpaperengine-gui" "$GUI_SRC" "$GUI_REF"
apply_patches "$GUI_SRC" "$FORGE_DIR/patches/gui"

# ---- 3. 工具链 ----
# Go 版本取自后端 go.mod, 与上游声明保持一致; bun/rust 落用户态 toolchains/
GO_VERSION="${GO_VERSION:-$(sed -n 's/^go \([0-9][0-9.]*\)$/\1/p' "$GUI_SRC/src/backend/go.mod" | head -n1)}"
[ -n "$GO_VERSION" ] || die "Cannot parse Go version from go.mod; set GO_VERSION env explicitly"
ensure_go "$GO_VERSION"
ensure_bun "$BUN_VERSION"
ensure_rust

# ---- 4. steamworks 原生模块 ----
# Workshop 功能依赖 steamworks.js; npm 预编译产物对麒麟不可用, 本地重编
# 后连同 Steam SDK 的 libsteam_api.so 一起覆盖进前端依赖
SWJS_SRC="$SOURCES/steamworks.js"
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

# ---- 5. 前端依赖 ----
# 全部走 npmmirror: bun registry 与 electron/electron-builder 二进制
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

# 用本地重编产物覆盖 npm 安装的同名预编译模块
NODE_SWJS="$GUI_SRC/node_modules/steamworks.js/dist/linux64"
[ -d "$NODE_SWJS" ] || die "node_modules/steamworks.js not found; bun install may have failed"
log "Overriding steamworks native module with local build ..."
cp "$SWJS_DIST/steamworksjs.linux-x64-gnu.node" "$NODE_SWJS/"
cp "$SWJS_DIST/libsteam_api.so" "$NODE_SWJS/"

# ---- 6. pkg/peony 模块接线 ----
# 本地 replace 由构建脚本现场注入, 不进补丁: apply_patches 每轮把 go.mod
# 重置回上游, 因此每次构建前重新注入。目标为 forge 自有模块 pkg/peony
# (纯 stdlib, 本地目录无需 go.sum)。
log "Wiring pkg/peony module into GUI go.mod ..."
(cd "$GUI_SRC/src/backend" && go mod edit \
	-require="lwe-forge/pkg/peony@v0.0.0" \
	-replace="lwe-forge/pkg/peony=$FORGE_DIR/pkg/peony")

# ---- 7. 构建 ----
# 流水线: Go 后端 (CGO) -> vite 前端 -> electron-builder --dir
log "Building GUI (bun run build) ..."
(cd "$GUI_SRC" && bun run build)

# ---- 8. 收取产物 ----
[ -d "$GUI_SRC/dist/linux-unpacked" ] || die "Build finished but dist/linux-unpacked not found"
rm -rf "$OUTPUT/linux-unpacked"
cp -a "$GUI_SRC/dist/linux-unpacked" "$OUTPUT/linux-unpacked"

log "=========================================="
log "GUI build complete: $OUTPUT/linux-unpacked"
log "Run: $OUTPUT/linux-unpacked/linux-wallpaperengine-gui"
log "=========================================="
