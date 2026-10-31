/* Neo's home screen and app drawer, as Phosh draws its notification shade: the home screen and the drawer are one
 * layer surface twice the screen's height, home on top, the drawer below it, anchored to the bottom edge. Folded,
 * the drawer hangs below the screen; a vertical swipe is phoc's own drag (zphoc_draggable_layer_surface_v1), which
 * moves the surface with the finger and animates it to rest. Neo draws nothing while it moves, so it is as smooth
 * as the shade on any GPU. Taps, page swipes (sideways first) and the drawer's scrolling stay Neo's.
 * The wallpaper is a second, static surface under it, drawn once. */
#include "neo-home.h"

#include <gio/gdesktopappinfo.h>
#include <math.h>
#include "neo-apps.h"
#include "neo-icons.h"
#include "neo-launch.h"
#include "neo-model.h"
#include "neo-parallax.h"
#include "neo-settings.h"
#include "neo-surf.h"
#include "neo-widgets.h"
#include "neo-wall.h"
#include "neo-wayland.h"

#include <gtk-layer-shell.h>
#include <handy.h>

#define ICON 60          /* logical px at Phosh's scale (≈ Launcher3's 54 dp) */
#define DOCK_ICON 56
#define ICON_LAND 52     /* landscape: the grid turns sideways, the dock becomes a column at the right edge */
#define DOCK_ICON_LAND 48
#define THRESHOLD 0.25   /* of the travel: how far the drawer must be pulled to settle the other way */
#define LIFT_START 12    /* px the finger moves after the hold before the icon follows it */
#define EDGE 28          /* px from a screen edge where resting turns the page */
#define EDGE_MS 550

struct _NeoHome {
  GtkApplication *app;
  gint64      show_t0;      /* µs, monotonic: when Phosh asked for the home (timing the way back) */
  GSettings  *settings;
  NeoIcons   *icons;
  NeoModel   *model;
  GHashTable *apps;
  NeoLaunch  *launch;

  GtkWidget  *wall_win, *wall_area;
  GdkPixbuf  *wall;
  GtkWidget  *win, *layers, *home, *pages, *carousel, *dots, *dock;
  GtkWidget  *float_layer;     /* a GtkFixed over everything: the icon being moved */
  struct Lift *lift;           /* the icon being held or moved, or NULL */
  struct LaunchCard *card;     /* an app's launch card, growing or waiting for the app's window, or NULL */
  struct Slide *slide;         /* the pages sliding sideways under the finger or to rest, or NULL */
  struct Glide *glide;         /* the drawer's list as pictures the compositor scrolls (made once, kept) */
  GPtrArray  *pics;            /* PagePic per page: the pages as pictures, kept and redrawn only where they changed */
  GtkWidget  *strip;           /* weak: the clock and weather on the first page (it changes while not seen) */
  gboolean    picturing;       /* drawing a page into its picture: its own "draw" is no change */
  gboolean    pics_quiet;      /* the carousel is drawn again after a slide: it shows what the pictures showed */
  guint       pics_idle;
  GtkWidget  *drawer, *search, *drawer_scroll, *drawer_flow;
  GPtrArray  *drawer_cells;
  NeoDrag    *drag;
  NeoParallax *parallax;
  gboolean    surfs_ok;        /* the compositor moves subsurfaces: the pages slide as pictures */
  gint64      scroll_t;        /* µs: when the drawer's list or the pages last began to move under a finger */
  gint64      touch_t;         /* µs: when the last touch began */
  gboolean    touch_down;      /* a finger is on the screen */
  double      touch_x0, touch_y0;  /* where it came down (root coordinates) */
  gboolean    touch_moved;     /* and it has moved from there: a swipe, whoever moves with it (GTK or phoc) */

  int         W, Hv;           /* the usable screen (logical px), from the wallpaper window's allocation */
  gboolean    land;
  gboolean    want_shown;      /* shown, or waiting for the wallpaper window to learn the size */
  gboolean    locked, blanked; /* Phosh's lock screen up, the panel off: nothing of the home is seen */

  GQueue      icon_jobs;
  guint       icon_idle, apps_timer, rescan_timer, theme_timer;

  void      (*visible_cb) (gpointer, gboolean);
  gpointer    visible_data;
};

typedef struct {
  GtkWidget *image;            /* weak */
  GAppInfo  *info;
  int        px;
} IconJob;

static void fill_home (NeoHome *self);
static void launch_card_remove (NeoHome *self);
static void slide_free (NeoHome *self);
static void pics_stale (NeoHome *self);
static void pics_reset (NeoHome *self);
static void pics_prerender (NeoHome *self);
static void pics_mark_strip (NeoHome *self, int i);
static void glide_stale (NeoHome *self);
static gboolean finger_scrolls (NeoHome *self);
static gboolean touch_swiped (NeoHome *self);
static void glide_catch (NeoHome *self);
static void glide_setup (NeoHome *self);
static void glide_hide (NeoHome *self);
static void glide_prerender (NeoHome *self);
static void slide_pan (GtkGesturePan *g, GtkPanDirection dir, double offset, NeoHome *self);
static void slide_update (GtkGestureDrag *g, double ox, double oy, NeoHome *self);
static void slide_end (GtkGestureDrag *g, double ox, double oy, NeoHome *self);
static void slide_cancel (GtkGesture *g, GdkEventSequence *seq, NeoHome *self);
static void parallax_sync (NeoHome *self);
static GtkWidget *page_grid (void);
static void fill_drawer (NeoHome *self);
static void reload (NeoHome *self, gboolean rebuild);

int
neo_home_scale (NeoHome *self)
{
  GdkMonitor *m = gdk_display_get_monitor (gdk_display_get_default (), 0);
  return m ? gdk_monitor_get_scale_factor (m) : 1;
}

/* ---- icons ---- */

static void
set_image_pixbuf (GtkWidget *image, GdkPixbuf *pb, int scale)
{
  cairo_surface_t *s = gdk_cairo_surface_create_from_pixbuf (pb, scale, NULL);
  gtk_image_set_from_surface (GTK_IMAGE (image), s);
  cairo_surface_destroy (s);
}

static gboolean
icon_idle (gpointer data)
{
  NeoHome *self = data;
  IconJob *job = g_queue_pop_head (&self->icon_jobs);
  if (!job) {
    self->icon_idle = 0;
    return G_SOURCE_REMOVE;
  }
  if (job->image) {
    g_autoptr (GdkPixbuf) pb = neo_icons_render (self->icons, job->info, job->px);
    if (pb && job->image) {
      set_image_pixbuf (job->image, pb, neo_home_scale (self));
      glide_stale (self);
      pics_stale (self);
    }
    g_object_remove_weak_pointer (G_OBJECT (job->image), (gpointer *) &job->image);
  }
  g_object_unref (job->info);
  g_free (job);
  return G_SOURCE_CONTINUE;
}

/* Pack/shape icons are drawn one per idle tick, so the launcher stays usable while a new pack renders. */
static GtkWidget *
icon_image (NeoHome *self, GAppInfo *info, int size)
{
  int scale = neo_home_scale (self);
  gboolean pending;
  g_autoptr (GdkPixbuf) pb = neo_icons_lookup (self->icons, info, size * scale, &pending);
  GtkWidget *image = gtk_image_new ();
  set_image_pixbuf (image, pb, scale);
  if (pending) {
    IconJob *job = g_new0 (IconJob, 1);
    job->image = image;
    g_object_add_weak_pointer (G_OBJECT (image), (gpointer *) &job->image);
    job->info = g_object_ref (info);
    job->px = size * scale;
    g_queue_push_tail (&self->icon_jobs, job);
    if (!self->icon_idle)
      self->icon_idle = g_idle_add_full (G_PRIORITY_LOW, icon_idle, self, NULL);
  }
  return image;
}

GdkPixbuf *
neo_home_app_icon (NeoHome *self, GAppInfo *info, int px)
{
  gboolean pending;
  return neo_icons_lookup (self->icons, info, px, &pending);
}

/* ---- the drawer's place ---- */

static void
sync_drag_mode (NeoHome *self)
{
  if (!self->drag)
    return;
  /* open with its list scrolled: a swipe down scrolls the list; at the top of the list it pulls the drawer down */
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->drawer_scroll));
  gboolean scrolled = gtk_adjustment_get_value (adj) > 0.5;
  /* an icon held or moving is the finger's, not a drawer drag */
  neo_drag_set_mode (self->drag, self->lift || (neo_drag_is_unfolded (self->drag) && scrolled) ? NEO_DRAG_NONE
                                                                                                  : NEO_DRAG_FULL);
}

static void
reset_drawer (NeoHome *self)
{
  gtk_entry_set_text (GTK_ENTRY (self->search), "");
  gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->drawer_scroll)), 0);
}

static void
on_drag_end (gpointer data, gboolean unfolded)
{
  NeoHome *self = data;
  if (!unfolded)
    reset_drawer (self);
  else if (gtk_widget_get_visible (self->search))
    gtk_widget_grab_focus (self->search);
  if (unfolded)
    glide_prerender (self);
  sync_drag_mode (self);
}

static gboolean
drawer_open (NeoHome *self)
{
  return self->drag && (neo_drag_is_unfolded (self->drag) || neo_drag_is_moving (self->drag));
}

static void
close_drawer (NeoHome *self)
{
  if (self->drag && (neo_drag_is_unfolded (self->drag) || neo_drag_is_moving (self->drag)))
    neo_drag_slide (self->drag, FALSE);
}

/* ---- sizes ---- */

static gboolean
is_landscape (NeoHome *self)
{
  return self->W > self->Hv;
}

static int grid_cols (NeoHome *self) { return self->land ? self->model->rows : self->model->cols; }

static void
apply_orientation (NeoHome *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self->home), self->land ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self->dock), self->land ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_halign (self->dock, self->land ? GTK_ALIGN_END : GTK_ALIGN_FILL);
  gtk_widget_set_valign (self->dock, self->land ? GTK_ALIGN_FILL : GTK_ALIGN_END);
  GtkStyleContext *ctx = gtk_widget_get_style_context (self->win);
  if (self->land)
    gtk_style_context_add_class (ctx, "neo-landscape");
  else
    gtk_style_context_remove_class (ctx, "neo-landscape");
}

static void
drawer_columns (NeoHome *self)
{
  int n = MIN (g_settings_get_int (self->settings, "drawer-grid-columns"), neo_home_max_columns (self));
  if (self->land)
    n = MAX (n, (int) (n * (double) self->W / MAX (1, self->Hv) + 0.5));
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (self->drawer_flow), n);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (self->drawer_flow), n);
}

/* The surface is the usable screen twice over: the home screen, then the drawer. Folded, its bottom margin hangs
 * the drawer below the screen. */
static void
apply_size (NeoHome *self)
{
  GtkWindow *win = GTK_WINDOW (self->win);
  gtk_widget_set_size_request (self->home, self->W, self->Hv);
  gtk_widget_set_size_request (self->drawer, self->W, self->Hv);
  gtk_window_resize (win, self->W, 2 * self->Hv);
  gtk_widget_set_size_request (self->win, -1, 2 * self->Hv);
  /* phoc keeps a draggable surface's exclusive zone at -margin (they cancel out), but only when phoc moves the
   * margin itself. A margin from the client left the old zone behind: on rotation the portrait zone (713) against
   * the landscape margin (-313) took 400 px off a 360 px tall screen, the next window got a negative size and phoc
   * aborted, taking the session with it. So the pair always goes in one commit. */
  gtk_layer_set_margin (win, GTK_LAYER_SHELL_EDGE_BOTTOM, -self->Hv);
  gtk_layer_set_exclusive_zone (win, self->Hv);
  if (self->drag)
    neo_drag_set_margins (self->drag, -self->Hv, 0);
}

static void
draw_wall (GtkWidget *area, cairo_t *cr, NeoHome *self)
{
  int scale = gtk_widget_get_scale_factor (area);
  if (!self->wall || self->parallax) {      /* the parallax layers cover it */
    cairo_set_source_rgb (cr, 0x10 / 255.0, 0x14 / 255.0, 0x18 / 255.0);
    cairo_paint (cr);
    return;
  }
  cairo_scale (cr, 1.0 / scale, 1.0 / scale);
  gdk_cairo_set_source_pixbuf (cr, self->wall, 0, 0);
  cairo_paint (cr);
}

static void
load_wall (NeoHome *self)
{
  int scale = neo_home_scale (self);
  if (self->W <= 0)
    return;
  g_clear_object (&self->wall);
  if (self->parallax)        /* the layers cover it: not worth decoding (a rotation waits on it) */
    return;
  self->wall = neo_wall_get (self->W * scale, self->Hv * scale);
  gtk_widget_queue_draw (self->wall_area);
}

static void
on_wall_changed (gpointer data)
{
  load_wall (data);
}

GdkPixbuf *
neo_home_wallpaper (NeoHome *self, int w, int h)
{
  return neo_wall_get (w, h);
}

/* The wallpaper window spans the usable screen: its allocation is the size Neo lays out for (and a rotation's new
 * size arrives here first). */
