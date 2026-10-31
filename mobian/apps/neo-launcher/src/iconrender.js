// Copied unchanged from gnome/launcher/neolauncher@yesman.de/launcher/iconrender.js (cairo only, no shell imports).
// Icon rendering without any shell imports: run by launcher/render-worker.js in its own process, so a
// pack or shape change never blocks the shell (a render costs up to ~110 ms of cairo + PNG work).
// The job carries everything the shell knows: the effective shape, the switches, the pack and the
// app's own icon file. iconpack.js runs the same Renderer in the shell when the worker cannot start.
import Cairo from 'cairo';
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import GdkPixbuf from 'gi://GdkPixbuf';

const LEGACY_FG = 0.62;          // Neo "legacy treatment": the old icon sits at 62 % inside the shaped background

/** Apps whose icon is drawn like an Android adaptive icon, by desktop id: a colour (`bg`, #RRGGBB) behind either a
 *  foreground asset (`fg`, a file in assets/, drawn edge to edge — the asset keeps its own safe-zone padding) or the
 *  app's own icon as a white stencil (`fgFromIcon`, at `fgScale` of the box), clipped to the chosen shape. */
const ADAPTIVE = {};

export class Renderer {
    /** job: {id, px, path, shape, legacy, wrap, pack: {dir, meta, map}|null, srcFile, assetsDir, cacheDir} */
    constructor(job) {
        this.id = job.id; this.effectiveShape = job.shape; this.shape = job.rawShape ?? job.shape;
        this.legacy = job.legacy; this.wrap = job.wrap; this.pack = job.pack; this.srcFile = job.srcFile;
        this._cacheDir = job.cacheDir;
        this._assets = Gio.File.new_for_path(job.assetsDir);
    }
    run(px, path) { return this._render({get_id: () => this.id}, px, path); }

    /** Writes the PNG; false when nothing custom applies (the caller shows the stock icon). */
    _render(app, px, path) {
        const id = this.id;
        const adaptive = ADAPTIVE[id];
        if (adaptive) {
            if (adaptive.fgFromIcon) {
                const inner = Math.round(px * adaptive.fgScale);
                const icon = this._sourcePixbuf(app, inner);
                if (icon) return this._adaptive(adaptive.bg, icon, px, path, {mask: true, inner});
            } else {
                const fg = this._loadPixbuf(this._assets.get_child(adaptive.fg).get_path(), px);
                if (fg) return this._adaptive(adaptive.bg, fg, px, path);
            }
        }
        const packIcon = this.pack?.map?.[id];
        if (packIcon) {
            const pb = this._loadPixbuf(GLib.build_filenamev([this.pack.dir, 'icons', packIcon + '.png']), px);
            if (pb) {
                // "System" keeps the pack's own art. Any other shape re-wraps it the way Neo/Lawnchair do: the
                // pack icon over a background in the colour of its own edge, clipped to the shape — a OnePlus
                // circle becomes a squircle of the same colour instead of a circle floating in a squircle.
                if (this.shape === 'system') { this._surfaceOf(pb, px).writeToPNG(path); return true; }
                return this._reshape(pb, px, path);
            }
        }
        const src = this._sourcePixbuf(app, px);
        if (!src) return false;
        const back = this.pack?.meta?.iconback ?? [];
        if (this.wrap && this.pack && back.length) return this._wrapWithPack(id, src, px, path);
        if (this.legacy) return this._legacy(src, px, path);
        return false;
    }

    _wrapWithPack(id, src, px, path) {
        const meta = this.pack.meta;
        const pick = (list) => list.length ? list[hashInt(id) % list.length] : null;
        const surf = new Cairo.ImageSurface(Cairo.Format.ARGB32, px, px);
        const cr = new Cairo.Context(surf);
        const draw = (name) => {
            const pb = name && this._loadPixbuf(GLib.build_filenamev([this.pack.dir, 'icons', name + '.png']), px);
            if (!pb) return false;
            cr.setSourceSurface(this._surfaceOf(pb, px), 0, 0); cr.paint(); return true;
        };
        draw(pick(meta.iconback));
        // the app icon, scaled by the pack's factor around the centre
        const f = Number(meta.scale) || 1;
        const inner = Math.round(px * f);
        const fg = this._surfaceOf(src, inner);
        cr.setOperator(Cairo.Operator.OVER);
        cr.setSourceSurface(fg, Math.round((px - inner) / 2), Math.round((px - inner) / 2)); cr.paint();
        // iconmask: opaque mask pixels are cut away (PorterDuff DST_OUT)
        const mask = pick(meta.iconmask);
        if (mask) {
            const pb = this._loadPixbuf(GLib.build_filenamev([this.pack.dir, 'icons', mask + '.png']), px);
            if (pb) { cr.setOperator(Cairo.Operator.DEST_OUT); cr.setSourceSurface(this._surfaceOf(pb, px), 0, 0); cr.paint(); cr.setOperator(Cairo.Operator.OVER); }
        }
        draw(pick(meta.iconupon));
        surf.flush(); surf.writeToPNG(path);
        return true;
    }

