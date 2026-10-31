/* The home bar and the back edges (see neo-edges.h). Each is a narrow transparent layer window that takes the
 * touch where it starts. Phosh's own drag progress was useless for telling a swipe from a rest (it jumped to 1.0
 * ~80 ms into a slow swipe), so the finger's own travel, rest and speed decide (One UI):
 *   swipe up and let go          → home (nothing of the task view shows)
 *   swipe up and rest            → the task view
 * Back: the pull in from an edge closes what Neo has open, or sends Alt+Left to the app (wtype). While the screen
 * is locked the home bar steps aside: the lock screen's own "slide up to unlock" lives there. */
#include "neo-edges.h"

#include <gtk-layer-shell.h>
#include <math.h>

#include "neo-home.h"
#include "neo-recents.h"

#define STRIP 15            /* logical px: Phosh's PHOSH_HOME_BAR_HEIGHT */
#define ENGAGE 8            /* px up before the swipe starts */
#define HOLD_MS 110         /* resting this long within HOLD_SLOP px: the task view */
#define HOLD_SLOP 14
#define HOLD_MIN 40         /* px up a hold needs */
#define BACK_STRIP 14
#define ARM 48              /* inward travel that makes it a back gesture */
#define CLEAR_TOP 48        /* keep off Phosh's top bar and home bar */
#define CLEAR_BOTTOM 40
#define PILL 48

typedef struct _NeoEdges NeoEdges;

typedef struct {
  NeoEdges  *edges;
  GtkWidget *win, *pill;
  gboolean   left;
  guint      fade;
} Back;

struct _NeoEdges {
  NeoHome    *home;
  NeoRecents *recents;
  GtkApplication *app;
  GtkWidget  *bar;
  GtkGesture *bar_drag;
  /* the swipe on the bar */
  gboolean    on, held;
  double      anchor;
  guint       hold_id;
  Back        back[2];
};

