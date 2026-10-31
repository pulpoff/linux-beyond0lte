#!/usr/bin/env -S gjs -m
// Compass: the heading from iio-sensor-proxy (net.hadess.SensorProxy.Compass, fed by the ADSP's magnetometer
// through libssc) on a drawn compass rose. The rose turns so north on the dial points at real north.
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk?version=4.0';
import Gdk from 'gi://Gdk?version=4.0';
import Adw from 'gi://Adw?version=1';
import System from 'system';
// Geoclue is optional: without it the dial still works, the location line says so
let Geoclue = null;
try { Geoclue = (await import('gi://Geoclue?version=2.0')).default; } catch (_) {}

const BUS = 'net.hadess.SensorProxy';
const PATH = '/net/hadess/SensorProxy/Compass';
const IFACE = 'net.hadess.SensorProxy.Compass';
const NAMES = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];

class CompassWindow {
    constructor(app) {
        this.heading = null; this.shown = 0; this.position = null;
        Adw.StyleManager.get_default().color_scheme = Adw.ColorScheme.FORCE_DARK;
        const css = new Gtk.CssProvider();
        css.load_from_string(`
            window.compass, window.compass .compass-bg { background: #1c1c1e; color: #fff; }
            .compass-status { color: #8e8e93; font-size: 13px; }`);
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION);
        this.win = new Adw.ApplicationWindow({application: app, title: 'Compass', default_width: 360, default_height: 720, css_classes: ['compass']});
        const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, spacing: 0, css_classes: ['compass-bg']});
        this.area = new Gtk.DrawingArea({hexpand: true, vexpand: true});
        this.area.set_draw_func((_a, cr, w, h) => this._draw(cr, w, h));
        this.status = new Gtk.Label({label: '', css_classes: ['compass-status'], wrap: true, justify: Gtk.Justification.CENTER, margin_start: 24, margin_end: 24, margin_bottom: 40});
        box.append(this.area); box.append(this.status);
        this.win.set_content(box);
        this.win.connect('close-request', () => { if (this.releaseId) GLib.source_remove(this.releaseId); this._release(); return false; });
        // In the background the compass is let go: the sensor hub stops streaming (several DSP and CPU wakeups
        // a second) and nothing is drawn. Mobile shells cover an app without telling it, so the Neo Launcher's
        // de.yesman.Foreground says which app is on screen; without it, the window's focus decides.
        this.fgApp = null;
        const sync = () => this._setActive(this.fgApp !== null ? this.fgApp === 'de.yesman.Compass'
                                                                 : this.win.is_active && !(this.win.suspended ?? false));
        this.win.connect('notify::is-active', sync);
        this.win.connect('notify::suspended', sync);
        Gio.DBusProxy.new_for_bus(Gio.BusType.SESSION, Gio.DBusProxyFlags.NONE, null, 'de.yesman.Foreground',
            '/de/yesman/Foreground', 'de.yesman.Foreground', null, (_s, res) => {
                try { this.fg = Gio.DBusProxy.new_for_bus_finish(res); } catch (_) { return; }
                const read = () => {
                    const v = this.fg.get_cached_property('AppId'); this.fgApp = v ? v.unpack() : null;
                    if (this.fgWait) { GLib.source_remove(this.fgWait); this.fgWait = 0; }
                    sync();
                };
                this.fg.connect('g-properties-changed', read);
                this.fg.connect('notify::g-name-owner', read);
                read();
            });
        // the first claim waits for the launcher to say what is in front (a claim released right away crashes
        // iio-sensor-proxy); without the service the window's focus decides after 1.5 s
        this.fgWait = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1500, () => { this.fgWait = 0; sync(); return GLib.SOURCE_REMOVE; });
    }

    /**
     * iio-sensor-proxy crashes on a quick claim / release / claim of the compass (libssc), so the sensor is let go
     * only after 3 s in the background, and coming back within that time keeps it.
     */
    _setActive(on) {
        if (on === this.active) return;
        this.active = on;
        if (on) {
            if (this.releaseId) { GLib.source_remove(this.releaseId); this.releaseId = 0; }
            this._claim();
        } else {
            this._stopTick();
            if (!this.releaseId) this.releaseId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 3, () => {
                this.releaseId = 0;
                if (!this.active) this._release();
                return GLib.SOURCE_REMOVE;
            });
        }
    }

    /** Ease the needle towards the reading at the display's rate, only while it is still moving. */
    _startTick() { if (!this.tick && this.active) this.tick = this.area.add_tick_callback(() => this._animate()); }
    _stopTick() { if (this.tick) { this.area.remove_tick_callback(this.tick); this.tick = 0; } }

    _claim() {
        if (this.proxy) { this._claimCall(); return; }
        Gio.DBusProxy.new_for_bus(Gio.BusType.SYSTEM, Gio.DBusProxyFlags.NONE, null, BUS, PATH, IFACE, null, (_s, res) => {
            try { this.proxy = Gio.DBusProxy.new_for_bus_finish(res); } catch (e) { console.log(`[compass] no sensor service: ${e.message}`); this.status.label = `No sensor service: ${e.message}`; return; }
            const has = this.proxy.get_cached_property('HasCompass')?.unpack();
            if (has === false) { this.status.label = 'This phone reports no compass.'; return; }
            this.proxy.connect('g-properties-changed', (_p, changed) => {
                const h = changed.deep_unpack().CompassHeading;
                if (h !== undefined) this._reading(h.deep_unpack?.() ?? h);
            });
            this._claimCall();
        });
    }

    _claimCall() {
        if (!this.active || this.claimed) return;
        this.claimed = true;
        this.proxy.call('ClaimCompass', null, Gio.DBusCallFlags.NONE, -1, null, (p, r) => {
            try { p.call_finish(r); this.status.label = 'Hold the phone flat, away from metal. Move it in a figure 8 to calibrate.'; }
            catch (e) { this.claimed = false; console.log(`[compass] claim failed: ${e.message}`); this.status.label = `Could not use the compass: ${e.message}`; }
        });
    }

    _release() {
        if (!this.claimed) return;
        this.claimed = false;
        this.proxy?.call('ReleaseCompass', null, Gio.DBusCallFlags.NONE, 1000, null, (p, r) => { try { p.call_finish(r); } catch (_) {} });
    }

    _reading(h) {
        if (typeof h !== 'number' || h < 0) return;           // -1: not known yet
        this.heading = h;
        this._startTick();
    }

    _animate() {
        if (this.heading === null) { this.tick = 0; return GLib.SOURCE_REMOVE; }
        const diff = ((this.heading - this.shown + 540) % 360) - 180;  // the short way round
        if (Math.abs(diff) <= 0.05) { this.shown = this.heading; this.area.queue_draw(); this.tick = 0; return GLib.SOURCE_REMOVE; }
        this.shown = (this.shown + diff * 0.15 + 360) % 360;
        this.area.queue_draw();
        return GLib.SOURCE_CONTINUE;
    }

    /** A round dark dial: ticks and degrees on the rim, N/E/S/W and the intercardinals inside, all turning with
     *  the heading; a red pointer fixed at the top; the heading and where you are in the middle. */
    _draw(cr, w, h) {
        const rad = d => d * Math.PI / 180;
        const R = Math.min(w, h) / 2 * 0.86, cx = w / 2, cy = h / 2;   // a margin round the dial, as on Android
        const rim = R * 0.80;                                   // where the ticks start
        const font = (size, bold) => { cr.selectFontFace('Sans', 0, bold ? 1 : 0); cr.setFontSize(size); };
        const centred = (text, y) => { const e = cr.textExtents(text); cr.moveTo(cx - e.width / 2 - e.xBearing, y); cr.showText(text); };
        // body: a dark disc with a lighter rim band
        cr.setSourceRGB(0.16, 0.16, 0.17); cr.arc(cx, cy, R, 0, 2 * Math.PI); cr.fill();
        cr.setSourceRGB(0.11, 0.11, 0.12); cr.arc(cx, cy, rim - 2, 0, 2 * Math.PI); cr.fill();
        cr.setSourceRGBA(1, 1, 1, 0.08); cr.setLineWidth(1); cr.arc(cx, cy, rim * 0.72, 0, 2 * Math.PI); cr.stroke();

        cr.save(); cr.translate(cx, cy); cr.rotate(-rad(this.shown));
        // rim ticks: every 2°, longer every 10°, white and longest at the cardinals
        for (let a = 0; a < 360; a += 2) {
            const card = a % 90 === 0, ten = a % 10 === 0;
            cr.save(); cr.rotate(rad(a));
            cr.setSourceRGBA(1, 1, 1, card ? 1 : ten ? 0.85 : 0.45); cr.setLineWidth(card ? 3 : ten ? 1.8 : 1);
            cr.moveTo(0, -R + 4); cr.lineTo(0, -R + (card ? 22 : ten ? 15 : 10)); cr.stroke();
            cr.restore();
        }
        // text along the dial: degrees every 30° just inside the ticks, letters further in (they turn with it)
        const along = (a, dist, text, size, bold, rgb) => {
            cr.save(); cr.rotate(rad(a)); cr.translate(0, -dist);
            font(size, bold); const e = cr.textExtents(text);
            cr.setSourceRGB(...rgb); cr.moveTo(-e.width / 2 - e.xBearing, e.height / 2); cr.showText(text);
            cr.restore();
        };
        for (let a = 0; a < 360; a += 30) along(a, R - 38, String(a), R * 0.075, false, [0.92, 0.92, 0.94]);
        for (const [a, t] of [[0, 'N'], [90, 'E'], [180, 'S'], [270, 'W']])
            along(a, rim * 0.80, t, R * 0.14, true, t === 'N' ? [1, 0.18, 0.15] : [1, 1, 1]);
        for (const [a, t] of [[45, 'NE'], [135, 'SE'], [225, 'SW'], [315, 'NW']])
            along(a, rim * 0.80, t, R * 0.065, false, [0.75, 0.75, 0.78]);
        cr.restore();

        // the fixed red pointer at the top: where the phone points
        cr.setSourceRGB(1, 0.18, 0.15);
        cr.moveTo(cx, cy - R + 14); cr.lineTo(cx - 10, cy - R - 6); cr.lineTo(cx + 10, cy - R - 6); cr.closePath(); cr.fill();
        cr.setLineWidth(2.5); cr.moveTo(cx, cy - R + 10); cr.lineTo(cx, cy - R + 30); cr.stroke();

        // the middle: heading, a divider, where you are
        cr.setSourceRGB(1, 1, 1); font(R * 0.24, false);
        const hd = this.heading === null ? '—' : `${Math.round(this.heading) % 360}°${NAMES[Math.round(this.heading / 45) % 8]}`;
        centred(hd, cy - R * 0.04);
        cr.setSourceRGBA(1, 1, 1, 0.35); cr.setLineWidth(1);
        cr.moveTo(cx - R * 0.30, cy + R * 0.06); cr.lineTo(cx + R * 0.30, cy + R * 0.06); cr.stroke();
        // a location pin
        const py = cy + R * 0.17, pr = R * 0.045;
        cr.setSourceRGB(0.92, 0.92, 0.94);
        cr.arc(cx, py, pr, Math.PI, 0); cr.lineTo(cx, py + pr * 2.2); cr.closePath(); cr.fill();
        cr.setSourceRGB(0.11, 0.11, 0.12); cr.arc(cx, py, pr * 0.42, 0, 2 * Math.PI); cr.fill();
        cr.setSourceRGB(0.92, 0.92, 0.94); font(R * 0.07, true); centred('Location', cy + R * 0.38);
        font(R * 0.062, false);
        if (this.position) {
            const dms = (v, pos, neg) => { const a = Math.abs(v), d = Math.floor(a), m = Math.floor((a - d) * 60), s = ((a - d) * 60 - m) * 60;
                return `${d}°${String(m).padStart(2, '0')}'${s.toFixed(1)}"${v >= 0 ? pos : neg}`; };
            centred(dms(this.position[0], 'N', 'S'), cy + R * 0.49);
            centred(dms(this.position[1], 'E', 'W'), cy + R * 0.59);
        } else { cr.setSourceRGBA(1, 1, 1, 0.5); centred(this.locError ?? 'Locating…', cy + R * 0.49); }
    }
}

const app = new Adw.Application({application_id: 'de.yesman.Compass', flags: Gio.ApplicationFlags.DEFAULT_FLAGS});
app.connect('activate', () => { (app._w ??= new CompassWindow(app)).win.present(); });
app.run([System.programInvocationName, ...System.programArgs]);
