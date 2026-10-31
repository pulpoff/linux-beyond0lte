// Neo Launcher for Phosh: the home screen (pages of app icons, page dots, a dock) and the app drawer, drawn as a
// layer-shell surface over the app windows. Phosh (phosh 0.58.0-1+neo1, launcher=neo in de.yesman.neo) calls
// de.yesman.NeoLauncher.Home.Show() on the home gesture instead of unfolding its own app grid; a second home gesture
// while Neo shows opens Phosh's overview with the running apps (de.yesman.PhoshNeo.ShowOverview).
// Lock screen, notifications, quick settings and the power dialog stay Phosh's.
import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gdk from 'gi://Gdk?version=4.0';
import Graphene from 'gi://Graphene';
import GdkPixbuf from 'gi://GdkPixbuf';
import Gtk from 'gi://Gtk?version=4.0';
import Adw from 'gi://Adw?version=1';
import Gtk4LayerShell from 'gi://Gtk4LayerShell?version=1.0';
import {LayoutModel, installedApps, appName} from './model.js';
import {BackGesture} from './edges.js';
import {IconProvider} from './icons.js';
import {Sheet} from './sheet.js';
import {IconDrag} from './dnd.js';
import {Recents, collectSoon} from './recents.js';
import {HomeBar} from './homebar.js';

const APP_ID = 'de.yesman.NeoLauncher';
const HOME_XML = `<node><interface name="de.yesman.NeoLauncher.Home">
  <method name="Show"/>
  <method name="Hide"/>
  <property name="Visible" type="b" access="read"/>
</interface></node>`;
const ICON = 60, DOCK_ICON = 56;                     // logical px at Phosh's scale 2 (≈ Launcher3's 54 dp)
// landscape (Launcher3's vertical bar layout on a phone): the grid turns sideways, the dock becomes a column at the
// right edge and the home icons lose their names, or four rows would not fit in the 300 px the screen has left
const ICON_LAND = 52, DOCK_ICON_LAND = 48;
const GRAB = 28;                                     // px of the drawer above the finger while it is pulled up
const CATCH = 60;                                    // px of travel over which the drawer's top reaches the finger
const ENGAGE = 10;                                   // px of mostly vertical travel before a drag moves the drawer

function phoshNeo(method) {
    Gio.DBus.session.call('de.yesman.PhoshNeo', '/de/yesman/PhoshNeo', 'de.yesman.PhoshNeo', method,
        null, null, Gio.DBusCallFlags.NO_AUTO_START, -1, null, null);
}

