#!/usr/bin/env -S gjs -m
// Neo Browser: Chrome for Android's phone UI (dark) over WebKitGTK 6. GJS, GTK 4 and libadwaita, no Qt.
// Toolbar (home, address pill, tab count, menu) • tab switcher grid • menu sheet • address entry with suggestions •
// new-tab page • history / bookmarks / downloads pages • find in page • long-press link sheet • ad blocking.
// Session, history and bookmarks are JSON under ~/.local/share/de.yesman.NeoBrowser (the Qt browser's history and
// bookmarks are copied over at the first start); downloads go to ~/Downloads.
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk?version=4.0';
import Gdk from 'gi://Gdk?version=4.0';
import Adw from 'gi://Adw?version=1';
import Graphene from 'gi://Graphene';
import Soup from 'gi://Soup?version=3.0';
import WebKit from 'gi://WebKit?version=6.0';
import System from 'system';

const APP_ID = 'de.yesman.NeoBrowser';
const HERE = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const DATA = GLib.build_filenamev([GLib.get_user_data_dir(), APP_ID]);
const CACHE = GLib.build_filenamev([GLib.get_user_cache_dir(), APP_ID]);
const OLD_DATA = GLib.build_filenamev([GLib.get_user_data_dir(), 'neobrowser']);
const DOWNLOADS = GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_DOWNLOAD) || GLib.build_filenamev([GLib.get_home_dir(), 'Downloads']);
const HISTORY_MAX = 2000;
// The engine is WebKit, so the page is told so: an iPhone's Safari gets every site's mobile layout and no
// "your browser is not supported" for a Chrome name over a WebKit engine. Desktop mode is Safari on a Mac.
const MOBILE_UA = 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1';
const DESKTOP_UA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15';
const SEARCH_URL = 'https://www.google.com/search?q=';
// Ad blocking: EasyList in WebKit's content-blocker format, compiled once into WebKit's filter store (the network
// process then blocks without asking us), fetched again once a week. A copy beside the app covers the first start.
const FILTER_URL = 'https://easylist-downloads.adblockplus.org/easylist_min_content_blocker.json';
const FILTER_ID = 'easylist';
const FILTER_MAX_AGE = 7 * 24 * 3600;

// Android's scrollbar: a thin rounded thumb over the page, no track and no arrow buttons
const SCROLLBAR_CSS =
    '::-webkit-scrollbar{width:5px!important;height:5px!important;background:transparent!important}' +
    '::-webkit-scrollbar-track,::-webkit-scrollbar-corner{background:transparent!important}' +
    '::-webkit-scrollbar-button{display:none!important;width:0!important;height:0!important}' +
    '::-webkit-scrollbar-thumb{background:rgba(128,128,128,.55)!important;border-radius:3px!important;border:none!important}';

const CSS = `
window.neo, .neo-bg { background: #121316; color: #e3e3e6; }
.neo-bar { background: #1b1c1f; min-height: 56px; padding: 0 4px; }
.neo-bar.incognito, .ntp.incognito { background: #3c3f45; }
.ntp { background: #121316; }
button.neo-icon { min-width: 40px; min-height: 40px; padding: 4px; margin: 4px; border-radius: 999px; background: none; box-shadow: none; }
button.neo-icon:active { background: alpha(#e3e3e6, 0.12); }
button.neo-icon:disabled { opacity: 0.38; }
button.pill { background: #2e3035; border-radius: 20px; min-height: 40px; padding: 0 14px 0 12px; font-weight: normal; font-size: 16px; }
.pill-box { background: #2e3035; border-radius: 20px; min-height: 40px; padding: 0 0 0 14px; }
.tabcount { border: 2px solid #e3e3e6; border-radius: 5px; min-width: 18px; min-height: 18px; font-weight: bold; font-size: 12px; }
.tabcount.small { font-size: 10px; }
progressbar.neo-progress trough { min-height: 2px; background: #3a3c41; border-radius: 0; }
progressbar.neo-progress progress { min-height: 2px; background: #a8c7fa; border-radius: 0; }
entry.neo-entry, entry.neo-entry:focus-within { background: none; border: none; box-shadow: none; outline: none; min-height: 40px; font-size: 16px; color: #e3e3e6; padding: 0; }
.sheet { background: #2b2d31; border-radius: 24px; padding: 8px 0; }
.sheet.bottom { border-radius: 24px 24px 0 0; padding: 16px 0 8px 0; }
.scrim { background: alpha(black, 0.5); }
.scrim-clear { background: transparent; }
button.menu-row { border-radius: 0; min-height: 52px; padding: 0 20px; font-size: 16px; font-weight: normal; background: none; box-shadow: none; }
button.menu-row:active { background: alpha(#e3e3e6, 0.08); }
.menu-top button { background: #3b3d42; border-radius: 26px; min-width: 52px; min-height: 52px; margin: 4px 2px; }
.menu-top button:disabled { opacity: 0.38; }
.divider { background: #3a3c41; min-height: 1px; }
.ntp-title { font-size: 44px; font-weight: 500; }
button.search-pill { background: #2e3035; border-radius: 26px; min-height: 52px; padding: 0 18px; color: #a9abb1; font-size: 16px; font-weight: normal; }
.site-dot { background: #2e3035; border-radius: 24px; min-width: 48px; min-height: 48px; font-size: 20px; }
.site-name { font-size: 12px; }
button.site { background: none; box-shadow: none; padding: 4px 0; }
.dim { color: #a9abb1; }
.url { color: #a8c7fa; font-size: 13px; }
.sub { color: #a9abb1; font-size: 13px; }
.title-big { font-size: 20px; }
.row-dot { background: #2e3035; border-radius: 20px; min-width: 40px; min-height: 40px; font-size: 16px; }
list.neo-list, list.neo-list row { background: none; }
list.neo-list row { min-height: 60px; padding: 0 8px 0 16px; }
list.neo-list row:active { background: alpha(#e3e3e6, 0.08); }
.switcher { background: #1b1c20; }
button.fab { background: #a8c7fa; border-radius: 16px; min-width: 56px; min-height: 48px; padding: 0; }
.segment { background: #2a2c31; border-radius: 24px; padding: 4px; }
.segment button { border-radius: 20px; min-width: 56px; min-height: 40px; background: none; box-shadow: none; padding: 0; }
.segment button.on { background: #3b3d42; }
.tab-search { background: #2a2c31; border-radius: 26px; min-height: 52px; padding: 0 0 0 16px; margin: 0 14px 8px 14px; }
.tab-search entry { font-size: 17px; }
.tab-card { background: #2b2d31; border-radius: 22px; padding: 0 6px 6px 6px; }
.tab-card.sel { background: #a8c7fa; color: #062e6f; }
.tab-thumb { background: #ffffff; border-radius: 16px; }
.tab-card.incognito .tab-thumb { background: #3c3f45; }
.tab-thumb label { color: #5f6368; font-size: 13px; }
.favletter { background: #2e3035; border-radius: 6px; min-width: 22px; min-height: 22px; font-size: 12px; font-weight: bold; }
.tab-card.sel .favletter { background: #062e6f; color: #a8c7fa; }
.tab-title { font-size: 15px; font-weight: 500; }
flowbox.tabs flowboxchild { padding: 6px; background: none; }
.turn-up { -gtk-icon-transform: rotate(90deg); }
.turn-down { -gtk-icon-transform: rotate(-90deg); }
`;

