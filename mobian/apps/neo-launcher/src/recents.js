
// The task view (One UI's recents), driven by Neo's home bar (homebar.js) with the windows Phosh lists over
// de.yesman.PhoshNeo: while the finger pulls up from the bottom edge, the current app's card shrinks from the whole
// screen to the middle; when the finger rests the other apps' cards come in beside it; on release it settles as the
// task view, or steps aside for home. Cards scroll sideways, a tap switches to the app, a swipe up
// closes it, the pill at the bottom closes them all, a tap beside the cards goes home.
import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';
import Gdk from 'gi://Gdk?version=4.0';
import Gsk from 'gi://Gsk?version=4.0';
import Graphene from 'gi://Graphene';
import Gtk from 'gi://Gtk?version=4.0';
import Adw from 'gi://Adw?version=1';
import Gtk4LayerShell from 'gi://Gtk4LayerShell?version=1.0';
import {appName} from './model.js';

/**
 * A garbage collection a moment after the task view goes (or the home screen comes back): its cards held a full-size picture of every window (10 MB
 * each at 1080x2340), and the JS engine sees only the small wrappers, so on its own it got round to freeing them
 * minutes later, if at all. One collection is a few ms, after the closing animation.
 */
let _gcSource = 0;
export function collectSoon() {
    if (_gcSource) return;
    _gcSource = GLib.timeout_add(GLib.PRIORITY_LOW, 1500, () => { _gcSource = 0; System.gc(); return GLib.SOURCE_REMOVE; });
}

const BUS = 'de.yesman.PhoshNeo', PATH = '/de/yesman/PhoshNeo';
const FINAL = 0.62;          // a settled card's width, of the screen's
const GAP = 22;              // px between settled cards
const SHRINK_AT = 0.25;      // of the screen height: the finger's travel by which the card has shrunk all the way
const CHIP = 44;             // px above a card for its icon and name
const CLOSE_AT = 0.18;       // of the screen: a card pushed up this far closes on release
const FLING = 0.6;           // px/ms

function call(method, params = null, done = null) {
    Gio.DBus.session.call(BUS, PATH, BUS, method, params, null, Gio.DBusCallFlags.NO_AUTO_START, 2000, null,
        (c, res) => {
            let out = null;
            try { out = c.call_finish(res); } catch (e) { log(`neo-launcher: ${method}: ${e.message}`); }
            done?.(out);
        });
}

const lerp = (a, z, t) => a + (z - a) * t;
const clamp = (v, a, z) => Math.max(a, Math.min(z, v));

function formatBytes(n) {
    return n >= 1024 ** 3 ? `${(n / 1024 ** 3).toFixed(1)} GB` : `${Math.round(n / 1024 ** 2)} MB`;
}

/** One window: its thumbnail in a rounded card, its icon and name above it (the chip, drawn unscaled). */
class Card {
    constructor(view, w) {
        this.id = w.id;
        this.dy = 0;                                 // pushed up to close (px, up < 0)
        this.fade = 1;
        this.picture = new Gtk.Picture({content_fit: Gtk.ContentFit.COVER, can_shrink: true});
        this.frame = new Gtk.Box({css_classes: ['neo-recents-card'], overflow: Gtk.Overflow.HIDDEN});
        this.placeholder = new Gtk.Image({pixel_size: 96, hexpand: true, vexpand: true});
        this.frame.append(this.placeholder);
        this.frame.append(this.picture);
        this.frame.set_parent(view);
        const info = view.appInfo(w.app_id);
        this.chip = new Gtk.Box({spacing: 8, css_classes: ['neo-recents-chip'], halign: Gtk.Align.CENTER});
        this.icon = new Gtk.Image({pixel_size: 28});
        this.chip.append(this.icon);
        this.chip.append(new Gtk.Label({label: (info && appName(info)) || w.title || w.app_id, ellipsize: 3, max_width_chars: 14}));
        this.mem = new Gtk.Label({css_classes: ['neo-recents-mem'], visible: false});
        this.chip.append(this.mem);
        this.appId = w.app_id;
        this.chip.set_parent(view);
        const gicon = info ? view.iconFor(info) : Gio.ThemedIcon.new('application-x-executable');
        this.icon.set_from_gicon(gicon);
        this.placeholder.set_from_gicon(gicon);
        this.update(w);
    }

