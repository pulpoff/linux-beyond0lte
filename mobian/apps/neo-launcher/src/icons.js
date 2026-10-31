// Launcher icons: the theme's icon, or an icon pack's art, or the app icon wrapped/shaped the way Neo does
// (iconrender.js, shared with the GNOME Shell Neo). Every custom icon is rendered once into
// ~/.cache/neo-launcher/icons/<key>.png and shown as a file icon; the key covers the pack, the shape, the two
// switches, the app and the pixel size, so a settings change simply misses the cache and renders again.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gdk from 'gi://Gdk?version=4.0';
import Gtk from 'gi://Gtk?version=4.0';
import {Renderer} from './iconrender.js';

// Android apps whose pack icon would repeat a GNOME app's: desktop id -> pack drawable
const DISTINCT = {'waydroid.com.android.settings.desktop': 'flatdroid'};

const CACHE = GLib.build_filenamev([GLib.get_user_cache_dir(), 'neo-launcher', 'icons']);
export const PACK_DIRS = [
    GLib.build_filenamev([GLib.get_user_data_dir(), 'neo-launcher', 'iconpacks']),
    '/usr/share/neo-launcher/iconpacks',
];

function readJson(path) {
    try { return JSON.parse(new TextDecoder().decode(GLib.file_get_contents(path)[1])); } catch (_) { return null; }
}

/** The installed packs: [{id, title, dir}] (a pack in the user's dir wins over one of the same name). */
export function listPacks() {
    const packs = new Map();
    for (const base of [...PACK_DIRS].reverse()) {
        let en;
        try { en = Gio.File.new_for_path(base).enumerate_children('standard::name,standard::type', 0, null); } catch (_) { continue; }
        for (let fi; (fi = en.next_file(null));) {
            if (fi.get_file_type() !== Gio.FileType.DIRECTORY) continue;
            const dir = GLib.build_filenamev([base, fi.get_name()]);
            const meta = readJson(GLib.build_filenamev([dir, 'pack.json']));
            if (meta && GLib.file_test(GLib.build_filenamev([dir, 'gnome-map.json']), GLib.FileTest.EXISTS))
                packs.set(fi.get_name(), {id: fi.get_name(), title: meta.title ?? fi.get_name(), dir});
        }
    }
    return [...packs.values()].sort((a, b) => a.title.localeCompare(b.title));
}

export class IconProvider {
    constructor(settings) {
        this.settings = settings;
        this._pack = null; this._packId = null;
        GLib.mkdir_with_parents(CACHE, 0o755);
    }

    _currentPack() {
        const id = this.settings.get_string('icon-pack');
        if (id === this._packId) return this._pack;
        this._packId = id; this._pack = null;
        const info = id ? listPacks().find(p => p.id === id) : null;
        if (info) {
            const map = readJson(GLib.build_filenamev([info.dir, 'gnome-map.json'])) ?? {};
            // Android apps (Waydroid's waydroid.<package>.desktop): the pack's own Android mapping, package ->
            // drawable (appfilter.json, from the pack's appfilter.xml). A GNOME entry of the same id wins.
            for (const [pkg, drawable] of Object.entries(readJson(GLib.build_filenamev([info.dir, 'appfilter.json'])) ?? {}))
                map[`waydroid.${pkg}.desktop`] ??= drawable;
            // Android's settings are not GNOME's: the robot, not the same gear twice (when the pack has it)
            for (const [id, drawable] of Object.entries(DISTINCT))
                if (GLib.file_test(GLib.build_filenamev([info.dir, 'icons', `${drawable}.png`]), GLib.FileTest.EXISTS)) map[id] = drawable;
            this._pack = {dir: info.dir, meta: readJson(GLib.build_filenamev([info.dir, 'pack.json'])) ?? {}, map};
        }
        return this._pack;
    }

    /** The app's own icon file at about `px` device pixels, for the renderer to draw from. */
    _sourceFile(info, px) {
        const gicon = info.get_icon();
        if (!gicon) return null;
        if (gicon instanceof Gio.FileIcon) return gicon.get_file().get_path();
        try {
            const theme = Gtk.IconTheme.get_for_display(Gdk.Display.get_default());
            const paintable = theme.lookup_by_gicon(gicon, px, 1, Gtk.TextDirection.NONE, 0);
            return paintable?.get_file()?.get_path() ?? null;
        } catch (_) { return null; }
    }

    /**
     * The icon to show right now for `info` at `size` logical px on a display of `scale`, and, when a pack or shape
     * icon still has to be drawn, the job for render(): {gicon, job}. Drawing a pack for every app at once blocked
     * the launcher for seconds after picking a pack, so the caller draws the jobs one by one while the theme's icon
     * stands in.
     */
    lookup(info, size, scale = 1) {
        const stock = info.get_icon() ?? Gio.ThemedIcon.new('application-x-executable');
        const pack = this._currentPack();
        const shape = this.settings.get_string('icon-shape');
        const wrap = this.settings.get_boolean('icon-wrap-unthemed');
        const legacy = this.settings.get_boolean('icon-shape-legacy');
        if (!pack && shape === 'system' && !legacy) return {gicon: stock, job: null};   // the theme's icon
        const id = info.get_id();
        const px = Math.round(size * scale);
        // 'b' marks Android icons drawn since they take the pack's appfilter (and Android Settings its robot): their
        // older renders and '.none' markers are skipped
        const android = id.startsWith('waydroid.') ? 'b' : '';
        const key = `${this._packId || '-'}_${shape}_${wrap ? 1 : 0}${legacy ? 1 : 0}${android}_${id.replace(/[^A-Za-z0-9._-]/g, '_')}_${px}`;
        const path = GLib.build_filenamev([CACHE, `${key}.png`]);
        if (GLib.file_test(path, GLib.FileTest.EXISTS)) return {gicon: Gio.FileIcon.new(Gio.File.new_for_path(path)), job: null};
        const nothing = GLib.build_filenamev([CACHE, `${key}.none`]);         // drawn before: nothing custom applies
        if (GLib.file_test(nothing, GLib.FileTest.EXISTS)) return {gicon: stock, job: null};
        const effectiveShape = shape === 'system' ? (pack?.meta?.shape ?? 'squircle') : shape;
        return {gicon: stock, job: {info, id, px, path, nothing, shape: effectiveShape, rawShape: shape, legacy, wrap, pack}};
    }

    /** Draws one job from lookup(); the custom icon, or null when the stock icon stays. */
    render(job) {
        try {
            const done = new Renderer({id: job.id, px: job.px, path: job.path, shape: job.shape, rawShape: job.rawShape,
                legacy: job.legacy, wrap: job.wrap, pack: job.pack, srcFile: this._sourceFile(job.info, job.px),
                assetsDir: CACHE, cacheDir: CACHE}).run(job.px, job.path);
            if (done) return Gio.FileIcon.new(Gio.File.new_for_path(job.path));
            GLib.file_set_contents(job.nothing, '');
        } catch (e) { logError(e, `neo-launcher: icon for ${job.id}`); }
        return null;
    }
}
