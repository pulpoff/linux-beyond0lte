/* Lock screen and shell: what Phosh's Mobile Settings has, in our pages. The lock type (Swipe / Pattern / PIN,
 * de.yesman.neo lock-type with Phosh's require-unlock, the pattern set by ns-pattern.c), Phosh's plugins (quick
 * settings and lock screen widgets: sm.puri.phosh.plugins, their own preference dialogs loaded from
 * $libdir/phosh/plugins/prefs as Mobile Settings does), the top bar and app overview, the on-screen keyboard
 * (Stevia's or Squeekboard's settings, which one through the Phosh-OSK alternative) and emergency alerts. */
#include "ns-pages.h"
#include "ns-pattern.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

static gboolean
not_none_get (GValue *value, GVariant *v, gpointer data)
{
  g_value_set_boolean (value, g_strcmp0 (g_variant_get_string (v, NULL), "none") != 0);
  return TRUE;
}

/* `data` is what "on" writes */
static GVariant *
not_none_set (const GValue *value, const GVariantType *type, gpointer data)
{
  return g_variant_new_string (g_value_get_boolean (value) ? data : "none");
}

static GtkWidget *
not_none_row (GtkWidget *g, const char *title, const char *sub, GSettings *s, const char *key, const char *on)
{
  GtkWidget *r = ns_row_switch (g, title, sub, NULL, NULL);
  g_settings_bind_with_mapping (s, key, ns_row_get_switch (r), "active", G_SETTINGS_BIND_DEFAULT, not_none_get,
                                not_none_set, (gpointer) on, NULL);
  return r;
}

/* ---- Phosh's plugins ---- */

typedef struct {
  char *id, *name, *comment, *icon, *prefs;
} Plugin;

static void
plugin_free (Plugin *p)
{
  g_free (p->id);
  g_free (p->name);
  g_free (p->comment);
  g_free (p->icon);
  g_free (p->prefs);
  g_free (p);
}

/* $libdir/phosh/plugins: under /usr/lib/<triplet> on Debian */
static char *
plugins_dir (void)
{
  static char *dir;
  if (dir)
    return dir;
  g_autoptr (GDir) d = g_dir_open ("/usr/lib", 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    g_autofree char *p = g_build_filename ("/usr/lib", n, "phosh", "plugins", NULL);
    if (g_file_test (p, G_FILE_TEST_IS_DIR))
      return dir = g_steal_pointer (&p);
  }
  return dir = g_strdup ("/usr/lib/phosh/plugins");
}

static GPtrArray *
plugins (const char *type)
{
  GPtrArray *all = g_ptr_array_new_with_free_func ((GDestroyNotify) plugin_free);
  g_autoptr (GDir) d = g_dir_open (plugins_dir (), 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    if (!g_str_has_suffix (n, ".plugin"))
      continue;
    g_autofree char *path = g_build_filename (plugins_dir (), n, NULL);
    g_autoptr (GKeyFile) kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
      continue;
    g_auto (GStrv) types = g_key_file_get_string_list (kf, "Plugin", "Types", NULL, NULL);
    g_autofree char *so = g_key_file_get_string (kf, "Plugin", "Plugin", NULL);
    if (!types || !g_strv_contains ((const char *const *) types, type) || !so || !g_file_test (so, G_FILE_TEST_EXISTS)
        || g_key_file_get_boolean (kf, "Plugin", "NoDisplay", NULL))
      continue;
    Plugin *p = g_new0 (Plugin, 1);
    p->id = g_key_file_get_string (kf, "Plugin", "Id", NULL);
    p->name = g_key_file_get_locale_string (kf, "Plugin", "Name", NULL, NULL);
    p->comment = g_key_file_get_locale_string (kf, "Plugin", "Comment", NULL, NULL);
    /* some comments carry markup ("<b>This plugin is experimental.</b>"): the text alone */
    char *plain = NULL;
    if (p->comment && pango_parse_markup (p->comment, -1, 0, NULL, &plain, NULL, NULL)) {
      g_free (p->comment);
      p->comment = plain;
    }
    p->icon = g_key_file_get_string (kf, "Plugin", "Icon", NULL);
    if (g_key_file_has_key (kf, "Prefs", "Plugin", NULL))
      p->prefs = g_key_file_get_string (kf, "Prefs", "Id", NULL);
    /* "Caffeine Quick Setting" in a list titled Quick settings: "Caffeine" */
    if (p->name && g_str_has_suffix (p->name, " Quick Setting"))
      p->name[strlen (p->name) - strlen (" Quick Setting")] = 0;
    if (p->id && p->name)
      g_ptr_array_add (all, p);
    else
      plugin_free (p);
  }
  return all;
}

