/* A parallax wallpaper: transparent layers over a background that shift by their depth as the phone tilts. */
#pragma once

#include <gtk/gtk.h>

typedef struct _NeoParallax NeoParallax;

/* `dir` holds parallax.json ({"range": px, "layers": [{"file", "depth"}…]}, back to front) and the layer pictures */
NeoParallax *neo_parallax_new    (GtkWidget *wall_win, const char *dir);
void         neo_parallax_free   (NeoParallax *self);
void         neo_parallax_resize (NeoParallax *self, int w, int h, int scale);   /* the wallpaper window's size */
void         neo_parallax_run    (NeoParallax *self, gboolean run);              /* follow the tilt while shown */