static void
on_wall_allocate (GtkWidget *w, GdkRectangle *a, NeoHome *self)
{
  if (a->width <= 1 || a->height <= 1)
    return;
  if (a->width != self->W || a->height != self->Hv) {
    gboolean turned = self->W && (a->width > a->height) != is_landscape (self);
    self->W = a->width;
    self->Hv = a->height;
    gint64 t0 = g_get_monotonic_time (), t1, t2, t3;
    apply_size (self);
    t1 = g_get_monotonic_time ();
    load_wall (self);
    t2 = g_get_monotonic_time ();
    if (self->parallax)
      neo_parallax_resize (self->parallax, self->W, self->Hv, neo_home_scale (self));
    t3 = g_get_monotonic_time ();
    gboolean land = is_landscape (self);
    if (land != self->land || turned) {
      self->land = land;
      apply_orientation (self);
      fill_home (self);
      drawer_columns (self);
    }
    g_message ("laid out for %dx%d: size %.0f ms, wallpaper %.0f, parallax %.0f, home %.0f", self->W, self->Hv,
               (t1 - t0) / 1e3, (t2 - t1) / 1e3, (t3 - t2) / 1e3, (g_get_monotonic_time () - t3) / 1e3);
  }
  if (self->want_shown && !gtk_widget_get_visible (self->win))
    gtk_widget_show (self->win);
}

/* ---- cells ---- */

static void
cell_clicked (GtkButton *b, NeoHome *self)
{
  GAppInfo *info = g_object_get_data (G_OBJECT (b), "info");
  /* the release of a touch that slid the pages or moved the list: the button under it still heard a click */
  g_autoptr (GdkEvent) ev = gtk_get_current_event ();
  if (ev && ev->type == GDK_TOUCH_END && touch_swiped (self)) {
    g_debug ("launch: a click after the touch scrolled, ignored");
    return;
  }
  if (info)
    neo_launch_app (self->launch, info, GTK_WIDGET (b));
}

static void
menu_item (GtkWidget *box, const char *label, const char *icon, GCallback cb, gpointer data)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_container_add (GTK_CONTAINER (row), gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_MENU));
  GtkWidget *l = gtk_label_new (label);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_container_add (GTK_CONTAINER (row), l);
  GtkWidget *b = gtk_button_new ();
  gtk_button_set_relief (GTK_BUTTON (b), GTK_RELIEF_NONE);
  gtk_style_context_add_class (gtk_widget_get_style_context (b), "neo-menu-item");
  gtk_container_add (GTK_CONTAINER (b), row);
  g_signal_connect (b, "clicked", cb, data);
  gtk_container_add (GTK_CONTAINER (box), b);
}

static gboolean
destroy_idle (gpointer w)
{
  gtk_widget_destroy (w);
  g_object_unref (w);
  return G_SOURCE_REMOVE;
}

/* "closed" comes from inside gtk_popover_popdown (), which still uses the popover afterwards, and the item's
 * callback still needs its MenuCtx: destroy it once the main loop is back */
static void
menu_closed (GtkWidget *pop)
{
  g_idle_add (destroy_idle, g_object_ref (pop));
}

static GtkWidget *
menu_new (GtkWidget *relative)
{
  GtkWidget *pop = gtk_popover_new (relative);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class (gtk_widget_get_style_context (box), "neo-menu");
  gtk_container_add (GTK_CONTAINER (pop), box);
  g_signal_connect (pop, "closed", G_CALLBACK (menu_closed), NULL);
  return pop;
}

/* The window is the home over the drawer, twice the screen's height: with the drawer up the home is off screen
 * above, and GTK, which only sees the window, opened a menu for an icon at the drawer's top into that invisible
 * half. Above only when it fits in the half that shows; else below. `y`: the top of what it points at, `h` its
 * height, both in `relative`'s coordinates. */
static void
menu_place (NeoHome *self, GtkWidget *pop, GtkWidget *relative, int y, int h)
{
  int wx, wy;
  if (!gtk_widget_translate_coordinates (relative, self->win, 0, y, &wx, &wy))
    return;
  int top = drawer_open (self) ? self->Hv : 0, bottom = top + self->Hv;
  int mh = 0;
  gtk_widget_get_preferred_height (pop, NULL, &mh);
  mh += 12;                                   /* the arrow and a margin */
  gboolean above = wy - mh >= top || wy + h + mh > bottom;
  gtk_popover_set_position (GTK_POPOVER (pop), above ? GTK_POS_TOP : GTK_POS_BOTTOM);
}

static GtkWidget *
menu_box (GtkWidget *pop)
{
  return gtk_bin_get_child (GTK_BIN (pop));
}

static void
popdown_of (GtkWidget *b)
{
  GtkWidget *pop = gtk_widget_get_ancestor (b, GTK_TYPE_POPOVER);
  if (pop)
    gtk_popover_popdown (GTK_POPOVER (pop));
}

typedef struct {
  NeoHome  *self;
  GAppInfo *info;
  NeoPos    pos;
  char     *where;
} MenuCtx;

static void
menu_ctx_free (MenuCtx *c)
{
  g_object_unref (c->info);
  g_free (c->where);
  g_free (c);
}

static void
set_hidden (NeoHome *self, const char *id, gboolean hidden)
{
  g_auto (GStrv) list = g_settings_get_strv (self->settings, "hidden-from-home");
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  for (char **l = list; l && *l; l++)
    if (g_strcmp0 (*l, id))
      g_strv_builder_add (b, *l);
  if (hidden)
    g_strv_builder_add (b, id);
  g_auto (GStrv) out = g_strv_builder_end (b);
  g_settings_set_strv (self->settings, "hidden-from-home", (const char * const *) out);   /* changed:: rebuilds */
}

static void
app_info_cb (GtkWidget *b, MenuCtx *c)
{
  const char *argv[] = { "neo-settings", "--app", g_app_info_get_id (c->info), NULL };
  popdown_of (b);
  neo_launch_run (c->self->launch, argv);
}

static void
add_home_cb (GtkWidget *b, MenuCtx *c)
{
  const char *id = g_app_info_get_id (c->info);
  popdown_of (b);
  /* from the drawer: another icon on the home screen, also for an app the dock or a page has already */
  if (!neo_model_on_home (c->self->model, id)) {
    set_hidden (c->self, id, FALSE);
    return;
  }
  neo_model_add_shortcut (c->self->model, id);
  reload (c->self, TRUE);
}

static void
remove_cb (GtkWidget *b, MenuCtx *c)
{
  const char *id = g_app_info_get_id (c->info);
  popdown_of (b);
  /* this icon only; the app's last icon hides it from the home screen (the drawer keeps it) */
  if (neo_model_remove_one (c->self->model, id, c->pos))
    reload (c->self, TRUE);
  else
    set_hidden (c->self, id, TRUE);
}

static void
app_menu (NeoHome *self, GtkWidget *button)
{
  MenuCtx *c = g_new0 (MenuCtx, 1);
  c->self = self;
  c->info = g_object_ref (g_object_get_data (G_OBJECT (button), "info"));
  c->where = g_strdup (g_object_get_data (G_OBJECT (button), "where"));
  NeoPos *pos = g_object_get_data (G_OBJECT (button), "pos");
  c->pos = pos ? *pos : NEO_POS_NONE;
  GtkWidget *pop = menu_new (button);
  g_object_set_data_full (G_OBJECT (pop), "ctx", c, (GDestroyNotify) menu_ctx_free);
  menu_item (menu_box (pop), "App info", "help-about-symbolic", G_CALLBACK (app_info_cb), c);
  if (!g_strcmp0 (c->where, "drawer"))
    menu_item (menu_box (pop), "Add to home screen", "list-add-symbolic", G_CALLBACK (add_home_cb), c);
  else
    menu_item (menu_box (pop), "Remove", "list-remove-symbolic", G_CALLBACK (remove_cb), c);
  gtk_widget_show_all (pop);
  menu_place (self, pop, button, 0, gtk_widget_get_allocated_height (button));
  gtk_popover_popup (GTK_POPOVER (pop));
}

/* ---- moving icons (Launcher3): hold an icon, then move it ----
 * From the drawer the drawer goes and the icon is a copy (the app keeps the icons it has). A floating copy follows
 * the finger and the cell it left is dimmed; resting at the left or right screen edge turns the page (past the
 * last page a new one opens); letting go drops it on the cell or the dock slot under the finger. Hold and let go
 * without moving opens the icon's menu instead; a quick tap still launches. */

typedef struct Lift {
  GtkWidget *button;
  gboolean   moving;
  GtkWidget *floating;
  int        size;
  double     x, y;             /* the finger, in the window (= the home screen once the drawer is folded) */
  int        edge;             /* -1, 0, 1: the screen edge the finger rests at */
  guint      edge_id;
  gulong     gone_id;          /* the button's unmap */
  GdkEventSequence *seq;       /* the touch holding it */
  gint64     t;                /* µs: when the hold came */
} Lift;

static void
lift_free (NeoHome *self)
{
  Lift *l = self->lift;
  if (!l)
    return;
  g_clear_handle_id (&l->edge_id, g_source_remove);
  g_clear_signal_handler (&l->gone_id, l->button);
  if (l->floating)
    gtk_widget_destroy (l->floating);
  gtk_widget_set_opacity (l->button, 1);
  gtk_style_context_remove_class (gtk_widget_get_style_context (l->button), "neo-lifted");
  g_object_unref (l->button);
  g_free (l);
  self->lift = NULL;
  g_debug ("lift: freed");
  sync_drag_mode (self);
}

static GtkWidget *
page_nth (NeoHome *self, int n)
{
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (self->carousel));
  return g_list_nth_data (kids, n);
}

static int
page_now (NeoHome *self)
{
  return (int) round (hdy_carousel_get_position (HDY_CAROUSEL (self->carousel)));
}

static gboolean
turn_page (gpointer data)
{
  NeoHome *self = data;
  Lift *l = self->lift;
  int page = page_now (self) + l->edge;
  /* past the last page one new page opens, no more: resting there does not stack up empty ones */
  if (page < 0 || page > (int) self->model->pages->len)
    return G_SOURCE_CONTINUE;
  if (page >= (int) hdy_carousel_get_n_pages (HDY_CAROUSEL (self->carousel))) {
    GtkWidget *grid = page_grid ();      /* the model gets the page on the drop */
    gtk_widget_show (grid);
    hdy_carousel_insert (HDY_CAROUSEL (self->carousel), grid, -1);
  }
  hdy_carousel_scroll_to (HDY_CAROUSEL (self->carousel), page_nth (self, page));
  return G_SOURCE_CONTINUE;
}

static void
lift_move (NeoHome *self, double x, double y)
{
  Lift *l = self->lift;
  l->x = x;
  l->y = y;
  gtk_fixed_move (GTK_FIXED (self->float_layer), l->floating, (int) (x - l->size / 2.0), (int) (y - l->size / 2.0));
  int side = x < EDGE ? -1 : x > self->W - EDGE ? 1 : 0;
  if (side != l->edge) {
    l->edge = side;
    g_clear_handle_id (&l->edge_id, g_source_remove);
    if (side)
      l->edge_id = g_timeout_add (EDGE_MS, turn_page, self);
  }
}

static void
lift_begin (NeoHome *self, double x, double y)
{
  Lift *l = self->lift;
  l->moving = TRUE;
  if (!g_strcmp0 (g_object_get_data (G_OBJECT (l->button), "where"), "drawer"))
    close_drawer (self);
  gtk_widget_set_opacity (l->button, 0.25);
  l->size = self->land ? ICON_LAND : ICON;
  l->floating = icon_image (self, g_object_get_data (G_OBJECT (l->button), "info"), l->size);
  gtk_style_context_add_class (gtk_widget_get_style_context (l->floating), "neo-floating");
  gtk_fixed_put (GTK_FIXED (self->float_layer), l->floating, 0, 0);
  gtk_widget_show (l->floating);
  lift_move (self, x, y);
}

/* Where the finger is: a dock slot or a cell of the page showing, in the model's terms */
static gboolean
lift_target (NeoHome *self, double x, double y, NeoPos *out)
{
  int n = MAX (1, self->model->dock_size);
  int dx, dy;
  GtkAllocation a;
  gtk_widget_get_allocation (self->dock, &a);
  if (gtk_widget_translate_coordinates (self->win, self->dock, (int) x, (int) y, &dx, &dy)) {
    /* the dock is a row under the grid, or in landscape a column to its right */
    if (self->land ? (dx >= -40 && dx <= a.width) : (dy >= 0 && dy <= a.height + 40)) {
      int slot = self->land ? dy / MAX (1, a.height / n) : dx / MAX (1, a.width / n);
      *out = (NeoPos) { CLAMP (slot, 0, n - 1), -1, -1 };
      return TRUE;
    }
  }
  int page = page_now (self);
  GtkWidget *grid = page_nth (self, page);
  if (!grid || !gtk_widget_translate_coordinates (self->win, grid, (int) x, (int) y, &dx, &dy))
    return FALSE;
  gtk_widget_get_allocation (grid, &a);
  int cols = grid_cols (self), rows = MAX (1, neo_model_per_page (self->model) / MAX (1, cols));
  int col = CLAMP (dx / MAX (1, a.width / cols), 0, cols - 1);
  int row = CLAMP (dy / MAX (1, a.height / rows), 0, rows - 1);
  int slot = row * cols + col;
  if (page == 0)                              /* the widgets' cells are not the model's */
    slot = MAX (0, slot - self->model->reserved);
  *out = (NeoPos) { -1, page, slot };
  return TRUE;
}

static gboolean
refill_idle (gpointer data)
{
  fill_home (data);
  return G_SOURCE_REMOVE;
}

