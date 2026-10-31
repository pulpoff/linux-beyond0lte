/* Neo Settings: the phone's settings app, One UI style, in C and GTK4. The main menu groups everything the way a
 * phone does; each entry is one of our own pages (ns-pages.h), or an app (Software). The services the pages
 * follow (NetworkManager, BlueZ, the sound server) are connected at start, in the background. */
#include "ns-bt.h"
#include "ns-net.h"
#include "ns-pages.h"
#include "ns-sound.h"
#include "ns-ui.h"

typedef struct {
  const char *icon, *color, *title, *subtitle, *keywords;
  NsActivate  page;            /* our own page, or */
  const char *const *argv;     /* another app */
  const char *needs_schema;    /* shown only if this settings schema is installed */
} Entry;

static const char *const software[] = { "gnome-software", "--mode=updates", NULL };

/* NULL title: the next card */
static const Entry entries[] = {
  { "network-wireless-symbolic", "blue", "Connections", "Wi-Fi • Bluetooth • Mobile network",
    "wlan wifi internet sim data vpn hotspot flight airplane", ns_page_connections },
  { "bluetooth-symbolic", "blue", "Connected devices", "Bluetooth • Pair headphones and more",
    "pair headphones speaker watch", ns_page_bluetooth },
  { NULL },
  { "audio-volume-high-symbolic", "violet", "Sounds and vibration", "Sound mode • Volume • Ringtone",
    "ringtone speaker microphone vibrate mute silent haptic", ns_page_sound },
  { "preferences-system-notifications-symbolic", "orange", "Notifications", "App notifications • Do not disturb",
    "dnd banner wake", ns_page_notifications },
  { NULL },
  { "display-brightness-symbolic", "lime", "Display", "Brightness • Dark mode • Font size",
    "screen timeout night light rotate scale", ns_page_display },
  { "battery-full-symbolic", "green", "Battery", "Charge • Power and screen blank",
    "power suspend percentage", ns_page_battery },
  { NULL },
  { "preferences-desktop-wallpaper-symbolic", "pink", "Wallpaper and style", "Wallpapers • Parallax • Colour palette",
    "background accent dark theme", ns_page_wallpaper },
  { "view-app-grid-symbolic", "pink", "Home screen", "Layout • Icons • Widgets",
    "launcher neo grid dock drawer icon pack", ns_page_home, NULL, "de.yesman.neo" },
  { "system-lock-screen-symbolic", "blue", "Lock screen and shell", "Lock screen • Quick settings • Keyboard",
    "phosh panel pattern pin swipe osk keyboard widgets", ns_page_lockshell },
  { NULL },
  { "security-high-symbolic", "purple", "Security and privacy", "Screen lock • Permissions • Usage",
    "password pin camera microphone history trash", ns_page_privacy },
  { "find-location-symbolic", "purple", "Location", "Location services • App access",
    "gps geoclue", ns_page_location, NULL, "org.gnome.system.location" },
  { NULL },
  { "avatar-default-symbolic", "blue", "Accounts", "Online accounts",
    "google nextcloud email", ns_page_accounts },
  { NULL },
  { "view-grid-symbolic", "blue", "Apps", "Default apps • App settings",
    "permissions storage", ns_page_apps },
  { "preferences-system-symbolic", "violet", "General management", "Language and keyboard • Date and time",
    "region locale clock users mouse touchpad", ns_page_general },
  { "preferences-desktop-accessibility-symbolic", "green", "Accessibility", "Vision • Interaction",
    "zoom contrast screen reader animations", ns_page_accessibility },
  { NULL },
  { "software-update-available-symbolic", "blue", "Software update", "Download and install",
    "upgrade packages", NULL, software },
  { "help-about-symbolic", "grey", "About phone", "Status • Legal information • Phone name",
    "model kernel version memory storage", ns_page_about },
};

static GPtrArray *groups;        /* the cards, to hide the ones a search empties */
static GtkWidget *search_entry, *title_box;

static void
activate_entry (GtkWidget *row, gpointer data)
{
  const Entry *e = data;
  if (e->page)
    e->page (row, NULL);
  else if (e->argv)
    ns_spawn (e->argv);
}