/* a plugin's preferences: its own dialog, a GIO module next to the plugins */
static void
plugin_prefs (GtkButton *b, Plugin *p)
{
  const char *ep = g_object_get_data (G_OBJECT (b), "ns-ep");
  static gboolean scanned;
  if (!scanned) {
    scanned = TRUE;
    g_io_extension_point_register ("phosh-quick-setting-widget-prefs");
    g_io_extension_point_register ("phosh-lockscreen-widget-prefs");
    g_autofree char *dir = g_build_filename (plugins_dir (), "prefs", NULL);
    g_io_modules_scan_all_in_directory (dir);
  }
  GIOExtension *ext = g_io_extension_point_get_extension_by_name (g_io_extension_point_lookup (ep), p->prefs);
  if (!ext)
    return;
  GObject *o = g_object_new (g_io_extension_get_type (ext), NULL);
  if (ADW_IS_DIALOG (o))
    adw_dialog_present (ADW_DIALOG (o), GTK_WIDGET (b));
  else if (GTK_IS_WINDOW (o)) {
    gtk_window_set_transient_for (GTK_WINDOW (o), GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (b))));
    gtk_window_present (GTK_WINDOW (o));
  } else
    g_object_unref (o);
}

typedef struct {
  GSettings *s;
  char      *key, *type, *ep;
  GPtrArray *all;
  GtkWidget *group;
} Plugins;

static void plugins_fill (AdwNavigationPage *page);

static void
plugins_write (Plugins *ps, GStrv ids)
{
  g_settings_set_strv (ps->s, ps->key, (const char *const *) ids);
}

static void
plugin_toggled (GtkSwitch *sw, GParamSpec *pspec, AdwNavigationPage *page)
{
  Plugins *ps = g_object_get_data (G_OBJECT (page), "ns-plugins");
  if (ns_switch_syncing (GTK_WIDGET (sw)))
    return;
  const char *id = g_object_get_data (G_OBJECT (sw), "ns-id");
  g_auto (GStrv) cur = g_settings_get_strv (ps->s, ps->key);
  GStrvBuilder *b = g_strv_builder_new ();
  for (guint i = 0; cur[i]; i++)
    if (g_strcmp0 (cur[i], id))
      g_strv_builder_add (b, cur[i]);
  if (gtk_switch_get_active (sw))
    g_strv_builder_add (b, id);
  g_auto (GStrv) nv = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  plugins_write (ps, nv);
  ns_later (page);
}

/* one place earlier among the enabled ones */
static void
plugin_up (GtkButton *b, AdwNavigationPage *page)
{
  Plugins *ps = g_object_get_data (G_OBJECT (page), "ns-plugins");
  const char *id = g_object_get_data (G_OBJECT (b), "ns-id");
  g_auto (GStrv) cur = g_settings_get_strv (ps->s, ps->key);
  for (guint i = 1; cur[i]; i++)
    if (!g_strcmp0 (cur[i], id)) {
      char *t = cur[i - 1];
      cur[i - 1] = cur[i];
      cur[i] = t;
      break;
    }
  plugins_write (ps, cur);
  ns_later (page);
}

