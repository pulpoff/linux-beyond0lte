/* Launcher icons: the theme's icon, an icon pack's art, or the app icon wrapped/shaped the way Neo does. */
#pragma once

#include <gtk/gtk.h>

typedef struct _NeoIcons NeoIcons;

typedef struct {
  char *id, *title, *dir;
} NeoPack;

NeoIcons  *neo_icons_new        (GSettings *settings);
void       neo_icons_free       (NeoIcons *self);
/* installed packs, a user's pack winning over a system one of the same name; g_ptr_array_unref frees them */
GPtrArray *neo_icons_list_packs (void);
/* The icon for `info` at `px` device pixels, ready to show: a pack/shape icon from the cache, or the stock icon.
 * *pending is set when a custom icon still has to be drawn (neo_icons_render). */
GdkPixbuf *neo_icons_lookup     (NeoIcons *self, GAppInfo *info, int px, gboolean *pending);
/* Draws the custom icon (up to ~100 ms of cairo); NULL when the stock icon stays. */
GdkPixbuf *neo_icons_render     (NeoIcons *self, GAppInfo *info, int px);
/* the app's stock icon at px, never NULL */
GdkPixbuf *neo_icons_stock      (GAppInfo *info, int px);
void       neo_icons_reset      (NeoIcons *self);   /* the settings changed: read the pack again */
