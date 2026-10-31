/* Notifications, Apps and Default apps. An app's notifications are GNOME's per-app settings
 * (org.gnome.desktop.notifications.application at .../application/<munged id>/, listed in application-children),
 * which Phosh follows; its vibration is feedbackd's per-app profile and "scale to fit" phoc's. The app list is a
 * GtkListView, so only the rows on screen exist. */
#include "ns-pages.h"
#include "ns-ui.h"

#include <gio/gdesktopappinfo.h>

#define NOTIFY_APP "org.gnome.desktop.notifications.application"

static gint
by_name (gconstpointer a, gconstpointer b, gpointer data)
{
  g_autofree char *x = g_utf8_casefold (g_app_info_get_display_name ((GAppInfo *) a), -1);
  g_autofree char *y = g_utf8_casefold (g_app_info_get_display_name ((GAppInfo *) b), -1);
  return g_utf8_collate (x, y);
}

/* a relocatable schema at <prefix><munged id>/, or NULL when it is not installed */
static GSettings *
app_settings (const char *schema, const char *prefix, GAppInfo *app)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = src ? g_settings_schema_source_lookup (src, schema, TRUE) : NULL;
  if (!sc)
    return NULL;
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  g_autofree char *path = g_strconcat (prefix, id, "/", NULL);
  return g_settings_new_full (sc, NULL, path);
}

/* GNOME Settings' way: an app's notification settings count once its id is in application-children */
static GSettings *
notify_settings (GAppInfo *app)
{
  GSettings *s = app_settings (NOTIFY_APP, "/org/gnome/desktop/notifications/application/", app);
  if (!s)
    return NULL;
  g_autoptr (GSettings) top = g_settings_new ("org.gnome.desktop.notifications");
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  g_auto (GStrv) kids = g_settings_get_strv (top, "application-children");
  if (!g_strv_contains ((const char *const *) kids, id)) {
    GStrvBuilder *b = g_strv_builder_new ();
    g_strv_builder_addv (b, (const char **) kids);
    g_strv_builder_add (b, id);
    g_auto (GStrv) nk = g_strv_builder_end (b);
    g_strv_builder_unref (b);
    g_settings_set_strv (top, "application-children", (const char *const *) nk);
    if (ns_has_key (s, "application-id"))
      g_settings_set_string (s, "application-id", g_app_info_get_id (app));
  }
  return s;
}

static gboolean
uses_notifications (GAppInfo *app, const char *const *kids)
{
  if (!G_IS_DESKTOP_APP_INFO (app) || !g_app_info_should_show (app))
    return FALSE;
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  return g_desktop_app_info_get_boolean (G_DESKTOP_APP_INFO (app), "X-GNOME-UsesNotifications")
         || g_strv_contains (kids, id);
}

/* ---- an app's notifications ---- */

static void
app_notify_page (GtkWidget *row, gpointer app)
{
  GSettings *s = notify_settings (app);
  if (!s)
    return;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (g_app_info_get_display_name (app), &c);
  g_object_set_data_full (G_OBJECT (page), "ns-notify", s, g_object_unref);
  GtkWidget *g = ns_group (c, NULL);
  ns_row_switch (g, "Allow notifications", NULL, s, "enable");
  g = ns_group (c, NULL);
  GtkWidget *rows[4] = { NULL };
  guint n = 0;
  if (ns_has_key (s, "show-banners"))
    rows[n++] = ns_row_switch (g, "Pop-up", "Show a banner at the top of the screen", s, "show-banners");
  /* Phosh reads only "enable" and "show-banners" per app; sound and the lock screen are the app's feedback
   * profile (Sounds and vibration) and the lock screen's own setting */
  for (guint i = 0; i < n; i++)
    g_settings_bind (s, "enable", rows[i], "sensitive", G_SETTINGS_BIND_GET);
  ns_push (page);
}

static void
allowed_changed (GSettings *s, const char *key, GtkWidget *row)
{
  ns_row_set_subtitle (row, g_settings_get_boolean (s, "enable") ? "Allowed" : "Off");
}

