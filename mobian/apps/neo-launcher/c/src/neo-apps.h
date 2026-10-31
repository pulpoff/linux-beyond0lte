/* The apps a launcher shows and the names it shows them by. */
#pragma once

#include <gio/gdesktopappinfo.h>

/* desktop id -> GDesktopAppInfo (owned), the visible .desktop entries */
GHashTable  *neo_apps_installed  (void);
const char  *neo_app_name        (GAppInfo *info);
/* "<id>.desktop" or its lower-case variant for an app id from Phosh (a window's app id), or NULL */
GAppInfo    *neo_app_for_app_id  (const char *app_id);
gint         neo_app_compare     (gconstpointer a, gconstpointer b);   /* two GAppInfo* by name */
