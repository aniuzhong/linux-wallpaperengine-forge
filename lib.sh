#!/usr/bin/env bash

GUI_REF="${GUI_REF:-8855fad673932991dde0241530941a520b0e34a2}"
ENGINE_REF="${ENGINE_REF:-b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d}"
BUN_VERSION="${BUN_VERSION:-1.4.2}"
CMAKE_VERSION="${CMAKE_VERSION:-4.4.3}"

FORGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRD_PARTY="$FORGE_DIR/third_party"
TOOLCHAINS="$FORGE_DIR/toolchains"
OUTPUT="$FORGE_DIR/out"

log() { printf '\033[32m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*"; }
warn() { printf '\033[33m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*" >&2; }
die() { printf '\033[31m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*" >&2; exit 1; }

forge_arch() {
	case "$(uname -m)" in
		x86_64) printf 'amd64\n' ;;
		aarch64) printf 'arm64\n' ;;
		*) uname -m ;;
	esac
}

forge_fingerprint() {
	local kv id ver
	kv="$(. /etc/os-release 2>/dev/null && printf '%s %s' "${ID:-}" "${VERSION_ID:-${BUILD_ID:-}}")" || return 0
	id=${kv%% *}
	ver=${kv#* }
	[ -n "$id" ] && [ -n "$ver" ] && [ "$id" != "$kv" ] || return 0
	printf '%s/%s\n' "$id" "$ver"
}

forge_list_slices() {
	local d
	for d in "$FORGE_DIR"/*/*/; do
		[ -f "${d}target.sh" ] && printf '%s\n' "${d#"$FORGE_DIR"/}" | sed 's:/$::'
	done
	return 0
}

forge_check_guards() {
	local os_pretty os_idlike
	os_pretty="$(. /etc/os-release 2>/dev/null && printf '%s %s' "${PRETTY_NAME:-}" "${VERSION_US:-}")" || os_pretty=""
	os_idlike="$(. /etc/os-release 2>/dev/null && printf '%s' "${ID_LIKE:-}")" || os_idlike=""
	if [ -n "${GUARD_PRETTY:-}" ] && [[ "$os_pretty" != *"$GUARD_PRETTY"* ]]; then
		die "guard failed: PRETTY_NAME/VERSION_US '$os_pretty' does not contain '$GUARD_PRETTY'"
	fi
	if [ -n "${GUARD_ID_LIKE:-}" ] && [[ "$os_idlike" != *"$GUARD_ID_LIKE"* ]]; then
		die "guard failed: ID_LIKE '$os_idlike' does not contain '$GUARD_ID_LIKE'"
	fi
}

forge_resolve() {
	local requested="${FORGE_TARGET:-}" fp dir
	fp="$(forge_fingerprint)" || fp=""
	if [ -n "$requested" ]; then
		dir="$FORGE_DIR/$requested"
		[ -f "$dir/target.sh" ] || die "unknown target '$requested' (available: $(forge_list_slices | tr '\n' ' '))"
		if [ -n "$fp" ] && [ "$requested" != "$fp" ]; then
			warn "host detected as '$fp', building explicitly requested '$requested'"
		fi
	else
		[ -n "$fp" ] || die "cannot detect host distribution; available: $(forge_list_slices | tr '\n' ' ')"
		dir="$FORGE_DIR/$fp"
		[ -f "$dir/target.sh" ] || die "no slice for host fingerprint '$fp' ($(forge_arch)); available: $(forge_list_slices | tr '\n' ' ')"
	fi
	printf '%s\n' "$dir"
}

TARGET_DIR="$(forge_resolve)"
source "$TARGET_DIR/target.sh"
forge_check_guards

GO_MIRRORS=(
	"https://mirrors.aliyun.com/golang"
	"https://golang.google.cn/dl"
)
BUN_MIRROR="${BUN_MIRROR:-https://github.com/oven-sh/bun/releases/download}"
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

init_log() {
	mkdir -p "$OUTPUT"
	exec > >(tee -a "$OUTPUT/build-$1.log") 2>&1
	LOG_TAG="$1"
}

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

# Content scan used for patch-state detection. Prunes everything the
# reset/reapply flow cannot touch (build outputs, dependency trees): hashing
# build/ — including the 1.4GB CEF dist — twice per run was the bulk of the
# old per-run overhead.
tree_md5_scan () {
	( cd "$1" && find . -name .git -prune -o -path ./build -prune -o -path ./node_modules -prune \
		-o -path ./dist -prune -o -path ./.forge-patch-state -prune -o -type f -print0 2>/dev/null \
		| sort -z | xargs -0 -r md5sum 2>/dev/null ) || true
}

# Restores mtimes of files whose content is identical to a pre-build snapshot,
# so git checkout + patch re-application does not trigger mass recompilation.
# The before/after join runs in a single awk pass; only the touch calls are
# per-file.
preserve_mtimes_for_unchanged () {
	local repo=$1 hash_before=$2 mt_before=$3 hash_after=$4
	local ts file restored=0
	while IFS=$'\t' read -r ts file; do
		[ -n "$file" ] || continue
		touch -d "@$ts" "$repo/$file" && restored=$((restored + 1))
	done < <(awk -v fb="$hash_before" -v fm="$mt_before" '
		FILENAME == fb {
			p = index($0, " "); path = substr($0, p + 1); sub(/^ +/, "", path)
			before[path] = substr($0, 1, p - 1); next
		}
		FILENAME == fm {
			p = index($0, " "); mt[substr($0, p + 1)] = substr($0, 1, p - 1); next
		}
		{
			p = index($0, " "); path = substr($0, p + 1); sub(/^ +/, "", path)
			if (before[path] == substr($0, 1, p - 1) && (path in mt))
				printf "%s\t%s\n", mt[path], path
		}
	' "$hash_before" "$mt_before" "$hash_after")
	log "Preserved mtimes for $restored unchanged files"
}

apply_patch_list() {
	local repo_src=$1 list=$2 pool=$3
	local dir patch id

	local -a all_patches=()
	while IFS= read -r id; do
		[ -n "$id" ] || continue
		all_patches+=("$pool/$id.patch")
	done < "$list"
	if [ ${#all_patches[@]} -eq 0 ]; then
		log "Patch list is empty: $list"
		return 0
	fi

	# Fast path: identical patch set and byte-identical tree to the last
	# successful apply means the tree is already in the patched state — skip
	# the reset/reapply/restore cycle entirely. State lives untracked in the
	# repo, so manual edits, upstream ref changes or edited patches all miss
	# it and fall through to the slow path.
	local state_dir="$repo_src/.forge-patch-state"
	local state_patches="$state_dir/patches.md5" state_tree="$state_dir/tree.md5"
	local patch_fp
	patch_fp=$(cat "${all_patches[@]}" "$list" 2>/dev/null | md5sum 2>/dev/null | awk '{print $1}' || true)
	if [ -n "$patch_fp" ] && [ "$(cat "$state_patches" 2>/dev/null || true)" = "$patch_fp" ] && [ -f "$state_tree" ]; then
		if tree_md5_scan "$repo_src" | cmp -s - "$state_tree"; then
			log "Patch state intact, skipping reset/reapply"
			return 0
		fi
	fi

	local hash_before mt_before hash_after
	hash_before=$(mktemp)
	hash_after=$(mktemp)
	mt_before=$(mktemp)
	tree_md5_scan "$repo_src" > "$hash_before"
	( cd "$repo_src" && find . -name .git -prune -o -path ./build -prune -o -path ./node_modules -prune \
		-o -path ./dist -prune -o -path ./.forge-patch-state -prune -o -type f -printf '%T@ %p\n' 2>/dev/null | sort -k2 ) > "$mt_before"

	git -C "$repo_src" checkout -- . 2>/dev/null || true
	local newfiles=""
	newfiles=$(awk '/^--- \/dev\/null$/{nl=1; next} nl==1 && /^\+\+\+ b\//{sub(/^\+\+\+ b\//, ""); print; nl=0; next} {nl=0}' "${all_patches[@]}" | sort -u)
	for f in $newfiles; do
		rm -f "$repo_src/$f"
	done
	for patch in "${all_patches[@]}"; do
		if (cd "$repo_src" && git apply --check "$patch" 2>/dev/null); then
			(cd "$repo_src" && git apply "$patch")
			log "Applying patch: $(basename "$patch" .patch)"
		else
			die "Cannot apply patch (upstream may have moved): $patch"
		fi
	done

	tree_md5_scan "$repo_src" > "$hash_after"
	preserve_mtimes_for_unchanged "$repo_src" "$hash_before" "$mt_before" "$hash_after"

	mkdir -p "$state_dir"
	printf '%s\n' "$patch_fp" > "$state_patches"
	cp "$hash_after" "$state_tree"
	rm -f "$mt_before" "$hash_before" "$hash_after"
}

PROBE_MISSING=()
PROBE_NO_PC=0

probe_reset() {
	PROBE_MISSING=()
	PROBE_NO_PC=0
}

check_cmd() {
	local c oldIFS=$IFS
	IFS=:
	for c in $1; do
		if command -v "$c" >/dev/null 2>&1; then
			IFS=$oldIFS
			return 0
		fi
	done
	IFS=$oldIFS
	warn "Missing command: $1 (package: $2)"
	PROBE_MISSING+=("$2")
}

check_lib() {
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

check_header() {
	local p oldIFS=$IFS
	IFS=:
	for p in $1; do
		if [ -e "$p" ]; then
			IFS=$oldIFS
			return 0
		fi
	done
	IFS=$oldIFS
	warn "Missing header: $1 (package: $2)"
	PROBE_MISSING+=("$2")
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

ensure_rust() {
	export RUSTUP_HOME="$TOOLCHAINS/rustup"
	export CARGO_HOME="$TOOLCHAINS/cargo"
	export PATH="$CARGO_HOME/bin:$PATH"
	[ -x "$CARGO_HOME/bin/cargo" ] && { log "Cargo: $(cargo --version)"; return; }
	log "Installing Rust toolchain (rsproxy mirror) ..."
	export RUSTUP_DIST_SERVER
	export RUSTUP_UPDATE_ROOT
	fetch "/tmp/rustup-init" \
		"$RUSTUP_UPDATE_ROOT/dist/x86_64-unknown-linux-gnu/rustup-init" \
		"https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init" \
		|| die "Failed to download rustup-init"
	chmod +x /tmp/rustup-init
	/tmp/rustup-init -y --profile minimal --default-toolchain stable --no-modify-path
	rm -f /tmp/rustup-init
	mkdir -p "$CARGO_HOME"
	cat > "$CARGO_HOME/config.toml" <<'EOF'
[source.crates-io]
replace-with = 'rsproxy-sparse'

[source.rsproxy-sparse]
registry = "sparse+https://rsproxy.cn/index/"
EOF
	log "Cargo: $(cargo --version)"
}

ensure_cmake() {
	local min="$1" have=""
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

ensure_dep_shims() { :; }

if [ -f "$TARGET_DIR/env.sh" ]; then
	. "$TARGET_DIR/env.sh"
fi
