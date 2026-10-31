/* Security and privacy, Location, Accounts and Accessibility. GSettings for most of it; the apps that may use
 * the location from the portal's permission store, the online accounts from GNOME Online Accounts' object
 * manager, both asked in the background. */
#include "ns-pages.h"
#include "ns-ui.h"

#include <gio/gdesktopappinfo.h>
#define GOA_API_IS_SUBJECT_TO_CHANGE
#define GOA_BACKEND_API_IS_SUBJECT_TO_CHANGE
#include <goabackend/goabackend.h>

static GtkWidget *
inverted (GtkWidget *g, const char *title, const char *sub, GSettings *s, const char *key)
{
  GtkWidget *r = ns_row_switch (g, title, sub, NULL, NULL);
  g_settings_bind (s, key, ns_row_get_switch (r), "active", G_SETTINGS_BIND_INVERT_BOOLEAN);
  return r;
}

static void
int_picked (GtkWidget *row, guint i, gpointer s)
{
  const int *values = g_object_get_data (G_OBJECT (row), "ns-values");
  const char *key = g_object_get_data (G_OBJECT (row), "ns-key");
  g_autoptr (GVariant) v = g_settings_get_value (s, key);
  if (g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32))
    g_settings_set_uint (s, key, values[i]);
  else
    g_settings_set_int (s, key, values[i]);
}

/* an integer key from a fixed list */
static GtkWidget *
int_row (GtkWidget *g, const char *title, GSettings *s, const char *key, const int *values,
         const char *const *labels)
{
  g_autoptr (GVariant) v = g_settings_get_value (s, key);
  int cur = g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32) ? (int) g_variant_get_uint32 (v) : g_variant_get_int32 (v);
  guint sel = 0;
  for (guint i = 0; labels[i]; i++)
    if (values[i] == cur)
      sel = i;
  GtkWidget *r = ns_row_pick (g, title, labels, sel, int_picked, s);
  g_object_set_data (G_OBJECT (r), "ns-values", (gpointer) values);
  g_object_set_data (G_OBJECT (r), "ns-key", (gpointer) key);
  return r;
}

/* ---- Security and privacy ---- */

static const int ages[] = { -1, 1, 7, 30 };
static const char *const age_labels[] = { "Forever", "1 day", "7 days", "30 days", NULL };
static const int cleans[] = { 1, 7, 14, 30 };
static const char *const clean_labels[] = { "1 day", "7 days", "14 days", "30 days", NULL };

void
ns_page_privacy (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Security and privacy", &c);
  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *r = ns_row (g, "system-lock-screen-symbolic", "blue", "Lock screen", "Screen lock type • Lock after");
  ns_row_on_activate (r, ns_page_lock_screen, NULL);
  if (ns_settings_exists ("org.gnome.system.location")) {
    r = ns_row (g, "find-location-symbolic", "purple", "Location", "Location services • App access");
    ns_row_on_activate (r, ns_page_location, NULL);
  }
  r = ns_row (g, "system-users-symbolic", "orange", "Your account", "Name • Password");
  ns_row_on_activate (r, ns_page_users, NULL);

  GSettings *p = ns_page_settings (page, "org.gnome.desktop.privacy");
  if (!p) {
    ns_push (page);
    return;
  }
  g = ns_group (c, "Permissions");
  if (ns_has_key (p, "disable-camera"))
    inverted (g, "Camera", "Apps that ask through the system may use it", p, "disable-camera");
  if (ns_has_key (p, "disable-microphone"))
    inverted (g, "Microphone", "Apps that ask through the system may use it", p, "disable-microphone");

  g = ns_group (c, "History");
  ns_row_switch (g, "Remember recent files", "Apps show what you opened lately", p, "remember-recent-files");
  r = int_row (g, "Keep the list for", p, "recent-files-max-age", ages, age_labels);
  g_settings_bind (p, "remember-recent-files", r, "sensitive", G_SETTINGS_BIND_GET);

  g = ns_group (c, "Clean up");
  ns_row_switch (g, "Empty the trash", "Delete old files from it automatically", p, "remove-old-trash-files");
  ns_row_switch (g, "Delete temporary files", "Old ones, automatically", p, "remove-old-temp-files");
  int_row (g, "Older than", p, "old-files-age", cleans, clean_labels);

  /* no Diagnostics: no crash reporter is installed, and only GNOME Software and Shell read the usage keys */
  ns_push (page);
}

