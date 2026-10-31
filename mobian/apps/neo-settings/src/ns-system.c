/* Language and region, Date and time, Users, Power, Wallpapers, Software information and Display's screen scale.
 * The system services (AccountsService, localed, timedated, the compositor's DisplayConfig) are asked in the
 * background; what needs the administrator's password asks through polkit, which Phosh shows. */
#include "ns-bt.h"
#include "ns-pages.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>
#include <sys/utsname.h>
#include <unistd.h>

static GDBusConnection *
bus (GBusType t)
{
  return g_bus_get_sync (t, NULL, NULL);
}

/* ---- Language and region ---- */

typedef struct {
  GHashTable *langs, *countries;     /* "de" → "German", "DE" → "Germany" */
} Names;

static GHashTable *
iso_table (const char *file, const char *list, const char *key)
{
  GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr (JsonParser) p = json_parser_new ();
  if (!json_parser_load_from_file (p, file, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p)))
    return t;
  JsonObject *o = json_node_get_object (json_parser_get_root (p));
  JsonArray *a = json_object_has_member (o, list) ? json_object_get_array_member (o, list) : NULL;
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *e = json_array_get_object_element (a, i);
    const char *k = json_object_get_string_member_with_default (e, key, NULL);
    const char *k3 = json_object_get_string_member_with_default (e, "alpha_3", NULL);
    const char *n = json_object_get_string_member_with_default (e, "common_name", NULL);
    if (!n)
      n = json_object_get_string_member_with_default (e, "name", NULL);
    if (n && k)
      g_hash_table_insert (t, g_strdup (k), g_strdup (n));
    if (n && k3 && !g_hash_table_contains (t, k3))
      g_hash_table_insert (t, g_strdup (k3), g_strdup (n));
  }
  return t;
}

/* "de_DE.UTF-8" → "German (Germany)" */
static char *
locale_name (Names *n, const char *locale)
{
  g_autofree char *l = g_strdup (locale);
  char *cut = strpbrk (l, ".@");
  if (cut)
    *cut = 0;
  g_auto (GStrv) parts = g_strsplit (l, "_", 2);
  const char *lang = g_hash_table_lookup (n->langs, parts[0]);
  const char *country = parts[1] ? g_hash_table_lookup (n->countries, parts[1]) : NULL;
  if (!lang)
    return g_strdup (locale);
  return country ? g_strdup_printf ("%s (%s)", lang, country) : g_strdup (lang);
}

typedef struct {
  GPtrArray *locales, *labels;
  char      *path;                   /* the user in AccountsService */
  GtkWidget *group, *logout;
} Lang;

static void
lang_free (Lang *l)
{
  if (l->locales)
    g_ptr_array_unref (l->locales);
  if (l->labels)
    g_ptr_array_unref (l->labels);
  g_free (l->path);
  g_free (l);
}

static void
lang_picked (GtkWidget *row, guint i, gpointer data)
{
  Lang *l = data;
  const char *loc = l->locales->pdata[i];
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (!sys)
    return;
  if (l->path)
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", l->path, "org.freedesktop.Accounts.User", "SetLanguage",
                            g_variant_new ("(s)", loc), NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                            NULL, NULL, NULL);
  /* the system's too, as Mobile Settings does: the lock screen and the console follow */
  g_autofree char *lang = g_strconcat ("LANG=", loc, NULL);
  const char *env[] = { lang, NULL };
  g_dbus_connection_call (sys, "org.freedesktop.locale1", "/org/freedesktop/locale1", "org.freedesktop.locale1",
                          "SetLocale", g_variant_new ("(^asb)", env, TRUE), NULL,
                          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 60000, NULL, NULL, NULL);
  gtk_widget_set_visible (l->logout, TRUE);
}

static void
region_picked (GtkWidget *row, guint i, gpointer data)
{
  Lang *l = data;
  GSettings *s = g_object_get_data (G_OBJECT (row), "ns-locale");
  g_settings_set_string (s, "region", i == 0 ? "" : (const char *) l->locales->pdata[i - 1]);
  gtk_widget_set_visible (l->logout, TRUE);
}

static void
logout (GtkWidget *row, gpointer data)
{
  g_autoptr (GDBusConnection) ses = bus (G_BUS_TYPE_SESSION);
  if (ses)
    g_dbus_connection_call (ses, "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager",
                            "Logout", g_variant_new ("(u)", 0), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
lang_rows (AdwNavigationPage *page, const char *current)
{
  Lang *l = g_object_get_data (G_OBJECT (page), "ns-lang");
  if (!l->locales)
    return;
  g_autofree char *cur_lang = g_strdup (current && *current ? current : g_getenv ("LANG"));
  guint sel = G_MAXUINT;
  for (guint i = 0; i < l->locales->len; i++)
    if (cur_lang && !g_strcmp0 (l->locales->pdata[i], cur_lang))
      sel = i;
  ns_group_clear (l->group);
  GtkWidget *r = ns_row_pick (l->group, "Language", (const char *const *) l->labels->pdata, sel, lang_picked, l);
  /* "C.UTF-8" and the like: no language chosen yet, programs speak their own (English) */
  if (sel == G_MAXUINT)
    ns_row_set_subtitle (r, !cur_lang || g_str_has_prefix (cur_lang, "C.") || !g_strcmp0 (cur_lang, "C")
                            || !g_strcmp0 (cur_lang, "POSIX") ? "System default (English)" : cur_lang);
  GSettings *s = ns_page_settings (page, "org.gnome.system.locale");
  if (s) {
    g_autoptr (GPtrArray) rl = g_ptr_array_new ();
    g_ptr_array_add (rl, (gpointer) "Same as the language");
    for (guint i = 0; i + 1 < l->labels->len; i++)
      g_ptr_array_add (rl, l->labels->pdata[i]);
    g_ptr_array_add (rl, NULL);
    g_autofree char *region = g_settings_get_string (s, "region");
    guint rs = 0;
    for (guint i = 0; i < l->locales->len; i++)
      if (!g_strcmp0 (l->locales->pdata[i], region))
        rs = i + 1;
    r = ns_row_pick (l->group, "Formats", (const char *const *) rl->pdata, rs, region_picked, l);
    g_object_set_data (G_OBJECT (r), "ns-locale", s);
  }
}

static void
user_lang_got (GObject *src, GAsyncResult *res, gpointer page)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) lang = NULL;
  if (v)
    g_variant_get (v, "(v)", &lang);
  lang_rows (page, lang ? g_variant_get_string (lang, NULL) : NULL);
  g_object_unref (page);
}

