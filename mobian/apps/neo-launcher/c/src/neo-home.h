/* Neo's home screen and app drawer. */
#pragma once

#include <gtk/gtk.h>

typedef struct _NeoHome NeoHome;

NeoHome  *neo_home_new        (GtkApplication *app, GSettings *settings);
void      neo_home_show       (NeoHome *self);        /* Phosh's home gesture: first page, drawer closed */
void      neo_home_hide       (NeoHome *self);
void      neo_home_launch_card (NeoHome *self, GAppInfo *info, GtkWidget *from);   /* from: the tapped cell, or NULL */
void      neo_home_launch_end (NeoHome *self);       /* the app is up: Neo goes once the card has grown */
gboolean  neo_home_visible    (NeoHome *self);
gboolean  neo_home_back       (NeoHome *self);        /* the back gesture: TRUE when Neo handled it */
void      neo_home_set_visible_cb (NeoHome *self, void (*cb) (gpointer data, gboolean visible), gpointer data);
GdkPixbuf *neo_home_app_icon  (NeoHome *self, GAppInfo *info, int px);   /* the launcher's icon for an app */
GdkPixbuf *neo_home_wallpaper (NeoHome *self, int w, int h);
int       neo_home_scale      (NeoHome *self);
int       neo_home_max_columns (NeoHome *self);           /* what fits this screen, for the settings */
int       neo_home_max_rows   (NeoHome *self);
