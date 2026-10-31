// Moving icons on the home screen (Launcher3): touch and hold an icon, then move it. From the drawer the drawer goes
// and the icon is a copy (the app keeps the icons it has); an app may have several. A floating copy follows the
// finger, the cell it left is dimmed; resting at the left or right screen edge turns the page (past the last page a
// new one opens); letting go drops it on the cell or the dock slot under the finger. Hold and let go without moving
// opens the icon's menu instead; a quick tap still launches.
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk?version=4.0';
import Graphene from 'gi://Graphene';

const START = 12;            // px the finger moves after the hold before the icon follows it
const EDGE = 28;             // px from a screen edge where resting turns the page
const EDGE_MS = 550;

export class IconDrag {
    /** host: {root (the window's overlay), layer (a Gtk.Fixed above everything), carousel, dock, model, cols, rows,
     *  onDrop(), menu(button, info, where), icon(info) → Gtk.Image} */
    constructor(host) { this.h = host; this._s = null; }

    /** Adds hold + drag to a home/dock icon button. */
    attach(button, info, where) {
        const hold = new Gtk.GestureLongPress({touch_only: false});
        const drag = new Gtk.GestureDrag();
        hold.group(drag);                                   // one sequence: the hold's claim keeps the drag alive
        hold.connect('pressed', g => {
            g.set_state(Gtk.EventSequenceState.CLAIMED);   // no click, no page swipe after a hold
            button.add_css_class('neo-lifted');
            this._s = {button, info, where, moving: false};
        });
        drag.connect('drag-update', (g, dx, dy) => {
            const s = this._s;
            if (!s || s.button !== button) return;
            const [, sx, sy] = g.get_start_point();
            const p = this._toRoot(button, sx + dx, sy + dy);
            if (!s.moving && Math.hypot(dx, dy) > START) this._begin(s, p);
            if (s.moving) this._move(s, p);
        });
        drag.connect('drag-end', () => {
            const s = this._s;
            if (!s || s.button !== button) return;
            this._s = null;
            button.remove_css_class('neo-lifted');
            if (s.moving) this._drop(s);
            else this.h.menu(button, info, where);
        });
        button.add_controller(hold);
        button.add_controller(drag);
    }

    _toRoot(widget, x, y) {
        const [ok, p] = widget.compute_point(this.h.root, new Graphene.Point({x, y}));
        return ok ? p : new Graphene.Point({x, y});
    }

    _begin(s, p) {
        s.moving = true;
        if (s.where === 'drawer') this.h.leaveDrawer?.();
        s.button.set_opacity(0.25);
        s.float = this.h.icon(s.info);
        s.float.add_css_class('neo-floating');
        this.h.layer.put(s.float, 0, 0);
        s.size = s.float.get_pixel_size();
        this._move(s, p);
    }

    _move(s, p) {
        s.at = p;
        this.h.layer.move(s.float, p.x - s.size / 2, p.y - s.size / 2);
        const w = this.h.root.get_width();
        const side = p.x < EDGE ? -1 : p.x > w - EDGE ? 1 : 0;
        if (side !== s.edge) {
            s.edge = side;
            if (s.edgeId) { GLib.source_remove(s.edgeId); s.edgeId = 0; }
            if (side) s.edgeId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, EDGE_MS, () => { this._turn(s, side); return GLib.SOURCE_CONTINUE; });
        }
    }

    _turn(s, side) {
        const c = this.h.carousel;
        const page = Math.round(c.get_position()) + side;
        if (page < 0) return;
        if (page >= c.get_n_pages()) c.append(this.h.emptyPage());   // the model gets the page on drop
        c.scroll_to(c.get_nth_page(page), true);
    }

    /** Where the finger is: {dock: index} or {page, index}. */
    _target(p) {
        const dock = this.h.dock;
        const [okd, pd] = this.h.root.compute_point(dock, p);
        const n = Math.max(1, this.h.model.dockSize);
        // the dock is a row under the grid, or in landscape a column to its right
        if (this.h.landscape) {
            if (okd && pd.x >= -40 && pd.x <= dock.get_width())
                return {dock: Math.max(0, Math.min(n - 1, Math.floor(pd.y / (dock.get_height() / n))))};
        } else if (okd && pd.y >= 0 && pd.y <= dock.get_height() + 40) {
            return {dock: Math.max(0, Math.min(n - 1, Math.floor(pd.x / (dock.get_width() / n))))};
        }
        const c = this.h.carousel;
        const page = Math.round(c.get_position());
        const grid = c.get_nth_page(page);
        const [okg, pg] = this.h.root.compute_point(grid, p);
        if (!okg) return null;
        const {cols, rows} = this.h;                 // as drawn: turned sideways in landscape
        const col = Math.max(0, Math.min(cols - 1, Math.floor(pg.x / (grid.get_width() / cols))));
        const row = Math.max(0, Math.min(rows - 1, Math.floor(pg.y / (grid.get_height() / rows))));
        return {page, index: row * cols + col};
    }

    _drop(s) {
        if (s.edgeId) { GLib.source_remove(s.edgeId); s.edgeId = 0; }
        const target = s.at ? this._target(s.at) : null;
        this.h.layer.remove(s.float);
        s.button.set_opacity(1);
        if (target && s.where === 'drawer') {
            // from the drawer: a copy; the app keeps any icon it has already
            this.h.model.move(s.info.get_id(), target, null, {copy: true});
            this.h.unhide?.(s.info.get_id());
        } else if (target) {
            this.h.model.move(s.info.get_id(), target, s.button._pos);
        }
        // rebuild after this event: the dragged button must outlive its own gesture
        GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => { this.h.onDrop(); return GLib.SOURCE_REMOVE; });
    }
}