static void
plugins_fill (AdwNavigationPage *page)
{
  Plugins *ps = g_object_get_data (G_OBJECT (page), "ns-plugins");
  g_auto (GStrv) on = g_settings_get_strv (ps->s, ps->key);
  ns_group_clear (ps->group);
  /* the enabled ones in their order, then the rest */
  for (int pass = 0; pass < 2; pass++) {
    guint n = pass == 0 ? g_strv_length (on) : ps->all->len;
    for (guint i = 0; i < n; i++) {
      Plugin *p = NULL;
      if (pass == 0) {
        for (guint k = 0; k < ps->all->len && !p; k++)
          if (!g_strcmp0 (((Plugin *) ps->all->pdata[k])->id, on[i]))
            p = ps->all->pdata[k];
      } else if (!g_strv_contains ((const char *const *) on, ((Plugin *) ps->all->pdata[i])->id)) {
        p = ps->all->pdata[i];
      }
      if (!p)
        continue;
      g_autofree char *icon = p->icon ? g_strconcat (p->icon, "-symbolic", NULL) : NULL;
      const char *fallback = "application-x-addon-symbolic", *colour = "blue";
      /* the lock screen widgets ship no icon: give the known ones their own */
      static const struct { const char *id, *icon, *colour; } known[] = {
        { "emergency-info", "dialog-information-symbolic", "red" },
        { "ticket-box", "x-office-document-symbolic", "orange" },
        { "upcoming-events", "x-office-calendar-symbolic", "green" },
        { "calendar", "x-office-calendar-symbolic", "green" },
        { "media-players", "audio-x-generic-symbolic", "purple" },
        { "launcher-box", "view-app-grid-symbolic", "blue" },
      };
      for (guint k = 0; k < G_N_ELEMENTS (known); k++)
        if (!g_strcmp0 (p->id, known[k].id)) {
          fallback = known[k].icon;
          colour = known[k].colour;
        }
      GtkWidget *r = ns_row_toggle (ps->group, icon ? icon : fallback,
                                    pass == 0 ? colour : "grey", p->name, p->comment);
      /* the switch goes last, after the buttons */
      GtkWidget *sw = g_object_ref (ns_row_get_switch (r));
      gtk_box_remove (GTK_BOX (g_object_get_data (G_OBJECT (r), "ns-box")), sw);
      if (pass == 0 && i > 0) {
        GtkWidget *up = gtk_button_new_from_icon_name ("go-up-symbolic");
        gtk_widget_add_css_class (up, "flat");
        gtk_widget_add_css_class (up, "circular");
        gtk_widget_set_tooltip_text (up, "Move up");
        g_object_set_data (G_OBJECT (up), "ns-id", p->id);
        g_signal_connect (up, "clicked", G_CALLBACK (plugin_up), page);
        ns_row_suffix (r, up);
      }
      if (p->prefs) {
        GtkWidget *gear = gtk_button_new_from_icon_name ("emblem-system-symbolic");
        gtk_widget_add_css_class (gear, "flat");
        gtk_widget_add_css_class (gear, "circular");
        g_object_set_data (G_OBJECT (gear), "ns-ep", ps->ep);
        g_signal_connect (gear, "clicked", G_CALLBACK (plugin_prefs), p);
        ns_row_suffix (r, gear);
      }
      ns_row_suffix (r, sw);
      g_object_unref (sw);
      ns_switch_quiet (sw, pass == 0);
      g_object_set_data (G_OBJECT (sw), "ns-id", p->id);
      g_signal_connect (sw, "notify::active", G_CALLBACK (plugin_toggled), page);
    }
  }
}

static void
plugins_free (Plugins *ps)
{
  g_object_unref (ps->s);
  g_free (ps->key);
  g_free (ps->type);
  g_free (ps->ep);
  g_ptr_array_unref (ps->all);
  g_free (ps);
}

static void
plugins_page (GtkWidget *row, gpointer lockscreen)
{
  GSettings *s = ns_settings ("sm.puri.phosh.plugins");
  if (!s)
    return;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (lockscreen ? "Lock screen widgets" : "Quick settings", &c);
  Plugins *ps = g_new0 (Plugins, 1);
  ps->s = s;
  ps->key = g_strdup (lockscreen ? "lock-screen" : "quick-settings");
  ps->ep = g_strdup (lockscreen ? "phosh-lockscreen-widget-prefs" : "phosh-quick-setting-widget-prefs");
  ps->all = plugins (lockscreen ? "lockscreen" : "quick-setting");
  g_object_set_data_full (G_OBJECT (page), "ns-plugins", ps, (GDestroyNotify) plugins_free);
  ns_later_init (page, plugins_fill);
  gtk_icon_theme_add_search_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                  "/usr/share/phosh/plugins/icons");
  ns_note (c, lockscreen ? "Widgets shown on the lock screen, swiped through below the clock."
                         : "Extra tiles in the quick settings, after the built-in ones. The order here is theirs.");
  ps->group = ns_group (c, NULL);
  plugins_fill (page);
  ns_push (page);
}

/* ---- the lock type ---- */

static const char *const lock_labels[] = { "Swipe", "Pattern", "PIN", NULL };
static const char *const lock_types[] = { "swipe", "pattern", "pin" };

typedef struct {
  GSettings *neo, *lock;
  GtkWidget *type, *change, *shuffle;
} Lock;

static guint
lock_index (Lock *l)
{
  if (!g_settings_get_boolean (l->lock, "require-unlock"))
    return 0;
  g_autofree char *t = g_settings_get_string (l->neo, "lock-type");
  return !g_strcmp0 (t, "pattern") ? 1 : 2;
}

static void
lock_show (Lock *l)
{
  guint i = lock_index (l);
  ns_row_pick_select (l->type, i);
  gtk_widget_set_visible (l->change, i != 0);
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (l->change), "ns-title-label")),
                      i == 1 ? "Change pattern" : "Change PIN");
  if (l->shuffle)
    gtk_widget_set_visible (l->shuffle, i == 2);
}

static void
lock_apply (Lock *l, guint i)
{
  g_settings_set_string (l->neo, "lock-type", lock_types[i]);
  g_settings_set_boolean (l->lock, "require-unlock", i != 0);
}

typedef struct {
  Lock  *l;
  guint  i;
  GtkWidget *row;
} LockSet;

