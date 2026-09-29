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

cd "$(dirname "$0")"
SHIMS="$PWD/shims"
ROOT="$SHIMS/root"
SPEC="$PWD/dep-spec.list"
MERGE_USR="/opt/kare-applications/shadow/merge/usr"
MAX_ROUNDS=10

log() { printf '\033[36m[shims]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[shims]\033[0m %s\n' "$*" >&2; }

export PKG_CONFIG_PATH="$ROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$ROOT/usr/share/pkgconfig:${PKG_CONFIG_PATH:-}"

# 根依赖 = 两个构建脚本探测清单的并集 + shim 自身编译所需 (Qt 只取头)
# wayland-protocols: 引擎 wayland 后端的协议 XML 来源, 不被任何 pc 链引入, 须显式验证
ROOT_PCS="mpv glfw3 glew sdl2 liblz4 libavcodec libavformat libavutil libswscale libpulse fftw3 dbus-1 gmp egl gl \
	gtk+-3.0 ayatana-appindicator3-0.1 glib-2.0 Qt5Core Qt5Gui wayland-client wayland-protocols"

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
		dpkg -x "$d" "$dest/" || warn "extraction failed: $d"
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

# ---- ② 运行时库: 链接期 .so 符号链接的目标 -----------------------------------
log "extracting runtime libraries..."
while IFS='|' read -r pc devs runs; do
	case "$pc" in ['#']*|"") continue ;; esac
	pc=$(echo "$pc" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
	runs=$(echo "$runs" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
	[ -n "$runs" ] || continue
	download_first "$runs" && extract_debs "$ROOT"
done < "$SPEC"
normalize

# ---- ③ 工具与符号链接修补 -----------------------------------------------------
# node 别名: 上游 tsc 的 shebang 需要 node; bun 官方支持被链接为 node
mkdir -p "$SHIMS/usr/bin"
if [ -x "$PWD/bun/bin/bun" ] && [ ! -e "$SHIMS/usr/bin/node" ]; then
	ln -s "$PWD/bun/bin/bun" "$SHIMS/usr/bin/node"
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