static gboolean
installed (const Entry *e)
{
  if (e->needs_schema) {
    g_autoptr (GSettings) s = ns_settings (e->needs_schema);
    if (!s)
      return FALSE;
  }
  if (e->argv) {
    g_autofree char *path = g_find_program_in_path (e->argv[0]);
    return path != NULL;
  }
  return TRUE;
}

/* ---- search: every word somewhere in the title, subtitle or keywords ---- */

static void
search_changed (GtkSearchEntry *entry, gpointer data)
{
  g_autofree char *q = g_utf8_strdown (gtk_editable_get_text (GTK_EDITABLE (entry)), -1);
  g_auto (GStrv) words = g_str_tokenize_and_fold (q, NULL, NULL);
  for (guint i = 0; i < groups->len; i++) {
    GtkWidget *box = groups->pdata[i];
    gboolean any = FALSE;
    for (GtkWidget *r = gtk_widget_get_first_child (box); r; r = gtk_widget_get_next_sibling (r)) {
      const char *text = g_object_get_data (G_OBJECT (r), "ns-search");
      if (!text)
        continue;
      gboolean all = TRUE;
      for (guint w = 0; words && words[w]; w++)
        all = all && strstr (text, words[w]);
      gtk_widget_set_visible (r, all);
      any = any || all;
    }
    gtk_widget_set_visible (box, any);
    gtk_list_box_invalidate_headers (GTK_LIST_BOX (box));
  }
}

static void
search_toggled (GtkToggleButton *b, gpointer data)
{
  gboolean on = gtk_toggle_button_get_active (b);
  gtk_widget_set_visible (search_entry, on);
  gtk_widget_set_visible (title_box, !on);
  if (on)
    gtk_widget_grab_focus (search_entry);
  else
    gtk_editable_set_text (GTK_EDITABLE (search_entry), "");
}

static AdwNavigationPage *
main_page (void)
{
  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* One UI's head: "Settings" large on the left, search on the right */
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (head, "ns-main-head");
  title_box = gtk_label_new ("Settings");
  gtk_label_set_xalign (GTK_LABEL (title_box), 0);
  gtk_widget_set_hexpand (title_box, TRUE);
  gtk_widget_add_css_class (title_box, "ns-big");
  search_entry = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_entry), "Search settings");
  gtk_widget_set_hexpand (search_entry, TRUE);
  gtk_widget_set_visible (search_entry, FALSE);
  g_signal_connect (search_entry, "search-changed", G_CALLBACK (search_changed), NULL);
  GtkWidget *find = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (find), "system-search-symbolic");
  gtk_widget_add_css_class (find, "flat");
  gtk_widget_add_css_class (find, "circular");
  gtk_widget_set_valign (find, GTK_ALIGN_CENTER);
  g_signal_connect (find, "toggled", G_CALLBACK (search_toggled), NULL);
  gtk_box_append (GTK_BOX (head), title_box);
  gtk_box_append (GTK_BOX (head), search_entry);
  gtk_box_append (GTK_BOX (head), find);
  gtk_box_append (GTK_BOX (outer), head);

  GtkWidget *sc = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (sc, TRUE);
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (col, "ns-column");
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), col);
  gtk_box_append (GTK_BOX (outer), sc);

  groups = g_ptr_array_new ();
  GtkWidget *g = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (entries); i++) {
    const Entry *e = &entries[i];
    if (!e->title) {
      g = NULL;
      continue;
    }
    if (!installed (e))
      continue;
    if (!g) {
      g = ns_group (col, NULL);
      g_ptr_array_add (groups, g);
    }
    GtkWidget *r = ns_row (g, e->icon, e->color, e->title, e->subtitle);
    g_autofree char *text = g_strjoin (" ", e->title, e->subtitle, e->keywords, NULL);
    g_object_set_data_full (G_OBJECT (r), "ns-search", g_utf8_strdown (text, -1), g_free);
    ns_row_on_activate (r, activate_entry, (gpointer) e);
  }

  AdwNavigationPage *page = adw_navigation_page_new (outer, "Settings");
  adw_navigation_page_set_tag (page, "main");
  gtk_widget_add_css_class (GTK_WIDGET (page), "ns-root");
  return page;
}