static void
user_found (GObject *src, GAsyncResult *res, gpointer page)
{
  Lang *l = g_object_get_data (page, "ns-lang");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (!v) {
    lang_rows (page, NULL);
    g_object_unref (page);
    return;
  }
  g_variant_get (v, "(o)", &l->path);
  g_dbus_connection_call (G_DBUS_CONNECTION (src), "org.freedesktop.Accounts", l->path,
                          "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new ("(ss)", "org.freedesktop.Accounts.User", "Language"), G_VARIANT_TYPE ("(v)"),
                          G_DBUS_CALL_FLAGS_NONE, 3000, NULL, user_lang_got, page);
}

static void
locales_listed (GObject *src, GAsyncResult *res, gpointer page)
{
  Lang *l = g_object_get_data (page, "ns-lang");
  g_autofree char *out = NULL;
  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL);
  Names n = { iso_table ("/usr/share/iso-codes/json/iso_639-2.json", "639-2", "alpha_2"),
              iso_table ("/usr/share/iso-codes/json/iso_3166-1.json", "3166-1", "alpha_2") };
  l->locales = g_ptr_array_new_with_free_func (g_free);
  l->labels = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) lines = g_strsplit (out ? out : "", "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    const char *loc = g_strstrip (lines[i]);
    /* the UTF-8 ones that name a language */
    if (!*loc || !strstr (loc, "UTF-8") || g_str_has_prefix (loc, "C."))
      continue;
    g_ptr_array_add (l->locales, g_strdup (loc));
    g_ptr_array_add (l->labels, locale_name (&n, loc));
  }
  g_ptr_array_add (l->labels, NULL);
  g_hash_table_unref (n.langs);
  g_hash_table_unref (n.countries);
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys)
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", "/org/freedesktop/Accounts", "org.freedesktop.Accounts",
                            "FindUserById", g_variant_new ("(x)", (gint64) getuid ()), G_VARIANT_TYPE ("(o)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, user_found, page);
  else
    g_object_unref (page);
}

void
ns_page_language (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Language and region", &c);
  Lang *l = g_new0 (Lang, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-lang", l, (GDestroyNotify) lang_free);
  l->group = ns_group (c, NULL);
  ns_row_info (l->group, "Language", "…");
  ns_note (c, "A new language or format shows in apps after you log out and in again.");
  GtkWidget *g = ns_group (c, NULL);
  l->logout = ns_row_button (g, "Log out now", FALSE, logout, NULL);
  gtk_widget_set_visible (l->logout, FALSE);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "localectl", "list-locales", "--no-pager", NULL);
  if (p)
    g_subprocess_communicate_utf8_async (p, NULL, NULL, locales_listed, g_object_ref (page));
  ns_push (page);
}

/* ---- Date and time ---- */

typedef struct {
  GtkWidget *ntp, *zone;
  GPtrArray *zones;
} Time;

static void
time_free (Time *t)
{
  if (t->zones)
    g_ptr_array_unref (t->zones);
  g_free (t);
}

static void
timedate_call (const char *method, GVariant *args)
{
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys)
    g_dbus_connection_call (sys, "org.freedesktop.timedate1", "/org/freedesktop/timedate1",
                            "org.freedesktop.timedate1", method, args, NULL,
                            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 60000, NULL, NULL, NULL);
}

static void
ntp_set (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  if (!ns_switch_syncing (GTK_WIDGET (sw)))
    timedate_call ("SetNTP", g_variant_new ("(bb)", gtk_switch_get_active (sw), TRUE));
}

static void
zone_set (GtkWidget *row, gpointer zone)
{
  AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Time *t = g_object_get_data (G_OBJECT (page), "ns-time");
  timedate_call ("SetTimezone", g_variant_new ("(sb)", (const char *) zone, TRUE));
  g_autofree char *nice = g_strdelimit (g_strdup (zone), "_", ' ');
  ns_row_set_subtitle (t->zone, nice);
  AdwNavigationView *nav = ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW));
  adw_navigation_view_pop_to_page (nav, page);
}

