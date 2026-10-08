#!/usr/bin/env bash

set -euo pipefail
FORGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -gt 0 ] && [ -f "$FORGE_DIR/$1/target.sh" ]; then
	export FORGE_TARGET="$1"
	shift
fi

source "$FORGE_DIR/lib.sh"

step="${1:-all}"
if [ $# -gt 0 ]; then
	shift
fi

case "$step" in
	engine)
		bash "$FORGE_DIR/build-engine.sh"
		;;
	gui)
		bash "$FORGE_DIR/build-gui.sh"
		;;
	integration)
		if [ -z "${INTEGRATION_BUILD:-}" ]; then
			die "target has no INTEGRATION_BUILD declared"
		fi
		bash "$INTEGRATION_BUILD"
		;;
	package)
		bash "$FORGE_DIR/package.sh"
		;;
	all)
		bash "$FORGE_DIR/build-engine.sh"
		bash "$FORGE_DIR/build-gui.sh"
		if [ -n "${INTEGRATION_BUILD:-}" ]; then
			bash "$INTEGRATION_BUILD"
		else
			log "no INTEGRATION_BUILD declared, skipping"
		fi
		bash "$FORGE_DIR/package.sh"
		;;
	*)
		die "unknown step '$step' (engine|gui|integration|package|all)"
		;;
esac
