#!/usr/bin/env bash
# kare-env.sh — 麒麟 V11 (磐石/ostree + kare) 构建环境的检测与规范化
#
# 被 lib.sh 末尾自动 source。作用:
#   1. 检测是否处于 kare 环境 (shadow merge 树存在且 /usr 只读挂载);
#   2. 是 → 构建依赖只能活在 merge 树/垫片里, 导出规范环境变量;
#   3. 否 (常规宿主) → 什么也不做, 构建脚本走原生 apt 路径。
#
# 垫片的构建由 build-dep-shims.sh 完成 (声明式清单见 dep-spec.list),
# 本文件只负责"检测 + 指路", 不做任何写操作。

# ---- 检测 --------------------------------------------------------------------
# /usr 只读挂载 (磐石不可变底座) + kare shadow merge 树存在, 二者同时成立才激活
KARE_MERGE_USR="/opt/kare-applications/shadow/merge/usr"
KARE_ACTIVE=0

if [ -d "$KARE_MERGE_USR" ] && findmnt -no OPTIONS /usr 2>/dev/null | grep -qE "(^|,)ro(,|$)"; then
	KARE_ACTIVE=1
fi

if [ "$KARE_ACTIVE" != "1" ]; then
	# 非 kare 机器: 空操作, 构建脚本可无条件调用 ensure_dep_shims
	ensure_dep_shims() { :; }
	return 0 2>/dev/null || true
fi

kare_log() { printf '\033[36m[kare]\033[0m %s\n' "$*"; }
kare_log "kare environment detected (read-only /usr); build dependencies come from the merge tree + shims"

# ---- 规范环境 ----------------------------------------------------------------
# 依赖的三个所在: shims/ (自解产物)、merge 树 (用户经 kare apt 装入)、垫片工具
FORGE_TOOLCHAINS="${FORGE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}/toolchains"
KARE_SHIMS="$FORGE_TOOLCHAINS/shims"

KARE_PC="$KARE_SHIMS/root/usr/lib/x86_64-linux-gnu/pkgconfig:$KARE_SHIMS/root/usr/share/pkgconfig:$KARE_MERGE_USR/lib/x86_64-linux-gnu/pkgconfig:$KARE_MERGE_USR/share/pkgconfig:$KARE_MERGE_USR/lib/pkgconfig"
KARE_CPATH="$KARE_SHIMS/root/usr/include:$KARE_MERGE_USR/include"
KARE_LIBDIR="$KARE_SHIMS/root/usr/lib/x86_64-linux-gnu:$KARE_MERGE_USR/lib/x86_64-linux-gnu"

export PKG_CONFIG_PATH="${KARE_PC}${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CPATH="${KARE_CPATH}${CPATH:+:$CPATH}"
export LIBRARY_PATH="${KARE_LIBDIR}${LIBRARY_PATH:+:$LIBRARY_PATH}"
# cgo 不透传 LIBRARY_PATH, 链接期需要显式 -L
export CGO_LDFLAGS="-L${KARE_LIBDIR//:/ -L}"
# merge 树内的 git 是真 ELF 但编译前缀指向 /usr, 助手须指回 merge 树
export GIT_EXEC_PATH="$KARE_MERGE_USR/lib/git-core"
export GIT_TEMPLATE_DIR="$KARE_MERGE_USR/share/git-core/templates"
# merge 树的原生工具 (cmake/git/grim...) 优先于 /opt/kare 的 wrapper 版
# (wrapper 进 crun 容器执行, 脚本场景下 stdout 不可靠)
PATH="$KARE_MERGE_USR/bin:$KARE_SHIMS/usr/bin:$PATH"
# GitHub 连接不稳, bun 一律走 npmmirror
export BUN_MIRROR="${BUN_MIRROR:-https://registry.npmmirror.com/-/binary/bun}"

# ---- 垫片构建入口 (build-* 在依赖探测前调用) ---------------------------------
# 幂等: 完成标记存在且比依赖清单新 → 跳过; dep-spec.list 更新过 → 重建
ensure_dep_shims() {
	local marker="$KARE_SHIMS/.complete"
	local spec="$FORGE_TOOLCHAINS/dep-spec.list"
	if [ -f "$marker" ] && [ "$marker" -nt "$spec" ] && [ -d "$KARE_SHIMS/root/usr/include" ]; then
		kare_log "dependency shims ready (toolchains/shims/)"
		return 0
	fi
	kare_log "building dependency shims (one-time, ~2-5 minutes)..."
	bash "$FORGE_TOOLCHAINS/build-dep-shims.sh" || return 1
}