/* two steps: the region, then its cities */
static void
zone_region (GtkWidget *row, gpointer region)
{
  AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Time *t = g_object_get_data (G_OBJECT (page), "ns-time");
  GtkWidget *c;
  AdwNavigationPage *cities = ns_page_new (region, &c);
  GtkWidget *g = ns_group (c, NULL);
  g_autofree char *prefix = g_strconcat (region, "/", NULL);
  for (guint i = 0; i < t->zones->len; i++) {
    const char *z = t->zones->pdata[i];
    if (!g_str_has_prefix (z, prefix))
      continue;
    g_autofree char *nice = g_strdelimit (g_strdup (z + strlen (prefix)), "_/", ' ');
    GtkWidget *r = ns_row (g, NULL, NULL, nice, NULL);
    g_object_set_data (G_OBJECT (r), "ns-page", page);
    ns_row_on_activate (r, zone_set, (gpointer) z);
  }
  ns_push (cities);
}

static void
zones_page (GtkWidget *row, gpointer page)
{
  Time *t = g_object_get_data (G_OBJECT (page), "ns-time");
  if (!t->zones)
    return;
  GtkWidget *c;
  AdwNavigationPage *regions = ns_page_new ("Time zone", &c);
  GtkWidget *g = ns_group (c, NULL);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < t->zones->len; i++) {
    const char *z = t->zones->pdata[i];
    const char *slash = strchr (z, '/');
    if (!slash)
      continue;
    char *region = g_strndup (z, slash - z);
    if (g_hash_table_contains (seen, region) || !g_strcmp0 (region, "Etc")) {
      g_free (region);
      continue;
    }
    g_hash_table_add (seen, region);
    GtkWidget *r = ns_row (g, NULL, NULL, region, NULL);
    g_object_set_data (G_OBJECT (r), "ns-page", page);
    ns_row_on_activate (r, zone_region, region);
  }
  g_object_set_data_full (G_OBJECT (regions), "ns-seen", g_hash_table_ref (seen), (GDestroyNotify) g_hash_table_unref);
  ns_push (regions);
}

static void
zones_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Time *t = g_object_get_data (page, "ns-time");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (v) {
    g_autofree const char **z = NULL;
    g_variant_get (v, "(^a&s)", &z);
    t->zones = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; z[i]; i++)
      g_ptr_array_add (t->zones, g_strdup (z[i]));
  }
  g_object_unref (page);
}

static void
timedate_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Time *t = g_object_get_data (page, "ns-time");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  if (v) {
    g_variant_get (v, "(@a{sv})", &props);
    gboolean ntp = FALSE;
    const char *zone = NULL;
    g_variant_lookup (props, "NTP", "b", &ntp);
    g_variant_lookup (props, "Timezone", "&s", &zone);
    ns_switch_quiet (ns_row_get_switch (t->ntp), ntp);
    g_autofree char *nice = zone ? g_strdelimit (g_strdup (zone), "_", ' ') : NULL;
    ns_row_set_subtitle (t->zone, nice);
  }
  g_object_unref (page);
}

static gboolean
h24_get (GValue *value, GVariant *v, gpointer data)
{
  g_value_set_boolean (value, !g_strcmp0 (g_variant_get_string (v, NULL), "24h"));
  return TRUE;
}

static GVariant *
h24_set (const GValue *value, const GVariantType *type, gpointer data)
{
  return g_variant_new_string (g_value_get_boolean (value) ? "24h" : "12h");
}

void
ns_page_datetime (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Date and time", &c);
  Time *t = g_new0 (Time, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-time", t, (GDestroyNotify) time_free);
  GtkWidget *g = ns_group (c, NULL);
  t->ntp = ns_row_switch (g, "Automatic date and time", "From the network", NULL, NULL);
  g_signal_connect (ns_row_get_switch (t->ntp), "notify::active", G_CALLBACK (ntp_set), NULL);
  GSettings *dt = ns_page_settings (page, "org.gnome.desktop.datetime");
  if (ns_has_key (dt, "automatic-timezone"))
    ns_row_switch (g, "Automatic time zone", "From the phone's location", dt, "automatic-timezone");
  t->zone = ns_row (g, NULL, NULL, "Time zone", NULL);
  ns_row_on_activate (t->zone, zones_page, page);

  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");
  g = ns_group (c, "Clock");
  GtkWidget *r = ns_row_switch (g, "24-hour time", "13:00 rather than 1:00 PM", NULL, NULL);
  g_settings_bind_with_mapping (iface, "clock-format", ns_row_get_switch (r), "active", G_SETTINGS_BIND_DEFAULT,
                                h24_get, h24_set, NULL, NULL);
  ns_row_switch (g, "Show the weekday", "In the status bar", iface, "clock-show-weekday");
  ns_row_switch (g, "Show the date", "In the status bar", iface, "clock-show-date");
  ns_row_switch (g, "Show seconds", NULL, iface, "clock-show-seconds");

  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys) {
    g_dbus_connection_call (sys, "org.freedesktop.timedate1", "/org/freedesktop/timedate1",
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", "org.freedesktop.timedate1"), G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, timedate_got, g_object_ref (page));
    g_dbus_connection_call (sys, "org.freedesktop.timedate1", "/org/freedesktop/timedate1",
                            "org.freedesktop.timedate1", "ListTimezones", NULL, G_VARIANT_TYPE ("(as)"),
                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, zones_got, g_object_ref (page));
  }
  ns_push (page);
}

/* ---- Users ---- */

static void
real_name_set (const char *text, gpointer row)
{
  const char *path = g_object_get_data (G_OBJECT (row), "ns-path");
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (text && sys && path) {
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", path, "org.freedesktop.Accounts.User", "SetRealName",
                            g_variant_new ("(s)", text), NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                            NULL, NULL, NULL);
    ns_row_set_subtitle (row, text);
  }
  g_object_unref (row);
}

