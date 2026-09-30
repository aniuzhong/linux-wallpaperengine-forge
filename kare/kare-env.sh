#!/usr/bin/env bash
# kare-env.sh — 麒麟 V11 (磐石/ostree + kare) 构建环境的检测与规范化
#
# 被 lib.sh 末尾自动 source。作用:
#   1. kare 机器 (shadow merge 树存在) → 导出规范环境变量, 构建依赖只认
#      垫片与 merge 树;
#   2. 非 kare 机器 → 什么也不做, 构建脚本走原生 apt 路径。
#
# 垫片的构建由 build-dep-shims.sh 完成 (声明式清单见 dep-spec.list);
# 本文件仅做一处写操作: 在 shims/usr/bin 安放 dpkg 修复别名 (见下)。

# ---- 检测 --------------------------------------------------------------------
KARE_MERGE_USR="/opt/kare-applications/shadow/merge/usr"
KARE_ACTIVE=0

kare_log() { printf '\033[36m[kare]\033[0m %s\n' "$*"; }

# 认机器不认瞬时状态: merge 树目录存在即是 kare 机器。/usr 的 ro 挂载
# 与 merge 树内容都会随 kare 会话刷新抖动 (rw 窗口、悬空符号链接),
# 以它们为激活条件会让构建随机落入"无垫片环境"而半途失败 —— 垫片本就
# 自洽, 会话抖动期照常可用。
if [ -d "$KARE_MERGE_USR" ]; then
	KARE_ACTIVE=1
	if ! findmnt -no OPTIONS /usr 2>/dev/null | grep -qE "(^|,)ro(,|$)"; then
		kare_log "note: /usr is not read-only right now (kare session refresh?); shim env applied anyway"
	fi
fi

if [ "$KARE_ACTIVE" != "1" ]; then
	# 非 kare 机器: 空操作, 构建脚本可无条件调用 ensure_dep_shims
	ensure_dep_shims() { :; }
	return 0 2>/dev/null || true
fi

kare_log "kare environment detected (merge tree present); build dependencies come from the merge tree + shims"

# ---- 规范环境 ----------------------------------------------------------------
# 依赖的三个所在: shims/ (自解产物, 在 toolchains/)、merge 树 (用户经 kare
# apt 装入)、垫片工具。本文件住在 kare/, 产物住在 toolchains/ —— 源码与
# 产物分离, toolchains/ 整体可弃可清。
FORGE_DIR="${FORGE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
FORGE_TOOLCHAINS="$FORGE_DIR/toolchains"
KARE_SHIMS="$FORGE_TOOLCHAINS/shims"

KARE_PC="$KARE_SHIMS/root/usr/lib/x86_64-linux-gnu/pkgconfig:$KARE_SHIMS/root/usr/share/pkgconfig:$KARE_MERGE_USR/lib/x86_64-linux-gnu/pkgconfig:$KARE_MERGE_USR/share/pkgconfig:$KARE_MERGE_USR/lib/pkgconfig"
KARE_LIBDIR="$KARE_SHIMS/root/usr/lib/x86_64-linux-gnu:$KARE_MERGE_USR/lib/x86_64-linux-gnu"

export PKG_CONFIG_PATH="${KARE_PC}${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# CPATH 只放垫片, 不放 merge: gcc 会把同时出现在 CPATH (用户列表) 与
# -isystem (系统列表, 来自 CMake 对 find 结果的 SYSTEM 标记) 的同一目录
# 去重并保留系统列表位置 —— CPATH 里的垫片条目被悄悄丢弃后, 用户列表
# 只剩 merge 条目, merge 的旧版 glm 反而排到垫片之前 (-Werror=volatile
# 炸掉 gcc12)。垫片缺什么在 spec 里补, merge 不进编译搜索路径。
export CPATH="$KARE_SHIMS/root/usr/include${CPATH:+:$CPATH}"
export LIBRARY_PATH="${KARE_LIBDIR}${LIBRARY_PATH:+:$LIBRARY_PATH}"
# CMake 的 find_path/find_library 不读 CPATH/LIBRARY_PATH (那是 gcc 的)。
# 前缀只给垫片, 不给 merge 树: 注入 merge 头目录会让它的 -isystem 排到
# CPATH 之前 (merge 的旧版 glm 会以 -Werror=volatile 炸掉 gcc12 编译);
# 垫片缺什么就在 spec 里补什么 —— 自洽教义对 CMake 探测同样成立
export CMAKE_PREFIX_PATH="$KARE_SHIMS/root/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
# cgo 不透传 LIBRARY_PATH, 链接期需要显式 -L
export CGO_LDFLAGS="-L${KARE_LIBDIR//:/ -L}"
# merge 树内的 git 是真 ELF 但编译前缀指向 /usr, 助手须指回 merge 树
# (会话刷新期 git-core 可能悬空, 此时退回 PATH 自然解析并给出原生报错)
if [ -d "$KARE_MERGE_USR/lib/git-core" ]; then
	export GIT_EXEC_PATH="$KARE_MERGE_USR/lib/git-core"
	export GIT_TEMPLATE_DIR="$KARE_MERGE_USR/share/git-core/templates"
