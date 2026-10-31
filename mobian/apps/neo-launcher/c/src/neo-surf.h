/* A picture the compositor moves: a Wayland subsurface of a GTK window, holding a buffer drawn once. Placing it
 * (position, size, opacity) only changes the subsurface's position, its wp_viewport destination and its
 * wp_alpha_modifier multiplier: phoc samples the buffer there, nothing is drawn per frame. That is how Neo's task
 * view animates as smoothly as Phosh's shade. */
#pragma once

#include <gtk/gtk.h>
#include <cairo.h>

typedef struct _NeoSurf NeoSurf;

gboolean  neo_surf_init          (void);        /* FALSE: the compositor lacks subsurfaces, viewporter or shm */

NeoSurf  *neo_surf_new           (void);
void      neo_surf_free          (NeoSurf *self);
/* the picture: an ARGB32 (premultiplied) cairo image surface, copied; or a solid colour */
void      neo_surf_set_image     (NeoSurf *self, cairo_surface_t *image);
void      neo_surf_set_bgra      (NeoSurf *self, const guint8 *data, int w, int h, int stride);
void      neo_surf_set_solid     (NeoSurf *self, double r, double g, double b);
/* a canvas: a buffer kept mapped, cairo draws straight into it (the surface returned is the NeoSurf's; it changes
 * when the size does, or when the compositor still holds the buffer). Draw, then say which px changed. */
cairo_surface_t *neo_surf_canvas (NeoSurf *self, int w, int h);
void      neo_surf_canvas_changed (NeoSurf *self, int x, int y, int w, int h);
int       neo_surf_width         (NeoSurf *self);   /* the buffer's size, px */
int       neo_surf_height        (NeoSurf *self);

/* gives it the subsurface role on `parent` (GTK 3 makes a new wl_surface at every map), and takes it away */
void      neo_surf_attach        (NeoSurf *self, GtkWidget *parent_window);
void      neo_surf_detach        (NeoSurf *self);
gboolean  neo_surf_attached      (NeoSurf *self);
/* keep it just above `below` (NULL: right above the parent) */
void      neo_surf_place_above   (NeoSurf *self, NeoSurf *below);

/* where it shows, in the parent's logical px (sub-pixel positions round), and how opaque; hidden at alpha 0 */
void      neo_surf_place         (NeoSurf *self, double x, double y, double w, double h, double alpha);
/* a source crop in buffer px (the whole buffer if w <= 0) */
void      neo_surf_crop          (NeoSurf *self, double x, double y, double w, double h);

/* Positions apply on the parent's commit: call after placing a frame's worth. `done` runs once the compositor
 * has shown it (NULL: whatever callback is already asked for stays). */
void      neo_surf_commit_parent (GtkWidget *parent_window, void (*done) (gpointer data), gpointer data);
void      neo_surf_cancel_frame  (GtkWidget *parent_window);