static void
lift_drop (NeoHome *self)
{
  Lift *l = self->lift;
  GAppInfo *info = g_object_get_data (G_OBJECT (l->button), "info");
  const char *where = g_object_get_data (G_OBJECT (l->button), "where");
  NeoPos *from = g_object_get_data (G_OBJECT (l->button), "pos");
  NeoPos target;
  if (lift_target (self, l->x, l->y, &target)) {
    const char *id = g_app_info_get_id (info);
    if (!g_strcmp0 (where, "drawer")) {
      neo_model_move (self->model, id, target, NEO_POS_NONE, TRUE);
      set_hidden (self, id, FALSE);
    } else {
      neo_model_move (self->model, id, target, from ? *from : NEO_POS_NONE, FALSE);
    }
  }
  /* rebuild after this event: the dragged button must outlive its own gesture */
  g_idle_add (refill_idle, self);
}

/* the held button went away under the finger (a rotation, an app installed or an icon refresh rebuilds the
 * grid): its gestures die without a drag-end or cancel, so the lift must go here, or drag mode stays NONE */
static void
lift_button_gone (GtkWidget *button, NeoHome *self)
{
  if (self->lift && self->lift->button == button)
    lift_free (self);
}

static void
cell_hold (GtkGestureLongPress *g, double x, double y, GtkWidget *button)
{
  NeoHome *self = g_object_get_data (G_OBJECT (button), "home");
  /* the list or the pages move under this finger: it scrolls, it does not hold an icon */
  /* A scroll took this touch, and the icon under it stopped hearing it: its long-press timer still ran out,
   * mid-scroll or after the finger had gone, and held an icon nobody held (the menu, a stuck drawer). */
  if (!self->touch_down || touch_swiped (self)) {
    g_debug ("lift: a hold after the touch %s, ignored", !self->touch_down ? "ended" : "moved");
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);   /* no click, no page swipe after a hold */
  lift_free (self);
  g_debug ("lift: hold");
  Lift *l = self->lift = g_new0 (Lift, 1);
  l->button = g_object_ref (button);
  l->gone_id = g_signal_connect (button, "unmap", G_CALLBACK (lift_button_gone), self);
  l->seq = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (g));
  l->t = g_get_monotonic_time ();
  gtk_style_context_add_class (gtk_widget_get_style_context (button), "neo-lifted");
  /* phoc began a drawer drag on the touch; with this (and phoc's patch 0002) it lets the icon go */
  neo_drag_set_mode (self->drag, NEO_DRAG_NONE);
}

static void
cell_drag_update (GtkGestureDrag *g, double dx, double dy, GtkWidget *button)
{
  NeoHome *self = g_object_get_data (G_OBJECT (button), "home");
  Lift *l = self->lift;
  if (!l || l->button != button)
    return;
  double sx, sy;
  int wx, wy;
  gtk_gesture_drag_get_start_point (g, &sx, &sy);
  if (!gtk_widget_translate_coordinates (button, self->win, (int) sx, (int) sy, &wx, &wy))
    return;
  double x = wx + dx, y = wy + dy;
  if (!l->moving && hypot (dx, dy) > LIFT_START)
    lift_begin (self, x, y);
  if (l->moving)
    lift_move (self, x, y);
}

static void
cell_drag_end (GtkGestureDrag *g, double dx, double dy, GtkWidget *button)
{
  NeoHome *self = g_object_get_data (G_OBJECT (button), "home");
  Lift *l = self->lift;
  if (!l || l->button != button)
    return;
  if (l->moving)
    lift_drop (self);
  else if (self->scroll_t < l->t) {   /* a touch that scrolled after the hold opens no menu */
    g_debug ("menu: held %.0f ms, moved %.0f px, last scroll %.0f ms before the hold",
               (g_get_monotonic_time () - l->t) / 1e3, hypot (dx, dy), (l->t - self->scroll_t) / 1e3);
    app_menu (self, button);
  }
  lift_free (self);
}

/* the touch taken away (phoc began a drawer drag on it, or GTK lost it): no drop and no menu, and the lift must
 * go, or drag mode stays NONE and the drawer can neither open nor close again */
static void
cell_cancel (GtkGesture *g, GdkEventSequence *seq, GtkWidget *button)
{
  NeoHome *self = g_object_get_data (G_OBJECT (button), "home");
  if (self->lift && self->lift->button == button)
    lift_free (self);
}

/* A new touch while an icon is still held: the touch that held it ended without the button hearing (no drag-end,
 * cancel or unmap came, and drag mode stayed NONE: the drawer would neither open nor close). Let it go. */
static void
all_events (GdkEvent *e, gpointer data)
{
  NeoHome *self = data;
  if (e->type == GDK_TOUCH_BEGIN) {
    self->touch_t = g_get_monotonic_time ();
    self->touch_down = TRUE;
    self->touch_x0 = e->touch.x_root;
    self->touch_y0 = e->touch.y_root;
    self->touch_moved = FALSE;
    glide_catch (self);
  } else if (e->type == GDK_TOUCH_UPDATE) {
    if (hypot (e->touch.x_root - self->touch_x0, e->touch.y_root - self->touch_y0) > LIFT_START)
      self->touch_moved = TRUE;
  } else if (e->type == GDK_TOUCH_END || e->type == GDK_TOUCH_CANCEL) {
    self->touch_down = FALSE;
  }
  if (e->type == GDK_TOUCH_BEGIN && self->lift && e->touch.sequence != self->lift->seq) {
    g_debug ("lift: a new touch while an icon was held, letting it go");
    lift_free (self);
  }
  gtk_main_do_event (e);
}

/* phoc moves the surface: the finger is a drawer drag, so an icon held under it (not yet moving) is let go */
static void
on_surface_move (gpointer data, int margin)
{
  NeoHome *self = data;
  /* phoc moves the drawer under the finger: a swipe (GTK may hear nothing of it) */
  if (self->touch_down)
    self->scroll_t = g_get_monotonic_time ();
  if (self->lift && !self->lift->moving)
    lift_free (self);
}

static GtkWidget *
cell_new (NeoHome *self, GAppInfo *info, int size, int cell_w, gboolean label, const char *where, NeoPos pos)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_START);
  gtk_container_add (GTK_CONTAINER (box), icon_image (self, info, size));
  if (label) {
    GtkWidget *l = gtk_label_new (neo_app_name (info));
    gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
    /* a grid column is as wide as its widest label wants: the label gets the cell's width and ellipsizes inside
     * it (max-width-chars 1 keeps its natural width at that), or the names spread 6 columns past the screen's
     * edge and the drawer's flow box fell to 3 columns */
    if (cell_w > 0) {
      gtk_label_set_max_width_chars (GTK_LABEL (l), 1);
      gtk_widget_set_size_request (l, cell_w - 6, -1);
    } else {
      gtk_label_set_max_width_chars (GTK_LABEL (l), !g_strcmp0 (where, "drawer") ? 8 : 10);
    }
    gtk_style_context_add_class (gtk_widget_get_style_context (l), "neo-label");
    gtk_container_add (GTK_CONTAINER (box), l);
  }
  GtkWidget *button = gtk_button_new ();
  gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
  gtk_style_context_add_class (gtk_widget_get_style_context (button), "neo-cell");
  gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (button, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text (button, NULL);
  gtk_container_add (GTK_CONTAINER (button), box);
  g_object_set_data_full (G_OBJECT (button), "info", g_object_ref (info), g_object_unref);
  g_object_set_data_full (G_OBJECT (button), "where", g_strdup (where), g_free);
  g_object_set_data_full (G_OBJECT (button), "pos", g_memdup2 (&pos, sizeof pos), g_free);
  g_object_set_data (G_OBJECT (button), "home", self);
  g_signal_connect (button, "clicked", G_CALLBACK (cell_clicked), self);
  GtkGesture *hold = gtk_gesture_long_press_new (button);
  GtkGesture *drag = gtk_gesture_drag_new (button);
  gtk_gesture_group (hold, drag);             /* one sequence: the hold's claim keeps the drag alive */
  g_signal_connect (hold, "pressed", G_CALLBACK (cell_hold), button);
  g_signal_connect (drag, "drag-update", G_CALLBACK (cell_drag_update), button);
  g_signal_connect (drag, "drag-end", G_CALLBACK (cell_drag_end), button);
  g_signal_connect (drag, "cancel", G_CALLBACK (cell_cancel), button);
  g_object_set_data_full (G_OBJECT (button), "hold", hold, g_object_unref);
  g_object_set_data_full (G_OBJECT (button), "drag", drag, g_object_unref);
  return button;
}

/* ---- filling ---- */

static void
clear_container (GtkWidget *c)
{
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (c));
  for (GList *l = kids; l; l = l->next)
    gtk_widget_destroy (l->data);
}

static GtkWidget *
page_grid (void)
{
  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  gtk_grid_set_row_homogeneous (GTK_GRID (grid), TRUE);
  gtk_widget_set_hexpand (grid, TRUE);
  gtk_widget_set_vexpand (grid, TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (grid), "neo-page");
  return grid;
}

/* a tap on a widget: its app (Weather, Clocks), if installed */
static void
open_widget_app (const char *desktop_id, gpointer data)
{
  NeoHome *self = data;
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new (desktop_id);
  if (info)
    neo_launch_app (self->launch, G_APP_INFO (info), NULL);
}

/* the icon for a cell this wide: full size while it fits with some air, smaller for many columns */
static int
fit_icon (int icon, int cell_w)
{
  return cell_w > 0 ? CLAMP (cell_w - 14, 32, icon) : icon;
}

static void
fill_home (NeoHome *self)
{
  gboolean hide = g_settings_get_boolean (self->settings, "desktop-hide-app-labels") || self->land;
  int cols = grid_cols (self), per = neo_model_per_page (self->model);
  int icon = self->land ? ICON_LAND : ICON, dock_icon = self->land ? DOCK_ICON_LAND : DOCK_ICON;
  int dock_n = MAX (1, (int) self->model->dock->len);
  /* landscape: the dock is a column at the right edge, the grid gets the rest */
  int grid_w = self->land ? self->W - dock_icon - 40 : self->W - 8;
  int cell_w = self->W > 0 ? grid_w / MAX (1, cols) : 0;
  icon = fit_icon (icon, cell_w);
  if (self->W > 0)
    dock_icon = fit_icon (dock_icon, (self->land ? self->Hv - 16 : self->W - 20) / dock_n);

  /* the pending renders belong to icons about to go */
  IconJob *job;
  while ((job = g_queue_pop_head (&self->icon_jobs))) {
    if (job->image)
      g_object_remove_weak_pointer (G_OBJECT (job->image), (gpointer *) &job->image);
    g_object_unref (job->info);
    g_free (job);
  }

  pics_reset (self);
  g_autoptr (GList) old = gtk_container_get_children (GTK_CONTAINER (self->carousel));
  for (GList *l = old; l; l = l->next)
    gtk_container_remove (GTK_CONTAINER (self->carousel), l->data);

  int reserved = self->model->reserved;
  for (guint p = 0; p < self->model->pages->len; p++) {
    GPtrArray *page = self->model->pages->pdata[p];
    GtkWidget *grid = page_grid ();
    int skip = p == 0 ? reserved : 0;     /* the widgets' rows */
    if (skip) {
      GtkWidget *strip = neo_widgets_new (self->land, open_widget_app, self);
      gtk_grid_attach (GTK_GRID (grid), strip, 0, 0, cols, skip / cols);
      g_set_weak_pointer (&self->strip, strip);
    }
    guint i = 0;
    for (; i < page->len; i++) {
      GAppInfo *info = g_hash_table_lookup (self->apps, page->pdata[i]);
      int at = i + skip;
      if (info)
        gtk_grid_attach (GTK_GRID (grid), cell_new (self, info, icon, cell_w, !hide, "home", (NeoPos) { -1, p, i }),
                         at % cols, at / cols, 1, 1);
    }
    for (int at = i + skip; at < per; at++)     /* keep the grid's shape on a page that is not full */
      gtk_grid_attach (GTK_GRID (grid), gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), at % cols, at / cols, 1, 1);
    gtk_widget_show_all (grid);
    hdy_carousel_insert (HDY_CAROUSEL (self->carousel), grid, -1);
  }
  gtk_widget_set_visible (self->dots, self->model->pages->len > 1);

  clear_container (self->dock);
  for (guint i = 0; i < self->model->dock->len; i++) {
    GAppInfo *info = g_hash_table_lookup (self->apps, self->model->dock->pdata[i]);
    if (info)
      gtk_container_add (GTK_CONTAINER (self->dock), cell_new (self, info, dock_icon, 0, FALSE, "dock", (NeoPos) { i, -1, -1 }));
  }
  gtk_widget_show_all (self->dock);
}

static void
fill_drawer (NeoHome *self)
{
  glide_stale (self);
  clear_container (self->drawer_flow);
  drawer_columns (self);
  g_ptr_array_set_size (self->drawer_cells, 0);
  int n = gtk_flow_box_get_max_children_per_line (GTK_FLOW_BOX (self->drawer_flow));
  int cell_w = self->W > 0 ? (self->W - 16) / MAX (1, n) : 0;
  int icon = fit_icon (ICON, cell_w);
  GList *infos = g_list_sort (g_hash_table_get_values (self->apps), neo_app_compare);
  for (GList *l = infos; l; l = l->next) {
    GtkWidget *c = cell_new (self, l->data, icon, cell_w, TRUE, "drawer", NEO_POS_NONE);
    gtk_container_add (GTK_CONTAINER (self->drawer_flow), c);
    g_ptr_array_add (self->drawer_cells, c);
  }
  g_list_free (infos);
  gtk_widget_show_all (self->drawer_flow);
}

