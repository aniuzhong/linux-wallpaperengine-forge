#!/usr/bin/env bash
#
# kylin/v10 平台画像 — 纯声明, 无逻辑; 被 lib.sh source。
# 根目录为公共层, 本文件只声明"本片消费哪些公共内容 + 本片特有什么"。
#

GUARD_PRETTY=Desktop
GUARD_ID_LIKE=debian

export CC="${CC:-gcc-10}"
export CXX="${CXX:-g++-10}"

INTEGRATION_BUILD="$TARGET_DIR/build-shim.sh"

# ---- 补丁 ----
# 公共层在前、片内在后, 数组顺序即套用序
ENGINE_PATCH_DIRS=("$TARGET_DIR/patches/engine")
GUI_PATCH_DIRS=("$FORGE_DIR/patches/gui" "$TARGET_DIR/patches/gui")

# ---- Go 接线 ----
# GUI 后端经 go.mod replace 接入的 forge 模块: module 路径不变 (烤在补丁
# 的 import 里), 只指向本片目录
GO_REPLACE_PKG=lwe-forge/pkg/peony
GO_REPLACE_DIR="$TARGET_DIR/pkg/peony"

# ---- overlay ----
# 覆盖进 GUI 源码树的 forge 前端源码
OVERLAY_DIR="$TARGET_DIR/src/gui-workshop"

# ---- shim ----
# UKUI 桌面透明注入库 (build-shim.sh, 本片特有入口)
SHIM_SRC="$TARGET_DIR/src/peony-qt-desktop"