/* ---- Notifications ---- */

static const char *const urgencies[] = { "Never", "Any notification", "Normal and urgent ones", "Urgent ones only",
                                         NULL };
static const char *const urgency_nicks[] = { NULL, "low", "normal", "critical" };
static const char *const categories[][2] = {
  { "call", "Calls" }, { "im", "Messages" }, { "email", "Email" }, { "device", "Device events" },
  { "network", "Network events" }, { "presence", "Presence changes" }, { "transfer", "File transfers" },
};

/* the triggers follow the choices: "urgency" when there is one, "category" when any category is on */
static void
wake_triggers (GSettings *s, gboolean urgency)
{
  g_auto (GStrv) cats = g_settings_get_strv (s, "wakeup-screen-categories");
  g_auto (GStrv) cur = g_settings_get_strv (s, "wakeup-screen-triggers");
  GStrvBuilder *b = g_strv_builder_new ();
  if (g_strv_contains ((const char *const *) cur, "any"))
    g_strv_builder_add (b, "any");
  if (urgency)
    g_strv_builder_add (b, "urgency");
  if (cats[0])
    g_strv_builder_add (b, "category");
  g_auto (GStrv) t = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  g_settings_set_strv (s, "wakeup-screen-triggers", (const char *const *) t);
}

static void
urgency_picked (GtkWidget *row, guint i, gpointer s)
{
  if (i)
    g_settings_set_string (s, "wakeup-screen-urgency", urgency_nicks[i]);
  wake_triggers (s, i != 0);
}

static void
cats_changed (GSettings *s, const char *key, gpointer data)
{
  g_auto (GStrv) cur = g_settings_get_strv (s, "wakeup-screen-triggers");
  wake_triggers (s, g_strv_contains ((const char *const *) cur, "urgency"));
}

static void
wake_page (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Wake the screen", &c);
  GSettings *s = ns_page_settings (page, "sm.puri.phosh.notifications");
  if (!s)
    return;
  g_auto (GStrv) trig = g_settings_get_strv (s, "wakeup-screen-triggers");
  g_autofree char *urg = g_settings_get_string (s, "wakeup-screen-urgency");
  guint sel = 0;
  for (guint i = 1; g_strv_contains ((const char *const *) trig, "urgency") && i < G_N_ELEMENTS (urgency_nicks); i++)
    if (!g_strcmp0 (urg, urgency_nicks[i]))
      sel = i;
  GtkWidget *g = ns_group (c, NULL);
  ns_row_pick (g, "Turn the screen on for", urgencies, sel, urgency_picked, s);
  g = ns_group (c, "And always for");
  for (guint i = 0; i < G_N_ELEMENTS (categories); i++)
    ns_row_flag (g, categories[i][1], NULL, s, "wakeup-screen-categories", categories[i][0]);
  g_signal_connect (s, "changed::wakeup-screen-categories", G_CALLBACK (cats_changed), NULL);
  ns_push (page);
}

static void
app_switched (GtkSwitch *sw, GParamSpec *ps, GAppInfo *app)
{
  if (ns_switch_syncing (GTK_WIDGET (sw)))
    return;
  g_autoptr (GSettings) s = notify_settings (app);
  if (s)
    g_settings_set_boolean (s, "enable", gtk_switch_get_active (sw));
}

