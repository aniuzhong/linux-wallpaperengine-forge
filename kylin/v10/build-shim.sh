#!/usr/bin/env bash
#
# build-shim.sh — 编译 UKUI 桌面透明注入器 libpeony-alpha.so
#
# 源码:   <片>/src/peony-qt-desktop/ (forge 自有源码, 单翻译单元, 非上游克隆;
#         位置由 target.sh 的 SHIM_SRC 声明)
# 产物:   <片>/packaging/lib/libpeony-alpha.so (package.sh 原样收入套件)
# 日志:   out/build-shim.log
#
# 说明:   构建要求刻意压到最低: cmake >= 3.16 (麒麟系统 3.16 直接达标,
#         不足时 ensure_cmake 落用户态), Qt5 仅取头文件 (Qt 符号由宿主
#         peony 进程在加载期解析, 不链接 Qt), 无第三方依赖, 全程离线。
#         与 build-engine.sh / build-gui.sh 无先后依赖, 可在独立容器中
#         单独运行。
#
set -euo pipefail
# 本脚本位于片内 (<族>/<版本>/), lib.sh 在仓库根
FORGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$FORGE_DIR"
source "../../lib.sh"

init_log shim
SHIM_BUILD="$SHIM_SRC/build"
PAYLOAD="$TARGET_DIR/packaging/lib"
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD"

# ---- 1. 系统依赖探测 ----
# Qt5Core/Qt5Gui 只提供头文件与 .pc 描述, 对应同一个 qtbase5-dev
log "Probing system dependencies..."
probe_reset
check_cmd 'g++' g++
check_cmd "$CC" "$CC"
check_cmd "$CXX" "$CXX"
check_cmd pkg-config pkg-config
check_lib Qt5Core qtbase5-dev
check_lib Qt5Gui qtbase5-dev
check_lib x11 libx11-dev
check_lib xcb libxcb1-dev
probe_report

# ---- 2. 工具链 ----
# 本目标没有引擎 glslang 子模块的 3.22 要求, 系统 cmake >= 3.16 即用
ensure_cmake 3.16

# ---- 3. 配置 + 编译 + 安装 ----
# CMake 构建树绑定绝对路径: 仓库搬家后旧缓存不可用。本目标编译只需数秒,
# 直接重建, 无需像引擎那样抢救缓存。
if [ -f "$SHIM_BUILD/CMakeCache.txt" ] && ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$SHIM_SRC" "$SHIM_BUILD/CMakeCache.txt"; then
	warn "Build tree was configured under a different path; recreating it"
	rm -rf "$SHIM_BUILD"
fi

log "Configuring CMake ..."
# 编译器随片声明钉定 (CC/CXX), 显式传入理由同 build-engine.sh
cmake -S "$SHIM_SRC" -B "$SHIM_BUILD" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_INSTALL_PREFIX="$PAYLOAD"
log "Building ($(nproc) jobs) ..."
cmake --build "$SHIM_BUILD" -j"$(nproc)"
log "Installing into $PAYLOAD ..."
cmake --install "$SHIM_BUILD"

SHIM_SO="$PAYLOAD/libpeony-alpha.so"
[ -f "$SHIM_SO" ] || die "Install finished but libpeony-alpha.so not found in payload"

# ---- 4. 自检门禁: 把既定的人工静态验证固化为常驻检查 ----
# 拦截 ABI: 恰好 4 个 interposition 符号必须可导出 (mangled 拼写不可改动)
for symbol in \
	_ZN7QPixmapC1ERK7QStringPKc6QFlagsIN2Qt19ImageConversionFlagEE \
	_ZN7QPixmapC2ERK7QStringPKc6QFlagsIN2Qt19ImageConversionFlagEE \
	xcb_change_property \
	XChangeProperty; do
	nm -D --defined-only "$SHIM_SO" | grep -qE " ${symbol}$" \
		|| die "Interposition symbol missing from export table: $symbol"
done
# 卫生: 绝不链接 Qt (Qt 符号必须由宿主 peony 进程解析)
if ldd "$SHIM_SO" | grep -qi qt; then
	die "Shim links against Qt; it must resolve Qt symbols from the host process only"
fi

log "=========================================="
log "Shim build complete: $SHIM_SO"
log "Staged into packaging/lib/"
log "=========================================="