/* ---- the app ---- */

/* back (Alt+Left from Neo's edge swipe) and Escape: the page before, or out of Settings from the main menu */
static gboolean
go_back (GtkWidget *win, GVariant *args, gpointer nav)
{
  if (!adw_navigation_view_pop (nav))
    gtk_window_close (GTK_WINDOW (win));
  return TRUE;
}

static void
add_back (GtkWidget *win, GtkWidget *nav)
{
  GtkEventController *keys = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (keys), GTK_SHORTCUT_SCOPE_GLOBAL);
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_shortcut_trigger_parse_string ("Escape")),
                      gtk_callback_action_new (go_back, nav, NULL)));
  gtk_widget_add_controller (win, keys);
}

static void
dark_changed (AdwStyleManager *sm, GParamSpec *ps, GtkWidget *win)
{
  if (adw_style_manager_get_dark (sm))
    gtk_widget_add_css_class (win, "ns-dark");
  else
    gtk_widget_remove_css_class (win, "ns-dark");
}

static GtkWidget *nav_view;

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWindow *have = gtk_application_get_active_window (app);
  if (have) {
    gtk_window_present (have);
    return;
  }
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/de/yesman/NeoSettings/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);

  GtkWidget *win = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (win), "Settings");
  gtk_window_set_default_size (GTK_WINDOW (win), 360, 720);
  gtk_widget_add_css_class (win, "ns-window");
  AdwStyleManager *sm = adw_style_manager_get_default ();
  g_signal_connect_object (sm, "notify::dark", G_CALLBACK (dark_changed), win, 0);
  dark_changed (sm, NULL, win);
  GtkWidget *nav = nav_view = adw_navigation_view_new ();
  ns_ui_init (ADW_NAVIGATION_VIEW (nav));
  adw_navigation_view_add (ADW_NAVIGATION_VIEW (nav), main_page ());
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), nav);
  add_back (win, nav);
  gtk_window_present (GTK_WINDOW (win));
}

/* the app list, read once in the background so that pages listing apps find it read */
static void
apps_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  g_list_free_full (g_app_info_get_all (), g_object_unref);
  g_task_return_boolean (task, TRUE);
}

static void
startup (GApplication *app, gpointer data)
{
  /* the buses once, here: the pages then get them without waiting */
  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, NULL, NULL);
  ns_net_init ();
  ns_bt_init ();
  ns_sound_init ();
  g_autoptr (GTask) t = g_task_new (NULL, NULL, NULL, NULL);
  g_task_run_in_thread (t, apps_thread);
}

/* a page by name: `--page home` (the launcher's "Settings" opens the home screen's), and the panel names Phosh asks
 * for (its quick settings' long press, its status pages' "Settings" button): GNOME Settings' and Mobile
 * Settings' names, all answered here */
static const struct {
  const char *name;
  NsActivate  page;
} direct[] = {
  { "home", ns_page_home }, { "multitasking", ns_page_home },
  { "wallpaper", ns_page_wallpaper }, { "background", ns_page_wallpaper }, { "appearance", ns_page_wallpaper },
  { "wifi", ns_page_wifi }, { "wlan", ns_page_wifi },
  { "hotspot", ns_page_hotspot }, { "wifi-hotspot", ns_page_hotspot },
  { "wwan", ns_page_mobile }, { "mobile-broadband", ns_page_mobile }, { "mobile", ns_page_mobile },
  { "network", ns_page_network }, { "vpn", ns_page_network }, { "wired", ns_page_network },
  { "bluetooth", ns_page_bluetooth }, { "connections", ns_page_connections },
  { "sharing", ns_page_sharing }, { "printers", ns_page_printers },
  { "sound", ns_page_sound }, { "feedback", ns_page_sound },
  { "notifications", ns_page_notifications },
  { "power", ns_page_battery }, { "battery", ns_page_battery },
  { "display", ns_page_display }, { "displays", ns_page_display }, { "compositor", ns_page_display },
  { "lockscreen", ns_page_lock_screen }, { "lock-screen", ns_page_lock_screen }, { "screen-lock", ns_page_lock_screen },
  { "topbar", ns_page_lockshell }, { "quick-settings", ns_page_lockshell }, { "shell", ns_page_lockshell },
  { "keyboard", ns_page_keyboard }, { "osk", ns_page_keyboard }, { "mouse", ns_page_mouse },
  /* "privacy" comes from the location tile only */
  { "privacy", ns_page_location }, { "location", ns_page_location },
  { "online-accounts", ns_page_accounts }, { "accounts", ns_page_accounts },
  { "applications", ns_page_apps }, { "apps", ns_page_apps }, { "overview", ns_page_apps },
  { "default-apps", ns_page_default_apps },
  { "region", ns_page_language }, { "language", ns_page_language },
  { "datetime", ns_page_datetime }, { "date-time", ns_page_datetime },
  { "users", ns_page_users }, { "user-accounts", ns_page_users },
  { "system", ns_page_general },
  { "universal-access", ns_page_accessibility }, { "accessibility", ns_page_accessibility },
  { "info-overview", ns_page_about }, { "about", ns_page_about },
};