    setTexture(tex) {
        this.picture.set_paintable(tex);
        this.placeholder.set_visible(false);
        this.fresh = true;
    }

    update(w) {
        if (!w.path || !w.width || this.fresh) return;
        try {
            const [, data] = GLib.file_get_contents(w.path);
            const tex = Gdk.MemoryTexture.new(w.width, w.height, Gdk.MemoryFormat.B8G8R8A8_PREMULTIPLIED,
                new GLib.Bytes(data), w.stride);
            this.picture.set_paintable(tex);
            this.placeholder.set_visible(false);
        } catch (e) { log(`neo-launcher: thumbnail ${w.path}: ${e.message}`); }
    }

    destroy() { this.frame.unparent(); this.chip.unparent(); }
}

const RAM_COLORS = {used: '#ef5350', cache: '#ffb74d', free: '#66bb6a'};

/** /proc/meminfo in kB → bytes, by key. The kernel's own file: read in place (it never blocks). */
function meminfo() {
    const out = {};
    const [, data] = GLib.file_get_contents('/proc/meminfo');
    for (const line of new TextDecoder().decode(data).split('\n')) {
        const m = line.match(/^(\w+):\s+(\d+)/);
        if (m) out[m[1]] = Number(m[2]) * 1024;
    }
    return out;
}

/**
 * The phone's memory as a bar: used (apps, the kernel, GPU buffers: what cannot be dropped), cache (file pages the
 * kernel can drop at once), free, and a legend with the figures.
 * Shared memory counts as used, not cache: here that is mostly GPU buffers, which the kernel cannot reclaim.
 */
const RamBar = GObject.registerClass(class NeoRamBar extends Gtk.Box {
    _init() {
        super._init({orientation: Gtk.Orientation.VERTICAL, spacing: 6, css_classes: ['neo-recents-ram']});
        this._m = null;
        this.bars = new Gtk.DrawingArea({content_height: 10, hexpand: true});
        this.bars.set_draw_func((_a, cr, w, h) => { this._draw(cr, w, h); cr.$dispose(); });   // GJS: or each frame's context leaks
        // the legend: the phone's whole memory as a badge on the left (no colour of its own, it is not a part of
        // the bar), the three parts on the right, one per line, their dots in a column; the block centred
        this.total = new Gtk.Label({css_classes: ['neo-recents-total'], valign: Gtk.Align.CENTER});
        const parts = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, spacing: 2, valign: Gtk.Align.CENTER});
        this.items = ['used', 'cache', 'free'].map(() => {
            const l = new Gtk.Label({use_markup: true, xalign: 0});
            parts.append(l);
            return l;
        });
        const legend = new Gtk.Box({spacing: 14, halign: Gtk.Align.CENTER});
        legend.append(this.total);
        legend.append(parts);
        this.append(this.bars);
        this.append(legend);
    }

    refresh() {
        let mi;
        try { mi = meminfo(); } catch (e) { log(`neo-launcher: meminfo: ${e.message}`); return; }
        const total = mi.MemTotal || 0;
        if (!total) return;
        const free = mi.MemFree || 0;
        const cache = Math.max(0, (mi.Buffers || 0) + (mi.Cached || 0) + (mi.SReclaimable || 0) - (mi.Shmem || 0));
        const used = Math.max(0, total - free - cache);
        this._m = {total, used, cache, free};
        const dot = (c, name, v) => `<span foreground="${c}">●</span> ${name} ${formatBytes(v)}`;
        this.total.set_label(formatBytes(total));
        this.items[0].set_markup(dot(RAM_COLORS.used, 'Used', used));
        this.items[1].set_markup(dot(RAM_COLORS.cache, 'Cache', cache));
        this.items[2].set_markup(dot(RAM_COLORS.free, 'Free', free));
        this.bars.queue_draw();
    }

    _draw(cr, w, h) {
        const m = this._m;
        if (!m) return;
        const rgba = c => { const r = new Gdk.RGBA(); r.parse(c); return r; };
        const rounded = (x, y, ww, hh, r) => {
            cr.newSubPath();
            cr.arc(x + ww - r, y + r, r, -Math.PI / 2, 0); cr.arc(x + ww - r, y + hh - r, r, 0, Math.PI / 2);
            cr.arc(x + r, y + hh - r, r, Math.PI / 2, Math.PI); cr.arc(x + r, y + r, r, Math.PI, 1.5 * Math.PI);
            cr.closePath();
        };
        // RAM: 10 px, the three parts in one rounded track
        const rh = 10;
        cr.save();
        rounded(0, 0, w, rh, rh / 2); cr.clip();
        let x = 0;
        for (const [k, v] of [['used', m.used], ['cache', m.cache], ['free', m.free]]) {
            const ww = w * v / m.total;
            Gdk.cairo_set_source_rgba(cr, rgba(RAM_COLORS[k]));
            cr.rectangle(x, 0, ww + 0.5, rh); cr.fill();
            x += ww;
        }
        cr.restore();
    }
});

