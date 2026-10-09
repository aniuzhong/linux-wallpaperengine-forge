#!/usr/bin/env bash
# build-dep-shims.sh — 依据 dep-spec.list 幂等地构建 V11/kare 依赖垫片
#
# 被 kare-env.sh 的 ensure_dep_shims 调用, 也可手工执行。产物:
#   shims/root/                 dpkg -x 提取的 -dev/运行时文件 (pc/头文件/库)
#   shims/usr/bin/              wayland-scanner (kare upper 层真 ELF) 等工具
#   shims/usr/bin/node          bun 别名 (上游 tsc 的 shebang 需要 node)
#   shims/.complete             完成标记 (mtime 用于幂等判定)
#
# 三步: ① 按根依赖做 pkg-config 闭包循环 (报错驱动, 报告缺失 pc → 查
# dep-spec → 逐包下载提取); ② 提取各包的运行时库; ③ 规范化路径与符号链接。
# 全程无 root、不写系统目录。
set -uo pipefail

# 本脚本住在 kare/ (源码), 产物落在 toolchains/shims/ (可弃缓存);
# 下载的 .deb 落在垫片目录, 提取后即删, 不污染源码目录
KARE_DIR="$(cd "$(dirname "$0")" && pwd)"
FORGE_DIR="${FORGE_DIR:-$(cd "$KARE_DIR/../../.." && pwd)}"
SHIMS="$FORGE_DIR/toolchains/shims"
ROOT="$SHIMS/root"
SPEC="$KARE_DIR/dep-spec.list"
MERGE_USR="/opt/kare-applications/shadow/merge/usr"
MAX_ROUNDS=10
mkdir -p "$ROOT"
cd "$SHIMS"

log() { printf '\033[36m[shims]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[shims]\033[0m %s\n' "$*" >&2; }

# 提取一律用基础系统真 dpkg: PATH 里 merge 树的 dpkg 是 kare wrapper
# (日志写 /var/log/kare 需 root, 且依赖可能不存在的 /usr/bin/dpkg.real),
# 会让所有 dpkg -x 静默失败 (症状: "extraction failed" 刷屏, 闭包不收敛)
DPKG=/usr/bin/dpkg
[ -x "$DPKG" ] || DPKG="$(command -v dpkg)"

# 闭包探测只认垫片自身: PKG_CONFIG_LIBDIR 屏蔽 pkg-config 的默认系统
# 目录, 且不继承环境里的 merge 树路径 —— 否则"机器上已有"会掩盖垫片
# 缺口 (建垫片时 merge 树装着某包 → 不下载; kare 会话刷新清掉 merge
# 树后缺口才暴露)。垫片必须自洽, 收敛判定也只在垫片视角下成立。
export PKG_CONFIG_LIBDIR="$ROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$ROOT/usr/share/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"

# 根依赖 = 两个构建脚本探测清单的并集
# wayland-protocols: 引擎 wayland 后端的协议 XML 来源, 不被任何 pc 链引入, 须显式验证
ROOT_PCS="mpv glfw3 glew sdl2 liblz4 libavcodec libavformat libavutil libswscale libpulse fftw3 dbus-1 gmp egl gl \
	gtk+-3.0 ayatana-appindicator3-0.1 glib-2.0 wayland-client wayland-protocols"

# ---- 工具函数 ----------------------------------------------------------------

# spec_lookup <pc名> → 输出 "dev候选…|运行时候选…"
spec_lookup() {
	awk -F'|' -v key="$1" '
		/^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
		{
			gsub(/[[:space:]]/, "", $1)
			if ($1 == key) { gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); gsub(/^[[:space:]]+|[[:space:]]+$/, "", $3); print $2 "|" $3; exit }
		}' "$SPEC"
}

extract_debs() {
	local dest="$1" d
	mkdir -p "$dest"
	for d in *.deb; do
		[ -e "$d" ] || continue
		"$DPKG" -x "$d" "$dest/" || warn "extraction failed: $d"
		rm -f "$d"
	done
}