static void
open_page (GApplication *app, const char *name, const char *arg)
{
  g_application_activate (app);
  adw_navigation_view_pop_to_tag (ADW_NAVIGATION_VIEW (nav_view), "main");
  if (arg && *arg && (!g_strcmp0 (name, "applications") || !g_strcmp0 (name, "apps"))) {
    ns_page_app (arg);
    return;
  }
  for (guint i = 0; name && *name && i < G_N_ELEMENTS (direct); i++)
    if (g_str_equal (name, direct[i].name)) {
      direct[i].page (NULL, NULL);
      return;
    }
  if (name && *name)
    g_message ("no page for the panel '%s': the main menu", name);
}

/* org.gnome.Settings' "launch-panel" (and Mobile Settings' "set-panel"): (panel, [parameters]) */
static void
launch_panel (GSimpleAction *action, GVariant *param, gpointer app)
{
  const char *name = NULL;
  g_autoptr (GVariant) args = NULL;
  g_variant_get (param, "(&s@av)", &name, &args);
  /* the first parameter, a string: GNOME Settings' "applications" takes the app's id there */
  g_autoptr (GVariant) first = g_variant_n_children (args) ? g_variant_get_child_value (args, 0) : NULL;
  g_autoptr (GVariant) inner = first ? g_variant_get_variant (first) : NULL;
  const char *arg = inner && g_variant_is_of_type (inner, G_VARIANT_TYPE_STRING) ? g_variant_get_string (inner, NULL)
                                                                                   : NULL;
  open_page (app, name, arg);
}

static int
command_line (GApplication *app, GApplicationCommandLine *cl, gpointer data)
{
  const char *name = NULL, *id = NULL;
  GVariantDict *opts = g_application_command_line_get_options_dict (cl);
  g_variant_dict_lookup (opts, "page", "&s", &name);
  g_variant_dict_lookup (opts, "app", "&s", &id);
  if (id && !name)
    name = "applications";
  if (name)
    open_page (app, name, id);
  else
    g_application_activate (app);
  return 0;
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = adw_application_new ("de.yesman.NeoSettings", G_APPLICATION_HANDLES_COMMAND_LINE);
  const GOptionEntry options[] = {
    { "page", 0, 0, G_OPTION_ARG_STRING, NULL, "Open a page: home, wallpaper, wifi, hotspot, bluetooth, power…", "PAGE" },
    { "app", 0, 0, G_OPTION_ARG_STRING, NULL, "Open an app's page (its desktop file id)", "ID" },
    { NULL }
  };
  g_application_add_main_option_entries (G_APPLICATION (app), options);
  static const GActionEntry actions[] = {
    { .name = "launch-panel", .activate = launch_panel, .parameter_type = "(sav)" },
    { .name = "set-panel", .activate = launch_panel, .parameter_type = "(sav)" },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  g_signal_connect (app, "startup", G_CALLBACK (startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  g_signal_connect (app, "command-line", G_CALLBACK (command_line), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
