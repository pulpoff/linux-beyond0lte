/* The apps a launcher shows: visible .desktop entries, by id, without the Android apps that come with Waydroid's
 * image (GNOME has its own phone, contacts, messages, clock, files: these expect Android's telephony and home).
 * Play stays, and what it installs, and Android's settings under a name that tells it from GNOME's. */
#include "neo-apps.h"

static const char *android_builtin[] = {
  "com.android.calculator2", "com.android.deskclock", "com.android.documentsui", "com.android.gallery3d",
  "com.android.inputmethod.latin", "com.google.android.apps.messaging", "com.google.android.contacts",
  "com.google.android.dialer", "org.lineageos.aperture", "org.lineageos.eleven", "org.lineageos.etar",
  "org.lineageos.jelly", "org.lineageos.recorder", "org.lineageos.updater", NULL,
};

#define ANDROID_SETTINGS "waydroid.com.android.settings.desktop"

static gboolean
is_android_builtin (const char *id)
{
  if (!g_str_has_prefix (id, "waydroid.") || !g_str_has_suffix (id, ".desktop"))
    return FALSE;
  g_autofree char *pkg = g_strndup (id + strlen ("waydroid."), strlen (id) - strlen ("waydroid.") - strlen (".desktop"));
  return g_strv_contains (android_builtin, pkg);
}

GHashTable *
neo_apps_installed (void)
{
  GHashTable *apps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  GList *all = g_app_info_get_all ();

  for (GList *l = all; l; l = l->next) {
    GAppInfo *info = l->data;
    const char *id = g_app_info_get_id (info);
    if (!id)
      continue;
    /* Waydroid writes its Settings entry NoDisplay; Neo shows it as Android Settings */
    if (!g_app_info_should_show (info) && g_strcmp0 (id, ANDROID_SETTINGS))
      continue;
    if (is_android_builtin (id) || g_str_has_prefix (id, "de.yesman.NeoLauncher"))
      continue;
    /* a phone shows phone apps: nothing that runs in a terminal (htop, nvtop), and not Waydroid's whole-screen
     * Android session (its apps come one by one, as windows of their own) */
    if ((G_IS_DESKTOP_APP_INFO (info) && g_desktop_app_info_get_boolean (G_DESKTOP_APP_INFO (info), "Terminal")) ||
        !g_strcmp0 (id, "Waydroid.desktop"))
      continue;
    g_hash_table_replace (apps, g_strdup (id), g_object_ref (info));
  }
  g_list_free_full (all, g_object_unref);
  return apps;
}

const char *
neo_app_name (GAppInfo *info)
{
  if (!info)
    return "";
  if (!g_strcmp0 (g_app_info_get_id (info), ANDROID_SETTINGS))
    return "Android Settings";
  return g_app_info_get_display_name (info) ?: "";
}

GAppInfo *
neo_app_for_app_id (const char *app_id)
{
  if (!app_id || !*app_id)
    return NULL;
  g_autofree char *a = g_strconcat (app_id, ".desktop", NULL);
  g_autofree char *lower = g_ascii_strdown (a, -1);
  GDesktopAppInfo *info = g_desktop_app_info_new (a);
  if (!info)
    info = g_desktop_app_info_new (lower);
  return info ? G_APP_INFO (info) : NULL;
}

gint
neo_app_compare (gconstpointer a, gconstpointer b)
{
  g_autofree char *ka = g_utf8_collate_key (neo_app_name ((GAppInfo *) a), -1);
  g_autofree char *kb = g_utf8_collate_key (neo_app_name ((GAppInfo *) b), -1);
  return strcmp (ka, kb);
}
