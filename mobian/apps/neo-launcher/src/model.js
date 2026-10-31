// LayoutModel: what sits where on the home screen — the dock and the workspace pages — stored as JSON in
// ~/.config/neo-launcher/layout.json. The first start fills the dock from Phosh's favourites and the pages with
// every other app, A→Z; apps installed later go to the first free cell, uninstalled ones drop out.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

// desktop ids that another app took over
const REPLACED = {'de.yesman.Browser.desktop': 'de.yesman.NeoBrowser.desktop'};
const FILE = GLib.build_filenamev([GLib.get_user_config_dir(), 'neo-launcher', 'layout.json']);

// The apps that come with Waydroid's Android image: GNOME has its own phone, contacts, messages, clock, files and so
// on, and these do not fit it (they expect Android's telephony and home). Play stays, and what it installs, and
// Android's settings (app permissions, storage, accounts) under a name that tells it from GNOME's.
const ANDROID_BUILTIN = new Set(['com.android.calculator2', 'com.android.deskclock', 'com.android.documentsui',
    'com.android.gallery3d', 'com.android.inputmethod.latin', 'com.google.android.apps.messaging',
    'com.google.android.contacts', 'com.google.android.dialer', 'org.lineageos.aperture', 'org.lineageos.eleven',
    'org.lineageos.etar', 'org.lineageos.jelly', 'org.lineageos.recorder', 'org.lineageos.updater']);

const NAMES = {'waydroid.com.android.settings.desktop': 'Android Settings'};

/** The name Neo shows for an app. */
export function appName(info) { return NAMES[info?.get_id?.()] ?? info?.get_display_name?.() ?? ''; }

/** The apps a launcher shows: visible .desktop entries, by id. */
export function installedApps() {
    const apps = new Map();
    for (const info of Gio.AppInfo.get_all()) {
        const id = info.get_id();
        // Waydroid writes its Settings entry NoDisplay; Neo shows it as Android Settings
        if (!info.should_show() && id !== 'waydroid.com.android.settings.desktop') continue;
        if (ANDROID_BUILTIN.has(/^waydroid\.(.+)\.desktop$/.exec(id ?? '')?.[1])) continue;
        if (id && !id.startsWith('de.yesman.NeoLauncher')) apps.set(id, info);
    }
    return apps;
}

export class LayoutModel {
    constructor({cols, rows, dockSize}) {
        this.cols = cols; this.rows = rows; this.dockSize = dockSize;
        this.dock = []; this.pages = [];
    }

    get perPage() { return this.cols * this.rows; }

    load(apps) {
        let saved = null;
        try {
            const [, bytes] = GLib.file_get_contents(FILE);
            saved = JSON.parse(new TextDecoder().decode(bytes));
        } catch (_) { /* first start, or a broken file: start fresh */ }
        if (saved?.dock && saved?.pages) {
            this.dock = saved.dock; this.pages = saved.pages;
        } else {
            this._fresh(apps);
        }
        this.reconcile(apps);
    }

    _fresh(apps) {
        let favs = [];
        try { favs = new Gio.Settings({schema_id: 'sm.puri.phosh'}).get_strv('favorites'); } catch (_) {}
        this.dock = favs.filter(id => apps.has(id)).slice(0, this.dockSize);
        const rest = [...apps.keys()].filter(id => !this.dock.includes(id))
            .sort((a, b) => appName(apps.get(a)).localeCompare(appName(apps.get(b))));
        this.pages = [];
        for (let i = 0; i < rest.length; i += this.perPage) this.pages.push(rest.slice(i, i + this.perPage));
        if (!this.pages.length) this.pages.push([]);
    }

