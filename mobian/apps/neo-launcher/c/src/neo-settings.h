/* Launcher settings (long press on the home screen › Launcher settings): an ordinary app window, so Phosh shows it
 * like any other app while Neo steps aside. Everything is de.yesman.neo; Neo rebuilds itself on every change. */
#pragma once

#include <gtk/gtk.h>

typedef struct _NeoHome NeoHome;

void neo_settings_show (GtkApplication *app, GSettings *settings, NeoHome *home);
