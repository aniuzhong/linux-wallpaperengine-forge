#!/usr/bin/env bash
#
# 被 build-*.sh source，不直接执行。
#
# 布局约定 (按所有权划分):
#   src/           forge 自有源码 (TS: gui-workshop; C++: peony-qt-desktop)
#   pkg/           forge 自有 Go 模块 (peony, 经 go.mod replace 接入)
#   third_party/   上游克隆 (钉住版本, 含各自构建缓存; 勿直接修改,
#                  ensure_repo 会 checkout -f 冲掉)
#   toolchains/    用户态工具链 (go / bun / rust / cmake) 与大文件缓存 (cef)
#   patches/       forge 对 third_party 克隆的差异补丁 (条目说明见
#                  patches/*/README.md)
#   out/           产物与日志 (build-<目标>.log)

# ---- 版本钉 (环境变量可覆盖) ----
GUI_REF="${GUI_REF:-8855fad673932991dde0241530941a520b0e34a2}"
ENGINE_REF="${ENGINE_REF:-b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d}"
BUN_VERSION="${BUN_VERSION:-1.4.2}"
CMAKE_VERSION="${CMAKE_VERSION:-4.4.3}"

# ---- 编译器钉 (环境变量可覆盖) ----
export CC="${CC:-gcc-10}"
export CXX="${CXX:-g++-10}"

FORGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$FORGE_DIR/src"
THIRD_PARTY="$FORGE_DIR/third_party"
TOOLCHAINS="$FORGE_DIR/toolchains"
OUTPUT="$FORGE_DIR/out"

# ---- 镜像 ----
GO_MIRRORS=(
	"https://mirrors.aliyun.com/golang"
	"https://golang.google.cn/dl"
)
BUN_MIRROR="${BUN_MIRROR:-https://github.com/oven-sh/bun/releases/download}" # 无国内镜像,可自行替换
NPM_REGISTRY="https://registry.npmmirror.com"
ELECTRON_MIRROR="https://npmmirror.com/mirrors/electron/"
ELECTRON_BUILDER_BINARIES_MIRROR="https://npmmirror.com/mirrors/electron-builder-binaries/"
GOPROXY_URL="https://goproxy.cn,direct"
RUSTUP_DIST_SERVER="https://rsproxy.cn"
RUSTUP_UPDATE_ROOT="https://rsproxy.cn/rustup"
CMAKE_URLS=(
	"https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION"
	"https://cmake.org/files/v${CMAKE_VERSION%.*}"
)