# 提取后统一规范化: pc 的 prefix 指向垫片自身; 绝对路径符号链接改为相对
normalize() {
	find "$ROOT" -name '*.pc' -exec sed -i -E \
		-e "s|^prefix=/usr\$|prefix=$ROOT/usr|" \
		-e "s|^original_prefix=/usr\$|original_prefix=$ROOT/usr|" \
		-e "s|=/usr/|=$ROOT/usr/|g" \
		-e "s|= /usr/|= $ROOT/usr/|g" \
		-e "s|=/usr\$|=$ROOT/usr|" {} +
	while IFS= read -r link; do
		tgt=$(readlink "$link")
		case "$tgt" in
			# normalize 会被闭包循环多轮调用: 已改写为垫片内路径的链接必须
			# 跳过, 否则每轮再前置一次 $ROOT, 产出加倍的悬空路径
			"$ROOT"/*) ;;
			/*) ln -snf "$ROOT${tgt%%)}" "$link" 2>/dev/null || ln -snf "$(basename "$tgt")" "$link" ;;
		esac
	done < <(find -L "$ROOT/usr/lib" -maxdepth 3 -type l -name '*.so*' 2>/dev/null)
}

# 下载候选包列表 (逐个容错), 成功即停; 无候选则告警返回 1
download_first() {
	local pkg
	for pkg in $1; do
		if apt-get download "$pkg" >> /tmp/forge-dep-dl.log 2>&1; then
			log "  downloaded: $pkg"
			return 0
		fi
	done
	warn "  not in apt sources: $1"
	return 1
}

# ---- ① 闭包循环: 缺什么 pc 就按 spec 下载什么 --------------------------------
log "dependency closure loop (roots: $ROOT_PCS)"
for round in $(seq 1 $MAX_ROUNDS); do
	miss=$(pkg-config --errors-to-stdout --print-errors --cflags $ROOT_PCS 2>&1 |
		grep -oP "Package '\K[^']+(?=',)" | sort -u | tr '\n' ' ')
	if [ -z "${miss// /}" ]; then
		log "closure complete (round ${round})"
		break
	fi
	todo=""
	for p in $miss; do
		lookup=$(spec_lookup "$p")
		[ -n "$lookup" ] || case $p in
			lib*) lookup="$p-dev|" ;;
			*) lookup="lib$p-dev|" ;;
		esac
		todo="$todo ${lookup%%|*}"
	done
	log "round ${round} missing: $(echo $todo)"
	ok=0
	for t in $todo; do
		download_first "$t" && { extract_debs "$ROOT"; ok=1; }
	done
	[ "$ok" = 1 ] || { warn "closure made no progress, aborting"; exit 1; }
	normalize
done
pkg-config --exists $ROOT_PCS 2>/dev/null || { warn "closure did not converge:"; pkg-config --errors-to-stdout --print-errors --cflags $ROOT_PCS 2>&1 | head -4; exit 1; }

# ---- ②' 无 pc 的纯头文件包: 闭包循环探测不到, 按 spec 契约显式安装 --------
# spec 约定 pc 名为 "-" 的行"总是安装" (libglm-dev / freeglut3-dev
# 这类纯头文件包); 此前未实现, glm 一直靠基础 /usr 的侥幸存在, /usr 视图一翻
# 转就缺 —— find_package(GLUT) 与 glm include 因此失明
log "extracting no-pc header packages..."
while IFS='|' read -r pc devs runs; do
	case "$pc" in ['#']*|"") continue ;; esac
	pc=$(echo "$pc" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
	[ "$pc" = "-" ] || continue
	devs=$(echo "$devs" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
	[ -n "$devs" ] || continue
	download_first "$devs" && extract_debs "$ROOT"
done < "$SPEC"
normalize

# ---- ② 运行时库: 链接期 .so 符号链接的目标 -----------------------------------
log "extracting runtime libraries..."
while IFS='|' read -r pc devs runs; do
	case "$pc" in ['#']*|"") continue ;; esac
	pc=$(echo "$pc" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
	runs=$(echo "$runs" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
	[ -n "$runs" ] || continue
	# 运行时列语义 = 逐个全部尝试 (与 dev 列的"首个成功即停"不同): 列内
	# 是并存的拆分包或版本别名, 各含不同 .so —— 只取首个会漏 (如
	# libpangocairo-1.0-0 与 libpango-1.0-0); 下载失败的别名静默跳过
	ok=0
	for r in $runs; do
		if apt-get download "$r" >> /tmp/forge-dep-dl.log 2>&1; then
			log "  downloaded runtime: $r"
			ok=1
		fi
	done
	if [ "$ok" = 1 ]; then extract_debs "$ROOT"; else warn "  runtime not in apt sources: $runs"; fi
done < "$SPEC"
normalize

# ---- ②'' 断链自愈: dev 符号链接只由 dev 包提取产生, 闭包全绿的重跑不会
# 重建它们 —— 会话刷新清掉 merge 树后悬空的 dev 链接 (典型症状) 若只重跑
# 运行时提取将永不愈合。存在悬空 .so 链接即重提取全部 spec dev 包。
broken=$(find "$ROOT/usr/lib" -maxdepth 3 -type l -name '*.so*' 2>/dev/null |
	while IFS= read -r l; do [ -e "$l" ] || echo x; done | wc -l)
if [ "$broken" -gt 0 ]; then
	log "healing $broken broken link(s): re-extracting all spec dev packages..."
	while IFS='|' read -r pc devs runs; do
		case "$pc" in ['#']*|"") continue ;; esac
		devs=$(echo "$devs" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
		[ -n "$devs" ] || continue
		download_first "$devs" && extract_debs "$ROOT"
	done < "$SPEC"
	normalize
fi

# ---- ③ 工具与符号链接修补 -----------------------------------------------------
# node 别名: 上游 tsc 的 shebang 需要 node; bun 官方支持被链接为 node
mkdir -p "$SHIMS/usr/bin"
if [ -x "$FORGE_DIR/toolchains/bun/bin/bun" ] && [ ! -e "$SHIMS/usr/bin/node" ]; then
	ln -s "$FORGE_DIR/toolchains/bun/bin/bun" "$SHIMS/usr/bin/node"
	log "node alias -> bun"
fi
# wayland-scanner: 麒麟藏在 libsdl2-dev 且 /opt/kare/usr/bin 下是 wrapper 脚本,
# shadow upper 层才有真 ELF
if [ ! -x "$SHIMS/usr/bin/wayland-scanner" ]; then
	for cand in /opt/kare-applications/shadow/upper/usr/bin/wayland-scanner "$MERGE_USR/bin/wayland-scanner"; do
		if [ -x "$cand" ] && file "$cand" | grep -q ELF; then
			cp "$cand" "$SHIMS/usr/bin/wayland-scanner"
			log "wayland-scanner ← $cand"
			break
		fi
	done
fi
# cmake: merge 树 cmake 会话刷新期可能悬空, 回落到基础系统 cmake 的
# configure 会静默失败 (无任何报错文本)。垫片自备一份真 cmake, 会话
# 任何状态下都可用。cmake-data 是独立包 (Modules 全在里面), 缺它则
# CMAKE_ROOT 报错; 二进制以真实文件落位 (垫片下符号链接 exec 有
# ENOENT 诡异行为), Modules 留在 root/usr/share, 用 usr/share 目录
# 链接补齐二进制的 ../share 相对查找
if [ ! -x "$SHIMS/usr/bin/cmake" ]; then
	ok=1
	download_first "cmake" && extract_debs "$ROOT" || ok=0
	download_first "cmake-data" && extract_debs "$ROOT" || ok=0
	if [ "$ok" = 1 ] && [ -x "$ROOT/usr/bin/cmake" ] && [ -d "$ROOT/usr/share/cmake-3.28" ]; then
		cp "$ROOT/usr/bin/cmake" "$SHIMS/usr/bin/cmake"
		ln -sfn ../root/usr/share "$SHIMS/usr/share"
		log "cmake ← $ROOT/usr/bin/cmake (+ cmake-data)"
	else
		warn "cmake shim unavailable; builds will depend on the merge tree cmake"
	fi
fi
# 断链修补: .so 符号链接目标若不在垫片中, 指向 merge 树的运行时
while IFS= read -r link; do
	tgt=$(readlink "$link")
	[ -e "$link" ] && continue
	base=$(basename "$tgt")
	if [ -e "$MERGE_USR/lib/x86_64-linux-gnu/$base" ]; then
		ln -snf "$MERGE_USR/lib/x86_64-linux-gnu/$base" "$link"
	else
		warn "unresolvable broken link: $link -> $tgt"
	fi
done < <(find "$ROOT/usr/lib" -maxdepth 3 -type l -name '*.so*' 2>/dev/null)

touch "$SHIMS/.complete"
log "shims build complete: $SHIMS"
