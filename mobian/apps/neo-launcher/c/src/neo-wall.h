/* The wallpaper (org.gnome.desktop.background) as a picture of the screen's own pixels. */
#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>

/* the wallpaper cropped (cover) and scaled to w x h device pixels, cached per shape; NULL when there is none */
GdkPixbuf *neo_wall_get (int w, int h);
void       neo_wall_watch (void (*changed) (gpointer data), gpointer data);
