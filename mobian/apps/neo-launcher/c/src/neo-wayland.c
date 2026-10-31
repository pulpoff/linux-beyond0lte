/* phoc's zphoc_layer_shell_effects_v1 for GTK 3 layer-shell windows (gtk-layer-shell hands out the
 * zwlr_layer_surface_v1). gtk-layer-shell destroys the layer surface when the window unmaps, and the protocol wants
 * the draggable object gone first, so the drag object follows the window's map/unmap. */
#include "neo-wayland.h"

#include <gdk/gdkwayland.h>
#include <gtk-layer-shell.h>
#include "phoc-layer-shell-effects-unstable-v1-client-protocol.h"

static struct zphoc_layer_shell_effects_v1 *effects;

static void
registry_global (void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
  if (!g_strcmp0 (iface, zphoc_layer_shell_effects_v1_interface.name) && version >= 3)
    effects = wl_registry_bind (reg, name, &zphoc_layer_shell_effects_v1_interface, MIN (version, 4));
}

static void
registry_global_remove (void *data, struct wl_registry *reg, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { registry_global, registry_global_remove };

gboolean
neo_wayland_init (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  if (!GDK_IS_WAYLAND_DISPLAY (display))
    return FALSE;
  struct wl_display *wl = gdk_wayland_display_get_wl_display (display);
  struct wl_registry *reg = wl_display_get_registry (wl);
  wl_registry_add_listener (reg, &registry_listener, NULL);
  wl_display_roundtrip (wl);
  if (!effects)
    g_warning ("no zphoc_layer_shell_effects_v1 (v3+): the drawer cannot be dragged by the compositor");
  return effects != NULL;
}

gboolean
neo_wayland_has_effects (void)
{
  return effects != NULL;
}

static struct wl_surface *
wl_surface_of (GtkWindow *win)
{
  GdkWindow *w = gtk_widget_get_window (GTK_WIDGET (win));
  return w ? gdk_wayland_window_get_wl_surface (w) : NULL;
}

/* ---- draggable ---- */

struct _NeoDrag {
  GtkWindow   *win;
  struct zphoc_draggable_layer_surface_v1 *drag;
  int          folded, unfolded;
  double       threshold;
  NeoDragMode  mode;
  guint        handle;
  gboolean     is_unfolded, moving;
  NeoDragEnd   on_end;
  NeoDragMove  on_move;
  gpointer     data;
};

static void
commit (NeoDrag *self)
{
  struct wl_surface *s = wl_surface_of (self->win);
  if (s)
    wl_surface_commit (s);
}

static void
on_dragged (void *data, struct zphoc_draggable_layer_surface_v1 *d, int32_t margin)
{
  NeoDrag *self = data;
  self->moving = TRUE;
  if (self->on_move)
    self->on_move (self->data, margin);
}

static void
on_drag_end (void *data, struct zphoc_draggable_layer_surface_v1 *d, uint32_t state)
{
  NeoDrag *self = data;
  self->moving = FALSE;
  self->is_unfolded = state == ZPHOC_DRAGGABLE_LAYER_SURFACE_V1_DRAG_END_STATE_UNFOLDED;
  if (self->on_end)
    self->on_end (self->data, self->is_unfolded);
}

static void
on_animated (void *data, struct zphoc_draggable_layer_surface_v1 *d, int32_t margin)
{
  on_dragged (data, d, margin);
}

static const struct zphoc_draggable_layer_surface_v1_listener drag_listener = { on_dragged, on_drag_end, on_animated };

static void
push_state (NeoDrag *self)
{
  if (!self->drag)
    return;
  zphoc_draggable_layer_surface_v1_set_margins (self->drag, self->folded, self->unfolded);
  zphoc_draggable_layer_surface_v1_set_exclusive (self->drag, 0);
  zphoc_draggable_layer_surface_v1_set_threshold (self->drag, wl_fixed_from_double (self->threshold));
  zphoc_draggable_layer_surface_v1_set_drag_mode (self->drag, self->mode);
  zphoc_draggable_layer_surface_v1_set_drag_handle (self->drag, self->handle);
  commit (self);
}

static void
drag_on_map (GtkWidget *w, NeoDrag *self)
{
  struct zwlr_layer_surface_v1 *ls = gtk_layer_get_zwlr_layer_surface_v1 (self->win);
  if (!ls || !effects || self->drag)
    return;
  self->drag = zphoc_layer_shell_effects_v1_get_draggable_layer_surface (effects, ls);
  zphoc_draggable_layer_surface_v1_add_listener (self->drag, &drag_listener, self);
  self->is_unfolded = FALSE;
  self->moving = FALSE;
  push_state (self);
}

static void
drag_on_unmap (GtkWidget *w, NeoDrag *self)
{
  g_clear_pointer (&self->drag, zphoc_draggable_layer_surface_v1_destroy);
  self->is_unfolded = FALSE;
  self->moving = FALSE;
}

NeoDrag *
neo_drag_new (GtkWindow *win, NeoDragEnd on_end, NeoDragMove on_move, gpointer data)
{
  NeoDrag *self = g_new0 (NeoDrag, 1);
  self->win = win;
  self->threshold = 0.3;
  self->mode = NEO_DRAG_FULL;
  self->on_end = on_end;
  self->on_move = on_move;
  self->data = data;
  /* after GTK's own handlers: the layer surface exists once the window is mapped, and still exists before unmap */
  g_signal_connect_after (win, "map", G_CALLBACK (drag_on_map), self);
  g_signal_connect (win, "unmap", G_CALLBACK (drag_on_unmap), self);
  if (gtk_widget_get_mapped (GTK_WIDGET (win)))
    drag_on_map (GTK_WIDGET (win), self);
  return self;
}

void
neo_drag_set_margins (NeoDrag *self, int folded, int unfolded)
{
  if (self->folded == folded && self->unfolded == unfolded)
    return;
  self->folded = folded;
  self->unfolded = unfolded;
  push_state (self);
}

void
neo_drag_set_threshold (NeoDrag *self, double threshold)
{
  self->threshold = threshold;
  push_state (self);
}

void
neo_drag_set_mode (NeoDrag *self, NeoDragMode mode)
{
  if (self->mode == mode)
    return;
  g_debug ("drag mode %d -> %d (unfolded %d moving %d)", self->mode, mode, self->is_unfolded, self->moving);
  self->mode = mode;
  if (self->drag) {
    zphoc_draggable_layer_surface_v1_set_drag_mode (self->drag, mode);
    commit (self);
  }
}

NeoDragMode
neo_drag_get_mode (NeoDrag *self)
{
  return self->mode;
}

void
neo_drag_set_handle (NeoDrag *self, guint handle)
{
  if (self->handle == handle)
    return;
  self->handle = handle;
  if (self->drag) {
    zphoc_draggable_layer_surface_v1_set_drag_handle (self->drag, handle);
    commit (self);
  }
}

void
neo_drag_slide (NeoDrag *self, gboolean unfolded)
{
  if (!self->drag)
    return;
  self->moving = TRUE;
  zphoc_draggable_layer_surface_v1_set_state (self->drag, unfolded ? ZPHOC_DRAGGABLE_LAYER_SURFACE_V1_DRAG_END_STATE_UNFOLDED
                                                                   : ZPHOC_DRAGGABLE_LAYER_SURFACE_V1_DRAG_END_STATE_FOLDED);
  wl_display_flush (gdk_wayland_display_get_wl_display (gdk_display_get_default ()));
}

gboolean
neo_drag_is_unfolded (NeoDrag *self)
{
  return self->is_unfolded;
}

gboolean
neo_drag_is_moving (NeoDrag *self)
{
  return self->moving;
}

/* ---- stacking ---- */

typedef struct {
  GtkWindow *win, *above;
  struct zphoc_stacked_layer_surface_v1 *stacked;
} Stack;

static void
stack_apply (Stack *st)
{
  struct zwlr_layer_surface_v1 *ls = gtk_layer_get_zwlr_layer_surface_v1 (st->win);
  struct zwlr_layer_surface_v1 *target = gtk_layer_get_zwlr_layer_surface_v1 (st->above);
  if (!ls || !target || !effects || wl_proxy_get_version ((struct wl_proxy *) effects) < 3)
    return;
  if (!st->stacked)
    st->stacked = zphoc_layer_shell_effects_v1_get_stacked_layer_surface (effects, ls);
  zphoc_stacked_layer_surface_v1_stack_below (st->stacked, target);
  struct wl_surface *s = wl_surface_of (st->win);
  if (s)
    wl_surface_commit (s);
}

static void
stack_on_map (GtkWidget *w, Stack *st)
{
  if (gtk_widget_get_mapped (GTK_WIDGET (st->win)) && gtk_widget_get_mapped (GTK_WIDGET (st->above)))
    stack_apply (st);
}

static void
stack_on_unmap (GtkWidget *w, Stack *st)
{
  if (w == GTK_WIDGET (st->win))
    g_clear_pointer (&st->stacked, zphoc_stacked_layer_surface_v1_destroy);
}

void
neo_stack_below (GtkWindow *win, GtkWindow *above)
{
  Stack *st = g_new0 (Stack, 1);
  st->win = win;
  st->above = above;
  g_signal_connect_after (win, "map", G_CALLBACK (stack_on_map), st);
  g_signal_connect_after (above, "map", G_CALLBACK (stack_on_map), st);
  g_signal_connect (win, "unmap", G_CALLBACK (stack_on_unmap), st);
  stack_on_map (NULL, st);
}
