#!/usr/bin/env bash
#
# build-engine.sh — 编译 linux-wallpaperengine 引擎本体
#
# 源码:   上游克隆钉在 ENGINE_REF (third_party/), 构建前套用
#         patches/engine/ 全系列补丁
# 产物:   out/engine/ (扁平 payload: 安装前缀即载荷根, 引擎二进制与 CEF
#         运行时同级, 随套件整体分发)
# 日志:   out/build-engine.log
#
# 说明:   首次构建需从 CEF 官方构建源 (spotifycdn, 无国内镜像) 下载约
#         1GB 分发包; 下载结果常驻 toolchains/cef, 重建构建树时自动预置,
#         不重复下载。与 build-gui.sh / build-shim.sh 无先后依赖, 可在
#         独立容器中单独运行。
#
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source ./lib.sh

init_log engine
ENGINE_SRC="$THIRD_PARTY/linux-wallpaperengine"

# ---- 1. 系统依赖探测 ----
# 清单即引擎在麒麟 V10 SP1 上的全部构建依赖; 只探测并给出安装命令, 不代装。
log "Probing system dependencies..."
probe_reset
check_cmd git git
check_cmd curl curl
check_cmd cc build-essential
check_cmd 'g++' g++
check_cmd pkg-config pkg-config
check_lib gl libgl-dev
check_lib xrandr libxrandr-dev
check_lib xinerama libxinerama-dev
check_lib xcursor libxcursor-dev
check_lib xi libxi-dev
check_lib xxf86vm libxxf86vm-dev
check_lib xcb-randr libxcb-randr0-dev
check_lib glew libglew-dev
check_header /usr/include/GL/glut.h freeglut3-dev
check_lib sdl2 libsdl2-dev
check_lib liblz4 liblz4-dev
check_lib libavcodec libavcodec-dev
check_lib libavformat libavformat-dev
check_lib libavutil libavutil-dev
check_lib libswscale libswscale-dev
check_lib mpv libmpv-dev
check_lib glfw3 libglfw3-dev
check_lib libpulse libpulse-dev
check_lib fftw3 libfftw3-dev
check_lib freetype2 libfreetype-dev
check_lib dbus-1 libdbus-1-dev
check_lib zlib zlib1g-dev
check_lib libpng libpng-dev
check_lib gmp libgmp-dev
check_header /usr/include/glm/glm.hpp libglm-dev
probe_report

# ---- 2. 源码 + 子模块 + 补丁 ----
ensure_repo "https://github.com/Almamu/linux-wallpaperengine.git" "$ENGINE_SRC" "$ENGINE_REF"
log "Syncing submodules (slow on first run) ..."
git -C "$ENGINE_SRC" submodule update --init --recursive
apply_patches "$ENGINE_SRC" "$FORGE_DIR/patches/engine"

# ---- 3. 工具链 ----
# glslang 子模块要求 cmake >= 3.22, 麒麟系统只有 3.16, 不足时落用户态
ensure_cmake 3.22

# ---- 4. 配置 + 编译 + 安装 ----
BUILD_DIR="$ENGINE_SRC/build"
# 引擎产物扁平化: 安装前缀即载荷根 (可重定位, 无 /opt 嵌套)
PAYLOAD="$OUTPUT/engine"

# CMake 构建树绑定绝对路径, 环境切换 (容器 <-> 宿主机) 后旧缓存不可用;
# 重建前把 ~1GB 的 CEF 下载抢救进 toolchains/cef, 避免重复下载。
CEF_CACHE="$TOOLCHAINS/cef"
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$ENGINE_SRC" "$BUILD_DIR/CMakeCache.txt"; then
	warn "Build tree was configured under a different path; recreating it (CEF download cache preserved at $CEF_CACHE)"
	if [ -d "$BUILD_DIR/cef" ]; then
		rm -rf "$CEF_CACHE"
		mkdir -p "$CEF_CACHE"
		cp -al "$BUILD_DIR/cef/." "$CEF_CACHE/" 2>/dev/null || cp -a "$BUILD_DIR/cef/." "$CEF_CACHE/"
	fi
	rm -rf "$BUILD_DIR"
fi
# 预置 CEF 缓存: DownloadCEF 见到解压目录即跳过下载
if [ -n "$(ls -A "$CEF_CACHE" 2>/dev/null)" ]; then
	mkdir -p "$BUILD_DIR/cef"
	cp -al "$CEF_CACHE/." "$BUILD_DIR/cef/" 2>/dev/null || cp -a "$CEF_CACHE/." "$BUILD_DIR/cef/"
fi

log "Configuring CMake ..."
cmake -S "$ENGINE_SRC" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PAYLOAD" \
	-DBUILD_TESTING=OFF
log "Building ($(nproc) jobs) ..."
cmake --build "$BUILD_DIR" -j"$(nproc)"
log "Installing into $PAYLOAD ..."
cmake --install "$BUILD_DIR"

# install(TARGETS) 把主程序放在前缀根目录, 与 CEF 运行时同级
[ -x "$PAYLOAD/linux-wallpaperengine" ] || die "Install finished but linux-wallpaperengine not found in payload"

# ---- 5. 载荷清理: 剔除运行时无关的安装产物 ----
# install() 会把子模块的开发/测试产物一并装进前缀。安全性已经 readelf
# 核对: 引擎库的 23 项 NEEDED 中 glslang/spirv 均为静态链接, 唯一的子模块
# 动态依赖 kissfft 经 RUNPATH $ORIGIN/lib 加载。剔除:
#   - 测试/基准二进制 (api-test 与 kissfft 的 bm_*/fastconv*/fft 系)
#   - QuickJS 命令行工具 (qjs/qjsc/run-test262/function_source)
#   - 着色器 CLI (glslang/glslangValidator/spirv-cross/spirv-remap)
#   - 开发文件 (include/ share/ bin/, 以及 lib/ 下 kissfft 运行时库以外
#     的全部静态库与 cmake/pkgconfig 导出)
# CEF 运行时文件 (libcef.so/pak/icudtl/locales) 一律不碰。
log "Pruning non-runtime files from payload ..."
(
	cd "$PAYLOAD"
	rm -rf bin include share
	rm -f api-test \
		bm_fftw-float bm_kiss-float fastconv-float fastconvr-float fastfilt-float \
		ffr-float fft-float psdpng-float st-float testcpp-float tkfc-float tr-float \
		function_source run-test262 qjs qjsc \
		glslang glslangValidator spirv-cross spirv-remap
	find lib -mindepth 1 ! -name 'libkissfft-float.so*' -delete
)

log "=========================================="
log "Engine build complete: $PAYLOAD"
log "Run: $PAYLOAD/linux-wallpaperengine --help"
log "=========================================="