static void
real_name_tapped (GtkWidget *row, gpointer data)
{
  ns_ask (row, "Your name", NULL, g_object_get_data (G_OBJECT (row), "ns-name"), FALSE, "Save", real_name_set,
          g_object_ref (row));
}

typedef struct {
  GtkWidget *name, *login, *type, *others;
} Users;

static void
other_got (GObject *src, GAsyncResult *res, gpointer group)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  if (v) {
    g_variant_get (v, "(@a{sv})", &props);
    const char *real = "", *login = "";
    g_variant_lookup (props, "RealName", "&s", &real);
    g_variant_lookup (props, "UserName", "&s", &login);
    ns_row (group, "avatar-default-symbolic", "grey", *real ? real : login, login);
    ns_group_set_visible (group, TRUE);
  }
  g_object_unref (group);
}

static void
me_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Users *u = g_object_get_data (page, "ns-users");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  if (v) {
    g_variant_get (v, "(@a{sv})", &props);
    const char *real = "", *login = "";
    gint32 type = 0;
    g_variant_lookup (props, "RealName", "&s", &real);
    g_variant_lookup (props, "UserName", "&s", &login);
    g_variant_lookup (props, "AccountType", "i", &type);
    ns_row_set_subtitle (u->name, *real ? real : "Not set");
    g_object_set_data_full (G_OBJECT (u->name), "ns-name", g_strdup (real), g_free);
    ns_row_set_subtitle (u->login, login);
    ns_row_set_subtitle (u->type, type == 1 ? "Administrator" : "Standard");
  }
  g_object_unref (page);
}

static void
users_listed (GObject *src, GAsyncResult *res, gpointer page)
{
  Users *u = g_object_get_data (page, "ns-users");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  const char *me = g_object_get_data (G_OBJECT (u->name), "ns-path");
  if (v) {
    g_autofree const char **paths = NULL;
    g_variant_get (v, "(^a&o)", &paths);
    for (guint i = 0; paths[i]; i++)
      if (g_strcmp0 (paths[i], me))
        g_dbus_connection_call (G_DBUS_CONNECTION (src), "org.freedesktop.Accounts", paths[i],
                                "org.freedesktop.DBus.Properties", "GetAll",
                                g_variant_new ("(s)", "org.freedesktop.Accounts.User"), G_VARIANT_TYPE ("(a{sv})"),
                                G_DBUS_CALL_FLAGS_NONE, 3000, NULL, other_got, g_object_ref (u->others));
  }
  g_object_unref (page);
}