const View = GObject.registerClass(class NeoRecentsView extends Gtk.Widget {
    _init(owner) {
        super._init({hexpand: true, vexpand: true, overflow: Gtk.Overflow.HIDDEN});
        this.owner = owner;
        this.cards = [];
        this.shrink = 0;          // 0 the whole screen … 1 a settled card
        this.lift = 0;            // px the shrinking card follows the finger past SHRINK_AT
        this.reveal = 0;          // the other cards, chips and the pill: 0 hidden … 1 shown
        this.scroll = 0;          // which card is in the middle (fractional while scrolling)
        this.zoom = null;         // {index, t}: a tapped card growing back to the whole screen
        this.dim = new Gtk.Box({css_classes: ['neo-recents-dim']});
        this.dim.set_parent(this);
        this.closeAll = new Gtk.Button({label: 'Close all', css_classes: ['neo-recents-closeall', 'pill']});
        this.closeAll.connect('clicked', () => owner.closeAll());
        this.closeAll.set_parent(this);
        // the phone's memory under the cards, over the pill: a bar of used / cache / free
        this.ram = new RamBar();
        this.ram.set_parent(this);
        this.updateRam();
    }

    updateRam() { this.ram?.refresh(); }

    appInfo(appId) { return this.owner.appInfo(appId); }
    iconFor(info) { return this.owner.iconFor(info); }

    setCards(windows) {
        const keep = new Map(this.cards.map(c => [c.id, c]));
        this.cards = windows.map(w => {
            const c = keep.get(w.id);
            if (c) { keep.delete(w.id); c.update(w); return c; }
            return new Card(this, w);
        });
        for (const c of keep.values()) c.destroy();
        // the chips and the pill above the cards
        for (const c of this.cards) c.chip.insert_before(this, this.closeAll);
        this.queue_allocate();
    }

    /** The settled card's size and middle. */
    geometry() {
        const W = this.get_width(), H = this.get_height();
        const s = lerp(1, FINAL, this.shrink);
        return {W, H, s, cw: W * s, ch: H * s, step: W * FINAL + GAP,
            cx: W / 2, cy: lerp(H / 2, H * 0.46, this.shrink) - this.lift};
    }

    /** The card at (x, y), or -1. */
    cardAt(x, y) {
        const g = this.geometry();
        for (let i = 0; i < this.cards.length; i++) {
            const mx = g.cx + (i - this.scroll) * g.step;
            if (Math.abs(x - mx) <= g.cw / 2 && Math.abs(y - g.cy) <= g.ch / 2) return i;
        }
        return -1;
    }

    vfunc_measure(orientation) {
        const geo = Gdk.Display.get_default().get_monitors().get_item(0)?.get_geometry();
        const n = orientation === Gtk.Orientation.HORIZONTAL ? geo?.width ?? 360 : geo?.height ?? 800;
        return [0, n, -1, -1];
    }

    vfunc_size_allocate(W, H, baseline) {
        this.dim.allocate(W, H, baseline, null);
        this.dim.set_opacity(this.shrink * (this.zoom ? 1 - this.zoom.t : 1));
        const g = this.geometry();
        const focus = Math.round(this.scroll);
        this.cards.forEach((c, i) => {
            let s = g.s, x = g.cx + (i - this.scroll) * g.step, y = g.cy + c.dy, op = c.fade;
            if (i !== focus) op *= this.reveal;                       // the neighbours come in with the hold
            if (this.zoom) {
                if (i === this.zoom.index) {
                    const t = this.zoom.t;
                    s = lerp(g.s, 1, t); x = lerp(x, W / 2, t); y = lerp(y, H / 2, t);
                } else op *= 1 - this.zoom.t;
            }
            const t = new Gsk.Transform()
                .translate(new Graphene.Point({x: x - W * s / 2, y: y - H * s / 2}))
                .scale(s, s);
            c.frame.allocate(W, H, baseline, t);
            c.frame.set_opacity(op);
            const [, cnat] = c.chip.get_preferred_size();
            const chipW = Math.min(cnat.width, g.cw);
            c.chip.allocate(chipW, CHIP - 8, baseline,
                new Gsk.Transform().translate(new Graphene.Point({x: x - chipW / 2, y: y - H * s / 2 - CHIP})));
            c.chip.set_opacity(op * this.reveal * (this.zoom ? 1 - this.zoom.t : 1));
        });
        const [, bnat] = this.closeAll.get_preferred_size();
        this.closeAll.allocate(bnat.width, bnat.height, baseline,
            new Gsk.Transform().translate(new Graphene.Point({x: (W - bnat.width) / 2, y: H - bnat.height - 28})));
        this.closeAll.set_opacity(this.reveal * (this.zoom ? 1 - this.zoom.t : 1));
        this.closeAll.set_can_target(this.reveal > 0.5 && !this.zoom);
        // midway between a settled card's bottom edge and the pill
        const [, rnat] = this.ram.get_preferred_size();
        const cardBottom = H * 0.46 + H * FINAL / 2, pillTop = H - bnat.height - 28, ramW = Math.round(W * FINAL);
        this.ram.allocate(ramW, rnat.height, baseline,
            new Gsk.Transform().translate(new Graphene.Point({x: (W - ramW) / 2, y: (cardBottom + pillTop - rnat.height) / 2})));
        this.ram.set_opacity(this.reveal * (this.zoom ? 1 - this.zoom.t : 1));
    }

    vfunc_dispose() {
        for (const c of this.cards) c.destroy();
        this.cards = [];
        this.dim?.unparent(); this.dim = null;
        this.closeAll?.unparent(); this.closeAll = null;
        this.ram?.unparent(); this.ram = null;
        super.vfunc_dispose();
    }
});