    _adaptive(bg, fg, px, path, {mask = false, inner: maskPx = 0} = {}) {
        const surf = new Cairo.ImageSurface(Cairo.Format.ARGB32, px, px);
        const cr = new Cairo.Context(surf);
        shapePath(cr, this.effectiveShape, px);
        cr.clip();
        const n = parseInt(bg.slice(1), 16);
        cr.setSourceRGBA(((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255, 1); cr.paint();
        if (mask) {       // the icon's alpha as a stencil, painted white and centred
            cr.setSourceRGBA(1, 1, 1, 1);
            cr.maskSurface(this._surfaceOf(fg, maskPx), Math.round((px - maskPx) / 2), Math.round((px - maskPx) / 2));
            surf.flush(); surf.writeToPNG(path);
            return true;
        }
        // AdaptiveIconDrawable shows the central 72 of the 108 dp layer: the foreground is drawn 1.5× and cropped by the shape
        const inner = Math.round(px * 108 / 72 * 1.25);   // ×1.25 on top, as the H2O/OnePlus wraps scale a foreground: the thumb fills ~60 % of the box like on Android
        cr.setSourceSurface(this._surfaceOf(fg, inner), Math.round((px - inner) / 2), Math.round((px - inner) / 2)); cr.paint();
        surf.flush(); surf.writeToPNG(path);
        return true;
    }

    _reshape(pb, px, path) {
        const surf = new Cairo.ImageSurface(Cairo.Format.ARGB32, px, px);
        const cr = new Cairo.Context(surf);
        shapePath(cr, this.effectiveShape, px);
        cr.clip();
        const [r, g, b] = edgeColorOf(pb) ?? tintOf(pb);
        cr.setSourceRGBA(r, g, b, 1); cr.paint();
        cr.setSourceSurface(this._surfaceOf(pb, px), 0, 0); cr.paint();
        surf.flush(); surf.writeToPNG(path);
        return true;
    }

    _legacy(src, px, path) {
        const surf = new Cairo.ImageSurface(Cairo.Format.ARGB32, px, px);
        const cr = new Cairo.Context(surf);
        shapePath(cr, this.effectiveShape, px);
        cr.clip();
        const [r, g, b] = tintOf(src);
        cr.setSourceRGBA(r, g, b, 1); cr.paint();
        const inner = Math.round(px * LEGACY_FG);
        cr.setSourceSurface(this._surfaceOf(src, inner), Math.round((px - inner) / 2), Math.round((px - inner) / 2)); cr.paint();
        surf.flush(); surf.writeToPNG(path);
        return true;
    }

    /** The app's own icon (resolved to a file by the shell) as a pixbuf at `px`. */
    _sourcePixbuf(_app, px) { return this.srcFile ? this._loadPixbuf(this.srcFile, px) : null; }

    _loadPixbuf(path, px) {
        try { return GdkPixbuf.Pixbuf.new_from_file_at_scale(path, px, px, true); } catch (_) { return null; }
    }

    /** A pixbuf as a cairo surface of exactly `px`², centred (cairo reads PNG; the pixbuf is written once to a temp file). */
    _surfaceOf(pb, px) {
        const scaled = pb.get_width() === px && pb.get_height() === px ? pb : fit(pb, px);
        const tmp = GLib.build_filenamev([this._cacheDir, `.tmp-${GLib.get_monotonic_time()}.png`]);
        scaled.savev(tmp, 'png', [], []);
        const s = Cairo.ImageSurface.createFromPNG(tmp);
        GLib.unlink(tmp);
        return s;
    }
}

/** Scale into a px² square, aspect kept, centred on transparent. */
function fit(pb, px) {
    const w = pb.get_width(), h = pb.get_height();
    const f = Math.min(px / w, px / h);
    const sw = Math.max(1, Math.round(w * f)), sh = Math.max(1, Math.round(h * f));
    const out = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, true, 8, px, px);
    out.fill(0x00000000);
    pb.scale(out, Math.round((px - sw) / 2), Math.round((px - sh) / 2), sw, sh, (px - sw) / 2, (px - sh) / 2, f, f, GdkPixbuf.InterpType.BILINEAR);
    return out;
}

/** Average colour of the icon's opaque pixels, lifted towards white so the icon stays readable on it. */
function tintOf(pb) {
    const pixels = pb.get_pixels(), n = pb.get_n_channels(), stride = pb.get_rowstride(), w = pb.get_width(), h = pb.get_height();
    let r = 0, g = 0, b = 0, c = 0;
    for (let y = 0; y < h; y += 3) {
        for (let x = 0; x < w; x += 3) {
            const o = y * stride + x * n;
            if (n === 4 && pixels[o + 3] < 128) continue;
            r += pixels[o]; g += pixels[o + 1]; b += pixels[o + 2]; c++;
        }
    }
    if (!c) return [0.91, 0.92, 0.96];
    r /= c * 255; g /= c * 255; b /= c * 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b);
    if (max - min < 0.08) return [0.91, 0.92, 0.96];          // grey icon → neutral light background
    const mix = 0.55;                                       // pastel
    return [r + (1 - r) * mix, g + (1 - g) * mix, b + (1 - b) * mix];
}