void
ns_page_notifications (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Notifications", &c);
  GSettings *n = ns_page_settings (page, "org.gnome.desktop.notifications");
  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *r = ns_row_switch (g, "Do not disturb", "No pop-ups: notifications wait in the list", NULL, NULL);
  g_settings_bind (n, "show-banners", ns_row_get_switch (r), "active", G_SETTINGS_BIND_INVERT_BOOLEAN);
  ns_row_switch (g, "Show on the lock screen", NULL, n, "show-in-lock-screen");
  if (ns_settings_exists ("sm.puri.phosh.notifications")) {
    r = ns_row (g, NULL, NULL, "Wake the screen", "Which notifications turn the screen on");
    ns_row_on_activate (r, wake_page, NULL);
  }

  g = ns_group (c, "App notifications");
  g_auto (GStrv) kids = g_settings_get_strv (n, "application-children");
  GList *all = g_app_info_get_all ();
  all = g_list_sort_with_data (all, by_name, NULL);
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = g_settings_schema_source_lookup (src, NOTIFY_APP, TRUE);
  for (GList *l = all; sc && l; l = l->next) {
    GAppInfo *app = l->data;
    if (!uses_notifications (app, (const char *const *) kids))
      continue;
    g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
    g_autofree char *path = g_strconcat ("/org/gnome/desktop/notifications/application/", id, "/", NULL);
    g_autoptr (GSettings) s = g_settings_new_full (sc, NULL, path);
    GtkWidget *ar = ns_row_app (g, app, NULL);
    GtkWidget *sw = gtk_switch_new ();
    ns_switch_quiet (sw, g_settings_get_boolean (s, "enable"));
    g_object_set_data_full (G_OBJECT (ar), "ns-app", g_object_ref (app), g_object_unref);
    g_signal_connect (sw, "notify::active", G_CALLBACK (app_switched), app);
    ns_row_suffix (ar, sw);
    ns_row_on_activate (ar, app_notify_page, app);
  }
  g_list_free_full (all, g_object_unref);
  ns_push (page);
}

/* ---- an app ---- */

static void
open_app (GtkWidget *row, gpointer app)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GAppLaunchContext) ctx = G_APP_LAUNCH_CONTEXT (
    gdk_display_get_app_launch_context (gtk_widget_get_display (row)));
  if (!g_app_info_launch (app, NULL, ctx, &err))
    ns_row_set_subtitle (row, err->message);
}

/* its package and version: dpkg-query, in the background */
static void
version_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autofree char *out = NULL;
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out) {
    /* "pkg: /usr/share/applications/x.desktop" then the version, both from one shell */
    g_auto (GStrv) l = g_strsplit (g_strstrip (out), "\n", 2);
    if (l[0] && l[1])
      ns_row_set_subtitle (row, g_strstrip (l[1]));
  }
  g_object_unref (row);
}

static void
version_row (GtkWidget *g, GAppInfo *app)
{
  const char *file = G_IS_DESKTOP_APP_INFO (app) ? g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (app)) : NULL;
  if (!file || !g_str_has_prefix (file, "/usr/"))
    return;
  GtkWidget *r = ns_row_info (g, "Version", NULL);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "sh", "-c",
                                                "p=$(dpkg-query -S \"$1\" 2>/dev/null | head -n1 | cut -d: -f1) && "
                                                "echo \"$p\" && dpkg-query -W -f='${Version}' \"$p\"",
                                                "sh", file, NULL);
  if (p)
    g_subprocess_communicate_utf8_async (p, NULL, NULL, version_got, g_object_ref (r));
}

/* Uninstall: the package that owns the app's desktop file, removed with apt behind pkexec (Phosh asks for the
 * password). apt-get -s first, as the user: the button shows only when apt would remove something, and the
 * question names every package that goes (a game can take a meta package with it). Essential packages make apt
 * refuse the simulation, so they get no button. Apps not from a package (/usr/local, Flatpak) get none either. */
typedef struct {
  GtkWidget *button;
  char      *name, *pkg;
  char     **removes;
} Uninstall;

static void
uninstall_free (Uninstall *u)
{
  g_object_unref (u->button);
  g_free (u->name);
  g_free (u->pkg);
  g_strfreev (u->removes);
  g_free (u);
}