export class Recents {
    /** launcher: the NeoLauncher application (icons, wallpaper, showHome, hideLauncher). */
    constructor(launcher) {
        this.l = launcher;
        this.active = false;      // a drag or the task view is on screen
        this.settled = false;     // the task view takes input
    }

    appInfo(appId) {
        if (!appId) return null;
        for (const id of [`${appId}.desktop`, `${appId.toLowerCase()}.desktop`]) {
            try { const i = Gio.DesktopAppInfo.new(id); if (i) return i; } catch (_) {}
        }
        return null;
    }

    iconFor(info) { return this.l.icons.lookup(info, 28, this.l._scale()).gicon; }

    _window() {
        if (this.win) return this.win;
        const win = this.win = new Gtk.Window({application: this.l, title: 'Neo Recents', decorated: false});
        Gtk4LayerShell.init_for_window(win);
        Gtk4LayerShell.set_namespace(win, 'neo-recents');
        // above Phosh's home surface, which the home bar drags up over the app (TOP, like Neo's home)
        Gtk4LayerShell.set_layer(win, Gtk4LayerShell.Layer.OVERLAY);
        for (const e of [Gtk4LayerShell.Edge.TOP, Gtk4LayerShell.Edge.BOTTOM, Gtk4LayerShell.Edge.LEFT, Gtk4LayerShell.Edge.RIGHT])
            Gtk4LayerShell.set_anchor(win, e, true);
        Gtk4LayerShell.set_exclusive_zone(win, 0);
        Gtk4LayerShell.set_keyboard_mode(win, Gtk4LayerShell.KeyboardMode.NONE);
        win.add_css_class('neo-recents');
        // a layer window whose content asks for no size never maps: ask for the whole screen
        const geo = Gdk.Display.get_default().get_monitors().get_item(0)?.get_geometry();
        win.set_default_size(geo?.width ?? 360, geo?.height ?? 800);
        const overlay = new Gtk.Overlay();
        this.wall = new Gtk.Picture({content_fit: Gtk.ContentFit.COVER, can_shrink: true});
        overlay.set_child(this.wall);
        this.view = new View(this);
        overlay.add_overlay(this.view);
        win.set_child(overlay);
        this._input();
        return win;
    }

