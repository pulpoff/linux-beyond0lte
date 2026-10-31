// The app drawer as a sheet that follows the finger (Launcher3's AllAppsTransitionController): a swipe up on the home
// screen pulls it up, a swipe down from the top of its list pulls it down, and on release it settles open or closed
// by how far it travelled and how fast. The child keeps its size; only its allocation is translated, so dragging
// costs no relayout of the app grid.
import GObject from 'gi://GObject';
import Gtk from 'gi://Gtk?version=4.0';
import Gsk from 'gi://Gsk?version=4.0';
import Graphene from 'gi://Graphene';
import Adw from 'gi://Adw?version=1';

const SETTLE_MS = 260;
const FLING = 0.5;          // px/ms: a faster release settles in its direction whatever the distance

export const Sheet = GObject.registerClass({
    Signals: {'settled': {param_types: [GObject.TYPE_BOOLEAN]}},
}, class Sheet extends Gtk.Widget {
    _init(child) {
        super._init({hexpand: true, vexpand: true});
        this._child = child;
        child.set_parent(this);
        this._progress = 0;          // 0 closed … 1 open
        this._anim = null;
        this._sync();
    }

    get progress() { return this._progress; }
    get isOpen() { return this._progress >= 1; }

    setProgress(p) {
        this._progress = Math.max(0, Math.min(1, p));
        this._sync();
        this.queue_allocate();
        this.onProgress?.(this._progress);
    }

    /** While closed it takes no input, so the home screen under it works as before. It stays mapped (parked below
     *  the screen): mapping the whole drawer on the first frame of a swipe made every swipe start with a stall. */
    _sync() {
        this.set_can_target(this._progress > 0);
    }

    /** Settles open (true) or closed (false). */
    settle(open, animate = true) {
        this._anim?.skip();
        const to = open ? 1 : 0;
        if (!animate || this._progress === to) { this.setProgress(to); this.emit('settled', open); return; }
        const target = Adw.CallbackAnimationTarget.new(v => this.setProgress(v));
        this._anim = new Adw.TimedAnimation({widget: this, value_from: this._progress, value_to: to,
            duration: SETTLE_MS * Math.abs(to - this._progress) + 60, easing: Adw.Easing.EASE_OUT_CUBIC, target});
        this._anim.connect('done', () => { this._anim = null; this.emit('settled', open); });
        this._anim.play();
    }

    /** Release of a drag that moved the sheet by `dy` (px, up < 0) at `velocity` (px/ms, up < 0). */
    release(velocity) {
        if (Math.abs(velocity) > FLING) this.settle(velocity < 0);
        else this.settle(this._progress > 0.5);
    }

    vfunc_measure(orientation, forSize) { return this._child.measure(orientation, forSize).slice(0, 2).concat([-1, -1]); }

    vfunc_size_allocate(width, height, baseline) {
        const offset = Math.round((1 - this._progress) * height);
        const t = new Gsk.Transform().translate(new Graphene.Point({x: 0, y: offset}));
        this._child.allocate(width, height, baseline, t);
    }

    vfunc_dispose() {
        this._anim?.skip();
        this._child?.unparent();
        this._child = null;
        super.vfunc_dispose();
    }
});