/* ---- Location ---- */

static const char *const accuracy[] = { "country", "city", "neighborhood", "street", "exact", NULL };
static const char *const accuracy_labels[] = { "Country", "City", "Neighbourhood", "Street", "Exact", NULL };

static GDBusConnection *
session_bus (void)
{
  return g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
}

static void
app_access (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  const char *app = g_object_get_data (G_OBJECT (sw), "ns-app");
  g_autoptr (GDBusConnection) bus = session_bus ();
  const char *perm[] = { gtk_switch_get_active (sw) ? "EXACT" : "NONE", "0", NULL };
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.impl.portal.PermissionStore",
                            "/org/freedesktop/impl/portal/PermissionStore",
                            "org.freedesktop.impl.portal.PermissionStore", "SetPermission",
                            g_variant_new ("(sbss^as)", "location", TRUE, "location", app, perm), NULL,
                            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
apps_got (GObject *src, GAsyncResult *res, gpointer group)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) perms = NULL;
  guint n = 0;
  if (v) {
    g_autoptr (GVariant) data = NULL;
    g_variant_get (v, "(@a{sas}v)", &perms, &data);
    GVariantIter it;
    const char *app;
    g_autofree const char **p = NULL;
    g_variant_iter_init (&it, perms);
    while (g_variant_iter_next (&it, "{&s^a&s}", &app, &p)) {
      g_autofree char *file = g_strconcat (app, ".desktop", NULL);
      g_autoptr (GDesktopAppInfo) info = *app ? g_desktop_app_info_new (file) : NULL;
      GtkWidget *r = info ? ns_row_app (group, G_APP_INFO (info), NULL) : ns_row (group, NULL, NULL, app, NULL);
      GtkWidget *sw = gtk_switch_new ();
      gtk_switch_set_active (GTK_SWITCH (sw), p && p[0] && g_strcmp0 (p[0], "NONE"));
      g_object_set_data_full (G_OBJECT (sw), "ns-app", g_strdup (app), g_free);
      g_signal_connect (sw, "notify::active", G_CALLBACK (app_access), NULL);
      ns_row_suffix (r, sw);
      g_clear_pointer (&p, g_free);
      n++;
    }
  }
  ns_group_set_visible (group, n > 0);
  gtk_widget_set_visible (g_object_get_data (group, "ns-none"), n == 0);
  g_object_unref (group);
}