static void
filter_drawer (NeoHome *self)
{
  glide_stale (self);
  g_autofree char *q = g_utf8_strdown (gtk_entry_get_text (GTK_ENTRY (self->search)), -1);
  g_strstrip (q);
  for (guint i = 0; i < self->drawer_cells->len; i++) {
    GtkWidget *c = self->drawer_cells->pdata[i];
    GAppInfo *info = g_object_get_data (G_OBJECT (c), "info");
    g_autofree char *hay = g_strdup_printf ("%s %s %s %s", neo_app_name (info), g_app_info_get_name (info),
                                            g_app_info_get_description (info) ?: "", g_app_info_get_executable (info) ?: "");
    g_autofree char *low = g_utf8_strdown (hay, -1);
    gtk_widget_set_visible (gtk_widget_get_parent (c), !*q || strstr (low, q));   /* the GtkFlowBoxChild */
  }
}

static void
search_activate (GtkEntry *e, NeoHome *self)
{
  for (guint i = 0; i < self->drawer_cells->len; i++) {
    GtkWidget *c = self->drawer_cells->pdata[i];
    if (gtk_widget_get_visible (gtk_widget_get_parent (c))) {
      neo_launch_app (self->launch, g_object_get_data (G_OBJECT (c), "info"), c);
      return;
    }
  }
}

/* the phone's screen held upright, in logical px (the panel's, whatever the rotation) */
static void
screen_upright (int *w, int *h)
{
  GdkDisplay *d = gdk_display_get_default ();
  GdkMonitor *m = d ? gdk_display_get_primary_monitor (d) : NULL;
  if (!m && d && gdk_display_get_n_monitors (d) > 0)
    m = gdk_display_get_monitor (d, 0);
  GdkRectangle r = { 0, 0, 360, 760 };
  if (m)
    gdk_monitor_get_geometry (m, &r);
  *w = MIN (r.width, r.height);
  *h = MAX (r.width, r.height);
}

/* How many columns and rows of icons fit: a column needs a 48 px icon and a short name (66 px), a row an icon and
 * its name (84 px) below the status bar, the widgets, the page dots, the dock and the home bar (≈ 200 px). */
int
neo_home_max_columns (NeoHome *self)
{
  int w, h;
  screen_upright (&w, &h);
  return MAX (3, w / 66);
}

int
neo_home_max_rows (NeoHome *self)
{
  int w, h;
  screen_upright (&w, &h);
  return MAX (3, (h - 200) / 84);
}

static void
reload (NeoHome *self, gboolean rebuild)
{
  g_clear_pointer (&self->apps, g_hash_table_unref);
  self->apps = neo_apps_installed ();
  /* a value from a bigger screen (or set by hand) is held to what fits here */
  int cols = MIN (g_settings_get_int (self->settings, "desktop-grid-columns"), neo_home_max_columns (self));
  int rows = MIN (g_settings_get_int (self->settings, "desktop-grid-rows"), neo_home_max_rows (self));
  int dock = MIN (g_settings_get_int (self->settings, "dock-num-icons"), neo_home_max_columns (self));
  g_auto (GStrv) hidden = g_settings_get_strv (self->settings, "hidden-from-home");
  if (rebuild || !self->model || self->model->cols != cols || self->model->rows != rows) {
    g_clear_pointer (&self->model, neo_model_free);
    self->model = neo_model_new (cols, rows, dock);
    neo_model_set_hidden (self->model, hidden);
    neo_model_load (self->model, self->apps);
  } else {
    neo_model_set_hidden (self->model, hidden);
    neo_model_reconcile (self->model, self->apps);
  }
  neo_model_set_reserved (self->model, g_settings_get_boolean (self->settings, "home-widgets") ?
                                       self->model->cols * NEO_WIDGET_ROWS : 0);
  fill_home (self);
  fill_drawer (self);
}

/* an install or upgrade changes the app list many times while its files land: an app missing for a moment was taken
 * as uninstalled and left the dock. Reload once the list has been quiet for 3 s. */
static gboolean
icons_rescan (gpointer data)
{
  NeoHome *self = data;
  self->rescan_timer = 0;
  gtk_icon_theme_rescan_if_needed (gtk_icon_theme_get_default ());
  return G_SOURCE_REMOVE;
}

static gboolean
theme_quiet (gpointer data)
{
  NeoHome *self = data;
  self->theme_timer = 0;
  g_message ("icons: the icon theme changed, drawing the icons again");
  reload (self, TRUE);
  return G_SOURCE_REMOVE;
}

static void
on_theme_changed (GtkIconTheme *theme, NeoHome *self)
{
  g_clear_handle_id (&self->theme_timer, g_source_remove);
  self->theme_timer = g_timeout_add (500, theme_quiet, self);
}

static gboolean
apps_quiet (gpointer data)
{
  NeoHome *self = data;
  self->apps_timer = 0;
  reload (self, FALSE);
  return G_SOURCE_REMOVE;
}

static void
on_apps_changed (GAppInfoMonitor *m, NeoHome *self)
{
  g_clear_handle_id (&self->apps_timer, g_source_remove);
  self->apps_timer = g_timeout_add_seconds (3, apps_quiet, self);
  /* an install's icons reach the theme's cache at the end of the apt run, after its apps showed up here: look
   * again a little later (a theme that changed says so, and the icons are drawn anew) */
  g_clear_handle_id (&self->rescan_timer, g_source_remove);
  self->rescan_timer = g_timeout_add_seconds (20, icons_rescan, self);
}

static void
load_parallax (NeoHome *self)
{
  g_autofree char *dir = g_settings_get_string (self->settings, "wallpaper-parallax");
  g_clear_pointer (&self->parallax, neo_parallax_free);
  if (*dir)
    self->parallax = neo_parallax_new (self->wall_win, dir);
  if (self->parallax && self->W > 0)
    neo_parallax_resize (self->parallax, self->W, self->Hv, neo_home_scale (self));
  parallax_sync (self);
  load_wall (self);          /* the still picture again when the parallax is off */
  gtk_widget_queue_draw (self->wall_area);
}

static void
on_setting (GSettings *s, const char *key, NeoHome *self)
{
  if (!g_strcmp0 (key, "wallpaper-parallax")) {
    load_parallax (self);
    return;
  }
  static const char *rebuild[] = { "desktop-grid-columns", "desktop-grid-rows", "dock-num-icons", "drawer-grid-columns",
                                   "desktop-hide-app-labels", "icon-pack", "icon-shape", "icon-wrap-unthemed",
                                   "icon-shape-legacy", "hidden-from-home", "home-widgets", NULL };
  if (!g_strv_contains (rebuild, key))
    return;
  neo_icons_reset (self->icons);
  reload (self, TRUE);
}

/* ---- the home screen's own menu ---- */

static void
open_settings_cb (GtkWidget *b, NeoHome *self)
{
  popdown_of (b);
  neo_settings_show (self->app, self->settings, self);
}

static void
wallpaper_cb (GtkWidget *b, NeoHome *self)
{
  const char *argv[] = { "neo-settings", "--page", "wallpaper", NULL };
  popdown_of (b);
  neo_launch_run (self->launch, argv);
}

/* A hold the finger did not make: the touch that revealed home (the swipe up
 * lands on the freshly mapped window), or a sequence whose release went to
 * phoc's drag and never reached us, so GTK's timer still fires. */
static gboolean
hold_is_stray (GtkGestureLongPress *g, NeoHome *self)
{
  GdkEventSequence *seq = gtk_gesture_get_last_updated_sequence (GTK_GESTURE (g));
  const GdkEvent *ev = gtk_gesture_get_last_event (GTK_GESTURE (g), seq);

  if (drawer_open (self) || g_get_monotonic_time () - self->show_t0 < 800000)
    return TRUE;
  /* the pages slid under this finger (it stopped mid-swipe): a scroll, not a hold; claiming it would cancel the slide */
  if (!self->touch_down || touch_swiped (self)) {
    g_debug ("lift: a home hold after the touch %s, ignored", !self->touch_down ? "ended" : "moved");
    return TRUE;
  }
  if (!ev)
    return TRUE;
  switch (ev->type) {
  case GDK_TOUCH_END:
  case GDK_TOUCH_CANCEL:
  case GDK_BUTTON_RELEASE:
    return TRUE;
  default:
    return FALSE;
  }
}

static void
home_hold (GtkGestureLongPress *g, double x, double y, NeoHome *self)
{
  if (hold_is_stray (g, self)) {
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
  GtkWidget *pop = menu_new (self->home);
  GdkRectangle r = { (int) x, (int) y, 1, 1 };
  gtk_popover_set_pointing_to (GTK_POPOVER (pop), &r);
  menu_item (menu_box (pop), "Launcher settings", "emblem-system-symbolic", G_CALLBACK (open_settings_cb), self);
  menu_item (menu_box (pop), "Wallpaper", "preferences-desktop-wallpaper-symbolic", G_CALLBACK (wallpaper_cb), self);
  gtk_widget_show_all (pop);
  menu_place (self, pop, self->home, r.y, 1);
  gtk_popover_popup (GTK_POPOVER (pop));
}

static gboolean
on_key (GtkWidget *w, GdkEventKey *ev, NeoHome *self)
{
  if (ev->keyval != GDK_KEY_Escape)
    return FALSE;
  if (drawer_open (self))
    close_drawer (self);
  else
    neo_home_hide (self);
  return TRUE;
}

/* ---- windows ---- */

static void
layer_window (GtkWindow *win, const char *ns)
{
  gtk_layer_init_for_window (win);
  gtk_layer_set_namespace (win, ns);
  gtk_layer_set_layer (win, GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_exclusive_zone (win, 0);    /* inside Phosh's top bar and home bar, not under them */
}

/* the first paint after a show: how long the way back to the home takes on our side */
static gboolean
first_paint (GtkWidget *w, cairo_t *cr, NeoHome *self)
{
  if (self->show_t0) {
    g_message ("home: painted %.0f ms after the show", (g_get_monotonic_time () - self->show_t0) / 1000.0);
    self->show_t0 = 0;
  }
  return FALSE;
}

static void
build (NeoHome *self)
{
  /* the wallpaper: the whole usable screen, under the moving surface */
  self->wall_win = gtk_application_window_new (self->app);
  gtk_window_set_title (GTK_WINDOW (self->wall_win), "Neo Wallpaper");
  layer_window (GTK_WINDOW (self->wall_win), "neo-wallpaper");
  for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++)
    gtk_layer_set_anchor (GTK_WINDOW (self->wall_win), e, TRUE);
  gtk_layer_set_keyboard_mode (GTK_WINDOW (self->wall_win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  self->wall_area = gtk_drawing_area_new ();
  g_signal_connect (self->wall_area, "draw", G_CALLBACK (draw_wall), self);
  gtk_container_add (GTK_CONTAINER (self->wall_win), self->wall_area);
  g_signal_connect (self->wall_win, "size-allocate", G_CALLBACK (on_wall_allocate), self);
  gtk_widget_show (self->wall_area);
  neo_wall_watch (on_wall_changed, self);

  /* home + drawer: anchored to the bottom and both sides, as tall as two screens */
  self->win = gtk_application_window_new (self->app);
  g_signal_connect_after (self->win, "draw", G_CALLBACK (first_paint), self);
  gtk_window_set_title (GTK_WINDOW (self->win), "Neo Launcher");
  layer_window (GTK_WINDOW (self->win), "neo-launcher");
  gtk_layer_set_anchor (GTK_WINDOW (self->win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (self->win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (self->win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  /* phoc speaks layer-shell v3: no on-demand focus; the launcher is full screen anyway */
  gtk_layer_set_keyboard_mode (GTK_WINDOW (self->win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
  gtk_widget_set_app_paintable (self->win, TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->win), "neo");
  GdkScreen *screen = gtk_widget_get_screen (self->win);
  GdkVisual *rgba = gdk_screen_get_rgba_visual (screen);
  if (rgba)
    gtk_widget_set_visual (self->win, rgba);
  g_signal_connect (self->win, "key-press-event", G_CALLBACK (on_key), self);

  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = gtk_overlay_new ();
  gtk_container_add (GTK_CONTAINER (overlay), column);
  self->float_layer = gtk_fixed_new ();
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->float_layer);
  gtk_overlay_set_overlay_pass_through (GTK_OVERLAY (overlay), self->float_layer, TRUE);
  gtk_container_add (GTK_CONTAINER (self->win), overlay);
  gtk_widget_show (overlay);
  gtk_widget_show (self->float_layer);
  /* sees every touch before GTK does, and takes none part in GTK's gestures */
  gdk_event_handler_set (all_events, self, NULL);

  /* home: pages + dots + dock */
  self->home = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->home), "neo-home");
  self->pages = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (self->pages, TRUE);
  gtk_widget_set_vexpand (self->pages, TRUE);
  self->carousel = hdy_carousel_new ();
  hdy_carousel_set_allow_long_swipes (HDY_CAROUSEL (self->carousel), FALSE);
  gtk_widget_set_vexpand (self->carousel, TRUE);
  gtk_widget_set_hexpand (self->carousel, TRUE);
  self->dots = hdy_carousel_indicator_dots_new ();
  hdy_carousel_indicator_dots_set_carousel (HDY_CAROUSEL_INDICATOR_DOTS (self->dots), HDY_CAROUSEL (self->carousel));
  gtk_style_context_add_class (gtk_widget_get_style_context (self->dots), "neo-dots");
  self->dock = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (self->dock), TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->dock), "neo-dock");
  gtk_container_add (GTK_CONTAINER (self->pages), self->carousel);
  self->surfs_ok = neo_surf_init ();
  if (self->surfs_ok) {
    hdy_carousel_set_interactive (HDY_CAROUSEL (self->carousel), FALSE);
    GtkGesture *pan = gtk_gesture_pan_new (self->pages, GTK_ORIENTATION_HORIZONTAL);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (pan), GTK_PHASE_CAPTURE);
    gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (pan), FALSE);
    g_signal_connect (pan, "pan", G_CALLBACK (slide_pan), self);
    g_signal_connect (pan, "drag-update", G_CALLBACK (slide_update), self);
    g_signal_connect (pan, "drag-end", G_CALLBACK (slide_end), self);
    g_signal_connect (pan, "cancel", G_CALLBACK (slide_cancel), self);
    g_object_set_data_full (G_OBJECT (self->pages), "pan", pan, g_object_unref);
  }
  gtk_container_add (GTK_CONTAINER (self->pages), self->dots);
  gtk_container_add (GTK_CONTAINER (self->home), self->pages);
  gtk_container_add (GTK_CONTAINER (self->home), self->dock);
  GtkGesture *hold = gtk_gesture_long_press_new (self->home);
  g_signal_connect (hold, "pressed", G_CALLBACK (home_hold), self);
  g_object_set_data_full (G_OBJECT (self->home), "hold", hold, g_object_unref);

  /* drawer: search + every app A→Z */
  self->drawer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->drawer), "neo-drawer");
  self->search = gtk_search_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->search), "Search apps");
  gtk_style_context_add_class (gtk_widget_get_style_context (self->search), "neo-search");
  g_signal_connect_swapped (self->search, "search-changed", G_CALLBACK (filter_drawer), self);
  g_signal_connect (self->search, "activate", G_CALLBACK (search_activate), self);
  self->drawer_scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->drawer_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (self->drawer_scroll, TRUE);
  self->drawer_flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (self->drawer_flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (self->drawer_flow), TRUE);
  gtk_widget_set_valign (self->drawer_flow, GTK_ALIGN_START);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->drawer_flow), "neo-drawer-grid");
  gtk_container_add (GTK_CONTAINER (self->drawer_scroll), self->drawer_flow);
  gtk_container_add (GTK_CONTAINER (self->drawer), self->search);
  gtk_container_add (GTK_CONTAINER (self->drawer), self->drawer_scroll);
  g_signal_connect_swapped (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->drawer_scroll)),
                            "value-changed", G_CALLBACK (sync_drag_mode), self);
  glide_setup (self);

  gtk_container_add (GTK_CONTAINER (column), self->home);
  gtk_container_add (GTK_CONTAINER (column), self->drawer);
  gtk_widget_show_all (column);
  g_settings_bind (self->settings, "search-drawer-enabled", self->search, "visible", G_SETTINGS_BIND_GET);

  load_parallax (self);
  self->drag = neo_drag_new (GTK_WINDOW (self->win), on_drag_end, on_surface_move, self);
  neo_drag_set_threshold (self->drag, THRESHOLD);
  neo_stack_below (GTK_WINDOW (self->wall_win), GTK_WINDOW (self->win));
}