const NeoLauncher = GObject.registerClass(class NeoLauncher extends Adw.Application {

    constructor() {
        super({application_id: APP_ID, flags: Gio.ApplicationFlags.DEFAULT_FLAGS});
        this.settings = new Gio.Settings({schema_id: 'de.yesman.neo'});
        this._dbus = Gio.DBusExportedObject.wrapJSObject(HOME_XML, this);
    }

    // de.yesman.NeoLauncher.Home
    get Visible() { return !!this.win?.get_visible(); }
    Show() {
        // Phosh calls as soon as the bus name appears, and GApplication owns the name before startup has built
        // the window: remember it and show once the window exists
        if (!this.win) { this._pendingShow = true; return; }
        // the home gesture while Neo already shows: back to the first page with the drawer closed (recents is
        // swipe up and hold, decided by Phosh)
        this.showHome();
    }
    Hide() { this.hideLauncher(); }

    vfunc_dbus_register(connection, path) {
        this._dbus.export(connection, path);
        return super.vfunc_dbus_register(connection, path);
    }
    vfunc_dbus_unregister(connection, path) {
        this._dbus.unexport_from_connection(connection);
        super.vfunc_dbus_unregister(connection, path);
    }

    vfunc_startup() {
        super.vfunc_startup();
        this.hold();                                 // resident: the window comes and goes, the service stays
        const css = new Gtk.CssProvider();
        css.load_from_path(GLib.build_filenamev([GLib.path_get_dirname(import.meta.url.replace('file://', '')), 'style.css']));
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION);
        Adw.StyleManager.get_default().color_scheme = Adw.ColorScheme.FORCE_DARK;
        this.icons = new IconProvider(this.settings);
        this._build();
        this._back = new BackGesture(this, () => this._handleBack());
        this.recents = new Recents(this);
        this.homeBar = new HomeBar(this, this.recents, () => this.showHome());
        if (this._pendingShow) { this._pendingShow = false; this.showHome(); }
        Gio.DBus.session.signal_subscribe('de.yesman.PhoshNeo', 'de.yesman.PhoshNeo', 'WindowShown', '/de/yesman/PhoshNeo',
            null, Gio.DBusSignalFlags.NONE, (_c, _s, _p, _i, _n, params) => this._onWindowShown(params.deepUnpack()[0]));
        Gio.DBus.session.signal_subscribe('org.gnome.Calls', 'org.freedesktop.DBus.ObjectManager', 'InterfacesAdded', '/org/gnome/Calls',
            null, Gio.DBusSignalFlags.NONE, (_c, _s, _p, _i, _n, params) => {
                const call = params.deepUnpack()[1]['org.gnome.Calls.Call'];
                if (call?.Inbound?.deepUnpack()) this._raiseIncomingCall();
            });
        this._monitor = Gio.AppInfoMonitor.get();
        // an install or upgrade changes the app list many times while its files land: an app missing for a moment
        // was taken as uninstalled and left the dock (the whole dock emptied while Yesman was installed). Reload once
        // the list has been quiet for 3 s.
        this._monitor.connect('changed', () => {
            if (this._appsTimer) GLib.source_remove(this._appsTimer);
            this._appsTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 3000, () => { this._appsTimer = 0; this._reload(); return GLib.SOURCE_REMOVE; });
        });
        for (const k of ['desktop-grid-columns', 'desktop-grid-rows', 'dock-num-icons', 'drawer-grid-columns', 'desktop-hide-app-labels',
            'icon-pack', 'icon-shape', 'icon-wrap-unthemed', 'icon-shape-legacy', 'hidden-from-home'])
            this.settings.connect(`changed::${k}`, () => this._reload(true));
    }

    vfunc_activate() { /* started by the session: stay hidden until Phosh asks */ }

    _build() {
        const app = this;
        const win = this.win = new Gtk.Window({application: this, title: 'Neo Launcher', decorated: false});
        Gtk4LayerShell.init_for_window(win);
        Gtk4LayerShell.set_namespace(win, 'neo-launcher');
        Gtk4LayerShell.set_layer(win, Gtk4LayerShell.Layer.TOP);
        for (const e of [Gtk4LayerShell.Edge.TOP, Gtk4LayerShell.Edge.BOTTOM, Gtk4LayerShell.Edge.LEFT, Gtk4LayerShell.Edge.RIGHT])
            Gtk4LayerShell.set_anchor(win, e, true);
        Gtk4LayerShell.set_exclusive_zone(win, 0);   // inside Phosh's top bar and home bar, not under them
        Gtk4LayerShell.set_keyboard_mode(win, Gtk4LayerShell.KeyboardMode.EXCLUSIVE);   // phoc speaks layer-shell v3: no on-demand focus; the launcher is full screen anyway
        win.add_css_class('neo');

        const overlay = new Gtk.Overlay();
        this.wallpaper = new Gtk.Picture({content_fit: Gtk.ContentFit.COVER, can_shrink: true});
        overlay.set_child(this.wallpaper);
        this._loadWallpaper();

        // the home screen underneath, the drawer as a sheet over it that follows the finger
        this.layers = new Gtk.Overlay({vexpand: true});
        overlay.add_overlay(this.layers);
        this._spotOverlay = overlay;                 // where the empty-space menu anchors
        // what size the window really got (an odd layout, icons spread out and the top row cut off, was seen after
        // the compositor configured the launcher with a zero size): one line per change, from an empty drawing
        // area spanning the window (its resize signal fires on allocation changes only)
        const probe = new Gtk.DrawingArea({can_target: false, hexpand: true, vexpand: true});
        probe.connect('resize', (_a, w, h) => {
            const geo = this._monitor0?.get_geometry();
            log(`neo-launcher: laid out at ${w}x${h} on a ${geo?.width}x${geo?.height} screen`);
        });
        overlay.add_overlay(probe);
        this.dragLayer = new Gtk.Fixed({can_target: false});   // the icon being moved floats here
        win.set_child(overlay);

        // home: pages + dots + dock
        const home = this._home = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, css_classes: ['neo-home']});
        const pages = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, hexpand: true, vexpand: true});
        this.carousel = new Adw.Carousel({vexpand: true, hexpand: true, allow_long_swipes: false});
        this.dots = new Adw.CarouselIndicatorDots({carousel: this.carousel, css_classes: ['neo-dots']});
        this.dock = new Gtk.Box({css_classes: ['neo-dock'], homogeneous: true, halign: Gtk.Align.FILL});
        pages.append(this.carousel); pages.append(this.dots);
        home.append(pages); home.append(this.dock);
        this.layers.set_child(home);

        // drawer: search + every app A→Z
        const drawer = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, css_classes: ['neo-drawer']});
        this._drawerBox = drawer;
        this.search = new Gtk.SearchEntry({placeholder_text: 'Search apps', css_classes: ['neo-search']});
        this.search.connect('search-changed', () => this._filterDrawer());
        this.search.connect('activate', () => { const first = this._drawerCells.find(c => c.get_visible()); if (first) this._launch(first._info); });
        this.drawerScroll = new Gtk.ScrolledWindow({vexpand: true, hscrollbar_policy: Gtk.PolicyType.NEVER});
        this.drawerGrid = new Gtk.FlowBox({selection_mode: Gtk.SelectionMode.NONE, homogeneous: true, valign: Gtk.Align.START,
            css_classes: ['neo-drawer-grid'], activate_on_single_click: true});
        this.drawerScroll.set_child(this.drawerGrid);
        // the drawer's icons fade in under a veil of the drawer's own colour: one rectangle per frame, where the
        // grid's opacity had GTK render all its icons off screen on every frame of the swipe (frames past 8.3 ms)
        const drawerBody = new Gtk.Overlay({vexpand: true});
        drawerBody.set_child(this.drawerScroll);
        this._drawerVeil = new Gtk.Box({css_classes: ['neo-drawer-veil'], can_target: false});
        drawerBody.add_overlay(this._drawerVeil);
        drawer.append(this.search); drawer.append(drawerBody);
        this.settings.bind('search-drawer-enabled', this.search, 'visible', Gio.SettingsBindFlags.GET);
        this.sheet = new Sheet(drawer);
        // the old Neo: the home screen's icons fade out as the drawer comes up, the drawer's fade in on the way
        // the GPU budget at 120 Hz has no room for a full-screen fade (a wallpaper veil or the grid's opacity:
        // 57 % of frames at 120 Hz against 94 % without): the home icons stay and the drawer's near-opaque
        // background covers them as it comes up (switching them off at once looked broken); only the drawer's
        // icons fade in, under a veil that spans the drawer
        this.sheet.onProgress = p => {
            const ease = (a, b) => { const t = Math.min(1, Math.max(0, (p - a) / (b - a))); return t * t * (3 - 2 * t); };
            const veil = 0.65 * (1 - ease(0, 0.85));          // dimmed from the start, never invisible
            this._drawerVeil.set_opacity(veil);
            this._drawerVeil.set_visible(veil > 0);
        };
        this.sheet.onProgress(this.sheet.progress);
        this.sheet.connect('settled', (_s, open) => { if (!open) this._resetDrawer(); else if (this.search.get_visible()) this.search.grab_focus(); });
        this.layers.add_overlay(this.sheet);

        overlay.add_overlay(this.dragLayer);
        this.iconDrag = new IconDrag({
            root: overlay, layer: this.dragLayer, carousel: this.carousel, dock: this.dock,
            get model() { return app.model; },
            get cols() { return app.gridCols; },
            get rows() { return app.gridRows; },
            get landscape() { return app._land; },
            emptyPage: () => this._emptyPage(),
            // a drag out of the drawer: the drawer goes, the home screen takes the icon
            leaveDrawer: () => this._closeDrawer(true),
            unhide: id => { const list = new Set(this.settings.get_strv('hidden-from-home')); if (list.delete(id)) this.settings.set_strv('hidden-from-home', [...list]); },
            icon: info => new Gtk.Image({gicon: this.icons.lookup(info, ICON, this._scale()).gicon, pixel_size: ICON}),
            menu: (button, info, where) => this._appMenu(button, info, where),
            onDrop: () => this._fillHome(),
        });
        this._gestures(home, drawer);
        const holdHome = new Gtk.GestureLongPress({touch_only: false});
        holdHome.connect('pressed', (g, x, y) => {
            g.set_state(Gtk.EventSequenceState.CLAIMED);
            const spot = new Gtk.Box({width_request: 1, height_request: 1, halign: Gtk.Align.START, valign: Gtk.Align.START,
                margin_start: Math.round(x), margin_top: Math.round(y)});
            this._spotOverlay.add_overlay(spot);
            this._homeMenu(spot);
            GLib.timeout_add(GLib.PRIORITY_DEFAULT, 30000, () => { this._spotOverlay.remove_overlay(spot); return GLib.SOURCE_REMOVE; });
        });
        home.add_controller(holdHome);
        const keys = new Gtk.EventControllerKey();
        keys.connect('key-pressed', (_c, keyval) => {
            if (keyval !== Gdk.KEY_Escape) return false;
            if (this.sheet.isOpen) this._closeDrawer(); else this.hideLauncher();
            return true;
        });
        win.add_controller(keys);
        // the screen turned (Phosh's auto-rotate): lay the home screen out for the new shape
        this._monitor0 = Gdk.Display.get_default().get_monitors().get_item(0);
        this._monitor0?.connect('notify::geometry', () => this._syncOrientation());
        // the window's new size comes with the configure, before GTK lays it out: switch in that same frame
        const watchSurface = () => {
            const surf = win.get_surface();
            for (const p of ['notify::width', 'notify::height']) surf?.connect(p, () => this._syncOrientation());
        };
        if (win.get_realized()) watchSurface(); else win.connect('realize', watchSurface);
        this._land = this._isLandscape();
        this._applyOrientation();
        this._reload(true);
    }

    /** The window's own shape first: its new size arrives before the monitor's, and laid out by the monitor's the
     *  old layout was drawn stretched into the new size for a frame (the "deformed" launcher). */
    _isLandscape() {
        const surf = this.win?.get_surface();
        if (surf && surf.get_width() > 0 && surf.get_height() > 0) return surf.get_width() > surf.get_height();
        const geo = this._monitor0?.get_geometry();
        return !!geo && geo.width > geo.height;
    }

    /** The screen in logical px, its long side across in landscape (whichever of Gdk's notifications came first). */
    _screenSize() {
        const geo = (this._monitor0 ?? Gdk.Display.get_default().get_monitors().get_item(0))?.get_geometry();
        const a = geo?.width || 360, b = geo?.height || 800;
        const long = Math.max(a, b), short = Math.min(a, b);
        return (this._land ?? a > b) ? [long, short] : [short, long];
    }

    /** The home grid as drawn: the saved cols x rows, turned sideways in landscape. A page keeps its icons in the
     *  same reading order, so nothing moves in the saved layout and portrait looks as it did. */
    get gridCols() { return this._land ? this.model.rows : this.model.cols; }
    get gridRows() { return this._land ? this.model.cols : this.model.rows; }

    _syncOrientation() {
        const land = this._isLandscape();
        if (land === this._land) return;
        this._land = land;
        this._applyOrientation();
        this._loadWallpaper();                       // cropped to the screen's shape (made ahead, see there)
        this._fillHome(); this._drawerColumns();     // the drawer keeps its icons, only its columns change
    }

    _applyOrientation() {
        const land = this._land;
        this._home.orientation = land ? Gtk.Orientation.HORIZONTAL : Gtk.Orientation.VERTICAL;
        this.dock.orientation = land ? Gtk.Orientation.VERTICAL : Gtk.Orientation.HORIZONTAL;
        this.dock.halign = land ? Gtk.Align.END : Gtk.Align.FILL;
        this.dock.valign = land ? Gtk.Align.FILL : Gtk.Align.END;
        if (land) this.win.add_css_class('neo-landscape'); else this.win.remove_css_class('neo-landscape');
    }

    /** A vertical drag that moves the drawer sheet: `sign` -1 pulls it up (home), +1 down (drawer at its top). */
    _sheetDrag(widget, sign, canStart) {
        const drag = new Gtk.GestureDrag({propagation_phase: sign > 0 ? Gtk.PropagationPhase.CAPTURE : Gtk.PropagationPhase.BUBBLE});
        let engaged = false, from = 0, samples = [], startY = 0, gap = 0, at = 0, caught = 0;
        // The finger in the window's own coordinates: the drawer itself moves with the finger, and offsets measured
        // in its moving coordinates fed back into the next frame (the sheet jumped up and down)
        const rootY = g => {
            const [, x, y] = g.get_point(g.get_current_sequence());   // touch needs its sequence (null = the pointer)
            const [ok, p] = widget.compute_point(this.layers, new Graphene.Point({x, y}));
            return ok ? p.y : y;
        };
        drag.connect('drag-begin', g => { engaged = false; samples = []; startY = rootY(g); });
        drag.connect('drag-update', (g, dx, dyLocal) => {
            const dy = rootY(g) - startY;
            if (!engaged) {
                if (Math.abs(dy) < ENGAGE || Math.abs(dy) < Math.abs(dx) || Math.sign(dy) !== sign || !canStart()) return;
                engaged = true; from = this.sheet.progress;
                g.set_state(Gtk.EventSequenceState.CLAIMED);
                // opening: the drawer's top comes up to the finger (Launcher3) instead of trailing it from the
                // bottom edge; the gap closes over the first CATCH px so it does not jump
                const h0 = Math.max(1, this.layers.get_height());
                at = dy; caught = 0;
                gap = sign < 0 ? Math.max(0, (1 - from) * h0 - (startY + dy - GRAB)) : 0;
            }
            const h = Math.max(1, this.layers.get_height());
            if (sign < 0) {
                const finger = startY + dy - GRAB;                       // where the drawer's top belongs
                // by the furthest the finger has gone: measured from the start alone, the gap came back as the
                // finger returned near it, and the drawer fell away from the finger at the bottom of the screen
                caught = Math.max(caught, Math.abs(dy - at));
                const left = gap * Math.max(0, 1 - caught / CATCH);
                this.sheet.setProgress(1 - (finger + left) / h);
            } else {
                this.sheet.setProgress(from - dy / h);
            }
            samples.push([GLib.get_monotonic_time() / 1000, dy]);
            if (samples.length > 6) samples.shift();
        });
        // another gesture took the touch (a dock or home icon's hold): GTK cancels this one instead of ending it,
        // and the drawer stayed wherever it was, no longer under the finger: settle it
        drag.connect('cancel', () => {
            if (!engaged) return;
            engaged = false;
            log(`neo-launcher: drawer drag cancelled at ${this.sheet.progress.toFixed(2)}`);
            this.sheet.release(0);
        });
        drag.connect('drag-end', () => {
            if (!engaged) return;
            engaged = false;
            const recent = samples.filter(([t]) => t >= samples.at(-1)[0] - 80);
            const [t0, y0] = recent[0], [t1, y1] = recent.at(-1);
            this.sheet.release(t1 > t0 ? (y1 - y0) / (t1 - t0) : 0);
        });
        widget.add_controller(drag);
    }

    _gestures(home, drawer) {
        // swipe up on the home screen pulls the drawer up; the carousel keeps the horizontal swipes
        this._sheetDrag(home, -1, () => true);
        // swipe down in the drawer, while its list is at the top, pulls it back down
        this._sheetDrag(drawer, 1, () => this.drawerScroll.get_vadjustment().get_value() <= 0);
    }

    _loadWallpaper() {
        const bg = this._bgSettings ??= new Gio.Settings({schema_id: 'org.gnome.desktop.background'});
        if (!this._bgWatch) {
            // a new wallpaper (Settings, the launcher menu) shows at once, as on the lock screen
            this._bgWatch = bg.connect('changed', (_s, key) => {
                if (key.startsWith('picture-uri') || key === 'picture-options') this._loadWallpaper();
            });
        }
        // both shapes of the screen: the one it has now, and the turned one made while idle, so that a rotation only
        // swaps the picture (decoding and scaling it took the turn most of a second)
        const scale = this._scale(), [w, h] = this._screenSize();
        const W = Math.round(w * scale), H = Math.round(h * scale);
        const tex = this._wallTexture(bg, W, H);
        if (tex) {
            this.wallpaper.set_paintable(tex);
            this.recents?.wall?.set_paintable(tex);
        }
        GLib.idle_add(GLib.PRIORITY_LOW, () => { this._wallTexture(bg, H, W); return GLib.SOURCE_REMOVE; });
    }

    /** The wallpaper cropped and scaled to W x H device px, made once per shape and kept. */
    _wallTexture(bg, W, H) {
        for (const key of ['picture-uri-dark', 'picture-uri']) {
            const uri = bg.get_string(key);
            if (!uri) continue;
            const id = `${uri} ${bg.get_string('picture-options')}`;
            if (this._wallId !== id) { this._wallId = id; this._wallCache = new Map(); }
            const done = this._wallCache.get(`${W}x${H}`);
            if (done) return done;
            try {
                // decoded in full and scaled once to the screen's pixels with the best filter: the source (4K and
                // more) resampled on every frame cost page swipes their 120 Hz, and the decoder's own scaled
                // decode left the picture with a fraction of its colours (banding the lock screen does not show)
                let path = Gio.File.new_for_uri(uri).get_path();
                if (path.endsWith('.xml')) path = this._xmlWallpaper(path, W / H);
                if (!path) continue;
                // a vector picture is drawn straight at the size the screen needs
                const pb = path.endsWith('.svg')
                    ? (() => {
                        const probe = GdkPixbuf.Pixbuf.get_file_info(path);
                        const [, iw, ih] = probe;
                        return iw / ih > W / H ? GdkPixbuf.Pixbuf.new_from_file_at_scale(path, -1, H, true)
                            : GdkPixbuf.Pixbuf.new_from_file_at_scale(path, W, -1, true);
                    })()
                    : GdkPixbuf.Pixbuf.new_from_file(path);
                const k = Math.max(W / pb.get_width(), H / pb.get_height());     // cover
                const cw = Math.min(pb.get_width(), Math.round(W / k)), ch = Math.min(pb.get_height(), Math.round(H / k));
                const crop = pb.new_subpixbuf(Math.floor((pb.get_width() - cw) / 2), Math.floor((pb.get_height() - ch) / 2), cw, ch);
                const scaled = crop.scale_simple(W, H, GdkPixbuf.InterpType.HYPER);
                const tex = Gdk.Texture.new_for_pixbuf(scaled);
                this._wallCache.set(`${W}x${H}`, tex);
                return tex;
            } catch (e) { log(`neo-launcher: wallpaper ${uri}: ${e.message}`); }
        }
        return null;
    }

    /** The picture a GNOME background XML (desktop-base's themes) names: the first file of its first slide, of
     *  its sizes the one whose shape is nearest the screen's (`aspect` = width / height). */
    _xmlWallpaper(path, aspect) {
        let xml;
        try { xml = new TextDecoder().decode(GLib.file_get_contents(path)[1]); } catch (_) { return null; }
        const file = xml.match(/<file>([\s\S]*?)<\/file>/)?.[1];
        if (!file) return null;
        const sizes = [...file.matchAll(/<size\s+width="(\d+)"\s+height="(\d+)"\s*>\s*([^<]+?)\s*<\/size>/g)]
            .map(([, w, h, p]) => ({p, d: Math.abs(Math.log((w / h) / aspect))}));
        if (sizes.length) return sizes.sort((a, b) => a.d - b.d)[0].p;
        return file.trim() || null;
    }

    _reload(rebuild = false) {
        this.apps = installedApps();
        const cols = this.settings.get_int('desktop-grid-columns'), rows = this.settings.get_int('desktop-grid-rows');
        const dockSize = this.settings.get_int('dock-num-icons');
        if (rebuild || !this.model || this.model.cols !== cols || this.model.rows !== rows) {
            this.model = new LayoutModel({cols, rows, dockSize});
            this.model.hidden = new Set(this.settings.get_strv('hidden-from-home'));
            this.model.load(this.apps);
        } else {
            this.model.hidden = new Set(this.settings.get_strv('hidden-from-home'));
            this.model.reconcile(this.apps);
        }
        this._fillHome(); this._fillDrawer();
        collectSoon();          // the old pages' icons and widgets
    }

    /** Pack/shape icons are drawn one per idle tick, so the launcher stays usable while a new pack renders. */
    _queueIcon(job, image) {
        (this._iconQueue ??= []).push([job, image]);
        if (this._iconIdle) return;
        this._iconIdle = GLib.idle_add(GLib.PRIORITY_LOW, () => {
            const next = this._iconQueue.shift();
            if (!next) { this._iconIdle = 0; return GLib.SOURCE_REMOVE; }
            const [j, img] = next;
            if (img.get_root()) { const gicon = this.icons.render(j); if (gicon) img.set_from_gicon(gicon); }
            return GLib.SOURCE_CONTINUE;
        });
    }

    /** The display's scale (the window's own reads 1 until it is mapped, and icons would render blurred). */
    _scale() {
        const monitor = Gdk.Display.get_default().get_monitors().get_item(0);
        return monitor?.get_scale_factor() ?? 1;
    }

    _cell(info, size, {label = true, where = 'home', pos = null} = {}) {
        const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, spacing: 4, halign: Gtk.Align.CENTER, valign: Gtk.Align.START});
        const {gicon, job} = this.icons.lookup(info, size, this._scale());
        const image = new Gtk.Image({gicon, pixel_size: size, css_classes: ['neo-icon']});
        if (job) this._queueIcon(job, image);
        box.append(image);
        // the drawer's flow box lays out by the cells' natural width: longer labels pushed it from 4 columns to 3
        if (label) box.append(new Gtk.Label({label: appName(info), ellipsize: 3, max_width_chars: where === 'drawer' ? 8 : 10,
            css_classes: ['neo-label']}));
        // the press highlight hugs the icon and its name, not the whole grid cell below them
        const button = new Gtk.Button({child: box, css_classes: ['flat', 'neo-cell'], tooltip_text: appName(info),
            halign: Gtk.Align.CENTER, valign: Gtk.Align.START});
        button._info = info;
        button._pos = pos;                       // {dock: i} or {page, index}: which of the app's icons this is
        button.connect('clicked', () => this._launch(info, button));
        // hold and move to place it (from the drawer: a copy onto the home screen, as on Android), hold and let go
        // for the menu, tap to launch
        this.iconDrag.attach(button, info, where);
        return button;
    }

    _emptyPage() {
        const grid = new Gtk.Grid({column_homogeneous: true, row_homogeneous: true, hexpand: true, vexpand: true, css_classes: ['neo-page']});
        const cols = this.gridCols;
        for (let i = 0; i < this.model.perPage; i++) grid.attach(new Gtk.Box(), i % cols, Math.floor(i / cols), 1, 1);
        return grid;
    }

    _fillHome() {
        this._iconQueue = [];
        const hide = this.settings.get_boolean('desktop-hide-app-labels') || this._land;
        const cols = this.gridCols, icon = this._land ? ICON_LAND : ICON, dockIcon = this._land ? DOCK_ICON_LAND : DOCK_ICON;
        while (this.carousel.get_n_pages() > 0) this.carousel.remove(this.carousel.get_nth_page(0));
        this.model.pages.forEach((page, pi) => {
            const grid = new Gtk.Grid({column_homogeneous: true, row_homogeneous: true, hexpand: true, vexpand: true, css_classes: ['neo-page']});
            page.forEach((id, i) => {
                const info = this.apps.get(id);
                if (info) grid.attach(this._cell(info, icon, {label: !hide, pos: {page: pi, index: i}}), i % cols, Math.floor(i / cols), 1, 1);
            });
            // keep the grid's shape on a page that is not full
            for (let i = page.length; i < this.model.perPage; i++) grid.attach(new Gtk.Box(), i % cols, Math.floor(i / cols), 1, 1);
            this.carousel.append(grid);
        });
        this.dots.set_visible(this.model.pages.length > 1);
        let child; while ((child = this.dock.get_first_child())) this.dock.remove(child);
        this.model.dock.forEach((id, i) => { const info = this.apps.get(id); if (info) this.dock.append(this._cell(info, dockIcon, {label: false, where: 'dock', pos: {dock: i}})); });
    }

    /** Landscape: as many columns as the wider screen holds at the portrait column width. */
    _drawerColumns() {
        const n = this.settings.get_int('drawer-grid-columns'), [w, h] = this._screenSize();
        this.drawerGrid.max_children_per_line = this.drawerGrid.min_children_per_line =
            this._land ? Math.max(n, Math.round(n * w / h)) : n;
    }

    _fillDrawer() {
        this.drawerGrid.remove_all();
        this._drawerColumns();
        this._drawerCells = [...this.apps.values()]
            .sort((a, b) => appName(a).localeCompare(appName(b)))
            .map(info => this._cell(info, ICON, {where: 'drawer'}));
        for (const c of this._drawerCells) this.drawerGrid.append(c);
    }

    _filterDrawer() {
        const q = this.search.get_text().trim().toLowerCase();
        for (const c of this._drawerCells) {
            const i = c._info;
            const hay = [appName(i), i.get_name(), i.get_description?.() ?? '', i.get_executable?.() ?? ''].join(' ').toLowerCase();
            c.get_parent().set_visible(!q || hay.includes(q));    // the FlowBoxChild
        }
    }

    /** The back gesture while Neo shows: clear the search, then close the drawer. True = handled here. */
    _handleBack() {
        if (this._settingsWin()) { this._settingsWin().close(); this.showHome(); return true; }
        if (!this.win?.get_visible()) return false;
        if (this.sheet.progress > 0) {
            if (this.search.get_text()) this.search.set_text(''); else this._closeDrawer();
        }
        return true;
    }

    _menu(anchor, items) {
        const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, css_classes: ['neo-menu']});
        const pop = new Gtk.Popover({child: box, has_arrow: true, autohide: true});
        for (const [label, icon, action] of items) {
            const row = new Gtk.Box({spacing: 12});
            row.append(new Gtk.Image({icon_name: icon}));
            row.append(new Gtk.Label({label, xalign: 0, hexpand: true}));
            const b = new Gtk.Button({child: row, css_classes: ['flat', 'neo-menu-item']});
            b.connect('clicked', () => { pop.popdown(); action(); });
            box.append(b);
        }
        pop.set_parent(anchor);
        pop.connect('closed', () => GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => { pop.unparent(); return GLib.SOURCE_REMOVE; }));
        pop.popup();
    }

    _appMenu(anchor, info, where) {
        const id = info.get_id();
        const items = [['App info', 'help-about-symbolic', () => this._run(['gnome-control-center', 'applications', id.replace(/\.desktop$/, '')])]];
        const onHome = this.model.dock.includes(id) || this.model.pages.flat().includes(id);
        if (where === 'drawer') {
            // from the drawer: another icon on the home screen, also for an app the dock or a page has already
            items.push(['Add to home screen', 'list-add-symbolic', () => {
                if (!onHome) { this._setHidden(id, false); return; }
                this.model.addShortcut(id);
                this._reload(true);
            }]);
        } else {
            // this icon only; the app's last icon hides it from the home screen (the drawer keeps it)
            items.push(['Remove', 'list-remove-symbolic', () => {
                if (this.model.removeOne(id, anchor._pos)) this._reload(true);
                else this._setHidden(id, true);
            }]);
        }
        this._menu(anchor, items);
    }

    _homeMenu(anchor) {
        this._menu(anchor, [
            ['Launcher settings', 'emblem-system-symbolic', () => this._openSettings()],
            ['Wallpaper', 'preferences-desktop-wallpaper-symbolic', () => this._run(['gnome-control-center', 'background'])],
        ]);
    }

    _setHidden(id, hidden) {
        const list = new Set(this.settings.get_strv('hidden-from-home'));
        if (hidden) list.add(id); else list.delete(id);
        this.settings.set_strv('hidden-from-home', [...list]);       // changed:: rebuilds the home screen
    }

    _run(argv) {
        try { Gio.Subprocess.new(argv, Gio.SubprocessFlags.NONE); } catch (e) { logError(e, `neo-launcher: ${argv[0]}`); return; }
        this.hideLauncher();
    }

    async _openSettings() {
        const {showSettings} = await import('./settings.js');
        this.hideLauncher();
        showSettings(this);
    }

    /** The launcher settings window while it is open: Neo's own, so back and home have to close it. */
    _settingsWin() { return this.get_windows().find(w => w.title === 'Launcher settings') ?? null; }

    _openDrawer() { this.sheet.settle(true); }
    _closeDrawer(animate = true) { this.sheet.settle(false, animate); if (!animate) this._resetDrawer(); }
    _resetDrawer() {
        this.search.set_text('');
        this.drawerScroll.get_vadjustment().set_value(0);
    }

    _launch(info, from = null) {
        if (from && this.win.get_visible()) this._launchCard(info, from);
        // an app that already runs only presents its window on a second launch, and Phosh raises no window on
        // such a request: switch to its window instead (the newest one when it has several)
        const appId = info.get_id().replace(/\.desktop$/, '');
        Gio.DBus.session.call('de.yesman.PhoshNeo', '/de/yesman/PhoshNeo', 'de.yesman.PhoshNeo', 'GetWindows',
            null, null, Gio.DBusCallFlags.NO_AUTO_START, 1000, null, (c, res) => {
                let windows = [];
                try { windows = c.call_finish(res).deepUnpack()[0]; } catch (_) { /* Phosh without Neo's API */ }
                const running = windows.filter(w => w[1] === appId || w[1].toLowerCase() === appId.toLowerCase()).at(-1);
                log(`neo-launcher: launch ${appId}: ${windows.length} windows, running ${running?.[0] ?? 'no'}`);
                if (running) {
                    Gio.DBus.session.call('de.yesman.PhoshNeo', '/de/yesman/PhoshNeo', 'de.yesman.PhoshNeo', 'Activate',
                        new GLib.Variant('(s)', [running[0]]), null, Gio.DBusCallFlags.NO_AUTO_START, -1, null, null);
                } else {
                    try {
                        this._spawnApp(info);
                    } catch (e) { logError(e, `neo-launcher: launching ${info.get_id()}`); this._finishLaunch(); this.showHome(); return; }
                }
                if (!this._launching) this.hideLauncher();
            });
    }

    /** Starts the app in a scope of its own: launched directly it lived in neo-launcher.service, and restarting
     *  Neo killed every app opened from it. `gio launch` handles Exec lines and D-Bus activation alike. */
    _spawnApp(info) {
        const file = info.get_filename?.();
        // D-Bus activated apps (Console, Camera, …) are started by the bus, not by Neo, and only show a window on
        // the Activate call that follows: `gio launch` exited before that call went out, so they started windowless
        // and quit. Neo makes the call itself.
        const dbusActivatable = info.get_boolean?.('DBusActivatable');
        if (!file || dbusActivatable || !GLib.find_program_in_path('systemd-run')) {
            info.launch([], Gdk.Display.get_default().get_app_launch_context());
            return;
        }
        const id = info.get_id().replace(/\.desktop$/, '').replace(/[^A-Za-z0-9_.]/g, '_');
        // An Android app (Waydroid writes waydroid.<package>.desktop): full screen in a window of its own.
        // Waydroid's own `app launch` makes a small freeform window with Android's caption bar.
        const pkg = /^waydroid\.(.+)$/.exec(id)?.[1];
        const command = pkg && GLib.find_program_in_path('waydroid-open') ? ['waydroid-open', pkg] : ['gio', 'launch', file];
        const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.NONE});
        // systemd hands neo-launcher.service its own memory.pressure to watch; an app inheriting that watched
        // Neo's cgroup, and GLib's PSI monitor spun at 90 % CPU in Compass while memory stayed tight. Apps get
        // GLib's polling monitor instead.
        launcher.unsetenv('MEMORY_PRESSURE_WATCH');
        launcher.unsetenv('MEMORY_PRESSURE_WRITE');
        launcher.setenv('GIO_USE_MEMORY_MONITOR', 'poll', true);
        launcher.spawnv(['systemd-run', '--user', '--scope', '--collect', '--quiet', '--slice=app.slice',
            `--unit=app-neo-${id}-${GLib.get_monotonic_time()}`, ...command]);
    }

    /**
     * Android's app launch: a card grows from the icon to the whole screen and stays until the app's window is on
     * screen (Phosh's WindowShown), so the app opened before never flashes up in between. Neo lets go of the
     * keyboard meanwhile, or the new window could not take the focus.
     */
    _launchCard(info, from) {
        this._finishLaunch();
        const [ok, b] = from.compute_bounds(this._spotOverlay);
        const W = this._spotOverlay.get_width(), H = this._spotOverlay.get_height();
        const r0 = ok ? {x: b.get_x(), y: b.get_y(), w: b.get_width(), h: b.get_height()} : {x: W / 2 - 30, y: H / 2 - 30, w: 60, h: 60};
        const card = new Gtk.Box({css_classes: ['neo-launch-card'], overflow: Gtk.Overflow.HIDDEN});
        const middle = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, spacing: 24, hexpand: true, vexpand: true,
            halign: Gtk.Align.CENTER, valign: Gtk.Align.CENTER});
        middle.append(new Gtk.Image({gicon: this.icons.lookup(info, ICON, this._scale()).gicon, pixel_size: ICON}));
        // an app that takes its time (an Android one, above all when Android itself has to start) shows that it is
        // still coming: the card alone looked like nothing happened, and the launcher came back before the app
        const spinner = new Gtk.Spinner({spinning: true, width_request: 28, height_request: 28, opacity: 0});
        middle.append(spinner);
        card.append(middle);
        this.dragLayer.put(card, r0.x, r0.y);
        card.set_size_request(Math.round(r0.w), Math.round(r0.h));
        Gtk4LayerShell.set_keyboard_mode(this.win, Gtk4LayerShell.KeyboardMode.NONE);
        const launch = this._launching = {card, shown: false, done: false, appId: info.get_id().replace(/\.desktop$/, '').toLowerCase()};
        launch.spinnerTimeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
            launch.spinnerTimeout = 0;
            spinner.set_opacity(1);
            return GLib.SOURCE_REMOVE;
        });
        const target = Adw.CallbackAnimationTarget.new(t => {
            const lerp = (a, z) => a + (z - a) * t;
            this.dragLayer.move(card, lerp(r0.x, 0), lerp(r0.y, 0));
            card.set_size_request(Math.round(lerp(r0.w, W)), Math.round(lerp(r0.h, H)));
        });
        launch.anim = new Adw.TimedAnimation({widget: card, value_from: 0, value_to: 1, duration: 260,
            easing: Adw.Easing.EASE_OUT_CUBIC, target});
        launch.anim.connect('done', () => { launch.done = true; if (launch.shown) this._finishLaunch(); });
        launch.anim.play();
        // an Android app's first window can take long: Waydroid's session may still be starting
        const patience = launch.appId.startsWith('waydroid.') ? 30000 : 6000;
        launch.timeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT, patience, () => { launch.timeout = 0; this._finishLaunch(); return GLib.SOURCE_REMOVE; });
    }

    /** A window is on screen (Phosh): end the launch once the card has grown. */
    _onWindowShown(appId) {
        const launch = this._launching;
        if (!launch) return;
        // giving up the keyboard re-activates the app shown before, and Phosh reports that one too: wait for ours
        const a = (appId ?? '').toLowerCase(), b = launch.appId;
        if (!a || !(a === b || a.includes(b) || b.includes(a))) return;
        launch.shown = true;
        if (launch.done) this._finishLaunch();
    }

    /** gnome-calls presents its window on an incoming call, but phoc does not focus a window that asks without an
     *  activation token: it mapped under Neo (or under the app on screen) and the call rang with no way to answer
     *  it short of opening Calls by hand. Bring the newest Calls window up, as Android's full-screen intent does.
     *  The call window maps a moment after the call appears on the bus (and the main window may already be up), so
     *  raise the newest Calls window a few times over two seconds: the last one raised is the call window. */
    _raiseIncomingCall(tries = 0) {
        Gio.DBus.session.call('de.yesman.PhoshNeo', '/de/yesman/PhoshNeo', 'de.yesman.PhoshNeo', 'GetWindows',
            null, null, Gio.DBusCallFlags.NO_AUTO_START, 1000, null, (c, res) => {
                let windows = [];
                try { windows = c.call_finish(res).deepUnpack()[0]; } catch (_) { /* Phosh without Neo's API */ }
                const calls = windows.filter(w => w[1].toLowerCase() === 'org.gnome.calls');
                if (tries < 6)
                    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 350, () => { this._raiseIncomingCall(tries + 1); return GLib.SOURCE_REMOVE; });
                if (!calls.length) return;
                if (tries === 0 || calls.at(-1)[0] !== this._callWin) log(`neo-launcher: incoming call: raising ${calls.at(-1)[0]}`);
                this._callWin = calls.at(-1)[0];
                this._finishLaunch();
                this.recents?.hide();
                this.hideLauncher();
                Gio.DBus.session.call('de.yesman.PhoshNeo', '/de/yesman/PhoshNeo', 'de.yesman.PhoshNeo', 'Activate',
                    new GLib.Variant('(s)', [calls.at(-1)[0]]), null, Gio.DBusCallFlags.NO_AUTO_START, -1, null, null);
            });
    }

    _finishLaunch() {
        const launch = this._launching;
        if (!launch) return;
        this._launching = null;
        if (launch.timeout) GLib.source_remove(launch.timeout);
        if (launch.spinnerTimeout) GLib.source_remove(launch.spinnerTimeout);
        launch.anim?.skip();
        this.hideLauncher();
        this.dragLayer.remove(launch.card);
    }

    showHome() {
        this._finishLaunch();
        // the home screen is back: what the app's launch and the task view left behind is collected now (the engine
        // sees only small wrappers around icons and pictures and left 190 MB uncollected in 20 minutes, 2026-10-14)
        collectSoon();
        // the first opening uploaded every drawer icon to the GPU mid-swipe (frames of 30-120 ms): the first time
        // the home screen shows, draw the drawer once at no visible height, so the textures are there
        if (!this._drawerWarm) {
            this._drawerWarm = true;
            GLib.timeout_add(GLib.PRIORITY_LOW, 400, () => {
                // rendered into a texture nobody shows, with the window's own renderer (its texture cache)
                try {
                    const renderer = this.win.get_renderer();
                    const w = this.layers.get_width(), h = this.layers.get_height();
                    if (renderer && w > 0 && h > 0) {
                        const snap = new Gtk.Snapshot();
                        new Gtk.WidgetPaintable({widget: this._drawerBox}).snapshot(snap, w, h);
                        const node = snap.to_node();
                        if (node) renderer.render_texture(node, null);
                    }
                } catch (e) { logError(e, 'neo-launcher: drawer warm-up'); }
                return GLib.SOURCE_REMOVE;
            });
        }
        this._settingsWin()?.close();
        Gtk4LayerShell.set_keyboard_mode(this.win, Gtk4LayerShell.KeyboardMode.EXCLUSIVE);
        // home while Neo shows: slide back to the first page (and the drawer down); coming from an app, be there
        const shown = this.win.get_visible();
        this._closeDrawer(shown);
        this.carousel.scroll_to(this.carousel.get_nth_page(0), shown);
        this.win.present();
        this._dbus.emit_property_changed('Visible', new GLib.Variant('b', true));
    }

    hideLauncher() {
        if (!this.win?.get_visible()) return;
        this.win.set_visible(false);
        this._dbus.emit_property_changed('Visible', new GLib.Variant('b', false));
    }
});

new NeoLauncher().run([imports.system.programInvocationName, ...ARGV]);