    /** The wallpaper shows behind the cards only once they have shrunk: before that the card covers it all. */
    _sync() {
        this.wall.set_opacity(clamp(this.view.shrink * 3, 0, 1));
        this.view.queue_allocate();
    }

    _begin() {
        this._stop();
        this.active = true; this.settled = false; this.decided = false; this.shown = false;
        this._window();
        this.wall.set_paintable(this.l.wallpaper.get_paintable());
        Object.assign(this.view, {shrink: 0, lift: 0, reveal: 0, scroll: 0, zoom: null});
        this.view.setCards([]);
        this.win.set_can_target(false);
        this._shot = null;
        this._session = {};
        // the screen is the current app's picture only while an app shows: from the home screen it would be the
        // launcher, and it went onto the card of the app focused last
        if (!this.l.win?.get_visible())
            this._capture();
        this._fetch(true);
    }

    /** The screen as it is (the current app), for its card: Phosh's thumbnails are only as fresh as its last
     *  overview. Taken before anything of the task view is on screen. */
    _capture() {
        let proc;
        try {
            proc = Gio.Subprocess.new(['grim', '-s', '1.5', '-t', 'ppm', '-'], Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE);
        } catch (e) { logError(e, 'neo-launcher: grim'); return; }
        const begun = this._session;
        proc.communicate_async(null, null, (pr, res) => {
            let bytes;
            try { [, bytes] = pr.communicate_finish(res); } catch (e) { logError(e, 'neo-launcher: grim'); return; }
            if (this._session !== begun || !this.active || !bytes) return;
            const data = bytes.toArray();
            // P6\n<w> <h>\n255\n<rgb…>
            let i = 0, fields = [];
            while (fields.length < 4 && i < 64) {
                let tok = '';
                while (i < data.length && /\s/.test(String.fromCharCode(data[i]))) i++;
                while (i < data.length && !/\s/.test(String.fromCharCode(data[i]))) tok += String.fromCharCode(data[i++]);
                fields.push(tok);
            }
            i++;
            const [magic, w, h] = [fields[0], +fields[1], +fields[2]];
            if (magic !== 'P6' || !w || !h) return;
            this._shot = Gdk.MemoryTexture.new(w, h, Gdk.MemoryFormat.R8G8B8, new GLib.Bytes(data.subarray(i, i + w * h * 3)), w * 3);
            this._applyShot();
        });
    }

    /** Each app's memory beside its name, read once per opening by appmem.py in a process of its own: reading
     *  /proc of a process stuck in the kernel blocks the reader, and Neo's main loop (the system gestures) must
     *  never be that reader. A helper that hangs only leaves the figures out. */
    _measure() {
        const session = this._session;
        const ids = [...new Set(this.view.cards.map(c => c.appId).filter(Boolean))];
        if (!ids.length || this._measuring) return;
        const helper = GLib.build_filenamev([GLib.path_get_dirname(import.meta.url.replace('file://', '')), 'appmem.py']);
        let proc;
        try {
            proc = Gio.Subprocess.new(['timeout', '-s', 'KILL', '3', 'python3', helper, ...ids],
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE);
        } catch (e) { logError(e, 'neo-launcher: appmem'); return; }
        this._measuring = true;
        proc.communicate_utf8_async(null, null, (pr, res) => {
            this._measuring = false;
            let mem = {};
            try { mem = JSON.parse(pr.communicate_utf8_finish(res)[1] || '{}'); } catch (_) { return; }
            if (this._session !== session) return;
            for (const c of this.view.cards) {
                const bytes = mem[c.appId];
                if (bytes) { c.mem.set_label(formatBytes(bytes)); c.mem.set_visible(true); }
            }
        });
    }

    _applyShot() {
        const c = this._shot && this.view.cards[this._focus];
        if (c) c.setTexture(this._shot);
    }