void
ns_page_location (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Location", &c);
  GSettings *loc = ns_page_settings (page, "org.gnome.system.location");
  if (!loc)
    return;
  GtkWidget *g = ns_group (c, NULL);
  ns_row_switch (g, "Use location", "Apps may ask where the phone is", loc, "enabled");
  if (ns_has_key (loc, "max-accuracy-level")) {
    GtkWidget *r = ns_row_choice (g, "Accuracy", loc, "max-accuracy-level", accuracy, accuracy_labels);
    g_settings_bind (loc, "enabled", r, "sensitive", G_SETTINGS_BIND_GET);
  }
  g = ns_group (c, "App access");
  ns_group_set_visible (g, FALSE);
  GtkWidget *none = ns_note (c, "No app has asked for the location yet.");
  gtk_widget_set_visible (none, FALSE);
  g_object_set_data (G_OBJECT (g), "ns-none", none);
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.impl.portal.PermissionStore",
                            "/org/freedesktop/impl/portal/PermissionStore",
                            "org.freedesktop.impl.portal.PermissionStore", "Lookup",
                            g_variant_new ("(ss)", "location", "location"), G_VARIANT_TYPE ("(a{sas}v)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, apps_got, g_object_ref (g));
  ns_push (page);
}

/* ---- Accounts ---- */

static const char *const features[][2] = {
  { "MailDisabled", "Mail" }, { "CalendarDisabled", "Calendar" }, { "ContactsDisabled", "Contacts" },
  { "FilesDisabled", "Files" }, { "PhotosDisabled", "Photos" }, { "DocumentsDisabled", "Documents" },
  { "ChatDisabled", "Chat" }, { "MapsDisabled", "Maps" }, { "MusicDisabled", "Music" },
  { "TodoDisabled", "Tasks" },
};

static void
goa_set (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_call (bus, "org.gnome.OnlineAccounts", g_object_get_data (G_OBJECT (sw), "ns-path"),
                            "org.freedesktop.DBus.Properties", "Set",
                            g_variant_new ("(ssv)", "org.gnome.OnlineAccounts.Account",
                                           g_object_get_data (G_OBJECT (sw), "ns-prop"),
                                           g_variant_new_boolean (!gtk_switch_get_active (sw))),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
goa_removed (gboolean yes, gpointer row)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (yes && bus) {
    g_dbus_connection_call (bus, "org.gnome.OnlineAccounts", g_object_get_data (G_OBJECT (row), "ns-path"),
                            "org.gnome.OnlineAccounts.Account", "Remove", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                            NULL, NULL, NULL);
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
  }
}

static void
goa_remove (GtkWidget *row, gpointer data)
{
  ns_confirm (row, "Remove this account?", "Its mail, calendars and contacts leave the phone. The account itself "
              "stays with its provider.", "Remove", TRUE, goa_removed, row);
}

/* signing in is GOA's own: its backend library brings each provider's dialog (a web page for most), shown over
 * our window. The client is made once, when first needed. */
static GoaClient *goa;

typedef void (*WithGoa) (GtkWidget *row);

static void
goa_ready (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  goa = goa_client_new_finish (res, &err);
  WithGoa then = g_object_steal_data (G_OBJECT (row), "ns-then");
  if (goa && then)
    then (row);
  else if (!goa)
    ns_row_set_subtitle (row, err->message);
  g_object_unref (row);
}

static void
with_goa (GtkWidget *row, WithGoa then)
{
  if (goa) {
    then (row);
    return;
  }
  g_object_set_data (G_OBJECT (row), "ns-then", then);
  goa_client_new (NULL, goa_ready, g_object_ref (row));
}

static void
added (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GoaObject) obj = goa_provider_add_account_finish (GOA_PROVIDER (src), res, &err);
  if (obj) {
    AdwNavigationView *nav = ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW));
    if (nav) {
      adw_navigation_view_pop (nav);
      AdwNavigationPage *list = adw_navigation_view_get_visible_page (nav);
      if (list && g_object_get_data (G_OBJECT (list), "ns-accounts"))
        ns_later (list);
    }
  } else if (!g_error_matches (err, GOA_ERROR, GOA_ERROR_DIALOG_DISMISSED)) {
    ns_row_set_subtitle (row, err->message);
  }
  g_object_unref (row);
}

static void
provider_go (GtkWidget *row)
{
  GoaProvider *p = g_object_get_data (G_OBJECT (row), "ns-provider");
  goa_provider_add_account (p, goa, row, NULL, added, g_object_ref (row));
}

static void
provider_tapped (GtkWidget *row, gpointer data)
{
  with_goa (row, provider_go);
}

static void
providers_got (GObject *src, GAsyncResult *res, gpointer group)
{
  GList *all = NULL;
  g_autoptr (GError) err = NULL;
  if (!goa_provider_get_all_finish (&all, res, &err))
    ns_row (group, NULL, NULL, "No providers", err->message);
  for (GList *l = all; l; l = l->next) {
    GoaProvider *p = l->data;
    g_autofree char *name = goa_provider_get_provider_name (p, NULL);
    GtkWidget *r = ns_row (group, NULL, NULL, name, NULL);
    g_autoptr (GIcon) icon = goa_provider_get_provider_icon (p, NULL);
    GtkWidget *img = gtk_image_new_from_gicon (icon);
    gtk_image_set_pixel_size (GTK_IMAGE (img), 32);
    GtkWidget *box = g_object_get_data (G_OBJECT (r), "ns-box");
    gtk_box_prepend (GTK_BOX (box), img);
    g_object_set_data_full (G_OBJECT (r), "ns-provider", g_object_ref (p), g_object_unref);
    ns_row_on_activate (r, provider_tapped, NULL);
  }
  g_list_free_full (all, g_object_unref);
  g_object_unref (group);
}

static void
add_account (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Add account", &c);
  GtkWidget *g = ns_group (c, NULL);
  goa_provider_get_all (providers_got, g_object_ref (g));
  ns_push (page);
}