static void
me_found (GObject *src, GAsyncResult *res, gpointer page)
{
  Users *u = g_object_get_data (page, "ns-users");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (v) {
    const char *path;
    g_variant_get (v, "(&o)", &path);
    g_object_set_data_full (G_OBJECT (u->name), "ns-path", g_strdup (path), g_free);
    g_dbus_connection_call (G_DBUS_CONNECTION (src), "org.freedesktop.Accounts", path,
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", "org.freedesktop.Accounts.User"), G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, me_got, g_object_ref (page));
    g_dbus_connection_call (G_DBUS_CONNECTION (src), "org.freedesktop.Accounts", "/org/freedesktop/Accounts",
                            "org.freedesktop.Accounts", "ListCachedUsers", NULL, G_VARIANT_TYPE ("(ao)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, users_listed, g_object_ref (page));
  }
  g_object_unref (page);
}

void
ns_page_users (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Users", &c);
  Users *u = g_new0 (Users, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-users", u, g_free);
  GtkWidget *g = ns_group (c, "Your account");
  u->name = ns_row (g, NULL, NULL, "Name", NULL);
  ns_row_on_activate (u->name, real_name_tapped, NULL);
  u->login = ns_row_info (g, "User name", g_get_user_name ());
  u->type = ns_row_info (g, "Account type", NULL);
  g = ns_group (c, NULL);
  GtkWidget *r = ns_row (g, NULL, NULL, "Screen lock and password", "A pattern or PIN is also your password");
  ns_row_on_activate (r, ns_page_lock_screen, NULL);
  u->others = ns_group (c, "Other users");
  ns_group_set_visible (u->others, FALSE);
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys)
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", "/org/freedesktop/Accounts", "org.freedesktop.Accounts",
                            "FindUserById", g_variant_new ("(x)", (gint64) getuid ()), G_VARIANT_TYPE ("(o)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, me_found, g_object_ref (page));
  ns_push (page);
}

/* ---- Power ---- */

static const int sleeps[] = { 0, 300, 600, 900, 1800, 3600 };
static const char *const sleep_labels[] = { "Never", "After 5 minutes", "After 10 minutes", "After 15 minutes",
                                            "After 30 minutes", "After 1 hour", NULL };

static void
sleep_picked (GtkWidget *row, guint i, gpointer power)
{
  const char *what = g_object_get_data (G_OBJECT (row), "ns-what");
  g_autofree char *type = g_strconcat ("sleep-inactive-", what, "-type", NULL);
  g_autofree char *timeout = g_strconcat ("sleep-inactive-", what, "-timeout", NULL);
  g_settings_set_string (power, type, i ? "suspend" : "nothing");
  if (i)
    g_settings_set_int (power, timeout, sleeps[i]);
}

static void
sleep_row (GtkWidget *g, GSettings *power, const char *title, const char *what)
{
  g_autofree char *type = g_strconcat ("sleep-inactive-", what, "-type", NULL);
  g_autofree char *timeout = g_strconcat ("sleep-inactive-", what, "-timeout", NULL);
  g_autofree char *t = g_settings_get_string (power, type);
  int s = g_settings_get_int (power, timeout);
  guint sel = 0;
  for (guint i = 1; g_strcmp0 (t, "nothing") && i < G_N_ELEMENTS (sleeps); i++)
    if (sleeps[i] <= s)
      sel = i;
  GtkWidget *r = ns_row_pick (g, title, sleep_labels, sel, sleep_picked, power);
  g_object_set_data (G_OBJECT (r), "ns-what", (gpointer) what);
}


void
ns_page_power (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Power and screen blank", &c);
  GSettings *power = ns_page_settings (page, "org.gnome.settings-daemon.plugins.power");
  if (!power)
    return;
  GtkWidget *g = ns_group (c, "Automatic suspend");
  sleep_row (g, power, "On battery", "battery");
  sleep_row (g, power, "Plugged in", "ac");
  ns_note (c, "Suspend saves the most power, but calls, messages and alarms can be missed while the phone sleeps.");
  g = ns_group (c, NULL);
  /* no "Power button": Phosh keeps the power key to itself, gnome-settings-daemon's action never runs */
  if (ns_has_key (power, "idle-dim"))
    ns_row_switch (g, "Dim the screen", "Before it turns off", power, "idle-dim");
  /* only with power-profiles-daemon, which switches the profile */
  if (ns_has_key (power, "power-saver-profile-on-low-battery") &&
      (g_file_test ("/usr/share/dbus-1/system-services/org.freedesktop.UPower.PowerProfiles.service",
                    G_FILE_TEST_EXISTS) ||
       g_file_test ("/usr/share/dbus-1/system-services/net.hadess.PowerProfiles.service", G_FILE_TEST_EXISTS)))
    ns_row_switch (g, "Power saving on low battery", NULL, power, "power-saver-profile-on-low-battery");
  ns_push (page);
}

/* ---- Wallpapers ---- */

#define WALL_W 150
#define WALL_H 300

typedef struct {
  GtkWidget *pic;
  char      *path;
  GdkTexture *tex;
} Thumb;

static char *
wall_cache (const char *path)
{
  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_MD5, path, -1);
  g_autofree char *name = g_strconcat ("wall-", sum, ".png", NULL);
  return g_build_filename (g_get_user_cache_dir (), "neo-settings", name, NULL);
}

static gboolean
thumb_show (gpointer data)
{
  Thumb *t = data;
  if (t->tex)
    gtk_picture_set_paintable (GTK_PICTURE (t->pic), GDK_PAINTABLE (t->tex));
  g_clear_object (&t->tex);
  g_object_unref (t->pic);
  g_free (t->path);
  g_free (t);
  return G_SOURCE_REMOVE;
}

/* one after the other in one thread: big pictures decoded side by side would want a lot of memory */
static void
thumbs_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  GPtrArray *thumbs = data;
  for (guint i = 0; i < thumbs->len && !g_cancellable_is_cancelled (cancel); i++) {
    Thumb *t = thumbs->pdata[i];
    thumbs->pdata[i] = NULL;
    g_autofree char *cache = wall_cache (t->path);
    g_autoptr (GdkPixbuf) pb = gdk_pixbuf_new_from_file (cache, NULL);
    if (!pb) {
      pb = gdk_pixbuf_new_from_file_at_scale (t->path, WALL_W * 2, WALL_H, TRUE, NULL);
      if (pb) {
        g_autofree char *dir = g_path_get_dirname (cache);
        g_mkdir_with_parents (dir, 0700);
        gdk_pixbuf_save (pb, cache, "png", NULL, NULL);
      }
    }
    if (pb)
      t->tex = ns_texture_for_pixbuf (pb);
    g_idle_add (thumb_show, t);
  }
  for (guint i = 0; i < thumbs->len; i++)
    if (thumbs->pdata[i])
      g_idle_add (thumb_show, thumbs->pdata[i]);
  g_task_return_boolean (task, TRUE);
}

static void
wall_apply (GtkWidget *w, const char *path, const char *dark, int where)
{
  g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
  g_autofree char *duri = dark ? g_filename_to_uri (dark, NULL, NULL) : g_strdup (uri);
  if (where != 1) {
    g_autoptr (GSettings) bg = g_settings_new ("org.gnome.desktop.background");
    g_settings_set_string (bg, "picture-uri", uri);
    g_settings_set_string (bg, "picture-uri-dark", duri);
    /* a picture chosen here replaces the parallax one, which would hide it */
    g_autoptr (GSettings) neo = ns_settings ("de.yesman.neo");
    if (neo && ns_has_key (neo, "wallpaper-parallax"))
      g_settings_set_string (neo, "wallpaper-parallax", "");
  }
  if (where != 0) {
    g_autoptr (GSettings) ss = g_settings_new ("org.gnome.desktop.screensaver");
    g_settings_set_string (ss, "picture-uri", uri);
  }
}

typedef struct {
  char *path, *dark;
  GtkWidget *w;
} Apply;

static void
apply_response (AdwAlertDialog *d, const char *response, Apply *a)
{
  int where = !g_strcmp0 (response, "home") ? 0 : !g_strcmp0 (response, "lock") ? 1
            : !g_strcmp0 (response, "both") ? 2 : -1;
  if (where >= 0)
    wall_apply (a->w, a->path, a->dark, where);
  g_free (a->path);
  g_free (a->dark);
  g_object_unref (a->w);
  g_free (a);
}

static void
wall_ask (GtkWidget *w, const char *path, const char *dark)
{
  AdwDialog *d = adw_alert_dialog_new ("Set as wallpaper", NULL);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "home", "Home screen", "lock", "Lock screen", "both",
                                  "Home and lock screens", "cancel", "Cancel", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "both", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_prefer_wide_layout (ADW_ALERT_DIALOG (d), FALSE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  Apply *a = g_new0 (Apply, 1);
  a->path = g_strdup (path);
  a->dark = g_strdup (dark);
  a->w = g_object_ref (w);
  g_signal_connect (d, "response", G_CALLBACK (apply_response), a);
  adw_dialog_present (d, w);
}

static void
wall_tapped (GtkFlowBox *flow, GtkFlowBoxChild *child, gpointer data)
{
  GtkWidget *pic = gtk_flow_box_child_get_child (child);
  wall_ask (GTK_WIDGET (flow), g_object_get_data (G_OBJECT (pic), "ns-path"), g_object_get_data (G_OBJECT (pic),
                                                                                                    "ns-dark"));
}

static void
file_chosen (GObject *src, GAsyncResult *res, gpointer w)
{
  g_autoptr (GFile) f = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, NULL);
  g_autofree char *path = f ? g_file_get_path (f) : NULL;
  if (path)
    wall_ask (w, path, NULL);
  g_object_unref (w);
}

static void
from_files (GtkWidget *row, gpointer data)
{
  GtkFileDialog *fd = gtk_file_dialog_new ();
  GtkFileFilter *ff = gtk_file_filter_new ();
  gtk_file_filter_set_name (ff, "Pictures");
  gtk_file_filter_add_mime_type (ff, "image/*");
  gtk_file_dialog_set_default_filter (fd, ff);
  g_autoptr (GFile) pics = g_file_new_for_path (g_get_user_special_dir (G_USER_DIRECTORY_PICTURES)
                                                ? g_get_user_special_dir (G_USER_DIRECTORY_PICTURES) : g_get_home_dir ());
  gtk_file_dialog_set_initial_folder (fd, pics);
  gtk_file_dialog_open (fd, GTK_WINDOW (gtk_widget_get_root (row)), NULL, file_chosen, g_object_ref (row));
  g_object_unref (ff);
  g_object_unref (fd);
}

static gboolean
is_picture (const char *n)
{
  return g_str_has_suffix (n, ".jpg") || g_str_has_suffix (n, ".jpeg") || g_str_has_suffix (n, ".png")
         || g_str_has_suffix (n, ".jxl") || g_str_has_suffix (n, ".webp") || g_str_has_suffix (n, ".svg");
}

static void
cancel_free (GCancellable *c)
{
  g_cancellable_cancel (c);
  g_object_unref (c);
}

void
ns_page_wallpapers (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Wallpapers", &c);
  GtkWidget *g = ns_group (c, NULL);
  ns_row_button (g, "Choose from your pictures", FALSE, from_files, NULL);

  g = ns_group (c, NULL);
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 2);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 4);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (flow), TRUE);
  g_signal_connect (flow, "child-activated", G_CALLBACK (wall_tapped), NULL);
  ns_row_widget (g, NULL, flow);

  GPtrArray *thumbs = g_ptr_array_new ();
  g_autofree char *user = g_build_filename (g_get_user_data_dir (), "backgrounds", NULL);
  const char *dirs[] = { user, "/usr/share/backgrounds/gnome", "/usr/share/backgrounds", NULL };
  for (guint d = 0; dirs[d]; d++) {
    g_autoptr (GDir) dir = g_dir_open (dirs[d], 0, NULL);
    g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
    for (const char *n; dir && (n = g_dir_read_name (dir));)
      if (is_picture (n) && !strstr (n, "-d."))     /* GNOME's dark variants go with their light ones */
        g_ptr_array_add (names, g_strdup (n));
    g_ptr_array_sort (names, (GCompareFunc) g_ascii_strcasecmp);
    for (guint i = 0; i < names->len; i++) {
      const char *n = names->pdata[i];
      g_autofree char *path = g_build_filename (dirs[d], n, NULL);
      GtkWidget *pic = gtk_picture_new ();
      gtk_picture_set_content_fit (GTK_PICTURE (pic), GTK_CONTENT_FIT_COVER);
      gtk_widget_set_size_request (pic, WALL_W / 2 + 40, WALL_H / 2 + 40);
      gtk_widget_set_overflow (pic, GTK_OVERFLOW_HIDDEN);
      gtk_widget_add_css_class (pic, "ns-wall-thumb");
      gtk_widget_add_css_class (pic, "ns-parallax-thumb");
      g_object_set_data_full (G_OBJECT (pic), "ns-path", g_strdup (path), g_free);
      /* "x-l.jxl" has a "x-d.jxl" for the dark style */
      const char *l = strstr (n, "-l.");
      if (l) {
        g_autofree char *dn = g_strdup (n);
        dn[l - n + 1] = 'd';
        g_autofree char *dark = g_build_filename (dirs[d], dn, NULL);
        if (g_file_test (dark, G_FILE_TEST_EXISTS))
          g_object_set_data_full (G_OBJECT (pic), "ns-dark", g_steal_pointer (&dark), g_free);
      }
      gtk_flow_box_append (GTK_FLOW_BOX (flow), pic);
      Thumb *t = g_new0 (Thumb, 1);
      t->pic = g_object_ref (pic);
      t->path = g_strdup (path);
      g_ptr_array_add (thumbs, t);
    }
  }
  GCancellable *cancel = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (page), "ns-cancel", cancel, (GDestroyNotify) cancel_free);
  g_autoptr (GTask) task = g_task_new (NULL, cancel, NULL, NULL);
  g_task_set_task_data (task, thumbs, (GDestroyNotify) g_ptr_array_unref);
  g_task_run_in_thread (task, thumbs_thread);
  ns_push (page);
}

