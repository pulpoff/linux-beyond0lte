// Back gesture (Android gesture navigation): a swipe in from the left or right screen edge. Two narrow, invisible
// layer-shell strips sit on the edges between Phosh's top bar and its home bar and catch the drag; a pull of ARM px
// inwards counts. What "back" does:
//   Neo shows    → close what is open on it (search text, drawer), as the old Neo did
//   an app shows → Alt+Left through a virtual keyboard (wtype): Adw.NavigationView, Gtk.Stack pages and browsers
//                  all go one page back on it
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';
import Gtk4LayerShell from 'gi://Gtk4LayerShell?version=1.0';

const STRIP = 14;          // logical px; a touch has to begin in this strip
const ARM = 48;            // inward travel that makes it a back gesture
const CLEAR_TOP = 48, CLEAR_BOTTOM = 40;   // keep off Phosh's top bar and home bar
const PILL = 48;           // the old Neo's back pill: a 44 px dark circle, 20 px arrow

export class BackGesture {
    constructor(app, onBack) {
        this._app = app;
        this._onBack = onBack;
        this._strips = ['left', 'right'].map(side => this._strip(side));
    }

    _strip(side) {
        const win = new Gtk.Window({application: this._app, decorated: false, default_width: STRIP, css_classes: ['neo-edge']});
        Gtk4LayerShell.init_for_window(win);
        Gtk4LayerShell.set_namespace(win, `neo-back-${side}`);
        Gtk4LayerShell.set_layer(win, Gtk4LayerShell.Layer.TOP);
        const edge = side === 'left' ? Gtk4LayerShell.Edge.LEFT : Gtk4LayerShell.Edge.RIGHT;
        for (const e of [edge, Gtk4LayerShell.Edge.TOP, Gtk4LayerShell.Edge.BOTTOM]) Gtk4LayerShell.set_anchor(win, e, true);
        Gtk4LayerShell.set_margin(win, Gtk4LayerShell.Edge.TOP, CLEAR_TOP);
        Gtk4LayerShell.set_margin(win, Gtk4LayerShell.Edge.BOTTOM, CLEAR_BOTTOM);
        Gtk4LayerShell.set_exclusive_zone(win, 0);
        Gtk4LayerShell.set_keyboard_mode(win, Gtk4LayerShell.KeyboardMode.NONE);
        // a frame has to be drawn or the surface is never mapped and gets no touch: an empty, fully transparent
        // window drew nothing (css: 1 % alpha); and the size request keeps it at STRIP (GTK made it 200 px wide)
        const area = new Gtk.Box({hexpand: true, vexpand: true, width_request: STRIP, css_classes: ['neo-edge-area']});
        win.set_child(area);
        win.set_size_request(STRIP, -1);
        const drag = new Gtk.GestureDrag();
        drag.connect('drag-update', (g, dx, dy) => {
            const inward = side === 'left' ? dx : -dx;
            const [, , sy] = g.get_start_point();
            this._pill(side, inward, sy + dy);
        });
        drag.connect('drag-end', (_g, dx, dy) => {
            const inward = side === 'left' ? dx : -dx;
            this._pill(side, -1, 0);
            if (inward >= ARM && Math.abs(dy) < inward * 1.5) this._back();
        });
        area.add_controller(drag);
        win.present();
        // The first configure is 200 px wide whatever the size request; the next one, after any change, is STRIP.
        // Until then the part beyond STRIP is pushed off the screen edge (a negative margin), so the strip never
        // covers more of the apps than STRIP; checked a few times while the width settles.
        let checks = 0;
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, 60, () => {
            const w = win.get_width();
            if (w > 0) Gtk4LayerShell.set_margin(win, edge, Math.min(0, STRIP - w));
            return ++checks < 12 ? GLib.SOURCE_CONTINUE : GLib.SOURCE_REMOVE;
        });
        return win;
    }

    /** The arrow pill beside the finger while it pulls in from `side` (`inward` < 0 hides it). y is in strip
     *  coordinates, which start CLEAR_TOP below the screen top. */
    _pill(side, inward, y) {
        let p = (this._pills ??= {})[side];
        if (inward < 8) { if (p?.get_visible()) this._fadeOut(p); return; }
        if (p?._fade) { GLib.source_remove(p._fade); p._fade = 0; }
        if (!p) {
            p = this._pills[side] = new Gtk.Window({application: this._app, decorated: false, default_width: PILL, default_height: PILL,
                css_classes: ['neo-back-window']});
            Gtk4LayerShell.init_for_window(p);
            Gtk4LayerShell.set_namespace(p, 'neo-back-arrow');
            Gtk4LayerShell.set_layer(p, Gtk4LayerShell.Layer.OVERLAY);
            Gtk4LayerShell.set_anchor(p, side === 'left' ? Gtk4LayerShell.Edge.LEFT : Gtk4LayerShell.Edge.RIGHT, true);
            Gtk4LayerShell.set_anchor(p, Gtk4LayerShell.Edge.TOP, true);
            Gtk4LayerShell.set_keyboard_mode(p, Gtk4LayerShell.KeyboardMode.NONE);
            p._icon = new Gtk.Image({icon_name: side === 'left' ? 'go-previous-symbolic' : 'go-next-symbolic', pixel_size: 20,
                css_classes: ['neo-back-pill'], halign: Gtk.Align.CENTER, valign: Gtk.Align.CENTER});
            p.set_child(p._icon);
        }
        const t = Math.min(1, inward / ARM);
        // it slides out with the finger; 43 % while pulling, solid once letting go means back (old Neo: 110/255)
        Gtk4LayerShell.set_margin(p, side === 'left' ? Gtk4LayerShell.Edge.LEFT : Gtk4LayerShell.Edge.RIGHT, Math.round(4 + t * 20));
        Gtk4LayerShell.set_margin(p, Gtk4LayerShell.Edge.TOP, Math.max(0, Math.round(CLEAR_TOP + y - PILL / 2)));
        p.set_opacity(t >= 1 ? 1 : 0.43);
        p.set_visible(true);
    }

    /** Fades the pill out over ~150 ms instead of letting it vanish. */
    _fadeOut(p) {
        if (p._fade) return;
        p._fade = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 16, () => {
            const o = p.get_opacity() - 0.12;
            if (o > 0) { p.set_opacity(o); return GLib.SOURCE_CONTINUE; }
            p.set_visible(false); p._fade = 0;
            return GLib.SOURCE_REMOVE;
        });
    }

    _back() {
        if (this._onBack()) return;          // Neo handled it
        try {
            Gio.Subprocess.new(['wtype', '-M', 'alt', '-k', 'Left', '-m', 'alt'], Gio.SubprocessFlags.NONE);
        } catch (e) { logError(e, 'neo-launcher: back (wtype)'); }
    }

    destroy() {
        for (const w of Object.values(this._pills ?? {})) w.destroy();
        for (const w of this._strips) w.destroy();
        this._strips = [];
    }
}