static void
uninstall_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Uninstall *u = data;
  g_autoptr (GError) err = NULL;
  if (g_subprocess_wait_check_finish (G_SUBPROCESS (src), res, &err)) {
    GtkWidget *nav = gtk_widget_get_ancestor (u->button, ADW_TYPE_NAVIGATION_VIEW);
    if (nav)
      adw_navigation_view_pop (ADW_NAVIGATION_VIEW (nav));
  } else {
    int st = g_subprocess_get_if_exited (G_SUBPROCESS (src)) ? g_subprocess_get_exit_status (G_SUBPROCESS (src)) : -1;
    /* 126: the password was not given (pkexec's "not authorized"), nothing to report */
    gtk_button_set_label (GTK_BUTTON (u->button), st == 126 ? "Uninstall" : "Could not uninstall, try again");
    gtk_widget_set_sensitive (u->button, TRUE);
  }
  uninstall_free (u);
}

static void
uninstall_answered (gboolean yes, gpointer data)
{
  Uninstall *u = data;                   /* the question's own copy: button and package */
  if (!yes) {
    uninstall_free (u);
    return;
  }
  g_autoptr (GError) err = NULL;
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                &err, "pkexec", "env", "DEBIAN_FRONTEND=noninteractive",
                                                "apt-get", "-y", "remove", u->pkg, NULL);
  if (!p) {
    gtk_button_set_label (GTK_BUTTON (u->button), err->message);
    uninstall_free (u);
    return;
  }
  gtk_button_set_label (GTK_BUTTON (u->button), "Uninstalling…");
  gtk_widget_set_sensitive (u->button, FALSE);
  g_subprocess_wait_check_async (p, NULL, uninstall_done, u);
}

static void
uninstall_tapped (GtkButton *b, Uninstall *u)
{
  g_autofree char *list = g_strjoinv (", ", u->removes);
  g_autofree char *heading = g_strdup_printf ("Uninstall %s?", u->name);
  g_autofree char *body = g_strv_length (u->removes) > 1
    ? g_strdup_printf ("These packages will be removed: %s.", list)
    : g_strdup_printf ("The package %s will be removed.", list);
  Uninstall *q = g_new0 (Uninstall, 1);
  q->button = g_object_ref (u->button);
  q->pkg = g_strdup (u->pkg);
  ns_confirm (GTK_WIDGET (b), heading, body, "Uninstall", TRUE, uninstall_answered, q);
}

static void
uninstall_got (GObject *src, GAsyncResult *res, gpointer data)
{
  Uninstall *u = data;
  g_autofree char *out = NULL;
  /* the package on the first line, then what apt would remove, one per line */
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out) {
    g_auto (GStrv) l = g_strsplit (g_strstrip (out), "\n", -1);
    if (l[0] && *l[0] && l[1] && *l[1]) {
      u->pkg = g_strdup (l[0]);
      u->removes = g_strdupv (l + 1);
      g_signal_connect_data (u->button, "clicked", G_CALLBACK (uninstall_tapped), u,
                             (GClosureNotify) (void (*) (void)) uninstall_free, 0);
      gtk_widget_set_visible (u->button, TRUE);
      return;
    }
  }
  uninstall_free (u);
}

static void
uninstall_button (GtkWidget *c, GAppInfo *app)
{
  const char *file = G_IS_DESKTOP_APP_INFO (app) ? g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (app)) : NULL;
  if (!file || !g_str_has_prefix (file, "/usr/share/"))
    return;
  GtkWidget *b = gtk_button_new_with_label ("Uninstall");
  gtk_widget_add_css_class (b, "destructive-action");
  gtk_widget_add_css_class (b, "ns-uninstall");
  gtk_widget_set_visible (b, FALSE);
  gtk_box_append (GTK_BOX (c), b);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "sh", "-c",
                                                "p=$(dpkg-query -S \"$1\" 2>/dev/null | head -n1 | cut -d: -f1) && "
                                                "[ -n \"$p\" ] && echo \"$p\" && "
                                                "LC_ALL=C apt-get -s remove \"$p\" 2>/dev/null | sed -n 's/^Remv \\([^ ]*\\).*/\\1/p'",
                                                "sh", file, NULL);
  if (!p)
    return;
  Uninstall *u = g_new0 (Uninstall, 1);
  u->button = g_object_ref (b);
  u->name = g_strdup (g_app_info_get_display_name (app));
  g_subprocess_communicate_utf8_async (p, NULL, NULL, uninstall_got, u);
}