/* ---- Software information ---- */

static void
phosh_version (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autofree char *out = NULL;
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out && *out)
    ns_row_set_subtitle (row, g_strstrip (out));
  g_object_unref (row);
}

void
ns_page_software (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Software information", &c);
  GtkWidget *g = ns_group (c, NULL);
  ns_bt_name_row (g);

  g = ns_group (c, NULL);
  g_autofree char *deb = NULL;
  if (g_file_get_contents ("/etc/debian_version", &deb, NULL, NULL))
    g_strstrip (deb);
  ns_row_info (g, "Debian", deb);
  GtkWidget *ph = ns_row_info (g, "Phosh", NULL);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "dpkg-query", "-W", "-f=${Version}", "phosh", NULL);
  if (p)
    g_subprocess_communicate_utf8_async (p, NULL, NULL, phosh_version, g_object_ref (ph));
  g_autofree char *gtk = g_strdup_printf ("%u.%u.%u", gtk_get_major_version (), gtk_get_minor_version (),
                                          gtk_get_micro_version ());
  ns_row_info (g, "GTK", gtk);
  g_autofree char *adw = g_strdup_printf ("%u.%u.%u", adw_get_major_version (), adw_get_minor_version (),
                                          adw_get_micro_version ());
  ns_row_info (g, "libadwaita", adw);
  struct utsname u;
  uname (&u);
  ns_row_info (g, "Kernel", u.release);
  ns_row_info (g, "Kernel build", u.version);
  ns_row_info (g, "Settings", NS_VERSION);
  ns_push (page);
}