/* an account that wants its password again, or has settings of its own (IMAP servers…): GOA's account dialog */
static void
shown (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  if (!goa_provider_show_account_finish (GOA_PROVIDER (src), res, &err) &&
      !g_error_matches (err, GOA_ERROR, GOA_ERROR_DIALOG_DISMISSED))
    ns_row_set_subtitle (row, err->message);
  g_object_unref (row);
}

static void
account_go (GtkWidget *row)
{
  const char *path = g_object_get_data (G_OBJECT (row), "ns-path");
  g_autolist (GoaObject) accounts = goa_client_get_accounts (goa);
  for (GList *l = accounts; l; l = l->next) {
    if (g_strcmp0 (g_dbus_object_get_object_path (l->data), path))
      continue;
    GoaAccount *acc = goa_object_peek_account (l->data);
    g_autoptr (GoaProvider) p = goa_provider_get_for_provider_type (goa_account_get_provider_type (acc));
    if (p)
      goa_provider_show_account (p, goa, l->data, row, NULL, shown, g_object_ref (row));
    return;
  }
}

static void
account_dialog (GtkWidget *row, gpointer data)
{
  with_goa (row, account_go);
}

static void
account_page (GtkWidget *row, gpointer props)
{
  const char *path = g_object_get_data (G_OBJECT (row), "ns-path");
  const char *provider = "Account", *who = NULL;
  g_variant_lookup (props, "ProviderName", "&s", &provider);
  g_variant_lookup (props, "PresentationIdentity", "&s", &who);
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (provider, &c);
  GtkWidget *g = ns_group (c, NULL);
  ns_row_info (g, "Account", who);
  g = ns_group (c, "Use for");
  for (guint i = 0; i < G_N_ELEMENTS (features); i++) {
    gboolean off;
    if (!g_variant_lookup (props, features[i][0], "b", &off))
      continue;
    GtkWidget *r = ns_row_switch (g, features[i][1], NULL, NULL, NULL);
    GtkWidget *sw = ns_row_get_switch (r);
    gtk_switch_set_active (GTK_SWITCH (sw), !off);
    g_object_set_data_full (G_OBJECT (sw), "ns-path", g_strdup (path), g_free);
    g_object_set_data (G_OBJECT (sw), "ns-prop", (gpointer) features[i][0]);
    g_signal_connect (sw, "notify::active", G_CALLBACK (goa_set), NULL);
  }
  g = ns_group (c, NULL);
  gboolean attention = FALSE;
  g_variant_lookup (props, "AttentionNeeded", "b", &attention);
  GtkWidget *r = ns_row_button (g, attention ? "Sign in again" : "Account settings", FALSE, account_dialog, NULL);
  g_object_set_data_full (G_OBJECT (r), "ns-path", g_strdup (path), g_free);
  r = ns_row_button (g, "Remove account", TRUE, goa_remove, NULL);
  g_object_set_data_full (G_OBJECT (r), "ns-path", g_strdup (path), g_free);
  ns_push (page);
}

static void
accounts_got (GObject *src, GAsyncResult *res, gpointer group)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  guint n = 0;
  ns_group_clear (group);
  if (v) {
    g_autoptr (GVariant) objs = g_variant_get_child_value (v, 0);
    GVariantIter it;
    const char *path;
    GVariant *ifaces;
    g_variant_iter_init (&it, objs);
    while (g_variant_iter_next (&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
      GVariant *acc = g_variant_lookup_value (ifaces, "org.gnome.OnlineAccounts.Account", G_VARIANT_TYPE_VARDICT);
      g_variant_unref (ifaces);
      if (!acc)
        continue;
      const char *provider = "", *who = "";
      gboolean attention = FALSE;
      g_variant_lookup (acc, "ProviderName", "&s", &provider);
      g_variant_lookup (acc, "PresentationIdentity", "&s", &who);
      g_variant_lookup (acc, "AttentionNeeded", "b", &attention);
      GtkWidget *r = ns_row (group, "avatar-default-symbolic", "blue", provider,
                             attention ? "Needs you to sign in again" : who);
      g_object_set_data_full (G_OBJECT (r), "ns-path", g_strdup (path), g_free);
      g_object_set_data_full (G_OBJECT (r), "ns-props", acc, (GDestroyNotify) g_variant_unref);
      ns_row_on_activate (r, account_page, acc);
      n++;
    }
  }
  ns_group_set_visible (group, n > 0);
  gtk_widget_set_visible (g_object_get_data (group, "ns-none"), n == 0);
  g_object_unref (group);
}