static GtkWidget *
strip_window (GtkApplication *app, const char *ns, GtkLayerShellLayer layer, int w, int h)
{
  GtkWidget *win = gtk_application_window_new (app);
  gtk_window_set_decorated (GTK_WINDOW (win), FALSE);
  GdkVisual *rgba = gdk_screen_get_rgba_visual (gtk_widget_get_screen (win));
  if (rgba)
    gtk_widget_set_visual (win, rgba);
  gtk_widget_set_app_paintable (win, TRUE);
  gtk_layer_init_for_window (GTK_WINDOW (win));
  gtk_layer_set_namespace (GTK_WINDOW (win), ns);
  gtk_layer_set_layer (GTK_WINDOW (win), layer);
  gtk_layer_set_keyboard_mode (GTK_WINDOW (win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  gtk_widget_set_size_request (win, w, h);
  gtk_style_context_add_class (gtk_widget_get_style_context (win), "neo-edge");
  return win;
}

/* a frame has to be drawn or the surface never maps and gets no touch: nearly transparent */
static gboolean
draw_faint (GtkWidget *w, cairo_t *cr, gpointer data)
{
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba (cr, 0, 0, 0, 0.01);
  cairo_paint (cr);
  return TRUE;
}

static GtkWidget *
touch_area (GtkWidget *win)
{
  GtkWidget *area = gtk_drawing_area_new ();
  gtk_widget_add_events (area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                               GDK_TOUCH_MASK);
  g_signal_connect (win, "draw", G_CALLBACK (draw_faint), NULL);
  gtk_container_add (GTK_CONTAINER (win), area);
  gtk_widget_show (area);
  return area;
}

/* ---- the home bar ---- */

static gboolean
held (gpointer data)
{
  NeoEdges *self = data;
  self->hold_id = 0;
  if (self->anchor < HOLD_MIN)          /* resting on the bar itself is no swipe */
    return G_SOURCE_REMOVE;
  self->held = TRUE;
  neo_recents_hold (self->recents);
  return G_SOURCE_REMOVE;
}

static void
bar_begin (GtkGestureDrag *g, double x, double y, NeoEdges *self)
{
  self->on = self->held = FALSE;
  self->anchor = 0;
  g_clear_handle_id (&self->hold_id, g_source_remove);
}

static void
bar_update (GtkGestureDrag *g, double dx, double dy, NeoEdges *self)
{
  double up = -dy;
  if (!self->on) {
    if (up < ENGAGE)
      return;
    self->on = TRUE;
    if (self->recents)
      neo_recents_begin (self->recents);
  }
  if (self->recents && !self->held)
    neo_recents_track (self->recents, up - ENGAGE);
  if (!self->held && fabs (up - self->anchor) > HOLD_SLOP) {
    self->anchor = up;
    g_clear_handle_id (&self->hold_id, g_source_remove);
    if (self->recents)
      self->hold_id = g_timeout_add (HOLD_MS, held, self);
  }
}

static void
bar_end (GtkGestureDrag *g, double dx, double dy, NeoEdges *self)
{
  g_clear_handle_id (&self->hold_id, g_source_remove);
  if (!self->on)
    return;
  self->on = FALSE;
  if (self->held) {
    neo_recents_release (self->recents, TRUE);
  } else {
    if (self->recents)
      neo_recents_release (self->recents, FALSE);
    neo_home_show (self->home);
  }
}

static void
on_locked (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *signal,
           GVariant *params, gpointer data)
{
  NeoEdges *self = data;
  gboolean locked = FALSE;
  g_variant_get (params, "(b)", &locked);
  gtk_widget_set_visible (self->bar, !locked);
}

static void
got_locked (GObject *src, GAsyncResult *res, gpointer data)
{
  NeoEdges *self = data;
  g_autoptr (GVariant) out = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  gboolean locked = FALSE;
  if (out)
    g_variant_get (out, "(b)", &locked);
  gtk_widget_set_visible (self->bar, !locked);
}

static void
make_bar (NeoEdges *self)
{
  GtkWidget *win = self->bar = strip_window (self->app, "neo-home-bar", GTK_LAYER_SHELL_LAYER_OVERLAY, -1, STRIP);
  gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
  gtk_layer_set_exclusive_zone (GTK_WINDOW (win), -1);    /* on the screen edge itself, over Phosh's bar */
  GtkWidget *area = touch_area (win);
  self->bar_drag = gtk_gesture_drag_new (area);
  g_signal_connect (self->bar_drag, "drag-begin", G_CALLBACK (bar_begin), self);
  g_signal_connect (self->bar_drag, "drag-update", G_CALLBACK (bar_update), self);
  g_signal_connect (self->bar_drag, "drag-end", G_CALLBACK (bar_end), self);

  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  g_dbus_connection_signal_subscribe (bus, "org.gnome.ScreenSaver", "org.gnome.ScreenSaver", "ActiveChanged",
                                      "/org/gnome/ScreenSaver", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_locked, self, NULL);
  gtk_widget_show (win);
  g_dbus_connection_call (bus, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver",
                          "GetActive", NULL, G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL,
                          got_locked, self);
}

/* ---- back ---- */

static gboolean
fade_step (gpointer data)
{
  Back *b = data;
  double o = gtk_widget_get_opacity (b->pill) - 0.12;
  if (o > 0) {
    gtk_widget_set_opacity (b->pill, o);
    return G_SOURCE_CONTINUE;
  }
  gtk_widget_hide (b->pill);
  b->fade = 0;
  return G_SOURCE_REMOVE;
}

static gboolean
draw_pill (GtkWidget *w, cairo_t *cr, Back *b)
{
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba (cr, 0, 0, 0, 0);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  double c = PILL / 2.0;
  cairo_arc (cr, c, c, 22, 0, 2 * G_PI);
  cairo_set_source_rgba (cr, 32 / 255.0, 33 / 255.0, 36 / 255.0, 0.85);
  cairo_fill (cr);
  /* the arrow, 20 px */
  double d = b->left ? 1 : -1;
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_set_line_width (cr, 2.5);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_move_to (cr, c + 7 * d, c);
  cairo_line_to (cr, c - 7 * d, c);
  cairo_move_to (cr, c - 1 * d, c - 6);
  cairo_line_to (cr, c - 7 * d, c);
  cairo_line_to (cr, c - 1 * d, c + 6);
  cairo_stroke (cr);
  return TRUE;
}

/* the pill takes no touches */
static void
no_input (GtkWidget *w, gpointer data)
{
  cairo_region_t *empty = cairo_region_create ();
  gdk_window_input_shape_combine_region (gtk_widget_get_window (w), empty, 0, 0);
  cairo_region_destroy (empty);
}

/* the arrow pill beside the finger while it pulls in (inward < 0 hides it); y in the strip's coordinates */
static void
pill (Back *b, double inward, double y)
{
  GtkLayerShellEdge edge = b->left ? GTK_LAYER_SHELL_EDGE_LEFT : GTK_LAYER_SHELL_EDGE_RIGHT;
  if (inward < 8) {
    if (b->pill && gtk_widget_get_visible (b->pill) && !b->fade)
      b->fade = g_timeout_add (16, fade_step, b);
    return;
  }
  g_clear_handle_id (&b->fade, g_source_remove);
  if (!b->pill) {
    GtkWidget *p = b->pill = strip_window (b->edges->app, "neo-back-arrow", GTK_LAYER_SHELL_LAYER_OVERLAY, PILL, PILL);
    gtk_layer_set_anchor (GTK_WINDOW (p), edge, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (p), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    g_signal_connect (p, "draw", G_CALLBACK (draw_pill), b);
    g_signal_connect (p, "realize", G_CALLBACK (no_input), NULL);
  }
  double t = MIN (1, inward / ARM);
  /* it slides out with the finger; 43 % while pulling, solid once letting go means back */
  gtk_layer_set_margin (GTK_WINDOW (b->pill), edge, (int) round (4 + t * 20));
  gtk_layer_set_margin (GTK_WINDOW (b->pill), GTK_LAYER_SHELL_EDGE_TOP, MAX (0, (int) round (CLEAR_TOP + y - PILL / 2.0)));
  gtk_widget_set_opacity (b->pill, t >= 1 ? 1 : 0.43);
  gtk_widget_show (b->pill);
}

static void
back_update (GtkGestureDrag *g, double dx, double dy, Back *b)
{
  double sx, sy;
  gtk_gesture_drag_get_start_point (g, &sx, &sy);
  pill (b, b->left ? dx : -dx, sy + dy);
}

static void
back_end (GtkGestureDrag *g, double dx, double dy, Back *b)
{
  double inward = b->left ? dx : -dx;
  pill (b, -1, 0);
  if (inward < ARM || fabs (dy) >= inward * 1.5)
    return;
  if (neo_home_back (b->edges->home))           /* Neo handled it */
    return;
  const char *argv[] = { "wtype", "-M", "alt", "-k", "Left", "-m", "alt", NULL };
  g_autoptr (GError) err = NULL;
  g_autoptr (GSubprocess) p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_NONE, &err);
  if (!p)
    g_warning ("back (wtype): %s", err->message);
}

static void
make_back (NeoEdges *self, Back *b, gboolean left)
{
  b->edges = self;
  b->left = left;
  GtkLayerShellEdge edge = left ? GTK_LAYER_SHELL_EDGE_LEFT : GTK_LAYER_SHELL_EDGE_RIGHT;
  GtkWidget *win = b->win = strip_window (self->app, left ? "neo-back-left" : "neo-back-right",
                                          GTK_LAYER_SHELL_LAYER_TOP, BACK_STRIP, -1);
  gtk_layer_set_anchor (GTK_WINDOW (win), edge, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_margin (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_TOP, CLEAR_TOP);
  gtk_layer_set_margin (GTK_WINDOW (win), GTK_LAYER_SHELL_EDGE_BOTTOM, CLEAR_BOTTOM);
  gtk_layer_set_exclusive_zone (GTK_WINDOW (win), 0);
  GtkWidget *area = touch_area (win);
  GtkGesture *drag = gtk_gesture_drag_new (area);
  g_object_set_data_full (G_OBJECT (area), "neo-drag", drag, g_object_unref);
  g_signal_connect (drag, "drag-update", G_CALLBACK (back_update), b);
  g_signal_connect (drag, "drag-end", G_CALLBACK (back_end), b);
  gtk_widget_show (win);
}

NeoEdges *
neo_edges_new (GtkApplication *app, NeoHome *home, NeoRecents *recents)
{
  NeoEdges *self = g_new0 (NeoEdges, 1);
  self->app = app;
  self->home = home;
  self->recents = recents;
  make_bar (self);
  make_back (self, &self->back[0], TRUE);
  make_back (self, &self->back[1], FALSE);
  return self;
}