static void
secret_done (gboolean ok, gpointer data)
{
  LockSet *ls = data;
  if (ok)
    lock_apply (ls->l, ls->i);
  lock_show (ls->l);
  g_object_unref (ls->row);
  g_free (ls);
}

static void
secret_dialog (Lock *l, GtkWidget *row, guint i)
{
  LockSet *ls = g_new0 (LockSet, 1);
  ls->l = l;
  ls->i = i;
  ls->row = g_object_ref (row);
  ns_pattern_dialog_present (row, i == 2, secret_done, ls);
}

static void
lock_picked (GtkWidget *row, guint i, gpointer data)
{
  Lock *l = data;
  if (i == lock_index (l))
    return;
  /* a pattern counts once it is drawn, confirmed and set as the password; a PIN keeps the password there is
   * (it is typed on Phosh's keypad), "Change PIN" sets a new one */
  if (i == 1) {
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
    secret_dialog (l, row, i);
    lock_show (l);
    return;
  }
  lock_apply (l, i);
}

static void
change_tapped (GtkWidget *row, gpointer data)
{
  Lock *l = data;
  secret_dialog (l, row, lock_index (l));
}

static const guint delays[] = { 0, 30, 60, 120, 300, 900 };
static const char *const delay_labels[] = { "Immediately", "30 seconds", "1 minute", "2 minutes", "5 minutes",
                                            "15 minutes", NULL };

static void
delay_picked (GtkWidget *row, guint i, gpointer ss)
{
  g_settings_set_uint (ss, "lock-delay", delays[i]);
}

static void
lock_free (Lock *l)
{
  g_signal_handlers_disconnect_by_data (l->neo, l);
  g_signal_handlers_disconnect_by_data (l->lock, l);
  g_object_unref (l->neo);
  g_object_unref (l->lock);
  g_free (l);
}

void
ns_page_lock_screen (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Lock screen", &c);
  GSettings *neo = ns_settings ("de.yesman.neo"), *lock = ns_settings ("sm.puri.phosh.lockscreen");
  GtkWidget *g = ns_group (c, NULL);
  if (neo && lock && ns_has_key (neo, "lock-type") && ns_has_key (lock, "require-unlock")) {
    Lock *l = g_new0 (Lock, 1);
    l->neo = neo;
    l->lock = lock;
    g_object_set_data_full (G_OBJECT (page), "ns-lock", l, (GDestroyNotify) lock_free);
    l->type = ns_row_pick (g, "Screen lock type", lock_labels, lock_index (l), lock_picked, l);
    l->change = ns_row (g, NULL, NULL, "Change PIN", "It is also your password");
    ns_row_on_activate (l->change, change_tapped, l);
    if (ns_has_key (lock, "shuffle-keypad"))
      l->shuffle = ns_row_switch (g, "Shuffle the keypad", "The digits in a new order each time", lock,
                                  "shuffle-keypad");
    g_signal_connect_swapped (neo, "changed::lock-type", G_CALLBACK (lock_show), l);
    g_signal_connect_swapped (lock, "changed::require-unlock", G_CALLBACK (lock_show), l);
    lock_show (l);
    ns_note (c, "Swipe unlocks without a password. Pattern and PIN unlock with your password: a pattern or PIN set "
                "here becomes your password (also for sudo, SSH and the console).");
  } else {
    g_clear_object (&neo);
    if (lock) {
      g_object_set_data_full (G_OBJECT (page), "ns-lock-s", lock, g_object_unref);
      ns_row_switch (g, "Require a PIN or password", NULL, lock, "require-unlock");
      ns_row_switch (g, "Shuffle the keypad", NULL, lock, "shuffle-keypad");
    }
  }

  GSettings *ss = ns_page_settings (page, "org.gnome.desktop.screensaver");
  g = ns_group (c, NULL);
  ns_row_switch (g, "Lock automatically", "When the screen turns off", ss, "lock-enabled");
  guint d = g_settings_get_uint (ss, "lock-delay"), sel = 0;
  for (guint i = 0; i < G_N_ELEMENTS (delays); i++)
    if (delays[i] <= d)
      sel = i;
  GtkWidget *r = ns_row_pick (g, "Lock after the screen turns off", delay_labels, sel, delay_picked, ss);
  g_settings_bind (ss, "lock-enabled", r, "sensitive", G_SETTINGS_BIND_GET);

  g = ns_group (c, "On the lock screen");
  GSettings *n = ns_page_settings (page, "org.gnome.desktop.notifications");
  ns_row_switch (g, "Notifications", "Show them on the lock screen", n, "show-in-lock-screen");
  not_none_row (g, "Wallpaper", "A picture behind the clock, or plain", ss, "picture-options", "zoom");
  if (ns_settings_exists ("sm.puri.phosh.plugins")) {
    r = ns_row (g, NULL, NULL, "Widgets", "Media, calendar, launchers…");
    ns_row_on_activate (r, plugins_page, GINT_TO_POINTER (1));
  }
  ns_push (page);
}