/* ---- the screen: the parallax (the accelerometer at 60 Hz and a 10 Hz poll) only while the home can be seen ---- */

static void
parallax_sync (NeoHome *self)
{
  if (self->parallax)
    neo_parallax_run (self->parallax, self->want_shown && !self->locked && !self->blanked);
}

static void
got_locked (GObject *bus, GAsyncResult *res, gpointer data)
{
  NeoHome *self = data;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (bus), res, NULL);
  if (v) {
    g_variant_get (v, "(b)", &self->locked);
    parallax_sync (self);
  }
}

static void
got_power_save (GObject *bus, GAsyncResult *res, gpointer data)
{
  NeoHome *self = data;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (bus), res, NULL);
  if (v) {
    g_autoptr (GVariant) mode = NULL;
    g_variant_get (v, "(v)", &mode);
    self->blanked = g_variant_get_int32 (mode) != 0;
    parallax_sync (self);
  }
}

static void
on_locked (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *signal,
           GVariant *params, gpointer data)
{
  NeoHome *self = data;
  g_variant_get (params, "(b)", &self->locked);
  parallax_sync (self);
}

static void
on_display_props (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *signal,
                  GVariant *params, gpointer data)
{
  NeoHome *self = data;
  g_autoptr (GVariant) changed = g_variant_get_child_value (params, 1);
  int mode;
  if (g_variant_lookup (changed, "PowerSaveMode", "i", &mode)) {
    self->blanked = mode != 0;
    parallax_sync (self);
  }
}

static void
watch_screen (NeoHome *self)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus)
    return;
  g_dbus_connection_signal_subscribe (bus, "org.gnome.ScreenSaver", "org.gnome.ScreenSaver", "ActiveChanged",
                                      "/org/gnome/ScreenSaver", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_locked, self, NULL);
  g_dbus_connection_signal_subscribe (bus, "org.gnome.Mutter.DisplayConfig", "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged", "/org/gnome/Mutter/DisplayConfig",
                                      "org.gnome.Mutter.DisplayConfig", G_DBUS_SIGNAL_FLAGS_NONE, on_display_props,
                                      self, NULL);
  g_dbus_connection_call (bus, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver",
                          "GetActive", NULL, G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, got_locked,
                          self);
  g_dbus_connection_call (bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                          "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new ("(ss)", "org.gnome.Mutter.DisplayConfig", "PowerSaveMode"),
                          G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, got_power_save, self);
}

NeoHome *
neo_home_new (GtkApplication *app, GSettings *settings)
{
  NeoHome *self = g_new0 (NeoHome, 1);
  self->app = app;
  self->settings = g_object_ref (settings);
  self->icons = neo_icons_new (settings);
  self->drawer_cells = g_ptr_array_new ();
  self->pics = g_ptr_array_new ();
  g_queue_init (&self->icon_jobs);

  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/de/yesman/NeoLauncher/style.css");
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (), GTK_STYLE_PROVIDER (css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", TRUE, NULL);

  build (self);
  self->launch = neo_launch_new (self);
  reload (self, TRUE);
  g_signal_connect (g_app_info_monitor_get (), "changed", G_CALLBACK (on_apps_changed), self);
  g_signal_connect (gtk_icon_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), self);
  g_signal_connect (settings, "changed", G_CALLBACK (on_setting), self);
  watch_screen (self);
  return self;
}

void
neo_home_set_visible_cb (NeoHome *self, void (*cb) (gpointer, gboolean), gpointer data)
{
  self->visible_cb = cb;
  self->visible_data = data;
}

gboolean
neo_home_visible (NeoHome *self)
{
  return gtk_widget_get_visible (self->win);
}

void
neo_home_show (NeoHome *self)
{
  gboolean shown = gtk_widget_get_visible (self->win);
  launch_card_remove (self);
  neo_launch_finish (self->launch);
  gtk_layer_set_keyboard_mode (GTK_WINDOW (self->win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
  /* home while Neo shows: slide back to the first page (and the drawer down); coming from an app, be there */
  if (shown) {
    close_drawer (self);
    g_autoptr (GList) pages = gtk_container_get_children (GTK_CONTAINER (self->carousel));
    if (pages)
      hdy_carousel_scroll_to (HDY_CAROUSEL (self->carousel), pages->data);
    return;
  }
  reset_drawer (self);
  g_autoptr (GList) pages = gtk_container_get_children (GTK_CONTAINER (self->carousel));
  if (pages)
    hdy_carousel_scroll_to_full (HDY_CAROUSEL (self->carousel), pages->data, 0);
  self->want_shown = TRUE;
  self->show_t0 = g_get_monotonic_time ();
  gtk_widget_show (self->wall_win);
  if (self->W > 0)          /* else once the wallpaper window has its size */
    gtk_widget_show (self->win);
  parallax_sync (self);
  pics_prerender (self);
  if (self->visible_cb)
    self->visible_cb (self->visible_data, TRUE);
  g_message ("home: show handled in %.0f ms", (g_get_monotonic_time () - self->show_t0) / 1000.0);
}

void
neo_home_hide (NeoHome *self)
{
  self->want_shown = FALSE;
  if (!gtk_widget_get_visible (self->wall_win) && !gtk_widget_get_visible (self->win))
    return;
  parallax_sync (self);
  /* a sequence still open when home goes must not pop the menu next time */
  GtkEventController *hold = g_object_get_data (G_OBJECT (self->home), "hold");
  if (hold)
    gtk_event_controller_reset (hold);
  gtk_widget_hide (self->wall_win);
  gtk_widget_hide (self->win);
  launch_card_remove (self);
  lift_free (self);
  slide_free (self);
  glide_hide (self);
  if (self->visible_cb)
    self->visible_cb (self->visible_data, FALSE);
}

/* The back gesture while Neo shows: clear the search, then close the drawer. */
gboolean
neo_home_back (NeoHome *self)
{
  if (!neo_home_visible (self))
    return FALSE;
  if (drawer_open (self)) {
    if (*gtk_entry_get_text (GTK_ENTRY (self->search)))
      gtk_entry_set_text (GTK_ENTRY (self->search), "");
    else
      close_drawer (self);
  }
  return TRUE;
}

/* ---- the drawer's scrolling ----
 * GtkScrolledWindow scrolled the list by drawing it again on every frame, in software, and phoc uploaded the
 * drawer at each one: with ~60 apps a frame took 25-70 ms and the list jumped. Neo scrolls it as phoc moves the
 * drawer: the list is drawn once into pictures (tiles, a texture can be only so tall) that are subsurfaces cropped
 * to the list's box; the finger and the fling only change that crop. Neo tracks the finger and flings itself, and
 * the scrolled window jumps to where the list comes to rest. */
#define GLIDE_TILE 4096          /* px: a tile's height in the buffer, under any GPU's texture limit */
#define GLIDE_TAU 0.36           /* s: a fling slows as exp(-t/tau) (Android's feel) */
#define GLIDE_VMAX 9000.0        /* px/s */
#define GLIDE_BACK_MS 260        /* past an end, how long the list takes to come back */

typedef struct Glide {
  GPtrArray *tiles;              /* cairo_surface_t, GLIDE_TILE px tall (the last one less) */
  GPtrArray *surfs;              /* NeoSurf, one per tile, while shown */
  gboolean   stale;
  int        scale;
  double     tile_h;             /* logical px */
  double     cw, ch;             /* the list (the flow box), logical px */
  guint      prerender_id;
  /* while it moves */
  gboolean   active, dragging, flinging, frame_pending;
  double     vx, vy, vw, vh;     /* the list's box (the scrolled window) in the window */
  double     fx, pad;            /* the flow box in that box: x, and y when scrolled to 0 */
  double     pos, max;           /* scrolled by (like the adjustment's value), and as far as it goes */
  double     start, oy0;         /* pos when the finger took it, and the finger's offset then */
  gboolean   started;
  double     vel;                /* px/s, + = further down the list */
  gint64     last_t;
  double     a_from, a_to, a_v;  /* the fling or the way back: from, to (way back), initial speed (fling) */
  gint64     a_t0;
  int        a_ms;               /* the way back's duration; 0 = a fling */
  gulong     paint_id;
  int        paints;
  guint      watchdog;           /* phoc did not say the last frame was shown: go on anyway */
} Glide;

static void
glide_drop_tiles (Glide *g)
{
  g_ptr_array_set_size (g->tiles, 0);
}

static void
glide_stale (NeoHome *self)
{
  if (self->glide)
    self->glide->stale = TRUE;
}

/* draw the list into the tiles (with the scrolled window drawn invisible meanwhile, its own opacity is not the
 * list's) */
static gboolean
glide_render (NeoHome *self)
{
  Glide *g = self->glide;
  int w = gtk_widget_get_allocated_width (self->drawer_flow);
  int h = gtk_widget_get_allocated_height (self->drawer_flow);
  if (w <= 1 || h <= 1)
    return FALSE;
  gint64 t0 = g_get_monotonic_time ();
  int s = neo_home_scale (self);
  glide_drop_tiles (g);
  g->scale = s;
  g->cw = w;
  g->ch = h;
  g->tile_h = floor ((double) GLIDE_TILE / s);
  for (double y = 0; y < h; y += g->tile_h) {
    int th = (int) MIN (g->tile_h, h - y);
    cairo_surface_t *img = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w * s, th * s);
    cairo_t *cr = cairo_create (img);
    cairo_scale (cr, s, s);
    cairo_translate (cr, 0, -y);
    cairo_rectangle (cr, 0, y, w, th);
    cairo_clip (cr);
    gtk_widget_draw (self->drawer_flow, cr);
    cairo_destroy (cr);
    g_ptr_array_add (g->tiles, img);
  }
  g->stale = FALSE;
  g_debug ("glide: the list drawn in %u tiles in %.1f ms", g->tiles->len, (g_get_monotonic_time () - t0) / 1e3);
  return TRUE;
}

/* the drawer is up: draw the list now, so the first scroll does not wait for it */
static gboolean
glide_prerender_idle (gpointer data)
{
  NeoHome *self = data;
  self->glide->prerender_id = 0;
  if (self->glide->stale && !self->glide->active && drawer_open (self))
    glide_render (self);
  return G_SOURCE_REMOVE;
}

static void
glide_prerender (NeoHome *self)
{
  if (self->glide && !self->glide->prerender_id)
    self->glide->prerender_id = g_timeout_add_full (G_PRIORITY_LOW, 150, glide_prerender_idle, self, NULL);
}

static void
glide_free_surfs (NeoHome *self)
{
  Glide *g = self->glide;
  if (g->paint_id) {
    g_signal_handler_disconnect (gtk_widget_get_frame_clock (self->win), g->paint_id);
    g->paint_id = 0;
  }
  if (g->frame_pending)
    neo_surf_cancel_frame (self->win);
  g->frame_pending = FALSE;
  g_clear_handle_id (&g->watchdog, g_source_remove);
  g_ptr_array_set_size (g->surfs, 0);
  gtk_widget_set_child_visible (self->drawer_flow, TRUE);
}

/* the home went away: no pictures left over */
static void
glide_hide (NeoHome *self)
{
  Glide *g = self->glide;
  if (!g)
    return;
  g->active = g->dragging = g->flinging = FALSE;
  glide_free_surfs (self);
}

static void
glide_place (NeoHome *self)
{
  Glide *g = self->glide;
  int s = g->scale;
  double a = g->pos - g->pad, b = a + g->vh;       /* the list's rows in the box */
  for (guint i = 0; i < g->surfs->len; i++) {
    NeoSurf *f = g->surfs->pdata[i];
    double ti = i * g->tile_h, th = MIN (g->tile_h, g->ch - ti);
    double ys = MAX (ti, a), ye = MIN (ti + th, b);
    if (ye - ys < 1) {
      neo_surf_place (f, 0, 0, 0, 0, 0);
      continue;
    }
    /* the subsurface sits on whole px; the crop takes the fraction, so the list moves by less than a px */
    double sy = g->vy + g->pad + ys - g->pos;
    double iy = floor (sy), frac = sy - iy, cs = ys - frac;
    if (cs < ti) {
      cs = ys;
      iy = round (sy);
    }
    double hh = floor (MIN (ye - ys, ti + th - cs));
    if (hh < 1) {
      neo_surf_place (f, 0, 0, 0, 0, 0);
      continue;
    }
    neo_surf_crop (f, 0, (cs - ti) * s, g->cw * s, hh * s);
    neo_surf_place (f, g->vx + g->fx, iy, g->cw, hh, 1);
  }
}

static void glide_frame (NeoHome *self);
static void glide_on_frame (gpointer data);

/* a frame callback can go unanswered (phoc skips a surface it has nothing new to show for): a fling waited on it
 * forever and the list stood still until the next touch */
static gboolean
glide_watchdog (gpointer data)
{
  NeoHome *self = data;
  self->glide->watchdog = 0;
  neo_surf_cancel_frame (self->win);
  glide_on_frame (self);
  return G_SOURCE_REMOVE;
}

static void
glide_on_frame (gpointer data)
{
  NeoHome *self = data;
  Glide *g = self->glide;
  g_clear_handle_id (&g->watchdog, g_source_remove);
  g->frame_pending = FALSE;
  if (g->active)
    glide_frame (self);
}

/* at rest: the scrolled window takes the place, and the pictures go once GTK has drawn it there */
static void
glide_after_paint (GdkFrameClock *clock, NeoHome *self)
{
  Glide *g = self->glide;
  if (++g->paints < 2)
    return;
  glide_free_surfs (self);
  neo_surf_commit_parent (self->win, NULL, NULL);
}

static void
glide_rest (NeoHome *self)
{
  Glide *g = self->glide;
  g_debug ("glide: rests at %.0f", g->pos);
  g->active = g->flinging = FALSE;
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->drawer_scroll));
  gtk_adjustment_set_value (adj, CLAMP (g->pos, 0, g->max));
  gtk_widget_set_child_visible (self->drawer_flow, TRUE);
  /* The pictures go where GTK will draw the list (on whole px): a fling that ran into an end stopped before its
   * last frame was placed, a few px short, and GTK's list showed beside the pictures, every icon twice. */
  int fx, fy;
  if (gtk_widget_translate_coordinates (self->drawer_flow, self->drawer_scroll, 0, 0, &fx, &fy)) {
    g->pos = g->pad - fy;
    glide_place (self);
    neo_surf_commit_parent (self->win, NULL, NULL);
  }
  sync_drag_mode (self);
  GdkFrameClock *clock = gtk_widget_get_frame_clock (self->win);
  if (!clock) {
    glide_free_surfs (self);
    return;
  }
  g->paints = 0;
  g->paint_id = g_signal_connect (clock, "after-paint", G_CALLBACK (glide_after_paint), self);
  gtk_widget_queue_draw (self->drawer_scroll);
}

