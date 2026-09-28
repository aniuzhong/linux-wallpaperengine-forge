#!/usr/bin/env bash
#
# package.sh — 组装套件发布物
#
# 前置:   三个构建入口的产物齐备:
#           out/linux-unpacked/                GUI 本体 (build-gui.sh)
#           out/engine/                        引擎本体 (build-engine.sh, 已裁剪)
#           out/integration/libpeony-alpha.so  注入库 (build-shim.sh)
# 产物:   out/lwe-forge-<GUI版本>+<引擎短哈希>-kylin10-x64.tar.gz
# 日志:   out/build-package.log
#
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log package

GUI_PAYLOAD="$OUTPUT/linux-unpacked"
ENG_PAYLOAD="$OUTPUT/engine"
SHIM_SO="$OUTPUT/integration/libpeony-alpha.so"
GUI_SRC="$THIRD_PARTY/linux-wallpaperengine-gui"

# ---- 1. 前置校验 ----
[ -d "$GUI_PAYLOAD" ] || die "out/linux-unpacked does not exist, please run build-gui.sh first"
[ -x "$ENG_PAYLOAD/linux-wallpaperengine" ] || die "out/engine is incomplete, please run build-engine.sh first"
[ -f "$SHIM_SO" ] || die "out/integration/libpeony-alpha.so does not exist, please run build-shim.sh first"

# ---- 2. 版本与命名 ----
# GUI 版本取自上游 package.json; 套件名携带引擎短哈希, 一眼可溯构建基线
GUI_VER=$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$GUI_SRC/package.json" | head -n1)
[ -n "$GUI_VER" ] || die "Cannot read GUI version number"
ENG_REF9="${ENGINE_REF:0:9}"
SUITE_NAME="lwe-forge-${GUI_VER}+${ENG_REF9}-kylin10-x64"
STAGE="$OUTPUT/$SUITE_NAME"

# ---- 3. 组装 ----
log "Assembling $SUITE_NAME ..."
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib"

# GUI 本体
cp -a "$GUI_PAYLOAD" "$STAGE/gui"
# 引擎本体 (扁平 payload)
cp -a "$ENG_PAYLOAD/." "$STAGE/engine/"
# 桌面透明注入库: GUI 后端经逐级向上查找定位 (gui/resources → 套件根 lib/)
cp -a "$SHIM_SO" "$STAGE/lib/"
# 包内引擎入口: 相对符号链接, 随包整体搬移
ln -s ../engine/linux-wallpaperengine "$STAGE/bin/linux-wallpaperengine"

# 启动器与说明书
install -m 755 "$FORGE_DIR/packaging/run-gui.sh" "$STAGE/run-gui.sh"
install -m 644 "$FORGE_DIR/packaging/README.md"  "$STAGE/README.md"

# VERSION 成分表: 三个组件的版本与全部补丁清单, 随包交付
{
	echo "suite  : lwe-forge"
	echo "gui    : $GUI_VER (ref $GUI_REF)"
	echo "engine : ref $ENGINE_REF"
	echo "shim   : peony-alpha (src/peony-qt-desktop)"
	echo "patches:"
	for p in "$FORGE_DIR"/patches/gui/*.patch;  do echo "  gui/$(basename "$p")"; done
	for p in "$FORGE_DIR"/patches/engine/*.patch; do echo "  engine/$(basename "$p")"; done
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