static void
accounts_refresh (AdwNavigationPage *page)
{
  GtkWidget *g = g_object_get_data (G_OBJECT (page), "ns-accounts");
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_call (bus, "org.gnome.OnlineAccounts", "/org/gnome/OnlineAccounts",
                            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                            G_VARIANT_TYPE ("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, accounts_got,
                            g_object_ref (g));
}

static void
accounts_changed (GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                  const char *signal, GVariant *params, gpointer page)
{
  ns_later (page);
}

static void
unsubscribe (GtkWidget *page, gpointer sub)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_signal_unsubscribe (bus, GPOINTER_TO_UINT (sub));
}

void
ns_page_accounts (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Accounts", &c);
  GtkWidget *g = ns_group (c, "Online accounts");
  ns_group_set_visible (g, FALSE);
  g_object_set_data (G_OBJECT (page), "ns-accounts", g);
  GtkWidget *none = ns_note (c, "No online accounts yet. Add one to get its mail, calendars and contacts here.");
  gtk_widget_set_visible (none, FALSE);
  g_object_set_data (G_OBJECT (g), "ns-none", none);
  GtkWidget *a = ns_group (c, NULL);
  ns_row_button (a, "Add account", FALSE, add_account, NULL);
  ns_later_init (page, accounts_refresh);
  /* an account added, removed or signed in again: the list again */
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus) {
    guint sub = g_dbus_connection_signal_subscribe (bus, "org.gnome.OnlineAccounts", NULL, NULL,
                                                    "/org/gnome/OnlineAccounts", NULL,
                                                    G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_PATH, accounts_changed, page, NULL);
    g_signal_connect_data (page, "destroy", G_CALLBACK (unsubscribe), GUINT_TO_POINTER (sub), NULL, 0);
  }
  accounts_refresh (page);
  ns_push (page);
}

/* ---- Accessibility ---- */

static const int cursors[] = { 24, 32, 48, 64, 96 };
static const char *const cursor_labels[] = { "Default", "Medium", "Large", "Larger", "Largest", NULL };

void
ns_page_accessibility (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Accessibility", &c);
  GSettings *a11y = ns_page_settings (page, "org.gnome.desktop.a11y.interface");
  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");
  GSettings *apps = ns_page_settings (page, "org.gnome.desktop.a11y.applications");
  GSettings *phosh = ns_page_settings (page, "sm.puri.phosh");

  GtkWidget *g = ns_group (c, "Vision");
  if (ns_has_key (a11y, "high-contrast"))
    ns_row_switch (g, "High contrast", "Stronger colours and outlines", a11y, "high-contrast");
  if (ns_has_key (phosh, "automatic-high-contrast"))
    ns_row_switch (g, "High contrast in bright light", "Switched by the light sensor", phosh,
                   "automatic-high-contrast");
  if (ns_has_key (iface, "enable-animations"))
    inverted (g, "Reduce animations", "Less movement on screen", iface, "enable-animations");
  GtkWidget *r = ns_row (g, NULL, NULL, "Font size", "Display settings");
  ns_row_on_activate (r, ns_page_display, NULL);
  if (ns_has_key (iface, "cursor-size"))
    int_row (g, "Pointer size", iface, "cursor-size", cursors, cursor_labels);
  /* the screen reader is Orca: only when it is installed */
  g_autofree char *orca = g_find_program_in_path ("orca");
  if (orca && ns_has_key (apps, "screen-reader-enabled"))
    ns_row_switch (g, "Screen reader", "Orca reads out what is on screen", apps, "screen-reader-enabled");

  g = ns_group (c, "Interaction");
  if (ns_has_key (apps, "screen-keyboard-enabled"))
    ns_row_switch (g, "On-screen keyboard", "Shown when text is typed", apps, "screen-keyboard-enabled");
  /* no sticky, slow or bounce keys: GNOME Shell's compositor does them, phoc does not */
  ns_push (page);
}