/* ---- Lock screen and shell ---- */

/* the alerts come only from a modem with cell broadcast: the row shows once ModemManager says one has it */
static void
cbc_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  gboolean any = FALSE;
  if (v) {
    g_autoptr (GVariant) objs = g_variant_get_child_value (v, 0);
    GVariantIter it;
    GVariant *ifaces;
    g_variant_iter_init (&it, objs);
    while (!any && g_variant_iter_next (&it, "{&o@a{sa{sv}}}", NULL, &ifaces)) {
      g_autoptr (GVariant) cb = g_variant_lookup_value (ifaces, "org.freedesktop.ModemManager1.Modem.CellBroadcast",
                                                        NULL);
      any = cb != NULL;
      g_variant_unref (ifaces);
    }
  }
  gtk_widget_set_visible (row, any);
  gtk_list_box_invalidate_headers (GTK_LIST_BOX (gtk_widget_get_parent (row)));
  g_object_unref (row);
}

static void
alerts_page (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Emergency alerts", &c);
  GSettings *cbd = ns_page_settings (page, "org.freedesktop.cbd");
  GtkWidget *g = ns_group (c, "Alerts from the mobile network");
  ns_row_flag (g, "Extreme threats", "To life and safety", cbd, "levels", "extreme");
  ns_row_flag (g, "Severe threats", "To life and safety", cbd, "levels", "severe");
  ns_row_flag (g, "Public safety", "What to do in a risky situation", cbd, "levels", "public-safety");
  ns_row_flag (g, "Amber alerts", "Child abduction emergencies", cbd, "levels", "amber");
  ns_row_flag (g, "Test alerts", "Periodic tests of the alert system", cbd, "levels", "test");
  ns_note (c, "Presidential alerts are always shown. Alerts need a SIM and a modem that receives them.");
  ns_push (page);
}

void
ns_page_lockshell (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Lock screen and shell", &c);
  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *r = ns_row (g, "system-lock-screen-symbolic", "blue", "Lock screen", "Screen lock type • Widgets");
  ns_row_on_activate (r, ns_page_lock_screen, NULL);
  if (ns_settings_exists ("sm.puri.phosh.plugins")) {
    r = ns_row (g, "view-app-grid-symbolic", "violet", "Quick settings", "Extra tiles and their order");
    ns_row_on_activate (r, plugins_page, NULL);
  }
  r = ns_row (g, "input-keyboard-symbolic", "grey", "Keyboard", "On-screen keyboard • Layouts");
  ns_row_on_activate (r, ns_page_keyboard, NULL);

  GSettings *phosh = ns_page_settings (page, "sm.puri.phosh");
  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");
  g = ns_group (c, "Status bar");
  ns_row_switch (g, "Show battery percentage", NULL, iface, "show-battery-percentage");
  if (ns_has_key (phosh, "shell-layout"))
    not_none_row (g, "Avoid the camera cutout", "Nothing under notches and display cutouts", phosh, "shell-layout",
                  "device");

  if (phosh) {
    g = ns_group (c, "App overview");
    if (ns_has_key (phosh, "app-filter-mode"))
      ns_row_flag (g, "Only mobile apps", "Hide apps not made for a phone screen", phosh, "app-filter-mode",
                   "adaptive");
    GSettings *bg = ns_page_settings (page, "org.gnome.desktop.background");
    not_none_row (g, "Wallpaper", "A picture behind the apps, or plain", bg, "picture-options", "zoom");
  }

  g = ns_group (c, "Advanced");
  GSettings *em = ns_page_settings (page, "sm.puri.phosh.emergency-calls");
  if (ns_has_key (em, "enabled"))
    ns_row_switch (g, "Emergency calls", "Only if you know they work with your provider", em, "enabled");
  if (ns_has_key (phosh, "enable-suspend"))
    ns_row_switch (g, "Suspend in the system menu", "Mobile data and alarms may be missed", phosh, "enable-suspend");
  GSettings *phoc = ns_page_settings (page, "sm.puri.phoc");
  if (ns_has_key (phoc, "scale-to-fit"))
    ns_row_switch (g, "Scale down all apps", "A last resort for apps that do not fit", phoc, "scale-to-fit");
  if (ns_settings_exists ("org.freedesktop.cbd")) {
    r = ns_row (g, NULL, NULL, "Emergency alerts", "Cell broadcast warnings");
    ns_row_on_activate (r, alerts_page, NULL);
    gtk_widget_set_visible (r, FALSE);
    g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (bus)
      g_dbus_connection_call (bus, "org.freedesktop.ModemManager1", "/org/freedesktop/ModemManager1",
                              "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                              G_VARIANT_TYPE ("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, cbc_got,
                              g_object_ref (r));
  }
  ns_push (page);
}

