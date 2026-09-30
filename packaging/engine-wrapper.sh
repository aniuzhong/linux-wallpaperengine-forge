#!/usr/bin/env bash
# Engine entry wrapper: forces the engine to run as an Xwayland client on
# GNOME Wayland sessions. mutter does not implement wlr-layer-shell, so the
# engine's built-in Wayland driver always fails ("Failed to bind to required
# interfaces"); the desktop-layer window path is X11 (patch 0002), and the
# window-layer arbitration is done by the wallpaper-sink extension.
# The GUI resolves the engine by name and lands here; the environment is
# fixed at this boundary.
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REAL="$DIR/../engine/linux-wallpaperengine"
if [ -n "${WAYLAND_DISPLAY:-}" ] && [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
    exec env XDG_SESSION_TYPE=x11 DISPLAY="${DISPLAY:-:0}" WAYLAND_DISPLAY=no-such-socket \
        "$REAL" "$@"
fi
exec "$REAL" "$@"
