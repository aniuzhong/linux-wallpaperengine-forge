#!/usr/bin/env bash
#
# build-gui.sh — 编译 linux-wallpaperengine-gui (Electron 前端 + Go 后端)
#
# 源码:   上游克隆钉在 GUI_REF (third_party/), 构建前套用 patches/gui/
#         全系列补丁; forge 自有模块不进补丁: pkg/background (壁纸契约,
#         Go) 由 go mod edit 现场接入; src/gui-workshop (Workshop 隔离,
#         TS) 整体覆盖到源码树对应路径, 上游演进经 UPSTREAM_BASE hash 告警
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
GUI_SRC="$THIRD_PARTY/linux-wallpaperengine-gui"

# kare (V11) 环境: 只读 /usr 上构建前先就位依赖垫片 (非 kare 机器为空操作)
ensure_dep_shims

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

# ---- 2. 源码 + 补丁 + forge overlay ----
ensure_repo "https://github.com/AzPepoze/linux-wallpaperengine-gui" "$GUI_SRC" "$GUI_REF"
# FORGE_SKIP_PATCHES=1 构建无补丁上游 (冒烟/实验用)
if [ "${FORGE_SKIP_PATCHES:-0}" = "1" ]; then
	warn "FORGE_SKIP_PATCHES=1 — building UNPATCHED upstream (smoke/experiment build)"
else
	apply_patches "$GUI_SRC" "$FORGE_DIR/patches/gui"
fi

# src/gui-workshop: forge 自有前端源码, 覆盖安装到源码树 (逻辑进自有
# 源码, 补丁只留 vite 接线 — 与 Go 侧 pkg/background 同一教义)。每次构建
# 无条件覆盖, 端状态确定。
OVERLAY_DIR="$SRC_DIR/gui-workshop"
for f in "$OVERLAY_DIR"/*.ts; do
	name=$(basename "$f")
	cp -f "$f" "$GUI_SRC/src/frontend/main/services/$name"
	log "Overlay: src/frontend/main/services/$name <- src/gui-workshop/"
done
# 上游演进告警: overlay 覆盖上游同路径文件会遮蔽上游改动, UPSTREAM_BASE
# 记录 overlay 所基于的上游 blob hash, 不一致即显性提醒对照合并
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

# ---- 6. pkg/background 模块接线 ----
# 本地 replace 由构建脚本现场注入, 不进补丁: apply_patches 每轮把 go.mod
# 重置回上游, 因此每次构建前重新注入。目标为 forge 自有模块 pkg/background
# (纯 stdlib, 本地目录无需 go.sum)。
log "Wiring pkg/background module into GUI go.mod ..."
(cd "$GUI_SRC/src/backend" && go mod edit \
	-require="lwe-forge/pkg/background@v0.0.0" \
	-replace="lwe-forge/pkg/background=$FORGE_DIR/pkg/background")

# ---- 7. 构建 ----
# 流水线: tsc 类型门禁 -> Go 后端 (CGO) -> vite 前端 -> electron-builder --dir
# esbuild/vite 不做类型检查, 未定义标识符等错误 (曾致 GUI 窗口无法创建)
# 必须由 tsc 在构建期拦截
log "Type-checking frontend (tsc --noEmit) ..."
(cd "$GUI_SRC" && ./node_modules/.bin/tsc --noEmit)
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
