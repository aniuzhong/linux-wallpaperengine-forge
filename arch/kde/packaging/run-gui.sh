#!/usr/bin/env bash
# Suite launcher (arch/kde):
#   1. Layout check (GUI / engine / bin wrapper present)
#   2. Single-instance guard (the backend binds a fixed /tmp socket;
#      multiple instances would lose track of each other)
#   3. chrome-sandbox self-heal (Electron's SUID sandbox requires
#      gui/chrome-sandbox to be root:root 4755; tar extraction as a
#      regular user loses that — falls back to --no-sandbox +
#      ELECTRON_DISABLE_SANDBOX when unfixable, to stay usable)
#   4. Launch the GUI with bin/ prepended to PATH (the GUI resolves the
#      engine by name and gets the wrapper, which fixes the environment
#      on Wayland sessions)
#
# No platform-integration stub is needed on KDE: desktop icon stacking is
# carried by KWin layer ordering (see run-gui layer notes in the suite README).
set -euo pipefail
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

die() { echo "run-gui: $*" >&2; exit 1; }

# ---- 1. Layout check ----
[ -x "$DIR/gui/linux-wallpaperengine-gui" ] || die "Suite incomplete: missing gui/linux-wallpaperengine-gui"
[ -x "$DIR/engine/linux-wallpaperengine" ] || die "Suite incomplete: missing engine/linux-wallpaperengine"
[ -x "$DIR/bin/linux-wallpaperengine" ] || die "Suite incomplete: missing bin/linux-wallpaperengine"

# ---- 1.5 Runtime library self-check ----
# The engine links system GL/mpv libraries and the backend links the tray
# indicator library; a missing one makes the backend retry-spawn the engine
# forever with only a black desktop as symptom. Fail loudly instead.
if command -v ldd >/dev/null 2>&1; then
	MISSING="$(ldd "$DIR/engine/linux-wallpaperengine" 2>/dev/null | awk '/not found/{print $1}'
	 ldd "$DIR/gui/resources/linux-wallpaperengine-gui" 2>/dev/null | awk '/not found/{print $1}' | sort -u)"
	if [ -n "$MISSING" ]; then
		die "Runtime libraries missing:
  $MISSING
  (Arch: sudo pacman -S glew glfw mpv libayatana-appindicator)"
	fi
fi

# ---- 2. Single-instance guard ----
if pgrep -f "$DIR/gui/linux-wallpaperengine-gui" >/dev/null 2>&1 ||
   pgrep -f "$DIR/gui/resources/linux-wallpaperengine-gui" >/dev/null 2>&1; then
    die "This suite already has a running instance (window may be in the tray). Exit it first:
  pkill -f \"$DIR\""
fi

# ---- 3. chrome-sandbox self-heal ----
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
# --no-sandbox on argv only protects the first Electron (the launcher): the
# real GUI window is a SECOND Electron spawn by the Go backend, whose command
# line is hardcoded upstream ("--ozone-platform=x11") and drops the flag, and
# the in-app appendSwitch("--no-sandbox") is a no-op (Electron switch names
# must not carry the "--" prefix). ELECTRON_DISABLE_SANDBOX is honored before
# Chromium parses argv and inherits along launcher -> backend -> Electron.
if [ "${#NO_SANDBOX[@]}" -gt 0 ]; then
	export ELECTRON_DISABLE_SANDBOX=1
fi

# ---- 4. Launch ----
exec env PATH="$DIR/bin:$PATH" "$DIR/gui/linux-wallpaperengine-gui" ${NO_SANDBOX[@]+"${NO_SANDBOX[@]}"} "$@"
