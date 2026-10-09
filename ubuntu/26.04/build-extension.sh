#!/usr/bin/env bash
#
# build-extension.sh — validate and stage the wallpaper-sink GNOME extension
#
# Source: src/wallpaper-sink/ (first-party extension, GNOME 45+ ESM style)
# Output: out/integration/gnome-extension/wallpaper-sink@lwe-forge/
#         (clean copy consumed by package.sh; not shipped files like
#         package.json/README.md stay behind)
# Log:    out/build-extension.log
#
# Gates:  node --check catches ESM syntax errors at build time — the native
#         feedback loop for this code is a logout+login, so a cheap static
#         gate is the highest-value check in the repo. Metadata validation
#         enforces the uuid and the GNOME 50 shell-version anchor (see
#         src/wallpaper-sink/README.md for why 50 is a measured boundary,
#         not conservatism).
#
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
source "../../lib.sh"

init_log extension
EXT_SRC="$SRC_DIR/wallpaper-sink"
EXT_UUID="wallpaper-sink@lwe-forge"
STAGE="$OUTPUT/integration/gnome-extension/$EXT_UUID"

# ---- 1. System dependency probe ----
log "Probing system dependencies..."
probe_reset
check_cmd node nodejs
probe_report

# ---- 2. ESM syntax gate ----
log "Syntax check (node --check) ..."
node --check "$EXT_SRC/extension.js"

# ---- 3. Metadata validation ----
log "Validating metadata.json ..."
node - "$EXT_SRC/metadata.json" <<'EOF'
const fs = require("fs");
const m = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
if (m.uuid !== "wallpaper-sink@lwe-forge") throw new Error("uuid mismatch");
if (!(m["shell-version"] || []).includes("50")) throw new Error("shell-version must include 50");
if (!m.name || !m.description) throw new Error("name/description required");
console.log("metadata OK:", m.uuid, "shell-version", m["shell-version"]);
EOF

# ---- 4. Stage clean copy ----
log "Staging to $STAGE ..."
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$EXT_SRC/metadata.json" "$EXT_SRC/extension.js" "$STAGE/"

log "=========================================="
log "Extension build complete: $STAGE"
log "=========================================="