/* ---- Display: the screen scale, through the compositor's DisplayConfig (Phosh's) ---- */

typedef struct {
  guint32  serial;
  char    *connector, *mode;
  GArray  *scales;
} Scale;

static void
scale_free (Scale *s)
{
  g_free (s->connector);
  g_free (s->mode);
  g_array_unref (s->scales);
  g_free (s);
}

static void
scale_applied (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  if (!v) {
    g_dbus_error_strip_remote_error (err);
    ns_row_set_subtitle (row, err->message);
  }
  g_object_unref (row);
}

static void
scale_picked (GtkWidget *row, guint i, gpointer data)
{
  Scale *s = g_object_get_data (G_OBJECT (row), "ns-scale");
  GVariantBuilder mons, lms;
  g_variant_builder_init (&mons, G_VARIANT_TYPE ("a(ssa{sv})"));
  g_variant_builder_add (&mons, "(ss@a{sv})", s->connector, s->mode, g_variant_new_array (G_VARIANT_TYPE ("{sv}"),
                                                                                         NULL, 0));
  g_variant_builder_init (&lms, G_VARIANT_TYPE ("a(iiduba(ssa{sv}))"));
  g_variant_builder_add (&lms, "(iidub@a(ssa{sv}))", 0, 0, g_array_index (s->scales, double, i), 0, TRUE,
                         g_variant_builder_end (&mons));
  g_autoptr (GDBusConnection) ses = bus (G_BUS_TYPE_SESSION);
  /* 2: persistent */
  if (ses)
    g_dbus_connection_call (ses, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                            "org.gnome.Mutter.DisplayConfig", "ApplyMonitorsConfig",
                            g_variant_new ("(uu@a(iiduba(ssa{sv}))@a{sv})", s->serial, 2, g_variant_builder_end (&lms),
                                           g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0)),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, scale_applied, g_object_ref (row));
}

static void
state_got (GObject *src, GAsyncResult *res, gpointer group)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (!v) {
    g_object_unref (group);
    return;
  }
  Scale *s = g_new0 (Scale, 1);
  s->scales = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (GVariant) monitors = g_variant_get_child_value (v, 1);
  g_autoptr (GVariant) logical = g_variant_get_child_value (v, 2);
  g_variant_get_child (v, 0, "u", &s->serial);
  double cur = 0;
  /* the built-in screen: the first monitor, its current mode (or its first) */
  if (g_variant_n_children (monitors)) {
    g_autoptr (GVariant) m = g_variant_get_child_value (monitors, 0);
    g_autoptr (GVariant) spec = g_variant_get_child_value (m, 0);
    g_autoptr (GVariant) modes = g_variant_get_child_value (m, 1);
    g_variant_get_child (spec, 0, "s", &s->connector);
    guint pick = 0;
    for (guint i = 0; i < g_variant_n_children (modes); i++) {
      g_autoptr (GVariant) mode = g_variant_get_child_value (modes, i);
      g_autoptr (GVariant) props = g_variant_get_child_value (mode, 6);
      gboolean current = FALSE;
      if (g_variant_lookup (props, "is-current", "b", &current) && current)
        pick = i;
    }
    if (g_variant_n_children (modes)) {
      g_autoptr (GVariant) mode = g_variant_get_child_value (modes, pick);
      g_autoptr (GVariant) sc = g_variant_get_child_value (mode, 5);
      g_variant_get_child (mode, 0, "s", &s->mode);
      for (guint k = 0; k < g_variant_n_children (sc); k++) {
        double d;
        g_variant_get_child (sc, k, "d", &d);
        g_array_append_val (s->scales, d);
      }
    }
  }
  if (g_variant_n_children (logical)) {
    g_autoptr (GVariant) lm = g_variant_get_child_value (logical, 0);
    g_variant_get_child (lm, 2, "d", &cur);
  }
  if (!cur) {
    /* Phosh lists no logical monitor here: the scale GTK sees */
    GListModel *ms = gdk_display_get_monitors (gdk_display_get_default ());
    g_autoptr (GdkMonitor) gm = g_list_model_get_n_items (ms) ? g_list_model_get_item (ms, 0) : NULL;
    cur = gm ? gdk_monitor_get_scale (gm) : 1;
  }
  if (!s->connector || !s->mode || s->scales->len < 2) {
    scale_free (s);
    g_object_unref (group);
    return;
  }
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  guint sel = 0;
  for (guint i = 0; i < s->scales->len; i++) {
    double d = g_array_index (s->scales, double, i);
    g_ptr_array_add (labels, g_strdup_printf ("%.0f%%", d * 100));
    if (ABS (d - cur) < ABS (g_array_index (s->scales, double, sel) - cur))
      sel = i;
  }
  g_ptr_array_add (labels, NULL);
  GtkWidget *r = ns_row_pick (group, "Screen scale", (const char *const *) labels->pdata, sel, scale_picked, NULL);
  g_object_set_data_full (G_OBJECT (r), "ns-scale", s, (GDestroyNotify) scale_free);
  g_object_unref (group);
}

