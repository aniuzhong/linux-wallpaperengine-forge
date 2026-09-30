#!/usr/bin/env bash
#
# Sourced by build-*.sh, never executed directly.
#
# Layout (by ownership):
#   src/           first-party sources (wallpaper-sink GNOME extension)
#   third_party/   upstream clones (pinned; do not edit in place — ensure_repo
#                  runs checkout -f and wipes local changes)
#   patches/       diffs against the third_party clones (see patches/engine/README.md)
#   toolchains/    large-file cache (CEF)
#   out/           artifacts and logs (build-<target>.log)

# ---- Version pins (overridable via environment) ----
ENGINE_REF="${ENGINE_REF:-b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d}"
# ENGINE_REF=latest — track upstream main tip (resolved to a concrete commit so
# builds stay reproducible; patch failures fail loudly in apply_patches)
if [ "$ENGINE_REF" = "latest" ]; then
	ENGINE_REF=$(git ls-remote https://github.com/Almamu/linux-wallpaperengine.git HEAD | cut -f1)
fi

FORGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$FORGE_DIR/src"
THIRD_PARTY="$FORGE_DIR/third_party"
TOOLCHAINS="$FORGE_DIR/toolchains"
OUTPUT="$FORGE_DIR/out"

log() { printf '\033[32m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*"; }
warn() { printf '\033[33m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*" >&2; }
die() { printf '\033[31m[%s]\033[0m %s\n' "${LOG_TAG:-build}" "$*" >&2; exit 1; }

# Called once per entry point: tees output to out/build-<target>.log and the terminal
init_log() {
	mkdir -p "$OUTPUT"
	exec > >(tee -a "$OUTPUT/build-$1.log") 2>&1
	LOG_TAG="$1"
}

# ensure_repo <url> <dir> <pinned-ref> — clone if missing, align to ref (switches when it moves)
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

# apply_patches <repo> <patch-dir>
# Reapplies all patches from a clean baseline (git HEAD) on every run: applying
# is transient, the end state deterministic, with no reliance on traces of a
# previous run — skip-if-applied detection breaks when patch contexts overlap.
apply_patches() {
	local repo="$1" dir="$2" patch name f
	git -C "$repo" checkout -- . 2>/dev/null || true
	# Remove files a patch would create (leftovers from the last run), else
	# git apply fails on "already exists"
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
	# Unset nullglob before returning: this file is sourced, a leak would change
	# all later globbing (empty matches expand to nothing, not the literal pattern)
	shopt -u nullglob
}

# ---- Dependency probe framework: entries declare checks, probe_report reports ----
PROBE_MISSING=()

probe_reset() {
	PROBE_MISSING=()
}

check_cmd() { # <command> <providing package>
	if ! command -v "$1" >/dev/null 2>&1; then
		warn "Missing command: $1 (package: $2)"
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
