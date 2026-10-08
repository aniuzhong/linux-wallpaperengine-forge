#!/usr/bin/env bash
#
# package.sh — 组装套件发布物
#
# 前置:   两个构建入口的产物齐备:
#           out/linux-unpacked/  GUI 本体 (build-gui.sh)
#           out/engine/          引擎本体 (build-engine.sh, 已裁剪)
# 产物:   out/lwe-forge-<GUI版本>+<引擎短哈希>-<片路径-架构>.tar.gz
# 日志:   out/build-package.log
#
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log package

GUI_PAYLOAD="$OUTPUT/linux-unpacked"
ENG_PAYLOAD="$OUTPUT/engine"
GUI_SRC="$THIRD_PARTY/linux-wallpaperengine-gui"

# ---- 1. 前置校验 ----
[ -d "$GUI_PAYLOAD" ] || die "out/linux-unpacked does not exist, please run build-gui.sh first"
[ -x "$ENG_PAYLOAD/linux-wallpaperengine" ] || die "out/engine is incomplete, please run build-engine.sh first"
[ -d "$TARGET_DIR/packaging" ] || die "missing $TARGET_DIR/packaging"

# ---- 2. 版本与命名 ----
# GUI 版本取自上游 package.json; 套件名携带引擎短哈希, 一眼可溯构建基线
GUI_VER=$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$GUI_SRC/package.json" | head -n1)
[ -n "$GUI_VER" ] || die "Cannot read GUI version number"
ENG_REF9="${ENGINE_REF:0:9}"
TARGET_TAG="$(printf '%s' "${TARGET_DIR#"$FORGE_DIR"/}" | tr '/' '-')-$(forge_arch)"
SUITE_NAME="lwe-forge-${GUI_VER}+${ENG_REF9}-${TARGET_TAG}"
STAGE="$OUTPUT/$SUITE_NAME"

# ---- 3. 组装 ----
log "Assembling $SUITE_NAME ..."
rm -rf "$STAGE"
mkdir -p "$STAGE/bin"

# GUI 本体
cp -a "$GUI_PAYLOAD" "$STAGE/gui"
# 引擎本体 (扁平 payload)
cp -a "$ENG_PAYLOAD/." "$STAGE/engine/"
# 包内引擎入口: 相对符号链接, 随包整体搬移
ln -s ../engine/linux-wallpaperengine "$STAGE/bin/linux-wallpaperengine"
cp -a "$TARGET_DIR/packaging/." "$STAGE/"

# VERSION 成分表: 组件版本与全部补丁清单, 随包交付。
# 枚举按 basename 排序, 与分目录前的单一目录输出保持逐字节一致。
{
	echo "suite  : lwe-forge"
	echo "gui    : $GUI_VER (ref $GUI_REF)"
	echo "engine : ref $ENGINE_REF"
	for f in "$STAGE"/lib/*; do
		[ -e "$f" ] || continue
		echo "integration: $(basename "$f")"
	done
	echo "patches:"
	for d in "${GUI_PATCH_DIRS[@]}"; do
		for p in "$d"/*.patch; do [ -e "$p" ] || continue; echo "  gui/$(basename "$p")"; done
	done | sort
	for d in "${ENGINE_PATCH_DIRS[@]}"; do
		for p in "$d"/*.patch; do [ -e "$p" ] || continue; echo "  engine/$(basename "$p")"; done
	done | sort
	echo "built  : $(date '+%F %T %Z')"
} > "$STAGE/VERSION"

# ---- 4. 压缩 ----
# 载荷 ~1.5 GB (CEF 占大头), 压缩需要几分钟
log "Compressing $SUITE_NAME.tar.gz (payload ~1.5 GB, takes a few minutes) ..."
tar -C "$OUTPUT" -czf "$OUTPUT/$SUITE_NAME.tar.gz" "$SUITE_NAME"
rm -rf "$STAGE"

log "=========================================="
log "Suite ready: $OUTPUT/$SUITE_NAME.tar.gz ($(du -h "$OUTPUT/$SUITE_NAME.tar.gz" | cut -f1))"
log "Usage: tar xzf $SUITE_NAME.tar.gz && cd $SUITE_NAME && ./run-gui.sh"
log "=========================================="
