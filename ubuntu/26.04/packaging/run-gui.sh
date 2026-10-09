#!/usr/bin/env bash
# Suite launcher:
#   1. Layout check (GUI / engine / bin wrapper present)
#   2. Install + activate the wallpaper-sink extension (GNOME 50 Wayland
#      icon arbitration; a one-time logout+login is needed after first
#      install, the script prints a clear notice when so)
#   3. Single-instance guard (the backend binds a fixed /tmp socket;
#      multiple instances would lose track of each other)
#   4. Launch the GUI with bin/ prepended to PATH (the GUI resolves the
#      engine by name and gets the wrapper, which fixes the environment
#      on Wayland sessions)
set -euo pipefail
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

die() { echo "run-gui: $*" >&2; exit 1; }

# ---- 1. Layout check ----
[ -x "$DIR/gui/linux-wallpaperengine-gui" ] || die "Suite incomplete: missing gui/linux-wallpaperengine-gui"
[ -x "$DIR/engine/linux-wallpaperengine" ] || die "Suite incomplete: missing engine/linux-wallpaperengine"
[ -x "$DIR/bin/linux-wallpaperengine" ] || die "Suite incomplete: missing bin/linux-wallpaperengine"

# ---- 2. wallpaper-sink extension ----
EXT_UUID="wallpaper-sink@lwe-forge"
EXT_SRC="$DIR/gnome-extension/$EXT_UUID"
EXT_DST="$HOME/.local/share/gnome-shell/extensions/$EXT_UUID"
if [ -d "$EXT_SRC" ]; then
    if [ ! -f "$EXT_DST/metadata.json" ]; then
        mkdir -p "$(dirname "$EXT_DST")"
        cp -a "$EXT_SRC" "$EXT_DST"
        echo "run-gui: installed $EXT_UUID (GNOME desktop-integration extension)"
    fi
    if command -v gnome-extensions >/dev/null 2>&1; then
        STATE=$(gnome-extensions info "$EXT_UUID" 2>/dev/null | sed -n 's/^ *State: //p' || true)
        if [ "$STATE" != "ACTIVE" ]; then
            gnome-extensions enable "$EXT_UUID" 2>/dev/null || true
            STATE=$(gnome-extensions info "$EXT_UUID" 2>/dev/null | sed -n 's/^ *State: //p' || true)
        fi
        if [ "$STATE" != "ACTIVE" ]; then
            echo "run-gui: WARNING: desktop icon integration needs one logout+login to take effect (extension installed)."
            echo "run-gui:   The wallpaper still plays this session, but desktop icons will stay covered by it."
        fi
    fi
fi

# ---- 3. Single-instance guard ----
if pgrep -f "$DIR/gui/linux-wallpaperengine-gui" >/dev/null 2>&1 ||
   pgrep -f "$DIR/gui/resources/linux-wallpaperengine-gui" >/dev/null 2>&1; then
    die "This suite already has a running instance (window may be in the tray). Exit it first:
  pkill -f \"$DIR\""
fi

# ---- 3.5 chrome-sandbox self-heal ----
# Electron's SUID sandbox requires gui/chrome-sandbox to be root:root 4755
# and the filesystem must not be mounted nosuid (extracting to /tmp hits
# this). Falls back to --no-sandbox + ELECTRON_DISABLE_SANDBOX when
# unfixable, to stay usable.
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

# ---- 3.6 Path pre-seeding: snap Steam ----
# The GUI backend's Steam auto-detection list does not include the snap
# layout. When detected, pre-write the two paths into the GUI config so
# users don't have to fill them manually (existing values are kept).
SNAP_STEAM="$HOME/snap/steam/common/.local/share/Steam"
if [ -d "$SNAP_STEAM/steamapps/common/wallpaper_engine/assets" ]; then
    CFG_DIR="$HOME/.config/linux-wallpaperengine-gui"
    CFG="$CFG_DIR/config.json"
    mkdir -p "$CFG_DIR"
    if command -v python3 >/dev/null 2>&1; then
        WE_DIR="$SNAP_STEAM/steamapps/common/wallpaper_engine" \
        WS_DIR="$SNAP_STEAM/steamapps/workshop/content/431960" \
        CFG_FILE="$CFG" \
        python3 - <<'PYEOF' || true
import json, os
p = os.environ["CFG_FILE"]
try:
    conf = json.load(open(p)) if os.path.exists(p) else {}
except Exception:
    conf = {}
changed = False
for key, val in (("wallpaperEngineDir", os.environ["WE_DIR"]), ("workshopDir", os.environ["WS_DIR"])):
    if not conf.get(key):
        conf[key] = val
        changed = True
if changed:
    json.dump(conf, open(p, "w"), indent=2)
    print("run-gui: pre-seeded snap Steam assets and workshop paths into", p)
PYEOF
    fi
fi

# ---- 3.7 One-time scaling migration: default -> fill ----
# Upstream GUI ships scaling=default: when the wallpaper aspect ratio differs
# from the screen (16:9 wallpapers on 16:10 panels being the common case),
# DefaultUVs samples ~5.6% beyond the texture edges; combined with
# clamping=clamp (GL_CLAMP_TO_EDGE) the screen shows ~80px edge-stretch smear
# bands top and bottom, on any wallpaper. fill (cover) keeps UVs inside
# [0,1] for every aspect combination. Migrate once and stamp afterwards so
# any later user choice in the GUI (including back to default) is respected.
CFG_DIR="$HOME/.config/linux-wallpaperengine-gui"
CFG="$CFG_DIR/config.json"
STAMP="$CFG_DIR/.lwe-forge-scaling-migrated"
if command -v python3 >/dev/null 2>&1 && [ ! -e "$STAMP" ]; then
    mkdir -p "$CFG_DIR"
    CFG_FILE="$CFG" STAMP_FILE="$STAMP" python3 - <<'PYEOF' || true
import json, os
cfg, stamp = os.environ["CFG_FILE"], os.environ["STAMP_FILE"]
conf = None
if os.path.exists(cfg):
    try:
        conf = json.load(open(cfg))
    except Exception:
        pass  # corrupt config: leave it to the GUI's own repair flow
if conf is None and not os.path.exists(cfg):
    conf = {}
if isinstance(conf, dict):
    action = ""
    if not conf.get("scaling"):
        conf["scaling"] = "fill"
        action = "pre-seeded" if os.path.exists(cfg) else "created"
    elif conf["scaling"] == "default":
        conf["scaling"] = "fill"
        action = "migrated"
    if action:
        json.dump(conf, open(cfg, "w"), indent=2)
        print("run-gui: %s scaling=fill in %s" % (action, cfg))
        print("run-gui:   (upstream default smears on aspect-mismatched screens;"
              " switch back in GUI settings if ever needed)")
    open(stamp, "w").close()
PYEOF
fi

exec env PATH="$DIR/bin:$PATH" "$DIR/gui/linux-wallpaperengine-gui" ${NO_SANDBOX[@]+"${NO_SANDBOX[@]}"} "$@"
