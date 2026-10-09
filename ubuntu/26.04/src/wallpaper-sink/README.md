# wallpaper-sink — GNOME desktop-integration extension (ubuntu/26.04)

Arbitrates the window layer between the linux-wallpaperengine wallpaper
(X11 window, BOTTOM render layer) and GNOME 50 DING desktop icons (Wayland,
DESKTOP render layer) so icons stay visible above the live wallpaper:

    static background < engine (BOTTOM) < DING icons (NORMAL layer bottom) < windows

Mechanism: intercepts `Meta.Window.prototype.set_type` to coerce DING's
DESKTOP requests to NORMAL, and re-asserts engine `lower()` on
window-created / restacked. See extension.js header for details.

## Version anchoring

`shell-version` pins GNOME 50 only. The arbitration relies on measured
layer semantics of mutter 50: X11 desktop windows are hard-assigned to
the BOTTOM layer and DING declares itself DESKTOP via the new
`set_type` API. Re-validate both facts before extending the list.

## Files

- `extension.js` — the whole extension (ESM, GNOME 45+ style)
- `metadata.json` — extension manifest (uuid, shell-version)
- `package.json` — build-only: `{"type":"module"}` so `node --check`
  parses extension.js as ESM; not shipped to the suite

## Validation

`build-extension.sh` runs the syntax and metadata gates and stages a
clean copy into `out/integration/gnome-extension/` for packaging.