    _fetch(first = false, then = null) {
        call('GetWindows', null, out => {
            if (!this.active) return;
            const windows = (out?.deepUnpack()[0] ?? []).map(([id, app_id, title, path, width, height, stride, focused]) =>
                ({id, app_id, title, path, width, height, stride, focused}));
            this.view.setCards(windows);
            if (first) {
                const f = windows.findIndex(w => w.focused);
                this._focus = f;
                this.view.scroll = f >= 0 ? f : Math.max(0, windows.length - 1);
            }
            this._applyShot();
            if (first) { this._measure(); this.view.updateRam(); }
            this.view.scroll = clamp(this.view.scroll, 0, Math.max(0, windows.length - 1));
            this._sync();
            then?.(windows);
        });
    }

    /** The screen height the home bar measures its swipe against. */
    height() { return this.view?.get_height() || Gdk.Display.get_default().get_monitors().get_item(0)?.get_geometry().height || 800; }

    // --- driven by the home bar (homebar.js): recents = swipe up + hold ---

    /** A swipe up from the bottom edge started: get the windows and the app's picture ready, show nothing yet (it
     *  may be home). In the task view a swipe up means home. */
    begin() {
        this.swallow = false;
        if (this.settled) { this.swallow = true; return; }
        this._begin();
    }

    move(_up) { /* nothing shows until the hold */ }

    /** The finger rests: the app shrinks into its card in the middle and the other apps' cards come in. */
    hold() {
        if (this.swallow || !this.active || this.decided || this.shown) return;
        this.shown = true;
        this._sync();
        this.win.present();
        this._animate({shrink: 1, reveal: 1}, 280, () => { if (this.decided) this._ready(); });
    }

    /** The finger let go: settle as the task view (true, after a hold) or step aside for home (false). */
    release(toRecents) {
        if (this.swallow) { this.swallow = false; this.goHome(); return; }
        if (!this.active || this.decided) return;
        this.decided = true;
        if (!toRecents || !this.shown) { this.hide(); return; }
        if (!this._anim) this._ready();
    }

    /** The task view takes input; with nothing running there is nothing to show: home. */
    _ready() {
        if (!this.view.cards.length) { this.hide(); this.l.showHome(); return; }
        this.settled = true;
        this.win.set_can_target(true);
    }

    /** Animates view fields from where they are to `to`. */
    _animate(to, ms, done = null) {
        this._stop();
        const v = this.view, from = {};
        for (const k in to) from[k] = v[k];
        const target = Adw.CallbackAnimationTarget.new(t => {
            for (const k in to) v[k] = lerp(from[k], to[k], t);
            this._sync();
        });
        this._anim = new Adw.TimedAnimation({widget: v, value_from: 0, value_to: 1, duration: ms,
            easing: Adw.Easing.EASE_OUT_CUBIC, target});
        this._anim.connect('done', () => { this._anim = null; done?.(); });
        this._anim.play();
    }

    _stop() {
        const a = this._anim;
        this._anim = null;
        a?.pause();
    }

    _fadeOut() {
        this.settled = false;
        this.win.set_can_target(false);
        const win = this.win;
        const target = Adw.CallbackAnimationTarget.new(t => win.set_opacity(1 - t));
        const a = new Adw.TimedAnimation({widget: this.view, value_from: 0, value_to: 1, duration: 160, target});
        a.connect('done', () => { this.hide(); win.set_opacity(1); });
        a.play();
    }

    hide() {
        this._stop();
        this.active = false; this.settled = false; this.decided = false;
        if (this.win) { this.win.set_visible(false); this.view.setCards([]); }
        this._shot = null;
        collectSoon();
    }

    // --- the settled task view ---