/* one frame: step the fling or the way back, place, commit, ask phoc to say when it is shown */
static void
glide_frame (NeoHome *self)
{
  Glide *g = self->glide;
  if (g->flinging) {
    double t = (g_get_monotonic_time () - g->a_t0) / 1e6;
    if (g->a_ms) {                                   /* back to the end it went past */
      double u = MIN (1.0, t * 1000 / g->a_ms), e = 1 - (1 - u) * (1 - u) * (1 - u);
      g->pos = g->a_from + (g->a_to - g->a_from) * e;
      if (u >= 1) {
        glide_rest (self);
        return;
      }
    } else {
      double k = exp (-t / GLIDE_TAU);
      g->pos = g->a_from + g->a_v * GLIDE_TAU * (1 - k);
      double v = g->a_v * k;
      if (g->pos <= 0 || g->pos >= g->max) {         /* an end: it stops there */
        g->pos = CLAMP (g->pos, 0, g->max);
        glide_rest (self);
        return;
      }
      if (fabs (v) < 15) {
        glide_rest (self);
        return;
      }
    }
  }
  glide_place (self);
  g->frame_pending = TRUE;
  neo_surf_commit_parent (self->win, glide_on_frame, self);
  g_clear_handle_id (&g->watchdog, g_source_remove);
  g->watchdog = g_timeout_add (50, glide_watchdog, self);
}

static void
glide_request (NeoHome *self)
{
  if (!self->glide->frame_pending)
    glide_frame (self);
}

static gboolean
glide_begin (NeoHome *self)
{
  Glide *g = self->glide;
  if (g->paint_id)                                   /* resting a moment ago: the pictures are still up */
    glide_free_surfs (self);
  int x, y, fx, fy;
  if (!gtk_widget_translate_coordinates (self->drawer_scroll, self->win, 0, 0, &x, &y) ||
      !gtk_widget_translate_coordinates (self->drawer_flow, self->drawer_scroll, 0, 0, &fx, &fy))
    return FALSE;
  if ((g->stale || !g->tiles->len || g->scale != neo_home_scale (self) ||
       g->cw != gtk_widget_get_allocated_width (self->drawer_flow) ||
       g->ch != gtk_widget_get_allocated_height (self->drawer_flow)) && !glide_render (self))
    return FALSE;
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->drawer_scroll));
  g->vx = x;
  g->vy = y;
  g->vw = gtk_widget_get_allocated_width (self->drawer_scroll);
  g->vh = gtk_widget_get_allocated_height (self->drawer_scroll);
  g->pos = gtk_adjustment_get_value (adj);
  g->max = MAX (0, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
  g->fx = fx;
  g->pad = fy + g->pos;
  for (guint i = 0; i < g->tiles->len; i++) {
    NeoSurf *f = neo_surf_new ();
    neo_surf_set_image (f, g->tiles->pdata[i]);
    neo_surf_attach (f, self->win);
    g_ptr_array_add (g->surfs, f);
  }
  g->active = TRUE;
  glide_frame (self);
  /* the pictures are up (in this commit): the list itself goes while they move. Not by opacity: the viewport
   * draws into a child window of its own, and the list showed through, every icon twice. */
  gtk_widget_set_child_visible (self->drawer_flow, FALSE);
  return TRUE;
}

/* A new touch: a fling stops where it is (the touch may go on to scroll it again), and a drag whose finger
 * never said it let go (no drag-end, no cancel) is over: it ignored every swipe after it and the list was stuck. */
static void
glide_catch (NeoHome *self)
{
  Glide *g = self->glide;
  if (!g || !g->active)
    return;
  if (g->dragging) {
    g_debug ("glide: a new touch while a drag was still open, ending it");
    g->dragging = FALSE;
  }
  g->pos = CLAMP (g->pos, 0, g->max);
  glide_rest (self);
}

static void
glide_pan (GtkGesturePan *pan, GtkPanDirection dir, double offset, NeoHome *self)
{
  Glide *g = self->glide;
  if (g->dragging)
    return;
  if (g->active)
    g_debug ("glide: a pan while still active (flinging %d)", g->flinging);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->drawer_scroll));
  /* at the top of the list a swipe down is phoc's: it pulls the drawer down */
  gboolean top = gtk_adjustment_get_value (adj) < 0.5;
  if ((self->lift && self->lift->moving) || !drawer_open (self) || (top && dir == GTK_PAN_DIRECTION_DOWN) ||
      !glide_begin (self)) {
    g_debug ("glide: not taken (%s)", self->lift && self->lift->moving ? "an icon moves" : !drawer_open (self) ?
               "drawer not open" : top && dir == GTK_PAN_DIRECTION_DOWN ? "top, down: the drawer's" : "begin failed");
    gtk_gesture_set_state (GTK_GESTURE (pan), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  g_debug ("glide: taken at %.0f of %.0f", g->pos, g->max);
  gtk_gesture_set_state (GTK_GESTURE (pan), GTK_EVENT_SEQUENCE_CLAIMED);
  self->scroll_t = g_get_monotonic_time ();
  lift_free (self);                                  /* an icon held under this finger lets go */
  neo_drag_set_mode (self->drag, NEO_DRAG_NONE);    /* the list's, not a drawer drag */
  g->dragging = TRUE;
  g->started = FALSE;
  g->start = g->pos;
  g->vel = 0;
  g->last_t = g_get_monotonic_time ();
}

static void
glide_update (GtkGestureDrag *d, double ox, double oy, NeoHome *self)
{
  Glide *g = self->glide;
  if (!g->dragging)
    return;
  if (!g->started) {
    g->started = TRUE;
    g->oy0 = oy;
  }
  double p = g->start - (oy - g->oy0);
  /* past an end the list follows the finger a third as far */
  if (p < 0)
    p *= 0.35;
  else if (p > g->max)
    p = g->max + (p - g->max) * 0.35;
  gint64 now = g_get_monotonic_time ();
  double dt = (now - g->last_t) / 1e6;
  if (dt > 0.002)
    g->vel = 0.7 * ((p - g->pos) / dt) + 0.3 * g->vel;
  g->last_t = now;
  g->pos = p;
  glide_request (self);
}

static void
glide_end (GtkGestureDrag *d, double ox, double oy, NeoHome *self)
{
  Glide *g = self->glide;
  if (!g->dragging)
    return;
  g->dragging = FALSE;
  if (g_get_monotonic_time () - g->last_t > 70000)  /* the finger rested before it let go */
    g->vel = 0;
  g->a_t0 = g_get_monotonic_time ();
  g->a_from = g->pos;
  if (g->pos < 0 || g->pos > g->max) {
    g->a_to = CLAMP (g->pos, 0, g->max);
    g->a_ms = GLIDE_BACK_MS;
  } else if (fabs (g->vel) > 60) {
    g->a_v = CLAMP (g->vel, -GLIDE_VMAX, GLIDE_VMAX);
    g->a_ms = 0;
  } else {
    glide_rest (self);
    return;
  }
  g->flinging = TRUE;
  glide_request (self);
}

static void
glide_cancel (GtkGesture *gest, GdkEventSequence *seq, NeoHome *self)
{
  glide_end (GTK_GESTURE_DRAG (gest), 0, 0, self);
}

static void
glide_setup (NeoHome *self)
{
  if (!self->surfs_ok)
    return;
  Glide *g = self->glide = g_new0 (Glide, 1);
  g->tiles = g_ptr_array_new_with_free_func ((GDestroyNotify) cairo_surface_destroy);
  g->surfs = g_ptr_array_new_with_free_func ((GDestroyNotify) neo_surf_free);
  g->stale = TRUE;
  /* the scrolled window keeps the wheel and the scrollbar; the finger is Neo's */
  gtk_scrolled_window_set_kinetic_scrolling (GTK_SCROLLED_WINDOW (self->drawer_scroll), FALSE);
  GtkGesture *pan = gtk_gesture_pan_new (self->drawer_scroll, GTK_ORIENTATION_VERTICAL);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (pan), GTK_PHASE_CAPTURE);
  g_signal_connect (pan, "pan", G_CALLBACK (glide_pan), self);
  g_signal_connect (pan, "drag-update", G_CALLBACK (glide_update), self);
  g_signal_connect (pan, "drag-end", G_CALLBACK (glide_end), self);
  g_signal_connect (pan, "cancel", G_CALLBACK (glide_cancel), self);
  g_object_set_data_full (G_OBJECT (self->drawer_scroll), "pan", pan, g_object_unref);
}