const host = u => { const m = (u || '').match(/^[a-z]+:\/\/([^/]+)/i); return m ? m[1].replace(/^www\./, '') : (u || ''); };
const letter = u => (host(u)[0] || '?').toUpperCase();
const fileIcon = name => Gio.FileIcon.new(Gio.File.new_for_path(`${HERE}/icons/${name}.svg`));
const icon = (name, size = 24) => new Gtk.Image({gicon: fileIcon(name), pixel_size: size});
const label = (text, cls = [], props = {}) => new Gtk.Label({label: text, css_classes: cls, xalign: 0, ...props});
function iconButton(name, cb, props = {}) {
    const b = new Gtk.Button({child: icon(name), css_classes: ['neo-icon'], valign: Gtk.Align.CENTER, ...props});
    b.connect('clicked', cb);
    return b;
}
function setIcon(button, name) { button.child.gicon = fileIcon(name); }

// ---- the JSON store: session, history (newest first, one entry per URL), bookmarks, preferences ------------------
class Store {
    constructor() {
        GLib.mkdir_with_parents(DATA, 0o700);
        for (const f of ['history.json', 'bookmarks.json'])   // the Qt browser's, once
            if (!GLib.file_test(`${DATA}/${f}`, GLib.FileTest.EXISTS) && GLib.file_test(`${OLD_DATA}/${f}`, GLib.FileTest.EXISTS))
                try { Gio.File.new_for_path(`${OLD_DATA}/${f}`).copy(Gio.File.new_for_path(`${DATA}/${f}`), Gio.FileCopyFlags.NONE, null, null); } catch (_) {}
        this.history = this.load('history.json', []);
        this.bookmarks = this.load('bookmarks.json', []);
        this.prefs = {adblock: true, ...this.load('prefs.json', {})};
        this._pending = new Map();
    }
    load(name, fallback) {
        try { return JSON.parse(new TextDecoder().decode(GLib.file_get_contents(`${DATA}/${name}`)[1])); } catch (_) { return fallback; }
    }
    // writes are batched: a page load touches history several times
    save(name, value) {
        if (this._pending.has(name)) GLib.source_remove(this._pending.get(name).id);
        const id = GLib.timeout_add(GLib.PRIORITY_LOW, 800, () => { this._write(name, value); this._pending.delete(name); return GLib.SOURCE_REMOVE; });
        this._pending.set(name, {id, value});
    }
    _write(name, value) {
        try { GLib.file_set_contents(`${DATA}/${name}`, JSON.stringify(value)); } catch (e) { logError(e); }
    }
    flush() {
        for (const [name, {id, value}] of this._pending) { GLib.source_remove(id); this._write(name, value); }
        this._pending.clear();
    }
    savePrefs() { this.save('prefs.json', this.prefs); }

    visit(url, title) {
        if (!url || /^(about|data|blob|chrome):/.test(url)) return;
        const old = this.history.find(h => h.url === url);
        this.history = this.history.filter(h => h.url !== url);
        this.history.unshift({url, title: title || old?.title || '', time: Date.now() / 1000, visits: (old?.visits ?? 0) + 1});
        this.history.length = Math.min(this.history.length, HISTORY_MAX);
        this.save('history.json', this.history);
    }
    retitle(url, title) {
        const h = this.history.find(x => x.url === url);
        if (h && title && h.title !== title) { h.title = title; this.save('history.json', this.history); }
    }
    clearHistory() { this.history = []; this.save('history.json', this.history); }
    suggest(text, limit) {
        const t = text.toLowerCase().trim();
        if (!t) return [];
        const hits = this.history.filter(h => h.url.toLowerCase().includes(t) || (h.title || '').toLowerCase().includes(t));
        const starts = h => h.url.split('://').pop().toLowerCase().startsWith(t);
        hits.sort((a, b) => (starts(b) - starts(a)) || ((b.visits ?? 1) - (a.visits ?? 1)));
        return hits.slice(0, limit);
    }
    topSites(limit) {
        const seen = new Set(), out = [];
        for (const h of [...this.history].sort((a, b) => (b.visits ?? 1) - (a.visits ?? 1))) {
            const k = host(h.url);
            if (seen.has(k)) continue;
            seen.add(k); out.push(h);
            if (out.length >= limit) break;
        }
        return out;
    }
    isBookmarked(url) { return this.bookmarks.some(b => b.url === url); }
    toggleBookmark(url, title) {
        if (this.isBookmarked(url)) this.bookmarks = this.bookmarks.filter(b => b.url !== url);
        else this.bookmarks.unshift({url, title, time: Date.now() / 1000});
        this.save('bookmarks.json', this.bookmarks);
    }
}

// ---- a tab: its WebView exists once the tab is shown (restored tabs wait, they cost nothing until then) ----------
let nextTabId = 1;
class Tab {
    constructor({url = '', title = '', incognito = false}) {
        this.id = nextTabId++;
        this.url = url; this.title = title; this.incognito = incognito;
        this.view = null; this.thumb = null; this.desktop = false;
        this.ntp = !url;              // the new-tab page shows over this tab (a new tab, or Home was pressed)
    }
    get uri() { return this.view?.uri || this.url; }
    get name() { return this.view?.title || this.title; }
}

class Browser {
    constructor(app) {
        this.app = app;
        this.store = new Store();
        this.tabs = [];
        this.current = null;
        this.downloads = [];
        this._dl = new Set();
        Adw.StyleManager.get_default().color_scheme = Adw.ColorScheme.FORCE_DARK;
        const css = new Gtk.CssProvider();
        css.load_from_string(CSS);
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION);

        this._setupEngine();
        this.win = new Adw.ApplicationWindow({application: app, title: 'Neo Browser', default_width: 360, default_height: 750, css_classes: ['neo']});
        this.toasts = new Adw.ToastOverlay();
        this.overlay = new Gtk.Overlay();
        this.toasts.set_child(this.overlay);
        this.pages = new Gtk.Stack({transition_type: Gtk.StackTransitionType.NONE, css_classes: ['neo-bg']});
        this.overlay.set_child(this.pages);
        this.pages.add_named(this._buildBrowse(), 'browse');
        this.pages.add_named(this._buildOmnibox(), 'omni');
        this.pages.add_named(this._buildSwitcher(), 'tabs');
        this.pages.add_named(this._buildList(), 'list');
        this.menu = this._sheet({halign: Gtk.Align.END, valign: Gtk.Align.START, dim: false});
        this.linkSheet = this._sheet({halign: Gtk.Align.FILL, valign: Gtk.Align.END, dim: true});
        this.win.set_content(this.toasts);
        this._shortcuts();
        this.win.connect('close-request', () => { this._writeSession(); this.store.flush(); return false; });