log() { printf '\033[32m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*"; }
warn() { printf '\033[33m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*" >&2; }
die() { printf '\033[31m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*" >&2; exit 1; }

# 每个入口调用一次:日志写入 out/build-<目标>.log,同时在终端显示
init_log() {
	mkdir -p "$OUTPUT"
	exec > >(tee -a "$OUTPUT/build-$1.log") 2>&1
	LOG_TAG="$1"
}

# fetch <输出文件> <候选URL...> — 依次尝试直到成功
fetch() {
	local out="$1" url
	shift
	for url in "$@"; do
		if curl -fL --retry 3 --connect-timeout 15 --progress-bar -o "$out" "$url"; then
			return 0
		fi
	done
	return 1
}

# ensure_repo <URL> <目录> <钉住ref> — 克隆(若缺)并对齐到 ref(变更时自动切换)
ensure_repo() {
	local url="$1" dir="$2" ref="$3"
	if [ ! -d "$dir/.git" ]; then
		log "Cloning $url ..."
		git clone "$url" "$dir"
	fi
	local cur
	cur=$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo "")
	if [ "$cur" != "$ref" ]; then
		log "Aligning source to $ref ..."
		git -C "$dir" fetch origin >/dev/null 2>&1 || true
		git -C "$dir" checkout -f "$ref" 2>/dev/null || {
			git -C "$dir" fetch origin "$ref" && git -C "$dir" checkout -f "$ref"
		}
	fi
	log "Source ready: $(basename "$dir") @ ${ref:0:9}"
}

# apply_patches <仓库> <补丁目录>
# 每次从干净基线(git HEAD)重新套用全部补丁: 补丁应用为瞬时操作,
# 端状态确定, 且不依赖上一次的套用痕迹 — 避免"已生效跳过"检测在
# 补丁上下文重叠时失效的问题。
apply_patches() {
	local repo="$1" dir="$2" patch name f
	git -C "$repo" checkout -- . 2>/dev/null || true
	# 移除补丁即将新建的文件残留 (上一次套用的产物), 否则 git apply 会因文件已存在而失败
	local newfiles
	newfiles=$(awk '/^--- \/dev\/null$/{nl=1; next} nl==1 && /^\+\+\+ b\//{sub(/^\+\+\+ b\//, ""); print; nl=0; next} {nl=0}' "$dir"/*.patch 2>/dev/null | sort -u)
	for f in $newfiles; do
		rm -f "$repo/$f"
	done
	shopt -s nullglob
	for patch in "$dir"/*.patch; do
		name=$(basename "$patch")
		if (cd "$repo" && git apply --check "$patch" 2>/dev/null); then
			(cd "$repo" && git apply "$patch")
			log "Applying patch: $name"
		else
			die "Cannot apply patch (upstream may have moved): $patch"
		fi
	done
	# 收尾必须关掉 nullglob: 本文件被 source 进调用方, 泄漏会改变其后
	# 所有通配行为 (空匹配展开为空串而非字面模式)
	shopt -u nullglob
}

# ---- 依赖探测框架:入口声明探测项,probe_report 统一报告 ----
PROBE_MISSING=()
PROBE_NO_PC=0

probe_reset() {
	PROBE_MISSING=()
	PROBE_NO_PC=0
}

check_cmd() { # <命令> <提供的软件包>
	if ! command -v "$1" >/dev/null 2>&1; then
		warn "Missing command: $1 (package: $2)"
		PROBE_MISSING+=("$2")
	fi
}

check_lib() { # <pkg-config模块> <提供的软件包>
	if ! command -v pkg-config >/dev/null 2>&1; then
		if [ "$PROBE_NO_PC" = "0" ]; then
			warn "Missing command: pkg-config (package: pkg-config)"
			PROBE_MISSING+=("pkg-config")
			PROBE_NO_PC=1
		fi
		return
	fi
	if ! pkg-config --exists "$1" 2>/dev/null; then
		warn "Missing dev library: $1 (package: $2)"
		PROBE_MISSING+=("$2")
	fi
}

check_header() { # <头文件路径> <提供的软件包>
	if [ ! -e "$1" ]; then
		warn "Missing header: $1 (package: $2)"
		PROBE_MISSING+=("$2")
	fi
}

probe_report() {
	if [ "${#PROBE_MISSING[@]}" -gt 0 ]; then
		local unique SUDO=""
		unique=$(printf '%s\n' "${PROBE_MISSING[@]}" | sort -u | tr '\n' ' ')
		[ "$(id -u)" = "0" ] || SUDO="sudo"
		warn "-------------------------------------------"
		warn "System dependencies incomplete. Install the following packages and rerun:"
		warn "  $SUDO apt-get update"
		warn "  $SUDO apt-get install -y $unique"
		warn "-------------------------------------------"
		exit 1
	fi
	log "System dependencies OK"
}

# ---- 工具链: 各 ensure_* 幂等,安装到 toolchains/ 并导出所需环境 ----

# ensure_go <版本> — 官方 tar 包,导出 GOPATH/GOPROXY 等
ensure_go() {
	local version="$1"
	export PATH="$TOOLCHAINS/go/bin:$PATH"
	export GOPATH="$TOOLCHAINS/gopath"
	export GOMODCACHE="$GOPATH/pkg/mod"
	export GOPROXY="$GOPROXY_URL"
	export GOTOOLCHAIN=local
	export CGO_ENABLED=1
	[ -x "$TOOLCHAINS/go/bin/go" ] && [ "$("$TOOLCHAINS/go/bin/go" env GOVERSION 2>/dev/null)" = "go$version" ] && return
	log "Downloading Go $version ..."
	rm -rf "$TOOLCHAINS/go"
	local urls=() base
	for base in "${GO_MIRRORS[@]}"; do urls+=("$base/go$version.linux-amd64.tar.gz"); done
	fetch "/tmp/forge-go.tar.gz" "${urls[@]}" || die "Failed to download Go $version"
	tar -C "$TOOLCHAINS" -xzf /tmp/forge-go.tar.gz
	rm -f /tmp/forge-go.tar.gz
	log "Go: $(go version)"
}

# ensure_bun <版本> — zip 包,GitHub Releases (BUN_MIRROR 可换镜像)
ensure_bun() {
	local version="$1"
	export PATH="$TOOLCHAINS/bun/bin:$PATH"
	[ -x "$TOOLCHAINS/bun/bin/bun" ] && [ "$("$TOOLCHAINS/bun/bin/bun" --version 2>/dev/null)" = "$version" ] && return
	log "Downloading Bun $version ..."
	rm -rf "$TOOLCHAINS/bun" /tmp/forge-bun-extract
	fetch "/tmp/forge-bun.zip" "$BUN_MIRROR/bun-v$version/bun-linux-x64.zip" || die "Failed to download Bun"
	unzip -q -o /tmp/forge-bun.zip -d /tmp/forge-bun-extract
	mkdir -p "$TOOLCHAINS/bun/bin"
	mv /tmp/forge-bun-extract/bun-linux-x64/bun "$TOOLCHAINS/bun/bin/bun"
	chmod +x "$TOOLCHAINS/bun/bin/bun"
	rm -rf /tmp/forge-bun-extract /tmp/forge-bun.zip
	log "Bun: $(bun --version)"
}

# ensure_rust — rustup (rsproxy 镜像) + stable minimal,crates 走 rsproxy
ensure_rust() {
	export RUSTUP_HOME="$TOOLCHAINS/rustup"
	export CARGO_HOME="$TOOLCHAINS/cargo"
	export PATH="$CARGO_HOME/bin:$PATH"
	[ -x "$CARGO_HOME/bin/cargo" ] && { log "Cargo: $(cargo --version)"; return; }
	log "Installing Rust toolchain (rsproxy mirror) ..."
	export RUSTUP_DIST_SERVER
	export RUSTUP_UPDATE_ROOT
	fetch "/tmp/forge-rustup-init" \
		"$RUSTUP_UPDATE_ROOT/dist/x86_64-unknown-linux-gnu/rustup-init" \
		"https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init" \
		|| die "Failed to download rustup-init"
	chmod +x /tmp/forge-rustup-init
	/tmp/forge-rustup-init -y --profile minimal --default-toolchain stable --no-modify-path
	rm -f /tmp/forge-rustup-init
	mkdir -p "$CARGO_HOME"
	cat > "$CARGO_HOME/config.toml" <<'EOF'
[source.crates-io]
replace-with = 'rsproxy-sparse'

[source.rsproxy-sparse]
registry = "sparse+https://rsproxy.cn/index/"
EOF
	log "Cargo: $(cargo --version)"
}

# ensure_cmake <最低版本> — 系统版本够用则用之,否则装用户态 Kitware 版
ensure_cmake() {
	local min="$1" have=""
	# 注意: cmake 可能不存在,管道需容忍 127,否则 pipefail 会先于安装杀死脚本
	have=$( { cmake --version 2>/dev/null || true; } | sed -n 's/^cmake version //p' | head -n1)
	if [ -n "$have" ] && [ "$(printf '%s\n' "$min" "$have" | sort -V | head -n1)" = "$min" ]; then
		log "CMake: $have (system)"
		return
	fi
	export PATH="$TOOLCHAINS/cmake/bin:$PATH"
	[ -x "$TOOLCHAINS/cmake/bin/cmake" ] && { log "CMake: $(cmake --version | head -n1 | awk '{print $3}') (user-space)"; return; }
	log "System cmake ${have:-missing} < $min, downloading user-space CMake $CMAKE_VERSION ..."
	rm -rf "$TOOLCHAINS/cmake" /tmp/forge-cmake.tar.gz
	local urls=() u
	for u in "${CMAKE_URLS[@]}"; do
		urls+=("$u/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz")
	done
	fetch "/tmp/forge-cmake.tar.gz" "${urls[@]}" || die "Failed to download CMake"
	mkdir -p "$TOOLCHAINS/cmake"
	tar -C "$TOOLCHAINS/cmake" -xzf /tmp/forge-cmake.tar.gz --strip-components=1
	rm -f /tmp/forge-cmake.tar.gz
	log "CMake: $(cmake --version | head -n1 | awk '{print $3}') (user-space)"
}