fi
# merge 树的原生工具 (cmake/git/grim...) 优先于 /opt/kare 的 wrapper 版
# (wrapper 进 crun 容器执行, 脚本场景下 stdout 不可靠)。垫片工具再优先
# 于 merge 树: shims/usr/bin 放的是对 merge 树破损工具的修复别名。
# 例: merge 树的 dpkg 是 kare wrapper (日志写 /var/log/kare 需 root, 且
# 依赖可能不存在的 /usr/bin/dpkg.real), 构建期必须用基础系统真 dpkg。
mkdir -p "$KARE_SHIMS/usr/bin"
if [ -x /usr/bin/dpkg ] && [ ! -e "$KARE_SHIMS/usr/bin/dpkg" ]; then
	ln -s /usr/bin/dpkg "$KARE_SHIMS/usr/bin/dpkg" 2>/dev/null || true
fi
PATH="$KARE_SHIMS/usr/bin:$KARE_MERGE_USR/bin:$PATH"

# ---- 工具链自检 ----------------------------------------------------------------
# merge 树由 kare 会话的 overlay 供给, 会话刷新期符号链接链可能悬空
# (cc → /etc/alternatives/cc → /usr/bin/gcc-12, g++ → g++-12 → ...);
# PATH 解析会自动回落到基础系统 (base 有 cc/c++), 所以这里只要求
# "存在可用的编译器", 不强求来自 merge 树 —— 全部缺失时才中止, 并把
# CMake "is not a full path to an existing compiler tool" 这类模糊报错
# 变成明确指引。悬空符号链接过不了 command -v。
if ! command -v cc >/dev/null 2>&1; then
	die "No usable C compiler (neither merge tree $KARE_MERGE_USR/bin nor base system has cc) — kare session may be refreshing or not ready, retry later"
fi
if ! command -v g++ >/dev/null 2>&1 && ! command -v c++ >/dev/null 2>&1; then
	die "No usable C++ compiler (neither merge tree nor base system has g++/c++) — kare session may be refreshing or not ready, retry later"
fi
if ! command -v make >/dev/null 2>&1 && ! command -v gmake >/dev/null 2>&1; then
	die "No usable make — kare session may be refreshing or not ready, retry later"
fi

# cmake 必须来自 merge 树/垫片: 会话刷新窗口里 PATH 会回落到基础系统的
# cmake (/opt/kare 的 wrapper 版进 crun 容器, 也不可靠), 其 configure
# 行为与 merge 版不一致, 且会在末尾无任何报错地失败 —— 三次实机复现,
# Configuring incomplete 之前均无错误文本。此时明确失败, 等刷新结束。
_cmake_bin=$(command -v cmake 2>/dev/null || true)
case "$_cmake_bin" in
	"")
		die "No usable cmake — kare session may be refreshing or not ready, retry later"
		;;
	"$KARE_SHIMS/usr/bin/"*|"$KARE_MERGE_USR/"*)
		;;
	*)
		die "cmake resolved outside the merge tree ($_cmake_bin) — base-system cmake configure fails silently; wait for the kare session refresh to finish (merge-tree cmake usable) and retry"
		;;
esac
unset _cmake_bin
# GitHub 连接不稳, bun 一律走 npmmirror
export BUN_MIRROR="${BUN_MIRROR:-https://registry.npmmirror.com/-/binary/bun}"

# ---- 垫片构建入口 (build-* 在依赖探测前调用) ---------------------------------
# 幂等: 完成标记存在且比依赖清单新 → 跳过; dep-spec.list 更新过 → 重建。
# 注意 build-dep-shims.sh 自身的改动不触发重建, 改完需删除标记或碰一下
# dep-spec.list (加空行) 强制重建一轮。
ensure_dep_shims() {
	local marker="$KARE_SHIMS/.complete"
	local spec="$FORGE_DIR/kare/dep-spec.list"
	if [ -f "$marker" ] && [ "$marker" -nt "$spec" ] && [ -d "$KARE_SHIMS/root/usr/include" ]; then
		kare_log "dependency shims ready (toolchains/shims/)"
		return 0
	fi
	kare_log "building dependency shims (one-time, ~2-5 minutes)..."
	bash "$FORGE_DIR/kare/build-dep-shims.sh" || return 1
}