        const s = this.store.load('session.json', {tabs: [], current: 0});
        for (const t of s.tabs || []) if (t.url) this.tabs.push(new Tab({url: t.url, title: t.title}));
        if (this.tabs.length) this.select(this.tabs[Math.min(s.current || 0, this.tabs.length - 1)]);
        else this.newTab({});
        this.win.present();
    }

    // ---- engine: a persistent session, an incognito one made when needed, the shared page styles and filters ----
    _setupEngine() {
        try {   // a heavy page grows its web process without end: past 768 MB WebKit kills and reloads it
            const m = WebKit.MemoryPressureSettings.new();
            m.set_memory_limit(768);
            WebKit.WebContext.set_memory_pressure_settings?.(m);
        } catch (_) {}
        GLib.mkdir_with_parents(`${DATA}/web`, 0o700);
        this.session = WebKit.NetworkSession.new(`${DATA}/web`, CACHE);
        this.session.get_cookie_manager().set_persistent_storage(`${DATA}/cookies.sqlite`, WebKit.CookiePersistentStorage.SQLITE);
        this.session.get_website_data_manager().set_favicons_enabled(true);
        this.session.connect('download-started', (_s, d) => this._trackDownload(d));
        this.incogSession = null;
        this.ucm = new WebKit.UserContentManager();
        this.ucm.add_style_sheet(new WebKit.UserStyleSheet(SCROLLBAR_CSS, WebKit.UserContentInjectedFrames.ALL_FRAMES,
                                                           WebKit.UserStyleLevel.USER, null, null));
        this.filters = WebKit.UserContentFilterStore.new(`${DATA}/filters`);
        this.filter = null;
        if (this.store.prefs.adblock) this._loadFilter();
    }
    _incognitoSession() {
        if (!this.incogSession) {
            this.incogSession = WebKit.NetworkSession.new_ephemeral();
            this.incogSession.connect('download-started', (_s, d) => this._trackDownload(d));
        }
        return this.incogSession;
    }

    // the compiled filter if there is one (and fresh), else compile the copy beside the app or fetch the list
    _loadFilter() {
        this.filters.load(FILTER_ID, null, (_st, res) => {
            try { this._applyFilter(this.filters.load_finish(res)); } catch (_) { this._applyFilter(null); }
            const stamp = this.store.prefs.filterTime ?? 0;
            if (!this.filter && GLib.file_test(`${HERE}/easylist.json`, GLib.FileTest.EXISTS) && !stamp)
                this._compileFilter(GLib.Bytes.new(GLib.file_get_contents(`${HERE}/easylist.json`)[1]), false);
            else if (Date.now() / 1000 - stamp > FILTER_MAX_AGE)
                this._fetchFilter();
        });
    }
    _fetchFilter() {
        const session = new Soup.Session({timeout: 60});
        session.send_and_read_async(Soup.Message.new('GET', FILTER_URL), GLib.PRIORITY_LOW, null, (_s, res) => {
            try {
                const bytes = session.send_and_read_finish(res);
                const status = session.get_async_result_message(res)?.status_code;
                if (status !== 200 || bytes.get_size() < 10000) throw new Error(`filter list: ${bytes.get_size()} bytes`);
                this._compileFilter(bytes, true);
            } catch (e) { log(`neobrowser: ad filter not fetched: ${e.message}`); }
        });
    }
    _compileFilter(bytes, fetched) {
        this.filters.save(FILTER_ID, bytes, null, (_st, res) => {
            try {
                const f = this.filters.save_finish(res);
                this.store.prefs.filterTime = fetched ? Date.now() / 1000 : 1;   // the bundled copy: fetch again soon
                this.store.savePrefs();
                if (this.store.prefs.adblock) this._applyFilter(f);
                if (!fetched) this._fetchFilter();
            } catch (e) { log(`neobrowser: ad filter not compiled: ${e.message}`); }
        });
    }
    _applyFilter(f) {
        if (this.filter) this.ucm.remove_filter(this.filter);
        this.filter = f;
        if (f) this.ucm.add_filter(f);
    }
    setAdblock(on) {
        this.store.prefs.adblock = on;
        this.store.savePrefs();
        if (on) this._loadFilter(); else this._applyFilter(null);
        this.current?.view?.reload();
    }

    _makeView(tab, related = null) {
        const settings = new WebKit.Settings({
            user_agent: tab.desktop ? DESKTOP_UA : MOBILE_UA, enable_smooth_scrolling: true,
            enable_developer_extras: false, javascript_can_open_windows_automatically: false,
            media_playback_requires_user_gesture: true,
        });
        // WebKit holds a page's rendering near 60 fps (Safari's choice for 120 Hz screens): on this 120 Hz panel a
        // scroll then ran at 40-60 fps. Without it, 117 fps measured.
        const features = WebKit.Settings.get_all_features();
        for (let i = 0; i < features.get_length(); i++)
            if (features.get(i).get_identifier() === 'PreferPageRenderingUpdatesNear60FPS') settings.set_feature_enabled(features.get(i), false);
        const props = {settings, user_content_manager: this.ucm, hexpand: true, vexpand: true};
        if (related) props.related_view = related;
        else props.network_session = tab.incognito ? this._incognitoSession() : this.session;
        const v = new WebKit.WebView(props);
        tab.view = v;
        if (tab.desktop) v.zoom_level = 0.5;
        const mine = () => tab === this.current;
        v.connect('notify::title', () => {
            if (!tab.incognito && !v.is_loading) this.store.retitle(v.uri, v.title);
            if (mine()) this._sync();
        });
        v.connect('notify::uri', () => { if (mine()) this._sync(); this.saveSession(); });
        v.connect('notify::estimated-load-progress', () => { if (mine()) this._syncProgress(); });
        v.connect('notify::is-loading', () => { if (mine()) this._syncProgress(); });
        v.connect('load-changed', (_v, ev) => {
            if (ev === WebKit.LoadEvent.FINISHED && !tab.incognito) this.store.visit(v.uri, v.title);
            if (ev === WebKit.LoadEvent.STARTED && tab.ntp && mine() && v.uri) { tab.ntp = false; this._showCurrent(); }
        });
        v.connect('create', () => this.newTab({incognito: tab.incognito, related: v}).view);
        v.connect('close', () => this.closeTab(tab));
        v.connect('decide-policy', (_v, d, type) => this._decide(d, type));
        v.connect('context-menu', (_v, _menu, hit) => this._contextMenu(tab, hit));
        v.connect('permission-request', (_v, req) => { this._askPermission(v, req); return true; });
        v.connect('enter-fullscreen', () => { this._fullscreen(true); return false; });
        v.connect('leave-fullscreen', () => { this._fullscreen(false); return false; });
        const fc = v.get_find_controller();
        fc.connect('counted-matches', (_f, n) => { if (mine()) { this.findCount = n; this._syncFind(); } });
        fc.connect('failed-to-find-text', () => { if (mine()) { this.findCount = 0; this._syncFind(); } });
        this.viewStack.add_child(v);
        return v;
    }

    _decide(d, type) {
        if (type === WebKit.PolicyDecisionType.RESPONSE) {
            if (!d.is_mime_type_supported() && d.is_main_frame_main_resource()) { d.download(); return true; }
            return false;
        }
        // tel:, mailto:, geo: … go to the app for them (Calls, the mail client)
        const uri = d.get_navigation_action()?.get_request()?.get_uri() ?? '';
        if (uri && !/^(https?|about|data|blob|file|javascript):/i.test(uri)) {
            d.ignore();
            new Gtk.UriLauncher({uri}).launch(this.win, null, (l, res) => { try { l.launch_finish(res); } catch (_) { this.toast('No app opens this link'); } });
            return true;
        }
        return false;
    }

    // ---- tabs ---------------------------------------------------------------------------------------------------
    newTab({url = '', incognito = false, foreground = true, related = null}) {
        const t = new Tab({url, incognito});
        this.tabs.push(t);
        if (related) { t.ntp = false; this._makeView(t, related); }
        else if (url) { this._makeView(t); t.view.load_uri(url); }
        if (foreground) this.select(t); else this._sync();
        this.saveSession();
        return t;
    }
    select(t) {
        this.current = t;
        if (!t.ntp && !t.view && t.url) { this._makeView(t); t.view.load_uri(t.url); }
        this._showCurrent();
        this.pages.visible_child_name = 'browse';
        this.saveSession();
    }
    _showCurrent() {
        const t = this.current;
        if (!t) return;
        if (t.ntp || !t.view) { this._fillNtp(); this.viewStack.visible_child = this.ntp; }
        else this.viewStack.visible_child = t.view;
        this._sync();
    }
    closeTab(t) {
        const i = this.tabs.indexOf(t);
        if (i < 0) return;
        this.tabs.splice(i, 1);
        if (t.view) { this.viewStack.remove(t.view); t.view = null; }
        if (!this.tabs.some(x => x.incognito)) this.incogSession = null;   // nothing of the incognito tabs is kept
        if (this.current === t) {
            const same = this.tabs.filter(x => x.incognito === t.incognito);
            const next = same.length ? same[Math.min(i, same.length - 1)] : this.tabs.at(-1);
            this.current = null;
            if (next) this.select(next);
        }
        if (!this.tabs.length) this.newTab({});
        this.saveSession();
        if (this.pages.visible_child_name === 'tabs') this._fillSwitcher();
    }
    closeAll(incognito) { for (const t of this.tabs.filter(x => x.incognito === incognito)) this.closeTab(t); }
    saveSession() {
        if (this._sessionId) return;
        this._sessionId = GLib.timeout_add(GLib.PRIORITY_LOW, 500, () => { this._sessionId = 0; this._writeSession(); return GLib.SOURCE_REMOVE; });
    }
    _writeSession() {
        const normal = this.tabs.filter(t => !t.incognito);
        GLib.file_set_contents(`${DATA}/session.json`, JSON.stringify({
            tabs: normal.map(t => ({url: t.ntp ? '' : t.uri, title: t.name})).filter(t => t.url),
            current: Math.max(0, normal.filter(t => !t.ntp).indexOf(this.current)),
        }));
    }
    open(text) {
        const s = text.trim();
        if (!s) return;
        let url;
        if (/^[a-z]+:\/\//i.test(s) || /^(about|data|file):/i.test(s)) url = s;
        else if (!/\s/.test(s) && /^[^\s]+\.[a-z]{2,}(:\d+)?(\/.*)?$/i.test(s)) url = `https://${s}`;
        else url = SEARCH_URL + encodeURIComponent(s);
        if (!this.current) this.newTab({});
        const t = this.current;
        t.ntp = false;
        if (!t.view) this._makeView(t);
        t.view.load_uri(url);
        this.select(t);
    }
    goBack() {
        if (this.menu.shown) return this.menu.close();
        if (this.linkSheet.shown) return this.linkSheet.close();
        const page = this.pages.visible_child_name;
        if (page === 'list' && this.listKind) { this.pages.visible_child_name = 'browse'; return; }
        if (page !== 'browse') { this.pages.visible_child_name = 'browse'; return; }
        if (this.barStack.visible_child_name === 'find') return this.closeFind();
        const t = this.current;
        if (!t) return;
        if (t.ntp && t.view?.uri) { t.ntp = false; this._showCurrent(); return; }
        if (t.view?.can_go_back()) { t.view.go_back(); return; }
        if (!t.ntp) this.closeTab(t);
    }
    _shortcuts() {
        const sc = new Gtk.ShortcutController({propagation_phase: Gtk.PropagationPhase.CAPTURE, scope: Gtk.ShortcutScope.GLOBAL});
        const add = (trigger, fn) => sc.add_shortcut(new Gtk.Shortcut({
            trigger: Gtk.ShortcutTrigger.parse_string(trigger), action: Gtk.CallbackAction.new(() => { fn(); return true; })}));
        add('<Alt>Left|Back', () => this.goBack());   // the launcher's edge back gesture sends Alt+Left
        add('<Control>l', () => this.openOmnibox(this.current?.ntp ? '' : this.current?.uri ?? ''));
        add('<Control>t', () => { this.newTab({}); this.openOmnibox(''); });
        add('<Control>f', () => this.openFind());
        this.win.add_controller(sc);
    }

    // ---- browse page: toolbar / find bar, progress, the pages ------------------------------------------------------
    _buildBrowse() {
        const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL});
        this.barStack = new Gtk.Stack({transition_type: Gtk.StackTransitionType.NONE});
        // toolbar
        this.toolbar = new Gtk.Box({css_classes: ['neo-bar'], spacing: 0});
        this.toolbar.append(iconButton('home', () => { if (this.current) { this.current.ntp = true; this._showCurrent(); this.saveSession(); } }));
        this.addrIcon = icon('globe');
        this.addrIcon.opacity = 0.85;
        this.addrLabel = label('', [], {hexpand: true, ellipsize: 3});
        const pillBox = new Gtk.Box({spacing: 8});
        pillBox.append(this.addrIcon); pillBox.append(this.addrLabel);
        const pill = new Gtk.Button({child: pillBox, css_classes: ['pill'], hexpand: true, valign: Gtk.Align.CENTER});
        pill.connect('clicked', () => this.openOmnibox(this.current?.ntp ? '' : this.current?.uri ?? ''));
        this.toolbar.append(pill);
        this.countLabel = new Gtk.Label({css_classes: ['tabcount']});
        const countBtn = new Gtk.Button({child: this.countLabel, css_classes: ['neo-icon'], valign: Gtk.Align.CENTER});
        countBtn.connect('clicked', () => this.openSwitcher());
        this.toolbar.append(countBtn);
        this.toolbar.append(iconButton('more', () => this.openMenu()));
        this.barStack.add_named(this.toolbar, 'bar');
        // find in page
        const find = new Gtk.Box({css_classes: ['neo-bar']});
        find.append(iconButton('back', () => this.closeFind()));
        this.findEntry = new Gtk.Entry({css_classes: ['neo-entry'], hexpand: true, placeholder_text: 'Find in page', valign: Gtk.Align.CENTER});
        this.findEntry.connect('changed', () => this._find());
        this.findEntry.connect('activate', () => this._findStep(1));
        find.append(this.findEntry);
        this.findLabel = label('', ['dim']);
        find.append(this.findLabel);
        // up and down are the back arrow turned (-gtk-icon-transform)
        const up = iconButton('back', () => this._findStep(-1)); up.child.add_css_class('turn-up');
        const down = iconButton('back', () => this._findStep(1)); down.child.add_css_class('turn-down');
        find.append(up); find.append(down);
        this.barStack.add_named(find, 'find');
        box.append(this.barStack);
        this.progress = new Gtk.ProgressBar({css_classes: ['neo-progress']});
        box.append(this.progress);
        // the pages, and the new-tab page
        this.viewStack = new Gtk.Stack({transition_type: Gtk.StackTransitionType.NONE, vexpand: true, hhomogeneous: false, vhomogeneous: false});
        this.ntp = new Gtk.ScrolledWindow({hscrollbar_policy: Gtk.PolicyType.NEVER, css_classes: ['ntp']});
        this.viewStack.add_child(this.ntp);
        box.append(this.viewStack);
        return box;
    }
    _fillNtp() {
        const t = this.current;
        const incog = !!t?.incognito;
        this.ntp.set_css_classes(incog ? ['ntp', 'incognito'] : ['ntp']);
        const col = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, spacing: 28, margin_top: 56, margin_start: 20, margin_end: 20, margin_bottom: 20});
        col.append(new Gtk.Label({label: incog ? 'Incognito' : 'Neo', css_classes: ['ntp-title']}));
        const sb = new Gtk.Box({spacing: 12});
        const si = icon('search'); si.opacity = 0.8;
        sb.append(si); sb.append(label('Search or type URL'));
        const search = new Gtk.Button({child: sb, css_classes: ['search-pill']});
        search.connect('clicked', () => this.openOmnibox(''));
        col.append(search);
        if (incog) {
            col.append(label("Pages you view in Incognito tabs won't stay in history, cookies or the site data store once you close them. Downloads and bookmarks are kept.",
                             ['dim'], {wrap: true}));
        } else {
            const grid = new Gtk.Grid({column_spacing: 8, row_spacing: 8, halign: Gtk.Align.CENTER, column_homogeneous: true});
            this.store.topSites(8).forEach((h, i) => {
                const b = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, spacing: 8, width_request: 72});
                b.append(new Gtk.Label({label: letter(h.url), css_classes: ['site-dot'], halign: Gtk.Align.CENTER}));
                b.append(new Gtk.Label({label: h.title || host(h.url), css_classes: ['site-name'], ellipsize: 3, max_width_chars: 9}));
                const btn = new Gtk.Button({child: b, css_classes: ['site']});
                btn.connect('clicked', () => this.open(h.url));
                grid.attach(btn, i % 4, Math.floor(i / 4), 1, 1);
            });
            col.append(grid);
        }
        this.ntp.set_child(col);
    }
    _sync() {
        const t = this.current;
        if (!t) return;
        const incog = t.incognito;
        this.toolbar.set_css_classes(incog ? ['neo-bar', 'incognito'] : ['neo-bar']);
        const uri = t.ntp ? '' : t.uri;
        this.addrLabel.label = uri ? host(uri) : 'Search or type URL';
        this.addrLabel.opacity = uri ? 1 : 0.7;
        this.addrIcon.gicon = fileIcon(uri.startsWith('https') ? 'tune' : 'globe');
        const n = this.tabs.filter(x => x.incognito === incog).length;
        this.countLabel.label = n > 99 ? ':D' : String(n);
        this.countLabel.set_css_classes(n > 9 ? ['tabcount', 'small'] : ['tabcount']);
        this.win.title = (!t.ntp && t.name) || 'Neo Browser';
        this._syncProgress();
    }
    _syncProgress() {
        const v = this.current?.ntp ? null : this.current?.view;
        this.progress.fraction = v?.is_loading ? Math.max(0.05, v.estimated_load_progress) : 0;
    }
    _fullscreen(on) {
        this.barStack.visible = this.progress.visible = !on;
        if (on) this.win.fullscreen(); else this.win.unfullscreen();
    }

    // ---- find in page ---------------------------------------------------------------------------------------------
    openFind() {
        if (!this.current?.view || this.current.ntp) return;
        this.findEntry.text = '';
        this.findCount = 0; this.findIndex = 0; this._syncFind();
        this.barStack.visible_child_name = 'find';
        this.findEntry.grab_focus();
    }
    closeFind() {
        this.current?.view?.get_find_controller().search_finish();
        this.barStack.visible_child_name = 'bar';
    }
    _find() {
        const fc = this.current?.view?.get_find_controller();
        if (!fc) return;
        const text = this.findEntry.text;
        if (!text) { fc.search_finish(); this.findCount = 0; this._syncFind(); return; }
        const opts = WebKit.FindOptions.CASE_INSENSITIVE | WebKit.FindOptions.WRAP_AROUND;
        this.findIndex = 1;
        fc.count_matches(text, opts, 1000);
        fc.search(text, opts, 1000);
    }
    _findStep(dir) {
        const fc = this.current?.view?.get_find_controller();
        if (!fc || !this.findCount) return;
        if (dir > 0) fc.search_next(); else fc.search_previous();
        this.findIndex = ((this.findIndex - 1 + dir + this.findCount) % this.findCount) + 1;
        this._syncFind();
    }
    _syncFind() {
        this.findLabel.label = this.findEntry.text ? `${this.findCount ? this.findIndex : 0}/${this.findCount}` : '';
    }

    // ---- address entry with suggestions -----------------------------------------------------------------------------
    _buildOmnibox() {
        const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, css_classes: ['neo-bg']});
        const bar = new Gtk.Box({css_classes: ['neo-bar'], spacing: 4});
        bar.append(iconButton('back', () => { this.pages.visible_child_name = 'browse'; }));
        const pill = new Gtk.Box({css_classes: ['pill-box'], hexpand: true, valign: Gtk.Align.CENTER});
        this.omniEntry = new Gtk.Entry({css_classes: ['neo-entry'], hexpand: true, input_purpose: Gtk.InputPurpose.URL,
                                        input_hints: Gtk.InputHints.NO_SPELLCHECK | Gtk.InputHints.LOWERCASE | Gtk.InputHints.NO_EMOJI});
        this.omniEntry.connect('changed', () => { this._omniFresh = false; this._fillSuggestions(); });
        this.omniEntry.connect('activate', () => this.open(this.omniEntry.text));
        pill.append(this.omniEntry);
        this.omniClear = iconButton('close', () => { this.omniEntry.text = ''; this.omniEntry.grab_focus(); });
        pill.append(this.omniClear);
        bar.append(pill);
        box.append(bar);
        this.suggestList = new Gtk.ListBox({css_classes: ['neo-list'], selection_mode: Gtk.SelectionMode.NONE});
        this.suggestList.connect('row-activated', (_l, row) => this.open(row._target));
        box.append(new Gtk.ScrolledWindow({child: this.suggestList, vexpand: true, hscrollbar_policy: Gtk.PolicyType.NEVER}));
        return box;
    }
    openOmnibox(text) {
        this.pages.visible_child_name = 'omni';
        this.omniEntry.text = text;
        this._omniFresh = true;        // until the first keystroke the list is the recent history
        this.omniEntry.grab_focus();
        this.omniEntry.select_region(0, -1);
        this._fillSuggestions();
    }
    _fillSuggestions() {
        const text = this.omniEntry.text.trim();
        this.omniClear.visible = this.omniEntry.text.length > 0;
        this.suggestList.remove_all();
        const items = [];
        if (!text || this._omniFresh) {
            // a tap on the address bar: the pages visited last, as a list under it (the open page not among them)
            const open = this.omniEntry.text;
            for (const h of this.store.history.filter(h => h.url !== open).slice(0, 12))
                items.push({kind: 'history', title: h.title || h.url, url: h.url});
        } else {
            items.push({kind: 'search', title: text, url: ''});
            for (const s of this.store.suggest(text, 6)) items.push({kind: 'history', title: s.title || s.url, url: s.url});
        }
        for (const it of items) {
            const row = new Gtk.Box({spacing: 20});
            const ic = icon(it.kind); ic.opacity = 0.8;
            row.append(ic);
            const col = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, hexpand: true, valign: Gtk.Align.CENTER});
            col.append(label(it.title, [], {ellipsize: 3}));
            if (it.url) col.append(label(it.url, ['url'], {ellipsize: 3}));
            row.append(col);
            row.append(iconButton('arrow_up_left', () => {
                this.omniEntry.text = it.url || it.title;
                this.omniEntry.grab_focus(); this.omniEntry.set_position(-1);
            }));
            const lr = new Gtk.ListBoxRow({child: row, activatable: true});
            lr._target = it.url || it.title;
            this.suggestList.append(lr);
        }
    }

    // ---- tab switcher ------------------------------------------------------------------------------------------------
    _buildSwitcher() {
        const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, css_classes: ['switcher']});
        const head = new Gtk.CenterBox({margin_start: 14, margin_end: 4, height_request: 64});
        const fab = new Gtk.Button({child: new Gtk.Image({gicon: fileIcon('add_dark'), pixel_size: 24}), css_classes: ['fab'], valign: Gtk.Align.CENTER});
        fab.connect('clicked', () => {
            this.newTab({incognito: this.switcherIncognito});
            if (!this.switcherIncognito) this.openOmnibox('');
        });
        head.set_start_widget(fab);
        const seg = new Gtk.Box({css_classes: ['segment'], spacing: 8, valign: Gtk.Align.CENTER});
        this.segNormalCount = new Gtk.Label({css_classes: ['tabcount']});
        this.segNormal = new Gtk.Button({child: this.segNormalCount});
        this.segNormal.connect('clicked', () => { this.switcherIncognito = false; this._fillSwitcher(); });
        this.segIncog = new Gtk.Button({child: icon('incognito')});
        this.segIncog.connect('clicked', () => { this.switcherIncognito = true; this._fillSwitcher(); });
        seg.append(this.segNormal); seg.append(this.segIncog);
        head.set_center_widget(seg);
        const more = iconButton('more', () => {
            closeBtnLabel.label = this.switcherIncognito ? 'Close Incognito tabs' : 'Close all tabs';
            pop.popup();
        });
        const pop = new Gtk.Popover({has_arrow: false});
        const closeBtnBox = new Gtk.Box({spacing: 20});
        const closeBtnLabel = label('Close all tabs');
        closeBtnBox.append(icon('close')); closeBtnBox.append(closeBtnLabel);
        const closeBtn = new Gtk.Button({child: closeBtnBox, css_classes: ['menu-row']});
        closeBtn.connect('clicked', () => { pop.popdown(); this.closeAll(this.switcherIncognito); this._fillSwitcher(); });
        pop.set_child(closeBtn);
        pop.set_parent(more);
        head.set_end_widget(more);
        box.append(head);
        const search = new Gtk.Box({css_classes: ['tab-search'], spacing: 12});
        const si = icon('search'); si.opacity = 0.8;
        search.append(si);
        this.tabFilter = new Gtk.Entry({css_classes: ['neo-entry'], hexpand: true, placeholder_text: 'Search your tabs', valign: Gtk.Align.CENTER});
        this.tabFilter.connect('changed', () => this._fillSwitcher());
        search.append(this.tabFilter);
        box.append(search);
        this.tabGrid = new Gtk.FlowBox({css_classes: ['tabs'], homogeneous: true, min_children_per_line: 2, max_children_per_line: 6,
                                        selection_mode: Gtk.SelectionMode.NONE, valign: Gtk.Align.START, margin_start: 4, margin_end: 4});
        box.append(new Gtk.ScrolledWindow({child: this.tabGrid, vexpand: true, hscrollbar_policy: Gtk.PolicyType.NEVER}));
        return box;
    }
    openSwitcher() {
        this.switcherIncognito = !!this.current?.incognito;
        this.tabFilter.text = '';
        this.pages.visible_child_name = 'tabs';
        this._fillSwitcher();
        this._snapshot(this.current);
    }
    // the current page as a small picture for its card: WebKit draws it at the screen's size, the GPU shrinks it
    _snapshot(t) {
        const v = t?.view;
        if (!v || t.ntp) return;
        v.get_snapshot(WebKit.SnapshotRegion.VISIBLE, WebKit.SnapshotOptions.NONE, null, (_v, res) => {
            try {
                const tex = v.get_snapshot_finish(res);
                const w = tex.get_width(), h = tex.get_height(), k = Math.min(1, 480 / w);
                const s = new Gtk.Snapshot();
                s.scale(k, k);
                tex.snapshot(s, w, h);
                const ch = Math.min(h, w * 1.3) * k;   // the top of the page: what a card shows
                t.thumb = this.win.get_renderer().render_texture(s.to_node(), new Graphene.Rect().init(0, 0, w * k, ch));
                if (this.pages.visible_child_name === 'tabs') this._fillSwitcher();
            } catch (e) { log(`neobrowser: no snapshot: ${e.message}`); }
        });
    }
    _fillSwitcher() {
        const inc = this.switcherIncognito;
        this.segNormalCount.label = String(this.tabs.filter(t => !t.incognito).length);
        this.segNormal.set_css_classes(inc ? [] : ['on']);
        this.segIncog.set_css_classes(inc ? ['on'] : []);
        const f = this.tabFilter.text.toLowerCase();
        this.tabGrid.remove_all();
        for (const t of this.tabs.filter(t => t.incognito === inc && (!f || `${t.name} ${t.uri}`.toLowerCase().includes(f)))) {
            const sel = t === this.current;
            const card = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, css_classes: ['tab-card', ...(sel ? ['sel'] : []), ...(t.incognito ? ['incognito'] : [])],
                                      height_request: 250});
            const top = new Gtk.Box({spacing: 8, margin_start: 6, height_request: 44});
            const fav = t.view?.favicon;
            top.append(fav ? new Gtk.Image({paintable: fav, pixel_size: 22, valign: Gtk.Align.CENTER})
                           : new Gtk.Label({label: t.ntp ? 'N' : letter(t.uri), css_classes: ['favletter'], valign: Gtk.Align.CENTER}));
            top.append(label(t.ntp ? 'New tab' : (t.name || host(t.uri) || 'New tab'), ['tab-title'], {hexpand: true, ellipsize: 3}));
            const close = new Gtk.Button({child: new Gtk.Image({gicon: fileIcon(sel ? 'close_dark' : 'close'), pixel_size: 22}),
                                          css_classes: ['neo-icon'], valign: Gtk.Align.CENTER});
            close.connect('clicked', () => this.closeTab(t));
            top.append(close);
            card.append(top);
            const thumb = new Gtk.Box({css_classes: ['tab-thumb'], vexpand: true, overflow: Gtk.Overflow.HIDDEN});
            if (t.thumb && !t.ntp)
                thumb.append(new Gtk.Picture({paintable: t.thumb, content_fit: Gtk.ContentFit.COVER, can_shrink: true, hexpand: true, vexpand: true}));
            else
                thumb.append(new Gtk.Label({label: t.ntp ? 'New tab' : host(t.uri) || 'New tab', hexpand: true, wrap: true, justify: Gtk.Justification.CENTER}));
            const click = new Gtk.GestureClick();
            click.connect('released', () => this.select(t));
            thumb.add_controller(click);
            card.append(thumb);
            this.tabGrid.append(card);
        }
    }

    // ---- history, bookmarks, downloads -------------------------------------------------------------------------------
    _buildList() {
        const box = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, css_classes: ['neo-bg']});
        const bar = new Gtk.Box({css_classes: ['neo-bar'], spacing: 4});
        bar.append(iconButton('back', () => { this.pages.visible_child_name = 'browse'; }));
        this.listTitle = label('', ['title-big'], {hexpand: true});
        bar.append(this.listTitle);
        this.listDelete = iconButton('delete', () => this.askClearData());
        bar.append(this.listDelete);
        box.append(bar);
        this.list = new Gtk.ListBox({css_classes: ['neo-list'], selection_mode: Gtk.SelectionMode.NONE});
        this.list.connect('row-activated', (_l, row) => row._activate?.());
        this.listScroll = new Gtk.ScrolledWindow({child: this.list, vexpand: true, hscrollbar_policy: Gtk.PolicyType.NEVER});
        box.append(this.listScroll);
        return box;
    }
    showList(kind) {
        this.listKind = kind;
        this.listTitle.label = {history: 'History', bookmarks: 'Bookmarks', downloads: 'Downloads'}[kind];
        this.listDelete.visible = kind === 'history';
        this.list.remove_all();
        this.listScroll.vadjustment.value = 0;
        this.pages.visible_child_name = 'list';
        const gen = this._listGen = (this._listGen ?? 0) + 1;
        if (kind === 'downloads') {
            for (const e of this.downloads) this.list.append(this._downloadRow(e));
            return;
        }
        const items = kind === 'history' ? this.store.history : this.store.bookmarks;
        // 2000 rows at once is a second's freeze on the phone: the first screenful now, the rest as it idles
        let i = 0;
        const step = () => {
            if (gen !== this._listGen) return GLib.SOURCE_REMOVE;
            for (const end = Math.min(items.length, i + (i ? 60 : 20)); i < end; i++) {
                const h = items[i];
                this.list.append(this._listRow(letter(h.url), h.title || h.url, host(h.url), () => this.open(h.url)));
            }
            return i < items.length ? GLib.SOURCE_CONTINUE : GLib.SOURCE_REMOVE;
        };
        if (step() === GLib.SOURCE_CONTINUE) GLib.idle_add(GLib.PRIORITY_LOW, step);
    }
    _listRow(dot, title, sub, activate) {
        const box = new Gtk.Box({spacing: 16});
        box.append(new Gtk.Label({label: dot, css_classes: ['row-dot'], valign: Gtk.Align.CENTER}));
        const col = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL, hexpand: true, valign: Gtk.Align.CENTER});
        col.append(label(title, [], {ellipsize: 3}));
        const subLabel = label(sub, ['sub'], {ellipsize: 3});
        col.append(subLabel);
        box.append(col);
        const row = new Gtk.ListBoxRow({child: box, activatable: true});
        row._activate = activate;
        row._sub = subLabel;
        return row;
    }

    // ---- downloads ---------------------------------------------------------------------------------------------------
    _trackDownload(d) {
        if (this._dl.has(d)) return;
        this._dl.add(d);
        const e = {d, name: '', path: '', done: false, failed: false, row: null};
        this.downloads.unshift(e);
        d.connect('decide-destination', (_d, suggested) => {
            GLib.mkdir_with_parents(DOWNLOADS, 0o755);
            const base = (suggested || 'download').replace(/\//g, '_');
            const dot = base.lastIndexOf('.') > 0 ? base.lastIndexOf('.') : base.length;
            let path = `${DOWNLOADS}/${base}`;
            for (let n = 1; GLib.file_test(path, GLib.FileTest.EXISTS); n++) path = `${DOWNLOADS}/${base.slice(0, dot)} (${n})${base.slice(dot)}`;
            d.set_destination(path);
            e.path = path; e.name = GLib.path_get_basename(path);
            this.toast(`Downloading ${e.name}`);
            this._syncDownload(e);
            return true;
        });
        d.connect('notify::estimated-progress', () => this._syncDownload(e));
        d.connect('failed', () => { e.failed = true; this._syncDownload(e); });
        d.connect('finished', () => {
            e.done = true; this._syncDownload(e);
            if (!e.failed) this.toast(`Downloaded ${e.name}`);
        });
    }
    _downloadText(e) {
        if (e.failed) return 'Failed';
        if (e.done) return `Done · ${GLib.format_size(e.d.get_received_data_length())}`;
        return `${Math.round(100 * e.d.estimated_progress)} %`;
    }
    _downloadRow(e) {
        const row = this._listRow('↓', e.name || 'Download', this._downloadText(e), () => {
            if (e.done && !e.failed) new Gtk.FileLauncher({file: Gio.File.new_for_path(e.path)}).launch(this.win, null, null);
        });
        e.row = row;
        return row;
    }
    _syncDownload(e) {
        if (!e.row || this.listKind !== 'downloads') return;
        e.row._sub.label = this._downloadText(e);
        e.row.child.get_last_child().get_first_child().label = e.name || 'Download';
    }
    savePage() {
        const v = this.current?.view;
        if (!v) return;
        const base = (v.title || host(v.uri) || 'page').replace(/[/\\:*?"<>|]/g, '_').slice(0, 80);
        GLib.mkdir_with_parents(DOWNLOADS, 0o755);
        let path = `${DOWNLOADS}/${base}.mhtml`;
        for (let n = 1; GLib.file_test(path, GLib.FileTest.EXISTS); n++) path = `${DOWNLOADS}/${base} (${n}).mhtml`;
        v.save_to_file(Gio.File.new_for_path(path), WebKit.SaveMode.MHTML, null, (_v, res) => {
            try { v.save_to_file_finish(res); this.toast(`Saved ${GLib.path_get_basename(path)}`); } catch (err) { this.toast('Page not saved'); }
        });
    }

    // ---- sheets: the menu (top right, no dimming) and the link sheet (bottom, dimmed) -------------------------------
    _sheet({halign, valign, dim}) {
        const scrim = new Gtk.Box({css_classes: [dim ? 'scrim' : 'scrim-clear'], visible: false, hexpand: true, vexpand: true});
        const holder = new Gtk.ScrolledWindow({hscrollbar_policy: Gtk.PolicyType.NEVER, propagate_natural_height: true,
                                               css_classes: ['sheet', ...(valign === Gtk.Align.END ? ['bottom'] : [])],
                                               halign, valign, visible: false, overflow: Gtk.Overflow.HIDDEN});
        if (valign === Gtk.Align.START) { holder.margin_top = 8; holder.margin_end = 8; holder.width_request = 300; }
        const sheet = {
            get shown() { return holder.visible; },
            open: child => {
                holder.set_child(child);
                holder.max_content_height = Math.max(200, this.win.get_height() - 24);
                scrim.visible = holder.visible = true;
            },
            close: () => { scrim.visible = holder.visible = false; },
        };
        const click = new Gtk.GestureClick();
        click.connect('released', () => sheet.close());
        scrim.add_controller(click);
        this.overlay.add_overlay(scrim);
        this.overlay.add_overlay(holder);
        return sheet;
    }
    _row(iconName, text, cb, check = null) {
        const box = new Gtk.Box({spacing: 20});
        box.append(icon(iconName));
        box.append(label(text, [], {hexpand: true}));
        if (check !== null) box.append(new Gtk.CheckButton({active: check, can_target: false}));
        const b = new Gtk.Button({child: box, css_classes: ['menu-row']});
        b.connect('clicked', () => { this.menu.close(); this.linkSheet.close(); cb(); });
        return b;
    }
    openMenu() {
        const t = this.current;
        const v = t && !t.ntp ? t.view : null;
        const uri = v?.uri ?? '';
        const col = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL});
        const top = new Gtk.Box({css_classes: ['menu-top'], halign: Gtk.Align.CENTER, margin_bottom: 4});
        const topButton = (name, on, act) => {
            const b = new Gtk.Button({child: icon(name), sensitive: on});
            b.connect('clicked', () => { this.menu.close(); act(); });
            top.append(b);
        };
        topButton('back', !!v?.can_go_back(), () => v.go_back());
        topButton('forward', !!v?.can_go_forward(), () => v.go_forward());
        const marked = !!uri && this.store.isBookmarked(uri);
        topButton(marked ? 'star' : 'star_border', !!uri, () => {
            this.store.toggleBookmark(uri, v.title);
            this.toast(this.store.isBookmarked(uri) ? 'Bookmarked' : 'Bookmark removed');
        });
        topButton('download', !!uri, () => this.savePage());
        topButton(v?.is_loading ? 'close' : 'refresh', !!v, () => (v.is_loading ? v.stop_loading() : v.reload()));
        col.append(top);
        const div = () => new Gtk.Box({css_classes: ['divider']});
        col.append(this._row('add', 'New tab', () => { this.newTab({}); this.openOmnibox(''); }));
        col.append(this._row('incognito', 'New Incognito tab', () => this.newTab({incognito: true})));
        col.append(div());
        col.append(this._row('history', 'History', () => this.showList('history')));
        col.append(this._row('delete', 'Delete browsing data', () => this.askClearData()));
        col.append(div());
        col.append(this._row('download', 'Downloads', () => this.showList('downloads')));
        col.append(this._row('bookmarks', 'Bookmarks', () => this.showList('bookmarks')));
        col.append(div());
        col.append(this._row('share', 'Copy link', () => { if (uri) { this.copy(uri); this.toast('Link copied'); } }));
        col.append(this._row('find', 'Find in page', () => this.openFind()));
        col.append(this._row('desktop', 'Desktop site', () => {
            if (!t) return;
            t.desktop = !t.desktop;
            if (!t.view) return;
            t.view.get_settings().user_agent = t.desktop ? DESKTOP_UA : MOBILE_UA;
            t.view.zoom_level = t.desktop ? 0.5 : 1;   // a desktop page laid out ~720 px wide, as a phone's desktop mode
            t.view.reload();
        }, !!t?.desktop));
        col.append(this._row('shield', 'Block ads', () => {
            this.setAdblock(!this.store.prefs.adblock);
            this.toast(this.store.prefs.adblock ? 'Ads are blocked' : 'Ads are allowed');
        }, !!this.store.prefs.adblock));
        this.menu.open(col);
    }
    _contextMenu(tab, hit) {
        // a text field keeps WebKit's own menu (paste); links and images get the sheet; elsewhere nothing
        if (hit.context_is_editable() || hit.context_is_selection()) return false;
        const link = hit.context_is_link() ? hit.get_link_uri() : '';
        const media = hit.context_is_image() ? hit.get_image_uri() : '';
        if (!link && !media) return true;
        const col = new Gtk.Box({orientation: Gtk.Orientation.VERTICAL});
        col.append(label(link || media, ['dim'], {ellipsize: 2, margin_start: 20, margin_end: 20, margin_bottom: 8}));
        const v = tab.view;
        if (link) {
            col.append(this._row('add', 'Open in new tab', () => { this.newTab({url: link, incognito: tab.incognito, foreground: false}); this.toast('Opened in a new tab'); }));
            col.append(this._row('incognito', 'Open in Incognito tab', () => this.newTab({url: link, incognito: true})));
            col.append(this._row('share', 'Copy link address', () => { this.copy(link); this.toast('Link copied'); }));
            col.append(this._row('download', 'Download link', () => this._trackDownload(v.download_uri(link))));
        }
        if (media) {
            col.append(this._row('download', 'Download image', () => this._trackDownload(v.download_uri(media))));
            col.append(this._row('add', 'Open image in new tab', () => this.newTab({url: media, incognito: tab.incognito})));
        }
        this.linkSheet.open(col);
        return true;
    }
    _askPermission(v, req) {
        let what = 'use a device feature';
        if (req instanceof WebKit.GeolocationPermissionRequest) what = 'know your location';
        else if (req instanceof WebKit.NotificationPermissionRequest) what = 'show notifications';
        else if (req instanceof WebKit.ClipboardPermissionRequest) what = 'see text and images copied to the clipboard';
        else if (req instanceof WebKit.UserMediaPermissionRequest)
            what = req.is_for_audio_device && req.is_for_video_device ? 'use your camera and microphone'
                 : req.is_for_video_device ? 'use your camera' : 'use your microphone';
        else if (req instanceof WebKit.MediaKeySystemPermissionRequest) what = 'play protected content';
        const d = new Adw.AlertDialog({heading: `${host(v.uri)} wants to ${what}`});
        d.add_response('block', 'Block');
        d.add_response('allow', 'Allow');
        d.set_response_appearance('allow', Adw.ResponseAppearance.SUGGESTED);
        d.set_close_response('block');
        d.connect('response', (_d, r) => (r === 'allow' ? req.allow() : req.deny()));
        d.present(this.win);
    }
    askClearData() {
        const d = new Adw.AlertDialog({heading: 'Delete browsing data',
                                       body: 'Deletes history, cookies, site data and cached images and files. Bookmarks and downloads stay.'});
        d.add_response('cancel', 'Cancel');
        d.add_response('delete', 'Delete data');
        d.set_response_appearance('delete', Adw.ResponseAppearance.DESTRUCTIVE);
        d.set_close_response('cancel');
        d.connect('response', (_d, r) => {
            if (r !== 'delete') return;
            this.store.clearHistory();
            this.session.get_website_data_manager().clear(WebKit.WebsiteDataTypes.ALL, 0, null, null);
            if (this.listKind === 'history' && this.pages.visible_child_name === 'list') this.showList('history');
            this.toast('Browsing data deleted');
        });
        d.present(this.win);
    }
    copy(text) { this.win.get_clipboard().set(text); }
    toast(text) { this.toasts.add_toast(new Adw.Toast({title: text, timeout: 2})); }
}

const app = new Adw.Application({application_id: APP_ID, flags: Gio.ApplicationFlags.HANDLES_OPEN});
let browser = null;
const ensure = () => { if (!browser) browser = new Browser(app); browser.win.present(); return browser; };
app.connect('activate', () => ensure());
app.connect('open', (_a, files) => {
    const b = ensure();
    for (const f of files) b.newTab({url: f.get_uri()});
});
app.run([System.programInvocationName, ...ARGV]);