/* ---- Keyboard ---- */

static gboolean
squeekboard (void)
{
  g_autofree char *t = g_file_read_link ("/etc/alternatives/Phosh-OSK", NULL);
  return t && strstr (t, "Squeekboard");
}

static void
osk_restarted (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  if (!g_subprocess_wait_check_finish (G_SUBPROCESS (src), res, &err))
    g_warning ("Restarting the keyboard: %s", err->message);
}

static void
osk_selected (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  if (!g_subprocess_wait_check_finish (G_SUBPROCESS (src), res, &err)) {
    ns_row_pick_select (row, squeekboard ());
  } else {
    /* the user manager resolved mobi.phosh.OSK.service through the alternative at its last reload */
    g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, NULL, "sh", "-c",
                                                  "systemctl --user daemon-reload && "
                                                  "systemctl --user restart mobi.phosh.OSK.service", NULL);
    if (p)
      g_subprocess_wait_check_async (p, NULL, osk_restarted, NULL);
  }
  g_object_unref (row);
}

static void
osk_picked (GtkWidget *row, guint i, gpointer data)
{
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, NULL, "pkexec", "/usr/libexec/neo-osk-select",
                                                i ? "squeekboard" : "stevia", NULL);
  if (p)
    g_subprocess_wait_check_async (p, NULL, osk_selected, g_object_ref (row));
}

static const double unfolds[] = { 0.5, 1.0, 2.0 };
static const char *const unfold_labels[] = { "Short", "Normal", "Long", NULL };

static void
unfold_picked (GtkWidget *row, guint i, gpointer phosh)
{
  g_settings_set_double (phosh, "osk-unfold-delay", unfolds[i]);
}

static const double heights[] = { 0.75, 0.9, 1.0, 1.15, 1.3, 1.5 };
static const char *const height_labels[] = { "75%", "90%", "100%", "115%", "130%", "150%", NULL };

static void
height_picked (GtkWidget *row, guint i, gpointer sq)
{
  g_settings_set_double (sq, g_object_get_data (G_OBJECT (row), "ns-key"), heights[i]);
}

static void
height_row (GtkWidget *g, GSettings *sq, const char *title, const char *key)
{
  if (!ns_has_key (sq, key))
    return;
  double v = g_settings_get_double (sq, key);
  guint sel = 2;
  for (guint i = 0; i < G_N_ELEMENTS (heights); i++)
    if (ABS (heights[i] - v) < 0.01)
      sel = i;
  GtkWidget *r = ns_row_pick (g, title, height_labels, sel, height_picked, sq);
  g_object_set_data (G_OBJECT (r), "ns-key", (gpointer) key);
}

/* "keep the physical size": both automatic scalings at once */
static gboolean
size_get (GValue *value, GVariant *v, gpointer data)
{
  g_autofree const char **f = g_variant_get_strv (v, NULL);
  g_value_set_boolean (value, g_strv_contains (f, "auto-portrait") || g_strv_contains (f, "auto-landscape"));
  return TRUE;
}

static GVariant *
size_set (const GValue *value, const GVariantType *type, gpointer osk)
{
  g_auto (GStrv) cur = g_settings_get_strv (osk, "scaling");
  GStrvBuilder *b = g_strv_builder_new ();
  if (g_strv_contains ((const char *const *) cur, "bottom-dead-zone"))
    g_strv_builder_add (b, "bottom-dead-zone");
  if (g_value_get_boolean (value)) {
    g_strv_builder_add (b, "auto-portrait");
    g_strv_builder_add (b, "auto-landscape");
  }
  g_auto (GStrv) nv = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return g_variant_new_strv ((const char *const *) nv, -1);
}

static void
completer_picked (GtkWidget *row, guint i, gpointer comp)
{
  GPtrArray *ids = g_object_get_data (G_OBJECT (row), "ns-ids");
  g_settings_set_string (comp, "default", ids->pdata[i]);
}

