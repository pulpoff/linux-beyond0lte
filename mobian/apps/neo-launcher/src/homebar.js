// The home bar gesture, Neo's own: a 15 px strip on the bottom screen edge, over Phosh's home bar (OVERLAY layer,
// ignoring exclusive zones), so Phosh never starts its drag. Phosh's drag progress came with a jump to 1.0 80 ms
// into a slow swipe and "home" a dozen times per release, so nothing could be told from it; here the finger's own
// travel, rest and speed decide (One UI):
//   swipe up and let go          → home (nothing of the task view shows)
//   swipe up and rest            → the task view
// While the screen is locked the strip steps aside: the lock screen's own "slide up to unlock" lives there.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';
import Gtk4LayerShell from 'gi://Gtk4LayerShell?version=1.0';

const STRIP = 15;            // logical px: Phosh's PHOSH_HOME_BAR_HEIGHT
const ENGAGE = 8;            // px up before the swipe starts
const HOLD_MS = 220, HOLD_SLOP = 10;   // resting this long within this many px: the task view
const HOLD_MIN = 40;         // px up a hold needs

export class HomeBar {
    /** recents: Recents (begin/move/hold/release); onHome(): Neo's home screen. */
    constructor(app, recents, onHome) {
        this._app = app;
        this._recents = recents;
        this._onHome = onHome;
        this._win = this._strip();
        Gio.DBus.session.signal_subscribe('org.gnome.ScreenSaver', 'org.gnome.ScreenSaver', 'ActiveChanged',
            '/org/gnome/ScreenSaver', null, Gio.DBusSignalFlags.NONE, (_c, _s, _p, _i, _n, params) =>
                this._win.set_visible(!params.deepUnpack()[0]));
        Gio.DBus.session.call('org.gnome.ScreenSaver', '/org/gnome/ScreenSaver', 'org.gnome.ScreenSaver', 'GetActive',
            null, null, Gio.DBusCallFlags.NO_AUTO_START, 1000, null, (c, res) => {
                try { this._win.set_visible(!c.call_finish(res).deepUnpack()[0]); } catch (_) { /* no screensaver: stay */ }
            });
    }

    _strip() {
        const win = new Gtk.Window({application: this._app, decorated: false, default_height: STRIP, css_classes: ['neo-edge']});
        Gtk4LayerShell.init_for_window(win);
        Gtk4LayerShell.set_namespace(win, 'neo-home-bar');
        Gtk4LayerShell.set_layer(win, Gtk4LayerShell.Layer.OVERLAY);
        for (const e of [Gtk4LayerShell.Edge.BOTTOM, Gtk4LayerShell.Edge.LEFT, Gtk4LayerShell.Edge.RIGHT])
            Gtk4LayerShell.set_anchor(win, e, true);
        Gtk4LayerShell.set_exclusive_zone(win, -1);   // on the screen edge itself, over Phosh's bar
        Gtk4LayerShell.set_keyboard_mode(win, Gtk4LayerShell.KeyboardMode.NONE);
        // drawn (1 % alpha) or never mapped; sized, or GTK makes it 200 px (see edges.js)
        const area = new Gtk.Box({hexpand: true, vexpand: true, height_request: STRIP, css_classes: ['neo-edge-area']});
        win.set_child(area);
        win.set_size_request(-1, STRIP);

        const drag = new Gtk.GestureDrag();
        let s = null;
        drag.connect('drag-begin', () => {
            s = {on: false, anchor: 0, last: {t: GLib.get_monotonic_time(), y: 0}, vy: 0, holdId: 0, held: false};
        });
        drag.connect('drag-update', (_g, _dx, dy) => {
            if (!s) return;
            const up = -dy;
            const now = GLib.get_monotonic_time(), dt = (now - s.last.t) / 1000;
            if (dt > 0) { s.vy = 0.6 * ((up - s.last.y) / dt) + 0.4 * s.vy; s.last = {t: now, y: up}; }
            if (!s.on) {
                if (up < ENGAGE) return;
                s.on = true;
                this._recents.begin();
            }
            this._recents.move(up);
            if (!s.held && Math.abs(up - s.anchor) > HOLD_SLOP) {
                s.anchor = up;
                if (s.holdId) GLib.source_remove(s.holdId);
                const st = s;
                st.holdId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, HOLD_MS, () => {
                    st.holdId = 0;
                    if (st.anchor < HOLD_MIN) return GLib.SOURCE_REMOVE;   // resting on the bar itself is no swipe
                    st.held = true; this._recents.hold();
                    return GLib.SOURCE_REMOVE;
                });
            }
        });
        drag.connect('drag-end', (_g, _dx, dy) => {
            const st = s; s = null;
            if (!st) return;
            if (st.holdId) GLib.source_remove(st.holdId);
            if (!st.on) return;
            const recents = st.held;
            if (recents) this._recents.release(true);
            else { this._recents.release(false); this._onHome(); }
        });
        area.add_controller(drag);
        win.present();
        let checks = 0;
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, 60, () => {
            const h = win.get_height();
            if (h > 0) Gtk4LayerShell.set_margin(win, Gtk4LayerShell.Edge.BOTTOM, Math.min(0, STRIP - h));
            return ++checks < 12 ? GLib.SOURCE_CONTINUE : GLib.SOURCE_REMOVE;
        });
        return win;
    }
}