    /** Drop uninstalled apps, place new ones in the first free cell. Returns true when something changed. */
    reconcile(apps) {
        let changed = false;
        const hidden = this.hidden ?? new Set();
        // an app that was replaced by another keeps its places (the dock slot, the home cells)
        for (const [from, to] of Object.entries(REPLACED)) {
            const placedOld = this.dock.includes(from) || this.pages.some(p => p.includes(from));
            if (!placedOld || apps.has(from) || !apps.has(to)) continue;
            // the new app's own icon (put on a free cell when it was installed) gives way to the old one's places
            this.dock = this.dock.filter(id => id !== to);
            this.pages = this.pages.map(p => p.filter(id => id !== to));
            this.dock = this.dock.map(id => (id === from ? to : id));
            this.pages = this.pages.map(p => p.map(id => (id === from ? to : id)));
            changed = true;
        }
        const keep = id => { const ok = apps.has(id) && !hidden.has(id); if (!ok) changed = true; return ok; };
        this.dock = this.dock.filter(keep);
        this.pages = this.pages.map(p => p.filter(keep));
        // a smaller dock (dock-num-icons) moves the extra icons onto the pages
        if (this.dock.length > this.dockSize) { this.dock.splice(this.dockSize); changed = true; }
        const placed = new Set([...this.dock, ...this.pages.flat()]);
        for (const id of apps.keys()) {
            if (placed.has(id) || hidden.has(id)) continue;
            let page = this.pages.find(p => p.length < this.perPage);
            if (!page) { page = []; this.pages.push(page); }
            page.push(id); changed = true;
        }
        if (!this.pages.length) this.pages.push([]);
        if (changed) this.save();
        return changed;
    }

    /** Takes one icon away: the one at `from` ({dock: index} or {page, index}) when it is `id`, else every icon of
     *  `id`. An app may sit in several places (a home shortcut of a dock app). */
    _take(id, from) {
        if (from?.dock !== undefined && this.dock[from.dock] === id) { this.dock.splice(from.dock, 1); return; }
        if (from?.page !== undefined && this.pages[from.page]?.[from.index] === id) { this.pages[from.page].splice(from.index, 1); return; }
        this.dock = this.dock.filter(x => x !== id);
        this.pages = this.pages.map(p => p.filter(x => x !== id));
    }

    /** Removes the icon at `from` only; false when it was the app's last one (the caller hides the app instead). */
    removeOne(id, from) {
        const count = this.dock.filter(x => x === id).length + this.pages.flat().filter(x => x === id).length;
        if (count < 2) return false;
        this._take(id, from);
        while (this.pages.length > 1 && !this.pages.at(-1).length) this.pages.pop();
        this.save();
        return true;
    }

    /** One more icon of `id` in the first free cell (a shortcut of an app that is in the dock already, say). */
    addShortcut(id) {
        let page = this.pages.find(p => p.length < this.perPage);
        if (!page) { page = []; this.pages.push(page); }
        page.push(id);
        this.save();
    }

    /** Moves `id` (the icon at `from`) to {dock: index} or {page, index}; full pages push their last icon on, a full
     *  dock pushes its last icon to the first free cell. */
    move(id, target, from = null, {copy = false} = {}) {
        if (!copy)
            this._take(id, from);
        if (target.dock !== undefined) {
            this.dock.splice(Math.min(target.dock, this.dock.length), 0, id);
            if (this.dock.length > this.dockSize) {
                const out = this.dock.pop();
                let page = this.pages.find(p => p.length < this.perPage);
                if (!page) { page = []; this.pages.push(page); }
                page.push(out);
            }
        } else {
            while (this.pages.length <= target.page) this.pages.push([]);
            const page = this.pages[target.page];
            page.splice(Math.min(target.index, page.length), 0, id);
            for (let i = target.page; i < this.pages.length; i++) {
                while (this.pages[i].length > this.perPage) {
                    if (i + 1 >= this.pages.length) this.pages.push([]);
                    this.pages[i + 1].unshift(this.pages[i].pop());
                }
            }
        }
        while (this.pages.length > 1 && !this.pages.at(-1).length) this.pages.pop();
        this.save();
    }

    save() {
        try {
            GLib.mkdir_with_parents(GLib.path_get_dirname(FILE), 0o755);
            GLib.file_set_contents(FILE, JSON.stringify({dock: this.dock, pages: this.pages}, null, 1));
        } catch (e) { logError(e, 'neo-launcher: saving the layout'); }
    }
}