void
ns_page_scale_row (GtkWidget *group)
{
  g_autoptr (GDBusConnection) ses = bus (G_BUS_TYPE_SESSION);
  if (ses)
    g_dbus_connection_call (ses, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                            "org.gnome.Mutter.DisplayConfig", "GetCurrentState", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                            3000, NULL, state_got, g_object_ref (group));
}

/* ---- Mouse, touchpad and hardware keyboard: phoc follows GNOME's peripherals settings ---- */

static void
speed_row (GtkWidget *g, GSettings *s)
{
  GtkWidget *scale;
  ns_row_scale (g, "Pointer speed", "input-mouse-symbolic", -1, 1, 0.1, &scale);
  g_settings_bind (s, "speed", gtk_range_get_adjustment (GTK_RANGE (scale)), "value", G_SETTINGS_BIND_DEFAULT);
}

static const guint delays[] = { 250, 500, 800 };
static const char *const delay_labels[] = { "Short", "Medium", "Long", NULL };
static const guint rates[] = { 15, 30, 60 };
static const char *const rate_labels[] = { "Fast", "Medium", "Slow", NULL };

static void
uint_picked (GtkWidget *row, guint i, gpointer s)
{
  const guint *values = g_object_get_data (G_OBJECT (row), "ns-uints");
  g_settings_set_uint (s, g_object_get_data (G_OBJECT (row), "ns-key"), values[i]);
}

static GtkWidget *
uint_row (GtkWidget *g, const char *title, GSettings *s, const char *key, const guint *values, guint n,
          const char *const *labels)
{
  guint v = g_settings_get_uint (s, key), sel = 0;
  for (guint i = 0; i < n; i++)
    if (ABS ((int) values[i] - (int) v) < ABS ((int) values[sel] - (int) v))
      sel = i;
  GtkWidget *r = ns_row_pick (g, title, labels, sel, uint_picked, s);
  g_object_set_data (G_OBJECT (r), "ns-uints", (gpointer) values);
  g_object_set_data (G_OBJECT (r), "ns-key", (gpointer) key);
  return r;
}

void
ns_page_mouse (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Mouse and touchpad", &c);
  GSettings *mouse = ns_page_settings (page, "org.gnome.desktop.peripherals.mouse");
  GSettings *pad = ns_page_settings (page, "org.gnome.desktop.peripherals.touchpad");
  GSettings *kbd = ns_page_settings (page, "org.gnome.desktop.peripherals.keyboard");
  if (mouse) {
    GtkWidget *g = ns_group (c, "Mouse");
    speed_row (g, mouse);
    ns_row_switch (g, "Natural scrolling", "The content moves with the wheel, like a touchscreen", mouse,
                   "natural-scroll");
    ns_row_switch (g, "Left-handed", "The right button is the main one", mouse, "left-handed");
  }
  if (pad) {
    GtkWidget *g = ns_group (c, "Touchpad");
    speed_row (g, pad);
    ns_row_switch (g, "Tap to click", "A tap counts as a click", pad, "tap-to-click");
    ns_row_switch (g, "Natural scrolling", "The content moves with your fingers", pad, "natural-scroll");
  }
  if (kbd) {
    GtkWidget *g = ns_group (c, "Hardware keyboard");
    ns_row_switch (g, "Repeat keys", "A held key types again and again", kbd, "repeat");
    GtkWidget *r = uint_row (g, "Delay before repeating", kbd, "delay", delays, G_N_ELEMENTS (delays), delay_labels);
    g_settings_bind (kbd, "repeat", r, "sensitive", G_SETTINGS_BIND_GET);
    r = uint_row (g, "Repeat speed", kbd, "repeat-interval", rates, G_N_ELEMENTS (rates), rate_labels);
    g_settings_bind (kbd, "repeat", r, "sensitive", G_SETTINGS_BIND_GET);
  }
  ns_note (c, "For a Bluetooth or USB mouse, touchpad or keyboard.");
  ns_push (page);
}