static void
completer_row (AdwNavigationPage *page, GtkWidget *g)
{
  GSettings *comp = ns_page_settings (page, "mobi.phosh.osk.Completers");
  if (!comp)
    return;
  GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (ids, g_strdup (""));
  g_ptr_array_add (labels, g_strdup ("Automatic"));
  g_autoptr (GDir) d = g_dir_open ("/usr/share/phosh-osk-stevia/completers", 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    g_autofree char *path = g_build_filename ("/usr/share/phosh-osk-stevia/completers", n, NULL);
    g_autoptr (GKeyFile) kf = g_key_file_new ();
    if (!g_str_has_suffix (n, ".completer") || !g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
      continue;
    char *id = g_key_file_get_string (kf, "Completer", "Id", NULL);
    char *name = g_key_file_get_locale_string (kf, "Completer", "Name", NULL, NULL);
    if (id && name) {
      g_ptr_array_add (ids, id);
      g_ptr_array_add (labels, name);
    } else {
      g_free (id);
      g_free (name);
    }
  }
  g_ptr_array_add (labels, NULL);
  g_autofree char *cur = g_settings_get_string (comp, "default");
  guint sel = 0;
  for (guint i = 0; i < ids->len; i++)
    if (!g_strcmp0 (ids->pdata[i], cur))
      sel = i;
  GtkWidget *r = ns_row_pick (g, "Word suggestions from", (const char *const *) labels->pdata, sel,
                              completer_picked, comp);
  g_object_set_data_full (G_OBJECT (r), "ns-ids", ids, (GDestroyNotify) g_ptr_array_unref);
  if (ns_has_key (comp, "auto-space"))
    ns_row_switch (g, "Space after punctuation", "Swaps the space and the mark after a completed word", comp,
                   "auto-space");
}

/* ---- keyboard layouts (org.gnome.desktop.input-sources), named from Stevia's list ---- */

typedef struct {
  GSettings *src;
  GPtrArray *ids, *names;      /* "xkb:us" and "English (US)", every layout Stevia knows */
  GtkWidget *group;
} Layouts;

static void
layouts_free (Layouts *l)
{
  g_ptr_array_unref (l->ids);
  g_ptr_array_unref (l->names);
  g_free (l);
}

static const char *
layout_name (Layouts *l, const char *type, const char *id)
{
  g_autofree char *key = g_strconcat (type, ":", id, NULL);
  for (guint i = 0; i < l->ids->len; i++)
    if (!g_strcmp0 (l->ids->pdata[i], key))
      return l->names->pdata[i];
  return id;
}

static void layouts_fill (AdwNavigationPage *page);

static void
layouts_set (Layouts *l, GVariantBuilder *b)
{
  g_settings_set_value (l->src, "sources", g_variant_builder_end (b));
}

static void
layout_remove (GtkButton *btn, AdwNavigationPage *page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  guint drop = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (btn), "ns-index"));
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ss)"));
  for (guint i = 0; i < g_variant_n_children (v); i++)
    if (i != drop)
      g_variant_builder_add_value (&b, g_variant_get_child_value (v, i));
  layouts_set (l, &b);
}

static void
layout_added (GtkWidget *row, gpointer data)
{
  AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  const char *key = l->ids->pdata[GPOINTER_TO_UINT (data)];
  g_auto (GStrv) tk = g_strsplit (key, ":", 2);
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ss)"));
  for (guint i = 0; i < g_variant_n_children (v); i++)
    g_variant_builder_add_value (&b, g_variant_get_child_value (v, i));
  g_variant_builder_add (&b, "(ss)", tk[0], tk[1]);
  layouts_set (l, &b);
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
}

static void
layout_add (GtkWidget *row, gpointer page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  GtkWidget *c;
  AdwNavigationPage *add = ns_page_new ("Add a layout", &c);
  GtkWidget *g = ns_group (c, NULL);
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  for (guint i = 0; i < l->ids->len; i++) {
    gboolean have = FALSE;
    for (guint k = 0; k < g_variant_n_children (v) && !have; k++) {
      const char *t, *id;
      g_variant_get_child (v, k, "(&s&s)", &t, &id);
      g_autofree char *key = g_strconcat (t, ":", id, NULL);
      have = !g_strcmp0 (key, l->ids->pdata[i]);
    }
    if (have)
      continue;
    GtkWidget *r = ns_row (g, NULL, NULL, l->names->pdata[i], NULL);
    g_object_set_data (G_OBJECT (r), "ns-page", page);
    ns_row_on_activate (r, layout_added, GUINT_TO_POINTER (i));
  }
  ns_push (add);
}

static void
layouts_fill (AdwNavigationPage *page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  ns_group_clear (l->group);
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  guint n = g_variant_n_children (v);
  for (guint i = 0; i < n; i++) {
    const char *t, *id;
    g_variant_get_child (v, i, "(&s&s)", &t, &id);
    GtkWidget *r = ns_row (l->group, NULL, NULL, layout_name (l, t, id), i == 0 ? "Used first" : NULL);
    if (n > 1) {
      GtkWidget *x = gtk_button_new_from_icon_name ("user-trash-symbolic");
      gtk_widget_add_css_class (x, "flat");
      gtk_widget_add_css_class (x, "circular");
      gtk_widget_set_tooltip_text (x, "Remove");
      g_object_set_data (G_OBJECT (x), "ns-index", GUINT_TO_POINTER (i));
      g_signal_connect (x, "clicked", G_CALLBACK (layout_remove), page);
      ns_row_suffix (r, x);
    }
  }
  ns_row_button (l->group, "Add a layout", FALSE, layout_add, page);
}