static const char *const fb_profiles[] = { "full", "quiet", "silent", NULL };
static const char *const fb_labels[] = { "Sound and vibration", "Vibration only", "Silent", NULL };

static void
app_page (GAppInfo *app)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (g_app_info_get_display_name (app), &c);
  g_object_set_data_full (G_OBJECT (page), "ns-app", g_object_ref (app), g_object_unref);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (head, "ns-page-head");
  GIcon *gi = g_app_info_get_icon (app);
  GtkWidget *img = gi ? gtk_image_new_from_gicon (gi) : gtk_image_new_from_icon_name ("application-x-executable");
  gtk_image_set_pixel_size (GTK_IMAGE (img), 72);
  GtkWidget *name = gtk_label_new (g_app_info_get_display_name (app));
  gtk_widget_add_css_class (name, "ns-big");
  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_box_append (GTK_BOX (head), img);
  gtk_box_append (GTK_BOX (head), name);
  const char *desc = g_app_info_get_description (app);
  if (desc) {
    GtkWidget *d = gtk_label_new (desc);
    gtk_label_set_wrap (GTK_LABEL (d), TRUE);
    gtk_label_set_justify (GTK_LABEL (d), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (d, "ns-sub");
    gtk_box_append (GTK_BOX (head), d);
  }
  gtk_box_append (GTK_BOX (c), head);

  GtkWidget *g = ns_group (c, NULL);
  ns_row_button (g, "Open", FALSE, open_app, app);

  g = ns_group (c, NULL);
  GSettings *ns = app_settings (NOTIFY_APP, "/org/gnome/desktop/notifications/application/", app);
  if (ns) {
    g_object_set_data_full (G_OBJECT (page), "ns-notify", ns, g_object_unref);
    GtkWidget *r = ns_row (g, "preferences-system-notifications-symbolic", "orange", "Notifications", NULL);
    allowed_changed (ns, NULL, r);
    g_signal_connect_object (ns, "changed::enable", G_CALLBACK (allowed_changed), r, 0);
    ns_row_on_activate (r, app_notify_page, app);
  }
  GSettings *fb = app_settings ("org.sigxcpu.feedbackd.application", "/org/sigxcpu/feedbackd/application/", app);
  if (fb) {
    g_object_set_data_full (G_OBJECT (page), "ns-fb", fb, g_object_unref);
    ns_row_choice (g, "Sounds and vibration", fb, "profile", fb_profiles, fb_labels);
  }
  GSettings *phoc = app_settings ("sm.puri.phoc.application", "/sm/puri/phoc/application/", app);
  if (phoc && ns_has_key (phoc, "scale-to-fit")) {
    g_object_set_data_full (G_OBJECT (page), "ns-phoc", phoc, g_object_unref);
    ns_row_switch (g, "Scale to fit", "Shrink an app that does not fit the screen", phoc, "scale-to-fit");
  } else {
    g_clear_object (&phoc);
  }

  g = ns_group (c, NULL);
  version_row (g, app);
  ns_row_info (g, "App ID", g_app_info_get_id (app));
  uninstall_button (c, app);
  ns_push (page);
}

/* ---- Apps: a list view ---- */

typedef struct {
  GtkWidget *sep, *icon, *title, *sub;
} Item;

/* one app's page straight away (the launcher's "App info", GNOME Settings' "applications" panel with an app id) */
void
ns_page_app (const char *id)
{
  g_autofree char *file = g_str_has_suffix (id, ".desktop") ? g_strdup (id) : g_strconcat (id, ".desktop", NULL);
  g_autoptr (GDesktopAppInfo) app = g_desktop_app_info_new (file);
  if (app)
    app_page (G_APP_INFO (app));
  else
    ns_page_apps (NULL, NULL);
}