    _input() {
        const v = this.view;
        const drag = new Gtk.GestureDrag();
        let mode = null, card = -1, start = 0, last = null;
        drag.connect('drag-begin', (g, x, y) => {
            if (!this.settled) { g.set_state(Gtk.EventSequenceState.DENIED); return; }
            this._stop();
            mode = null; card = v.cardAt(x, y); start = v.scroll; last = {t: GLib.get_monotonic_time(), dx: 0, dy: 0, vx: 0, vy: 0};
        });
        drag.connect('drag-update', (g, dx, dy) => {
            if (!this.settled) return;
            const now = GLib.get_monotonic_time(), dt = Math.max(1, (now - last.t) / 1000);
            last = {t: now, dx, dy, vx: (dx - last.dx) / dt, vy: (dy - last.dy) / dt};
            if (!mode && Math.hypot(dx, dy) > 12) mode = Math.abs(dx) > Math.abs(dy) ? 'scroll' : card >= 0 && dy < 0 ? 'close' : 'none';
            const geo = v.geometry();
            if (mode === 'scroll') {
                v.scroll = clamp(start - dx / geo.step, -0.3, v.cards.length - 0.7);
            } else if (mode === 'close') {
                const c = v.cards[card];
                c.dy = Math.min(0, dy);
                c.fade = 1 - clamp(-c.dy / (geo.H * 0.5), 0, 0.8);
            }
            v.queue_allocate();
        });
        drag.connect('drag-end', (g, dx, dy) => {
            if (!this.settled) return;
            if (mode === 'scroll') {
                let to = Math.round(v.scroll);
                if (Math.abs(last.vx) > FLING) to = last.vx < 0 ? Math.ceil(start + 0.01) : Math.floor(start - 0.01);
                this._animate({scroll: clamp(to, 0, v.cards.length - 1)}, 240);
            } else if (mode === 'close') {
                const c = v.cards[card], H = v.get_height();
                if (-c.dy > H * CLOSE_AT || last.vy < -FLING) this._closeCard(card);
                else this._settleCard(c);
            } else if (!mode) {
                const i = v.cardAt(...g.get_start_point().slice(1));
                if (i >= 0) this._open(i);
                else this.goHome();
            }
            mode = null;
        });
        v.add_controller(drag);
    }

    _settleCard(c) {
        const v = this.view, dy0 = c.dy, f0 = c.fade;
        const target = Adw.CallbackAnimationTarget.new(t => { c.dy = lerp(dy0, 0, t); c.fade = lerp(f0, 1, t); v.queue_allocate(); });
        new Adw.TimedAnimation({widget: v, value_from: 0, value_to: 1, duration: 200, easing: Adw.Easing.EASE_OUT_CUBIC, target}).play();
    }

    _closeCard(i) {
        const v = this.view, c = v.cards[i], dy0 = c.dy, H = v.get_height();
        const target = Adw.CallbackAnimationTarget.new(t => { c.dy = lerp(dy0, -H, t); c.fade = lerp(c.fade, 0, t); v.queue_allocate(); });
        const a = new Adw.TimedAnimation({widget: v, value_from: 0, value_to: 1, duration: 180, easing: Adw.Easing.EASE_IN_CUBIC, target});
        a.connect('done', () => {
            call('Close', new GLib.Variant('(s)', [c.id]));
            // the app's memory comes back once it has gone: read it again a moment later
            GLib.timeout_add(GLib.PRIORITY_LOW, 1200, () => { v.ram && v.updateRam(); return GLib.SOURCE_REMOVE; });
            v.cards.splice(i, 1);
            c.destroy();
            if (!v.cards.length) { this.goHome(); return; }
            // the next card slides in from the right; with none after it, the one before from the left
            const old = Math.round(v.scroll);
            if (i < v.cards.length && old >= i) v.scroll -= 1;
            this._animate({scroll: clamp(old, 0, v.cards.length - 1)}, 220);
        });
        a.play();
    }

    _open(i) {
        const v = this.view, c = v.cards[i];
        this.settled = false;
        this.win.set_can_target(false);
        call('Activate', new GLib.Variant('(s)', [c.id]));
        this.l.hideLauncher();
        v.zoom = {index: i, t: 0};
        const target = Adw.CallbackAnimationTarget.new(t => { v.zoom.t = t; this._sync(); });
        const a = new Adw.TimedAnimation({widget: v, value_from: 0, value_to: 1, duration: 240, easing: Adw.Easing.EASE_OUT_CUBIC, target});
        a.connect('done', () => this.hide());
        a.play();
    }

    closeAll() {
        call('CloseAll');
        this.goHome();
    }

    goHome() {
        this.l.showHome();
        this._fadeOut();
    }
}