static void
layouts_rows (AdwNavigationPage *page, GtkWidget *c)
{
  GSettings *src = ns_page_settings (page, "org.gnome.desktop.input-sources");
  if (!src)
    return;
  Layouts *l = g_new0 (Layouts, 1);
  l->src = src;
  l->ids = g_ptr_array_new_with_free_func (g_free);
  l->names = g_ptr_array_new_with_free_func (g_free);
  g_object_set_data_full (G_OBJECT (page), "ns-layouts", l, (GDestroyNotify) layouts_free);
  g_autoptr (JsonParser) p = json_parser_new ();
  if (json_parser_load_from_file (p, "/usr/share/phosh-osk-stevia/layouts.json", NULL)
      && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p))) {
    JsonObject *o = json_node_get_object (json_parser_get_root (p));
    JsonArray *a = json_object_has_member (o, "layouts") ? json_object_get_array_member (o, "layouts") : NULL;
    for (guint i = 0; a && i < json_array_get_length (a); i++) {
      JsonObject *e = json_array_get_object_element (a, i);
      const char *type = json_object_get_string_member_with_default (e, "type", NULL);
      const char *id = json_object_get_string_member_with_default (e, "layout-id", NULL);
      const char *name = json_object_get_string_member_with_default (e, "name", NULL);
      if (type && id && name) {
        g_ptr_array_add (l->ids, g_strconcat (type, ":", id, NULL));
        g_ptr_array_add (l->names, g_strdup (name));
      }
    }
  }
  l->group = ns_group (c, "Layouts");
  ns_later_init (page, layouts_fill);
  g_signal_connect_object (src, "changed::sources", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  layouts_fill (page);
}

void
ns_page_keyboard (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Keyboard", &c);
  GSettings *a11y = ns_page_settings (page, "org.gnome.desktop.a11y.applications");
  GSettings *phosh = ns_page_settings (page, "sm.puri.phosh");
  GtkWidget *g = ns_group (c, NULL);
  ns_row_switch (g, "On-screen keyboard", "Shown when text is typed", a11y, "screen-keyboard-enabled");
  gboolean sq = squeekboard ();
  if (g_file_test ("/usr/libexec/neo-osk-select", G_FILE_TEST_IS_EXECUTABLE))
    ns_row_pick (g, "Keyboard", (const char *const[]) { "Stevia", "Squeekboard", NULL }, sq, osk_picked, NULL);
  if (ns_has_key (phosh, "osk-unfold-delay")) {
    double d = g_settings_get_double (phosh, "osk-unfold-delay");
    ns_row_pick (g, "Long press on the home bar", unfold_labels, d >= 1.5 ? 2 : d >= 1.0 ? 1 : 0, unfold_picked,
                 phosh);
  }

  GSettings *osk = sq ? NULL : ns_page_settings (page, "mobi.phosh.osk");
  if (osk) {
    g = ns_group (c, "Typing");
    completer_row (page, g);
    ns_row_flag (g, "Suggest words", "When the app says it helps", osk, "completion-mode", "hint");
    ns_row_flag (g, "Suggestions from the menu", "Switch them on and off with the keyboard's menu key", osk,
                 "completion-mode", "manual");
    ns_row_flag (g, "Key preview", "The pressed key pops up above your finger", osk, "osk-features",
                 "key-indicator");
    g = ns_group (c, "Size");
    GtkWidget *r = ns_row_switch (g, "Keep the physical size", "Large enough on a high resolution screen", NULL, NULL);
    g_settings_bind_with_mapping (osk, "scaling", ns_row_get_switch (r), "active", G_SETTINGS_BIND_DEFAULT,
                                  size_get, size_set, osk, NULL);
    ns_row_flag (g, "Space below the keys", "Moves the keyboard up a little", osk, "scaling", "bottom-dead-zone");
    ns_row_switch (g, "Ignore a hardware keyboard", "Show it even with a keyboard connected", osk,
                   "ignore-hw-keyboards");
  }
  GSettings *sqs = sq ? ns_page_settings (page, "sm.puri.Squeekboard") : NULL;
  if (sqs) {
    g = ns_group (c, "Size");
    height_row (g, sqs, "Height, upright", "scale-in-vertical-screen-orientation");
    height_row (g, sqs, "Height, sideways", "scale-in-horizontal-screen-orientation");
  }
  layouts_rows (page, c);
  ns_push (page);
}