static void
item_setup (GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
  Item *it = g_new0 (Item, 1);
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (v, "ns-li");
  it->sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append (GTK_BOX (v), it->sep);
  GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18);
  gtk_widget_add_css_class (h, "ns-li-row");
  it->icon = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (it->icon), 40);
  gtk_widget_set_valign (it->icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (h), it->icon);
  GtkWidget *t = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign (t, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (t, TRUE);
  it->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (it->title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (it->title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (it->title, "ns-title");
  it->sub = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (it->sub), 0);
  gtk_label_set_ellipsize (GTK_LABEL (it->sub), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (it->sub, "ns-sub");
  gtk_box_append (GTK_BOX (t), it->title);
  gtk_box_append (GTK_BOX (t), it->sub);
  gtk_box_append (GTK_BOX (h), t);
  gtk_box_append (GTK_BOX (v), h);
  g_object_set_data_full (G_OBJECT (v), "ns-item", it, g_free);
  gtk_list_item_set_child (li, v);
}

static void
item_bind (GtkSignalListItemFactory *f, GtkListItem *li, GListModel *model)
{
  GtkWidget *v = gtk_list_item_get_child (li);
  Item *it = g_object_get_data (G_OBJECT (v), "ns-item");
  guint pos = gtk_list_item_get_position (li), n = g_list_model_get_n_items (model);
  GObject *o = gtk_list_item_get_item (li);
  /* the first item is the "Default apps" card on its own, then one card of apps */
  gboolean first = pos <= 1, last = pos == 0 || pos == n - 1;
  gtk_widget_set_visible (it->sep, !first);
  first ? gtk_widget_add_css_class (v, "ns-li-first") : gtk_widget_remove_css_class (v, "ns-li-first");
  last ? gtk_widget_add_css_class (v, "ns-li-last") : gtk_widget_remove_css_class (v, "ns-li-last");
  gtk_widget_remove_css_class (it->icon, "ns-icon");
  gtk_widget_remove_css_class (it->icon, "ns-blue");
  if (G_IS_APP_INFO (o)) {
    GAppInfo *app = G_APP_INFO (o);
    GIcon *gi = g_app_info_get_icon (app);
    if (gi)
      gtk_image_set_from_gicon (GTK_IMAGE (it->icon), gi);
    else
      gtk_image_set_from_icon_name (GTK_IMAGE (it->icon), "application-x-executable");
    gtk_image_set_pixel_size (GTK_IMAGE (it->icon), 40);
    gtk_label_set_text (GTK_LABEL (it->title), g_app_info_get_display_name (app));
    const char *d = g_app_info_get_description (app);
    gtk_label_set_text (GTK_LABEL (it->sub), d ? d : "");
    gtk_widget_set_visible (it->sub, d && *d);
  } else {
    gtk_image_set_from_icon_name (GTK_IMAGE (it->icon), "view-grid-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (it->icon), 20);
    gtk_widget_add_css_class (it->icon, "ns-icon");
    gtk_widget_add_css_class (it->icon, "ns-blue");
    gtk_label_set_text (GTK_LABEL (it->title), "Default apps");
    gtk_label_set_text (GTK_LABEL (it->sub), "Browser • Email • Music • Photos");
    gtk_widget_set_visible (it->sub, TRUE);
  }
}

static void
item_activated (GtkListView *lv, guint pos, GListModel *model)
{
  g_autoptr (GObject) o = g_list_model_get_item (model, pos);
  if (G_IS_APP_INFO (o))
    app_page (G_APP_INFO (o));
  else
    ns_page_default_apps (NULL, NULL);
}

void
ns_page_apps (GtkWidget *row, gpointer data)
{
  GListStore *store = g_list_store_new (G_TYPE_OBJECT);
  g_autoptr (GObject) head = g_object_new (G_TYPE_OBJECT, NULL);
  g_list_store_append (store, head);
  GList *all = g_app_info_get_all ();
  all = g_list_sort_with_data (all, by_name, NULL);
  for (GList *l = all; l; l = l->next)
    if (g_app_info_should_show (l->data))
      g_list_store_append (store, l->data);
  g_list_free_full (all, g_object_unref);

  GtkListItemFactory *f = gtk_signal_list_item_factory_new ();
  g_signal_connect (f, "setup", G_CALLBACK (item_setup), NULL);
  g_signal_connect (f, "bind", G_CALLBACK (item_bind), store);
  GtkSelectionModel *sel = GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (g_object_ref (store))));
  GtkWidget *lv = gtk_list_view_new (sel, f);
  gtk_list_view_set_single_click_activate (GTK_LIST_VIEW (lv), TRUE);
  g_signal_connect_data (lv, "activate", G_CALLBACK (item_activated), store,
                         (GClosureNotify) (void (*) (void)) g_object_unref, 0);
  ns_push (ns_page_new_list ("Apps", lv));
}

/* ---- Default apps ---- */

typedef struct {
  const char *title, *const *types;
} Kind;

static const char *const web[] = { "x-scheme-handler/http", "x-scheme-handler/https", "text/html",
                                   "x-scheme-handler/about", "x-scheme-handler/unknown", NULL };
static const char *const mail[] = { "x-scheme-handler/mailto", NULL };
static const char *const cal[] = { "text/calendar", NULL };
static const char *const music[] = { "audio/x-vorbis+ogg", "audio/mpeg", "audio/x-wav", "audio/x-flac", "audio/ogg",
                                     "audio/mp4", "audio/x-opus+ogg", NULL };
static const char *const video[] = { "video/x-ogm+ogg", "video/mp4", "video/webm", "video/x-matroska",
                                     "video/quicktime", "video/mpeg", NULL };
static const char *const photos[] = { "image/jpeg", "image/png", "image/gif", "image/webp", "image/heic",
                                      "image/tiff", NULL };

static const Kind kinds[] = {
  { "Browser", web }, { "Email", mail }, { "Calendar", cal }, { "Music", music }, { "Video", video },
  { "Photos", photos },
};

static void
default_picked (GtkWidget *row, guint i, gpointer kind)
{
  const Kind *k = kind;
  GPtrArray *apps = g_object_get_data (G_OBJECT (row), "ns-apps");
  GAppInfo *app = apps->pdata[i];
  for (guint t = 0; k->types[t]; t++)
    g_app_info_set_as_default_for_type (app, k->types[t], NULL);
}

void
ns_page_default_apps (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Default apps", &c);
  GtkWidget *g = ns_group (c, NULL);
  for (guint k = 0; k < G_N_ELEMENTS (kinds); k++) {
    GList *all = g_app_info_get_all_for_type (kinds[k].types[0]);
    g_autoptr (GAppInfo) def = g_app_info_get_default_for_type (kinds[k].types[0], FALSE);
    GPtrArray *apps = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr (GPtrArray) labels = g_ptr_array_new ();
    guint sel = G_MAXUINT;
    for (GList *l = all; l; l = l->next) {
      if (!g_app_info_should_show (l->data))
        continue;
      if (def && g_app_info_equal (def, l->data))
        sel = apps->len;
      g_ptr_array_add (labels, (gpointer) g_app_info_get_display_name (l->data));
      g_ptr_array_add (apps, g_object_ref (l->data));
    }
    g_ptr_array_add (labels, NULL);
    g_list_free_full (all, g_object_unref);
    GtkWidget *r;
    if (apps->len) {
      r = ns_row_pick (g, kinds[k].title, (const char *const *) labels->pdata, sel, default_picked,
                       (gpointer) &kinds[k]);
      if (sel == G_MAXUINT)
        ns_row_set_subtitle (r, "None chosen");
    } else {
      r = ns_row_info (g, kinds[k].title, "No app installed");
    }
    g_object_set_data_full (G_OBJECT (r), "ns-apps", apps, (GDestroyNotify) g_ptr_array_unref);
  }
  ns_push (page);
}