/* ---- the pages' sideways slide ----
 * HdyCarousel slid the pages by drawing them again on every frame: GTK 3 draws in software and phoc uploaded the
 * whole home at each one, which stuttered beside the drawer (phoc's own slide). Neo slides them the way the task
 * view animates: when the swipe starts each page near is drawn once into a subsurface (neo-surf.c), the finger and
 * the settling only move those, and the carousel jumps to the page it ends on. */
#define SLIDE_MS 320
#define SLIDE_START 10           /* px the finger moves sideways before the pages follow it */
#define SLIDE_FLING 450          /* px/s: a flick this fast turns the page however short */

typedef struct Slide {
  NeoSurf  *surf[3];             /* borrowed from the PagePics: the pages before, at and after the one it started on
                                  * (NULL past an end) */
  int       from, n;
  double    cx, cy, cw, ch;      /* the carousel in the window */
  double    dx, vx;              /* the pages' offset, and the finger's speed (px/s) */
  double    fx;                  /* the finger's own offset since it came down: the page is picked from this */
  double    ox0;                 /* the finger's offset when the slide was recognised (it moves from there) */
  gboolean  started;
  gint64    last_t;
  gboolean  animating, frame_pending, dirty;
  double    anim_from, anim_to;
  gint64    anim_t0;
  int       anim_ms, target;
  gulong    paint_id;            /* settled: waiting for GTK to draw the carousel at its new page */
  gulong    drawn_id;            /* the carousel's own "draw": that paint is the one that shows the page */
  gboolean  drawn;
  gint64    hist_t[16];          /* the finger's recent offsets (event times, µs), for its speed over a window */
  double    hist_x[16];
  int       hist_n;
} Slide;

#define SLIDE_VWINDOW 60000      /* µs: the speed is the move over about this long */

/* the finger's speed over the last SLIDE_VWINDOW: one event's step is noise (a release repeats the last position) */
static double
slide_speed (Slide *sl, gint64 now)
{
  int last = (sl->hist_n - 1) % 16;
  if (sl->hist_n < 2)
    return 0;
  int k = last;
  for (int i = 1; i < MIN (sl->hist_n, 16); i++) {
    int j = (last - i + 16) % 16;
    k = j;
    if (now - sl->hist_t[j] >= SLIDE_VWINDOW)
      break;
  }
  double dt = (sl->hist_t[last] - sl->hist_t[k]) / 1e6;
  return dt > 0.008 ? (sl->hist_x[last] - sl->hist_x[k]) / dt : 0;
}

static void
slide_free (NeoHome *self)
{
  Slide *sl = self->slide;
  if (!sl)
    return;
  self->slide = NULL;
  if (sl->drawn_id)
    g_signal_handler_disconnect (self->carousel, sl->drawn_id);
  if (sl->paint_id) {
    g_signal_handler_disconnect (gtk_widget_get_frame_clock (self->win), sl->paint_id);
    self->pics_quiet = FALSE;
    pics_mark_strip (self, sl->target);
  }
  if (sl->frame_pending)
    neo_surf_cancel_frame (self->win);
  /* the pictures are kept for the next slide: hidden, not freed */
  for (int k = 0; k < 3; k++)
    if (sl->surf[k])
      neo_surf_place (sl->surf[k], 0, 0, 0, 0, 0);
  gtk_widget_set_child_visible (self->carousel, TRUE);
  g_free (sl);
  pics_prerender (self);
}

static gboolean
slide_carousel_drawn (GtkWidget *carousel, cairo_t *cr, NeoHome *self)
{
  self->slide->drawn = TRUE;
  return FALSE;
}

/* the carousel is drawn at its new page: the pictures can go. Any two paints were not that: the second often came
 * with the clock's next tick, so for a second after each slide the next one began late, its pictures stale. */
static void
slide_after_paint (GdkFrameClock *clock, NeoHome *self)
{
  Slide *sl = self->slide;
  if (!sl->drawn) {
    gtk_widget_queue_draw (self->carousel);
    return;
  }
  g_signal_handler_disconnect (clock, sl->paint_id);
  sl->paint_id = 0;
  g_debug ("slide: settled, the carousel drawn %.1f ms after", (g_get_monotonic_time () - sl->anim_t0) / 1e3 - sl->anim_ms);
  /* that redraw drew what the target page's picture holds, but for a clock tick in between */
  self->pics_quiet = FALSE;
  pics_mark_strip (self, sl->target);
  slide_free (self);
  neo_surf_commit_parent (self->win, NULL, NULL);
}

static void
slide_settled (NeoHome *self)
{
  Slide *sl = self->slide;
  sl->animating = FALSE;
  GtkWidget *page = page_nth (self, sl->target);
  if (page)
    hdy_carousel_scroll_to_full (HDY_CAROUSEL (self->carousel), page, 0);
  gtk_widget_set_child_visible (self->carousel, TRUE);
  GdkFrameClock *clock = gtk_widget_get_frame_clock (self->win);
  if (!clock) {
    slide_free (self);
    return;
  }
  sl->drawn = FALSE;
  self->pics_quiet = TRUE;
  if (!sl->drawn_id)
    sl->drawn_id = g_signal_connect_after (self->carousel, "draw", G_CALLBACK (slide_carousel_drawn), self);
  sl->paint_id = g_signal_connect (clock, "after-paint", G_CALLBACK (slide_after_paint), self);
  gtk_widget_queue_draw (self->carousel);
}

static void slide_frame (NeoHome *self);

static void
slide_on_frame (gpointer data)
{
  NeoHome *self = data;
  Slide *sl = self->slide;
  if (!sl)
    return;
  sl->frame_pending = FALSE;
  if (sl->animating || sl->dirty)
    slide_frame (self);
}

/* one frame: place the pictures, commit, and ask phoc to say when it is shown */
static void
slide_frame (NeoHome *self)
{
  Slide *sl = self->slide;
  sl->dirty = FALSE;
  if (sl->animating) {
    double t = MIN (1.0, (g_get_monotonic_time () - sl->anim_t0) / (sl->anim_ms * 1000.0));
    double e = 1 - (1 - t) * (1 - t) * (1 - t);
    sl->dx = sl->anim_from + (sl->anim_to - sl->anim_from) * e;
    if (t >= 1) {
      slide_settled (self);
      return;
    }
  }
  for (int k = 0; k < 3; k++)
    if (sl->surf[k])
      neo_surf_place (sl->surf[k], sl->cx + (k - 1) * sl->cw + sl->dx, sl->cy, sl->cw, sl->ch, 1);
  sl->frame_pending = TRUE;
  neo_surf_commit_parent (self->win, slide_on_frame, self);
}

static void
slide_request (NeoHome *self)
{
  self->slide->dirty = TRUE;
  if (!self->slide->frame_pending)
    slide_frame (self);
}

/* ---- the pages as pictures ----
 * Drawing a page (GTK 3 draws in software) and handing it to phoc took 50-60 ms a page, so every slide began with
 * the pages standing still for 100 ms while the finger moved on. Each page now keeps its picture in a buffer phoc
 * reads (neo_surf_canvas); what GTK draws on the page marks that part changed, and only that part is drawn again:
 * at the next slide that is the clock's second hand, a few ms. Pages not in view get no "draw" while things change
 * on them, so the clock strip there is marked at every slide, and icons arriving or the pages rebuilt mark all. */

typedef struct {
  NeoHome        *home;
  GtkWidget      *page;          /* weak */
  gulong          draw_id;
  NeoSurf        *surf;
  cairo_region_t *dirty;         /* page px that changed since the picture was drawn */
  gboolean        full;          /* draw all of it */
  int             w, h, scale;
} PagePic;

static gboolean
pic_page_drawn (GtkWidget *page, cairo_t *cr, PagePic *p)
{
  if (p->home->picturing || p->home->pics_quiet)
    return FALSE;
  double x1, y1, x2, y2;
  cairo_clip_extents (cr, &x1, &y1, &x2, &y2);
  cairo_rectangle_int_t r = { (int) floor (x1), (int) floor (y1), (int) ceil (x2) - (int) floor (x1),
                              (int) ceil (y2) - (int) floor (y1) };
  if (r.width > 0 && r.height > 0)
    cairo_region_union_rectangle (p->dirty, &r);
  return FALSE;
}

static void
pic_free (PagePic *p)
{
  if (!p)
    return;
  if (p->page) {
    g_signal_handler_disconnect (p->page, p->draw_id);
    g_clear_weak_pointer (&p->page);
  }
  neo_surf_free (p->surf);
  cairo_region_destroy (p->dirty);
  g_free (p);
}

/* the pages are built anew: their pictures go with them */
static void
pics_reset (NeoHome *self)
{
  slide_free (self);
  for (guint i = 0; i < self->pics->len; i++)
    pic_free (self->pics->pdata[i]);
  g_ptr_array_set_size (self->pics, 0);
}

/* something changed that no "draw" told of (an icon arrived on a page not in view): all of every page */
static void
pics_stale (NeoHome *self)
{
  for (guint i = 0; i < self->pics->len; i++) {
    PagePic *p = self->pics->pdata[i];
    if (p)
      p->full = TRUE;
  }
}

static void
pic_mark_strip (NeoHome *self, PagePic *p)
{
  int sx, sy;
  if (self->strip && gtk_widget_is_ancestor (self->strip, p->page) &&
      gtk_widget_translate_coordinates (self->strip, p->page, 0, 0, &sx, &sy)) {
    cairo_rectangle_int_t r = { sx, sy, gtk_widget_get_allocated_width (self->strip),
                                gtk_widget_get_allocated_height (self->strip) };
    cairo_region_union_rectangle (p->dirty, &r);
  }
}

static void
pics_mark_strip (NeoHome *self, int i)
{
  if (i >= 0 && (guint) i < self->pics->len && self->pics->pdata[i])
    pic_mark_strip (self, self->pics->pdata[i]);
}

static PagePic *
pic_for (NeoHome *self, int i)
{
  GtkWidget *page = page_nth (self, i);
  if (!page)
    return NULL;
  if ((guint) i >= self->pics->len)
    g_ptr_array_set_size (self->pics, i + 1);
  PagePic *p = self->pics->pdata[i];
  if (p && p->page == page)
    return p;
  if (p && self->slide)                  /* the slide may hold the old picture */
    for (int k = 0; k < 3; k++)
      if (self->slide->surf[k] == p->surf)
        self->slide->surf[k] = NULL;
  pic_free (p);
  p = g_new0 (PagePic, 1);
  p->home = self;
  p->surf = neo_surf_new ();
  p->dirty = cairo_region_create ();
  p->full = TRUE;
  g_set_weak_pointer (&p->page, page);
  p->draw_id = g_signal_connect_after (page, "draw", G_CALLBACK (pic_page_drawn), p);
  self->pics->pdata[i] = p;
  return p;
}

/* page i's picture, drawn again where it changed; NULL past an end */
static NeoSurf *
pic_fresh (NeoHome *self, int i)
{
  PagePic *p = pic_for (self, i);
  if (!p)
    return NULL;
  int w = gtk_widget_get_allocated_width (p->page), h = gtk_widget_get_allocated_height (p->page);
  int s = neo_home_scale (self);
  if (w <= 0 || h <= 0)
    return NULL;
  if (w != p->w || h != p->h || s != p->scale)
    p->full = TRUE;
  /* the strip's clock and weather change while the page is not in view */
  if (i != page_now (self))
    pic_mark_strip (self, p);
  cairo_rectangle_int_t all = { 0, 0, w, h };
  if (p->full) {
    cairo_region_destroy (p->dirty);
    p->dirty = cairo_region_create_rectangle (&all);
  }
  cairo_region_intersect_rectangle (p->dirty, &all);
  cairo_surface_t *canvas = neo_surf_canvas (p->surf, w * s, h * s);
  if (!canvas)
    return NULL;
  p->w = w;
  p->h = h;
  p->scale = s;
  if (cairo_region_is_empty (p->dirty))
    return p->surf;

  gint64 t0 = g_get_monotonic_time ();
  cairo_t *cr = cairo_create (canvas);
  cairo_scale (cr, s, s);
  int n = cairo_region_num_rectangles (p->dirty);
  for (int k = 0; k < n; k++) {
    cairo_rectangle_int_t r;
    cairo_region_get_rectangle (p->dirty, k, &r);
    cairo_rectangle (cr, r.x, r.y, r.width, r.height);
  }
  cairo_clip (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  self->picturing = TRUE;
  gtk_widget_draw (p->page, cr);
  self->picturing = FALSE;
  cairo_destroy (cr);
  cairo_rectangle_int_t ext;
  cairo_region_get_extents (p->dirty, &ext);
  neo_surf_canvas_changed (p->surf, ext.x * s, ext.y * s, ext.width * s, ext.height * s);
  g_debug ("slide: page %d picture: %s %dx%d in %.1f ms", i, p->full ? "all" : "changed", ext.width, ext.height,
           (g_get_monotonic_time () - t0) / 1e3);
  cairo_region_destroy (p->dirty);
  p->dirty = cairo_region_create ();
  p->full = FALSE;
  return p->surf;
}

/* after a slide or a show: the pages next to the one in view are drawn while nothing moves, one per idle tick */
static gboolean
pics_prerender_idle (gpointer data)
{
  NeoHome *self = data;
  if (self->slide || !gtk_widget_get_mapped (self->win) || self->touch_down)
    return G_SOURCE_CONTINUE;            /* not now: a finger or a slide has the pages */
  int now = page_now (self);
  for (int i = now - 1; i <= now + 1; i++) {
    if (i < 0 || !page_nth (self, i))
      continue;
    PagePic *p = (guint) i < self->pics->len ? self->pics->pdata[i] : NULL;
    if (!p || p->page != page_nth (self, i) || p->full || !cairo_region_is_empty (p->dirty)) {
      pic_fresh (self, i);
      return G_SOURCE_CONTINUE;
    }
  }
  self->pics_idle = 0;
  return G_SOURCE_REMOVE;
}

static void
pics_prerender (NeoHome *self)
{
  if (!self->pics_idle && self->surfs_ok)
    self->pics_idle = g_timeout_add_full (G_PRIORITY_LOW, 250, pics_prerender_idle, self, NULL);
}

/* the touch event's own time (µs): queued events processed late in a burst still give the finger's real speed */
static gint64
gesture_time (GtkGesture *g)
{
  GdkEventSequence *seq = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (g));
  const GdkEvent *e = gtk_gesture_get_last_event (g, seq);
  guint32 t = e ? gdk_event_get_time (e) : GDK_CURRENT_TIME;
  return t != GDK_CURRENT_TIME ? (gint64) t * 1000 : g_get_monotonic_time ();
}