/** The colour just inside the icon's outline (a ring at 90–97 % of the radius): what a shaped background must match. null when the ring is see-through. */
function edgeColorOf(pb) {
    const pixels = pb.get_pixels(), n = pb.get_n_channels(), stride = pb.get_rowstride(), w = pb.get_width(), h = pb.get_height();
    const cx = w / 2, cy = h / 2, R = Math.min(w, h) / 2;
    let r = 0, g = 0, b = 0, c = 0, seen = 0;
    for (let i = 0; i < 72; i++) {
        const a = (i / 72) * 2 * Math.PI;
        for (const f of [0.9, 0.94, 0.97]) {
            const x = Math.round(cx + Math.cos(a) * R * f), y = Math.round(cy + Math.sin(a) * R * f);
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            const o = y * stride + x * n; seen++;
            if (n === 4 && pixels[o + 3] < 200) continue;
            r += pixels[o]; g += pixels[o + 1]; b += pixels[o + 2]; c++;
        }
    }
    if (!seen || c < seen * 0.5) return null;
    return [r / c / 255, g / c / 255, b / c / 255];
}

function hashInt(s) { let h = 0; for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return Math.abs(h); }

/** Adds the shape's closed path for a px² box. */
export function shapePath(cr, shape, s) {
    const c = s / 2;
    switch (shape) {
    case 'circle': cr.arc(c, c, c, 0, 2 * Math.PI); break;
    case 'square': roundedRect(cr, s, [0.06, 0.06, 0.06, 0.06]); break;
    case 'rounded': roundedRect(cr, s, [0.2, 0.2, 0.2, 0.2]); break;
    case 'teardrop': roundedRect(cr, s, [0.5, 0.5, 0.12, 0.5]); break;
    case 'squircle': superellipse(cr, s, 4, 4); break;
    case 'cupertino': superellipse(cr, s, 5, 5); break;
    case 'cylinder': superellipse(cr, s, 2.2, 6); break;
    case 'egg': {
        cr.moveTo(c + 0.46 * s, c);
        for (let i = 1; i <= 96; i++) {
            const t = (i / 96) * 2 * Math.PI, sy = Math.sin(t);
            cr.lineTo(c + 0.46 * s * Math.cos(t) * (1 + 0.14 * sy), c + 0.47 * s * sy);
        }
        cr.closePath(); break;
    }
    case 'octagon': polygon(cr, s, 8, Math.PI / 8); break;
    case 'hexagon': polygon(cr, s, 6, 0); break;
    case 'diamond': polygon(cr, s, 4, 0); break;
    default: superellipse(cr, s, 4, 4);
    }
}

function roundedRect(cr, s, [tl, tr, br, bl]) {
    const R = (f) => f * s;
    cr.newSubPath();
    cr.arc(s - R(tr), R(tr), R(tr), -Math.PI / 2, 0);
    cr.arc(s - R(br), s - R(br), R(br), 0, Math.PI / 2);
    cr.arc(R(bl), s - R(bl), R(bl), Math.PI / 2, Math.PI);
    cr.arc(R(tl), R(tl), R(tl), Math.PI, 3 * Math.PI / 2);
    cr.closePath();
}
/** |x|^nx + |y|^ny = 1, 0.5 px inset so the anti-aliased edge is not clipped. */
function superellipse(cr, s, nx, ny) {
    const c = s / 2, r = s / 2 - 0.5, N = 128;
    for (let i = 0; i <= N; i++) {
        const t = (i / N) * 2 * Math.PI, ct = Math.cos(t), st = Math.sin(t);
        const x = Math.sign(ct) * Math.pow(Math.abs(ct), 2 / nx), y = Math.sign(st) * Math.pow(Math.abs(st), 2 / ny);
        if (i === 0) cr.moveTo(c + r * x, c + r * y); else cr.lineTo(c + r * x, c + r * y);
    }
    cr.closePath();
}
function polygon(cr, s, n, rot) {
    const c = s / 2, r = s / 2 - 0.5;
    for (let i = 0; i < n; i++) {
        const a = rot - Math.PI / 2 + (i / n) * 2 * Math.PI;
        if (i === 0) cr.moveTo(c + r * Math.cos(a), c + r * Math.sin(a)); else cr.lineTo(c + r * Math.cos(a), c + r * Math.sin(a));
    }
    cr.closePath();
}
