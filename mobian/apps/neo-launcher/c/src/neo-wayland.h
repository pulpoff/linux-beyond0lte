/* phoc's layer-shell effects for Neo's GTK windows: a draggable surface that phoc itself drags and animates (the
 * way it moves Phosh's notification shade, with no client drawing per frame), and stacking of one layer surface
 * below another. */
#pragma once

#include <gtk/gtk.h>

typedef struct _NeoDrag NeoDrag;

typedef void (*NeoDragEnd)  (gpointer data, gboolean unfolded);
typedef void (*NeoDragMove) (gpointer data, int margin);

typedef enum {
  NEO_DRAG_FULL = 0,
  NEO_DRAG_HANDLE = 1,
  NEO_DRAG_NONE = 2,
} NeoDragMode;

gboolean  neo_wayland_init          (void);   /* FALSE: no phoc effects (not phoc, or too old) */
gboolean  neo_wayland_has_effects   (void);

/* `win` is a gtk-layer-shell window anchored to 3 edges; the drag object lives while the window is mapped */
NeoDrag  *neo_drag_new              (GtkWindow *win, NeoDragEnd on_end, NeoDragMove on_move, gpointer data);
void      neo_drag_set_margins      (NeoDrag *self, int folded, int unfolded);
void      neo_drag_set_threshold    (NeoDrag *self, double threshold);
void      neo_drag_set_mode         (NeoDrag *self, NeoDragMode mode);
NeoDragMode neo_drag_get_mode       (NeoDrag *self);
void      neo_drag_set_handle       (NeoDrag *self, guint handle);
void      neo_drag_slide            (NeoDrag *self, gboolean unfolded);   /* phoc animates it there */
gboolean  neo_drag_is_unfolded      (NeoDrag *self);
gboolean  neo_drag_is_moving        (NeoDrag *self);   /* dragged or animating since the last end */

/* keep `win` stacked below `above` (both layer windows in the same layer, both mapped) */
void      neo_stack_below           (GtkWindow *win, GtkWindow *above);