static void
slide_begin (NeoHome *self)
{
  slide_free (self);
  int x, y;
  if (!gtk_widget_translate_coordinates (self->carousel, self->win, 0, 0, &x, &y))
    return;
  gint64 t0 = g_get_monotonic_time ();
  Slide *sl = self->slide = g_new0 (Slide, 1);
  sl->from = page_now (self);
  sl->n = hdy_carousel_get_n_pages (HDY_CAROUSEL (self->carousel));
  sl->cx = x;
  sl->cy = y;
  sl->cw = gtk_widget_get_allocated_width (self->carousel);
  sl->ch = gtk_widget_get_allocated_height (self->carousel);
  for (int k = 0; k < 3; k++) {
    NeoSurf *f = sl->surf[k] = pic_fresh (self, sl->from - 1 + k);
    if (f) {
      /* GTK 3 gives the window a new wl_surface at every map: the subsurface goes on the one there is now */
      neo_surf_detach (f);
      neo_surf_attach (f, self->win);
    }
  }
  slide_frame (self);
  /* the pictures are up (in this commit): the carousel itself draws nothing while they move */
  gtk_widget_set_child_visible (self->carousel, FALSE);
  g_debug ("slide: pages ready in %.1f ms", (g_get_monotonic_time () - t0) / 1e3);
}

static void
slide_pan (GtkGesturePan *g, GtkPanDirection dir, double offset, NeoHome *self)
{
  if (self->slide && !self->slide->animating && !self->slide->paint_id)
    return;                                     /* sliding already */
  /* GTK's pan names a direction after a pixel or two: a tap's jitter is not a slide (it would eat the click) */
  if (fabs (offset) < SLIDE_START)
    return;
  if ((self->lift && self->lift->moving) || drawer_open (self) || !self->surfs_ok) {
    g_debug ("slide: refused (lift %d moving %d, drawer %d)", self->lift != NULL,
               self->lift && self->lift->moving, drawer_open (self));
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  g_debug ("slide: taken (%s, offset %.0f)", !self->slide ? "idle" : self->slide->animating ? "settling" : "drawing",
             offset);
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
  /* caught while settling: the carousel goes to where it was going, the new slide starts from there */
  if (self->slide && self->slide->animating) {
    GtkWidget *page = page_nth (self, self->slide->target);
    if (page)
      hdy_carousel_scroll_to_full (HDY_CAROUSEL (self->carousel), page, 0);
  }
  self->scroll_t = g_get_monotonic_time ();
  lift_free (self);
  slide_begin (self);
}

static void
slide_update (GtkGestureDrag *g, double ox, double oy, NeoHome *self)
{
  Slide *sl = self->slide;
  if (!sl || sl->animating || sl->paint_id)
    return;
  if (!sl->started) {
    sl->started = TRUE;
    sl->ox0 = ox;
  }
  sl->fx = ox;
  double dx = ox - sl->ox0;
  /* past the first or the last page: the pages follow the finger a little, and come back */
  if ((sl->from == 0 && dx > 0) || (sl->from == sl->n - 1 && dx < 0))
    dx *= 0.3;
  gint64 now = gesture_time (GTK_GESTURE (g));
  /* a repeat of the same place (the release's) is no move: it keeps the time the finger last moved */
  if (dx != sl->dx || !sl->hist_n) {
    sl->hist_t[sl->hist_n % 16] = now;
    sl->hist_x[sl->hist_n % 16] = dx;
    sl->hist_n++;
    sl->last_t = now;
  }
  sl->vx = slide_speed (sl, now);
  sl->dx = dx;
  slide_request (self);
}

static void
slide_end (GtkGestureDrag *g, double ox, double oy, NeoHome *self)
{
  Slide *sl = self->slide;
  if (!sl || sl->animating || sl->paint_id)
    return;
  /* a finger resting before it lets go flicks nothing (both times are the events', see gesture_time) */
  gint64 rest = sl->hist_n ? gesture_time (GTK_GESTURE (g)) - sl->last_t : 0;
  if (rest > 80000)
    sl->vx = 0;
  /* the page is picked from how far the finger went, the first SLIDE_START px it moved before the pages did too */
  double fx = sl->fx;
  int target = sl->from;
  if ((fx < -sl->cw * 0.25 || sl->vx < -SLIDE_FLING) && sl->vx < SLIDE_FLING && sl->from + 1 < sl->n)
    target = sl->from + 1;
  else if ((fx > sl->cw * 0.25 || sl->vx > SLIDE_FLING) && sl->vx > -SLIDE_FLING && sl->from > 0)
    target = sl->from - 1;
  g_debug ("slide: end finger %.0f of %.0f, vx %.0f px/s, rest %.0f ms -> page %d (from %d)", fx, sl->cw, sl->vx,
           rest / 1e3, target, sl->from);
  sl->target = target;
  sl->anim_from = sl->dx;
  sl->anim_to = -(target - sl->from) * sl->cw;
  sl->anim_ms = (int) CLAMP (fabs (sl->anim_to - sl->anim_from) / sl->cw * SLIDE_MS, 120, SLIDE_MS);
  sl->anim_t0 = g_get_monotonic_time ();
  sl->animating = TRUE;
  slide_request (self);
}

static void
slide_cancel (GtkGesture *g, GdkEventSequence *seq, NeoHome *self)
{
  g_debug ("slide: cancelled");
  slide_end (GTK_GESTURE_DRAG (g), 0, 0, self);
}

/* the drawer's list or the pages move under a finger */
/* this touch is a swipe, not a press: it moved, or something (the pages, the list, phoc's drawer) moved with it.
 * No long press, no menu and no click comes of it. */
static gboolean
touch_swiped (NeoHome *self)
{
  return finger_scrolls (self) || self->touch_moved || self->scroll_t > self->touch_t ||
         (self->drag && neo_drag_is_moving (self->drag));
}

static gboolean
finger_scrolls (NeoHome *self)
{
  return (self->glide && self->glide->dragging) || (self->slide && !self->slide->animating && !self->slide->paint_id);
}

/* ---- the launch card ---- */

/* Android's app launch: a card grows from the icon to the whole screen and stays, the app's icon in its middle,
 * until the app's window is on screen (neo-launch.c), so the app shown before never flashes up in between */
#define LAUNCH_MS 260

struct LaunchCard {
  GtkWidget *card, *spinner;
  double     x0, y0, w0, h0;
  gint64     t0;
  guint      tick, spin_timer;
  gboolean   grown, ended;
};

static void
launch_card_remove (NeoHome *self)
{
  struct LaunchCard *c = self->card;
  if (!c)
    return;
  self->card = NULL;
  g_clear_handle_id (&c->spin_timer, g_source_remove);
  if (c->tick)
    gtk_widget_remove_tick_callback (c->card, c->tick);
  gtk_widget_destroy (c->card);
  g_free (c);
}

/* the app came up while the card still grew: Neo goes once it has */
static gboolean
launch_card_grown_late (gpointer data)
{
  NeoHome *self = data;
  if (self->card && self->card->ended)
    neo_home_hide (self);
  return G_SOURCE_REMOVE;
}

static gboolean
launch_card_tick (GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
  NeoHome *self = data;
  struct LaunchCard *c = self->card;
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  if (!c->t0)
    c->t0 = now;
  double t = MIN (1.0, (now - c->t0) / (LAUNCH_MS * 1000.0));
  double e = 1 - (1 - t) * (1 - t) * (1 - t);                  /* ease-out cubic */
  /* the screen Neo shows: the home on top, or the drawer when it is up */
  double y1 = self->drag && neo_drag_is_unfolded (self->drag) ? self->Hv : 0;
  gtk_fixed_move (GTK_FIXED (self->float_layer), c->card, (int) round (c->x0 * (1 - e)),
                  (int) round (c->y0 + (y1 - c->y0) * e));
  gtk_widget_set_size_request (c->card, (int) round (c->w0 + (self->W - c->w0) * e),
                               (int) round (c->h0 + (self->Hv - c->h0) * e));
  if (t < 1)
    return G_SOURCE_CONTINUE;
  c->tick = 0;
  c->grown = TRUE;
  if (c->ended)
    g_idle_add (launch_card_grown_late, self);
  return G_SOURCE_REMOVE;
}

/* an app that takes its time (an Android one, above all when Android itself has to start) shows that it is still
 * coming: the card alone looked like nothing happened */
static gboolean
launch_card_spin (gpointer data)
{
  NeoHome *self = data;
  self->card->spin_timer = 0;
  gtk_spinner_start (GTK_SPINNER (self->card->spinner));
  gtk_widget_set_opacity (self->card->spinner, 1);
  return G_SOURCE_REMOVE;
}

void
neo_home_launch_card (NeoHome *self, GAppInfo *info, GtkWidget *from)
{
  launch_card_remove (self);
  if (!gtk_widget_get_mapped (self->float_layer))
    return;
  struct LaunchCard *c = self->card = g_new0 (struct LaunchCard, 1);
  /* from the cell's icon (a cell is a button around its icon and label) */
  GtkWidget *icon = from && GTK_IS_BIN (from) ? gtk_bin_get_child (GTK_BIN (from)) : NULL;
  if (icon && GTK_IS_CONTAINER (icon)) {
    g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (icon));
    icon = kids ? kids->data : NULL;
  }
  int x, y;
  if (icon && gtk_widget_get_mapped (icon) &&
      gtk_widget_translate_coordinates (icon, self->float_layer, 0, 0, &x, &y)) {
    c->x0 = x;
    c->y0 = y;
    c->w0 = gtk_widget_get_allocated_width (icon);
    c->h0 = gtk_widget_get_allocated_height (icon);
  } else {
    double top = self->drag && neo_drag_is_unfolded (self->drag) ? self->Hv : 0;
    c->w0 = c->h0 = ICON;
    c->x0 = (self->W - ICON) / 2.0;
    c->y0 = top + (self->Hv - ICON) / 2.0;
  }
  int size = CLAMP ((int) MIN (c->w0, c->h0), 32, 96);
  c->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class (gtk_widget_get_style_context (c->card), "neo-launch-card");
  /* the spinner rides over the card, below the icon: in the box it would push the icon up, and size the card */
  GtkWidget *middle = gtk_overlay_new ();
  gtk_widget_set_hexpand (middle, TRUE);
  gtk_widget_set_vexpand (middle, TRUE);
  GtkWidget *image = icon_image (self, info, size);
  gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
  gtk_container_add (GTK_CONTAINER (middle), image);
  c->spinner = gtk_spinner_new ();
  gtk_widget_set_size_request (c->spinner, 28, 28);
  gtk_widget_set_halign (c->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (c->spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (c->spinner, 2 * (size + 24));
  gtk_widget_set_opacity (c->spinner, 0);
  gtk_overlay_add_overlay (GTK_OVERLAY (middle), c->spinner);
  gtk_container_add (GTK_CONTAINER (c->card), middle);
  gtk_widget_show_all (c->card);
  gtk_widget_set_size_request (c->card, (int) c->w0, (int) c->h0);
  gtk_fixed_put (GTK_FIXED (self->float_layer), c->card, (int) c->x0, (int) c->y0);
  c->tick = gtk_widget_add_tick_callback (c->card, launch_card_tick, self, NULL);
  c->spin_timer = g_timeout_add (1000, launch_card_spin, self);
}

/* the launch is over (the app's window is up, or Neo gave up waiting): Neo goes, once the card has grown */
void
neo_home_launch_end (NeoHome *self)
{
  if (self->card && !self->card->grown) {
    self->card->ended = TRUE;
    return;
  }
  neo_home_hide (self);
}

/* for neo-launch.c */
void neo_home_release_keyboard (NeoHome *self);
void
neo_home_release_keyboard (NeoHome *self)
{
  gtk_layer_set_keyboard_mode (GTK_WINDOW (self->win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
}
