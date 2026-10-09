// wallpaper-sink@lwe-forge v4 — GNOME 50 Wayland desktop-integration arbiter.
//
// Layer semantics (measured on mutter 50, Meta.StackLayer): DESKTOP=0 <
// BOTTOM=1 < NORMAL=2. The engine (X11 desktop window, patch 0001) lands in
// BOTTOM; GNOME 50's DING icons (Wayland) enter the lower DESKTOP layer via
// set_type(DESKTOP) and keep self-lowering — left alone, icons sit forever
// under the wallpaper. X11 windows cannot enter DESKTOP, so the only way out
// is lifting DING into NORMAL: intercept Meta.Window.set_type and rewrite the
// DESKTOP request of DING windows (identified precisely by the customJS_ding
// marker) to NORMAL, giving the stack
//   engine (BOTTOM) < DING (bottom of NORMAL) < regular windows.
// Note: DING's extension replaces global.get_window_actors and filters itself
// out, so arbitration always walks workspace.list_windows() (native mutter
// API, unfiltered).
import Meta from 'gi://Meta';

const ENGINE_WM_CLASS = 'linux-wallpaperengine';

export default class WallpaperSinkExtension {
    enable() {
        if (!Meta.Window.prototype.__lweOrigSetType) {
            Meta.Window.prototype.__lweOrigSetType = Meta.Window.prototype.set_type;
            Meta.Window.prototype.set_type = function (type) {
                if (type === Meta.WindowType.DESKTOP && this.customJS_ding)
                    type = Meta.WindowType.NORMAL; // DING yields: icon layer lifted above the engine
                return this.__lweOrigSetType.call(this, type);
            };
        }
        this._windowCreatedId = global.display.connect('window-created',
            (_display, win) => this._onWindowCreated(win));
        this._restackedId = global.display.connect('restacked', () => this._assert());
        this._assert();
    }

    disable() {
        if (this._windowCreatedId) {
            global.display.disconnect(this._windowCreatedId);
            this._windowCreatedId = 0;
        }
        if (this._restackedId) {
            global.display.disconnect(this._restackedId);
            this._restackedId = 0;
        }
        // Prototype rewrite kept deliberately: DING's own events still request
        // the DESKTOP layer and need continuous arbitration; it dies with the session.
    }

    _isDing(win) {
        return (win.get_wm_class() || '') === 'gjs' && !!win.customJS_ding;
    }

    _onWindowCreated(_display, win) {
        const cls = win.get_wm_class() || '';
        if (cls === ENGINE_WM_CLASS || this._isDing(win)) {
            this._assert();
            return;
        }
        // Xwayland WM_CLASS may arrive after window-created; attach a one-shot listener
        const id = win.connect('notify::wm-class', () => {
            const c = win.get_wm_class() || '';
            if (c === ENGINE_WM_CLASS || this._isDing(win)) {
                win.disconnect(id);
                this._assert();
            }
        });
    }

    _assert() {
        const wins = global.workspace_manager.get_active_workspace().list_windows();
        for (const w of wins) {
            const cls = w.get_wm_class() || '';
            if (cls === ENGINE_WM_CLASS) {
                w.lower(); // engine down to the bottom of BOTTOM
            } else if (this._isDing(w) && w.get_window_type() === Meta.WindowType.DESKTOP) {
                w.set_type(Meta.WindowType.NORMAL); // lands in NORMAL via the rewritten set_type
                w.lower(); // bottom of NORMAL: above the engine, below all other windows
            }
        }
    }
}
