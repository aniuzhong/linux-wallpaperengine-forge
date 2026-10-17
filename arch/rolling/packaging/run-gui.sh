#!/usr/bin/env bash

set -euo pipefail
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

die() { echo "run-gui: $*" >&2; exit 1; }

[ -x "$DIR/gui/linux-wallpaperengine-gui" ] || die "Suite incomplete: missing gui/linux-wallpaperengine-gui"
[ -x "$DIR/engine/linux-wallpaperengine" ] || die "Suite incomplete: missing engine/linux-wallpaperengine"
[ -x "$DIR/bin/linux-wallpaperengine" ] || die "Suite incomplete: missing bin/linux-wallpaperengine"

if command -v ldd >/dev/null 2>&1; then
	MISSING="$(ldd "$DIR/engine/linux-wallpaperengine" 2>/dev/null | awk '/not found/{print $1}'
	 ldd "$DIR/gui/resources/linux-wallpaperengine-gui" 2>/dev/null | awk '/not found/{print $1}' | sort -u)"
	if [ -n "$MISSING" ]; then
		die "Runtime libraries missing:
  $MISSING
  (Arch: sudo pacman -S glew glfw mpv libayatana-appindicator)"
	fi
fi

if pgrep -f "$DIR/gui/linux-wallpaperengine-gui" >/dev/null 2>&1 ||
   pgrep -f "$DIR/gui/resources/linux-wallpaperengine-gui" >/dev/null 2>&1; then
    die "This suite already has a running instance (window may be in the tray). Exit it first:
  pkill -f \"$DIR\""
fi

SANDBOX="$DIR/gui/chrome-sandbox"
NO_SANDBOX=()
if [ -f "$SANDBOX" ]; then
	if ! { [ -u "$SANDBOX" ] && [ "$(stat -c %U "$SANDBOX")" = "root" ]; }; then
		if command -v sudo >/dev/null 2>&1; then
			echo "run-gui: asking sudo to repair gui/chrome-sandbox (one-time; this is what keeps the Electron window alive)" >&2
			sudo chown root:root "$SANDBOX" 2>/dev/null || true
			sudo chmod 4755 "$SANDBOX" 2>/dev/null || true
		fi
	fi
	if ! { [ -u "$SANDBOX" ] && [ "$(stat -c %U "$SANDBOX")" = "root" ]; }; then
		echo "run-gui: WARNING: cannot fix chrome-sandbox permissions, falling back to --no-sandbox" >&2
		NO_SANDBOX=(--no-sandbox)
	elif findmnt -no OPTIONS --target "$SANDBOX" 2>/dev/null | grep -qw nosuid; then
		echo "run-gui: WARNING: mount point is nosuid (e.g. /tmp), sandbox unavailable, falling back to --no-sandbox; extract the suite under your home or /opt instead" >&2
		NO_SANDBOX=(--no-sandbox)
	fi
fi

if [ "${#NO_SANDBOX[@]}" -gt 0 ]; then
	export ELECTRON_DISABLE_SANDBOX=1
fi

exec env PATH="$DIR/bin:$PATH" "$DIR/gui/linux-wallpaperengine-gui" ${NO_SANDBOX[@]+"${NO_SANDBOX[@]}"} "$@"
