/* Neo Settings' own pages (see ns-pages.h): the hubs and the small pages. The bigger ones have files of their own
 * (ns-net.c, ns-bt.c, ns-sound.c, ns-apps.c, ns-shell.c, ns-privacy.c, ns-system.c). Nothing here blocks: D-Bus
 * answers fill the page when they come. */
#include "ns-pages.h"
#include "ns-parallax.h"
#include "ns-ui.h"

#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static GtkWidget *
page_row (GtkWidget *g, const char *icon, const char *color, const char *title, const char *sub, NsActivate page)
{
  GtkWidget *r = ns_row (g, icon, color, title, sub);
  ns_row_on_activate (r, page, NULL);
  return r;
}

static void
toggle (GtkWidget *row, gpointer sw)
{
  gtk_switch_set_active (sw, !gtk_switch_get_active (sw));
}

/* ---- Connections ---- */

/* Flight mode is gnome-settings-daemon's rfkill switch */
static void
airplane_set (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  if (g_object_get_data (G_OBJECT (sw), "ns-syncing"))
    return;
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.gnome.SettingsDaemon.Rfkill", "/org/gnome/SettingsDaemon/Rfkill",
                            "org.freedesktop.DBus.Properties", "Set",
                            g_variant_new ("(ssv)", "org.gnome.SettingsDaemon.Rfkill", "AirplaneMode",
                                           g_variant_new_boolean (gtk_switch_get_active (sw))),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
airplane_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) on = NULL;
  if (v) {
    g_variant_get (v, "(v)", &on);
    GtkWidget *sw = ns_row_get_switch (row);
    g_object_set_data (G_OBJECT (sw), "ns-syncing", GINT_TO_POINTER (1));
    gtk_switch_set_active (GTK_SWITCH (sw), g_variant_get_boolean (on));
    g_object_set_data (G_OBJECT (sw), "ns-syncing", NULL);
    gtk_widget_set_visible (row, TRUE);
  }
  g_object_unref (row);
}

void
ns_page_connections (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Connections", &c);
  GtkWidget *g = ns_group (c, NULL);
  page_row (g, "network-wireless-symbolic", "blue", "Wi-Fi", "Networks • Saved networks", ns_page_wifi);
  page_row (g, "bluetooth-symbolic", "blue", "Bluetooth", "Pair • Connected devices", ns_page_bluetooth);
  GtkWidget *air = ns_row (g, "airplane-mode-symbolic", "orange", "Flight mode",
                           "Turn off calls, messages and mobile data");
  GtkWidget *sw = gtk_switch_new ();
  ns_row_suffix (air, sw);
  ns_row_on_activate (air, toggle, sw);
  g_signal_connect (sw, "notify::active", G_CALLBACK (airplane_set), NULL);
  gtk_widget_set_visible (air, FALSE);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.gnome.SettingsDaemon.Rfkill", "/org/gnome/SettingsDaemon/Rfkill",
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", "org.gnome.SettingsDaemon.Rfkill", "AirplaneMode"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, airplane_got,
                            g_object_ref (air));
  g = ns_group (c, NULL);
  page_row (g, "network-cellular-symbolic", "green", "Mobile network", "Mobile data • Access points • SIM",
            ns_page_mobile);
  page_row (g, "network-wireless-hotspot-symbolic", "green", "Mobile hotspot", "Share the mobile data",
            ns_page_hotspot);
  page_row (g, "network-wired-symbolic", "violet", "Network and VPN", "VPN • USB • Proxy", ns_page_network);
  g = ns_group (c, NULL);
  page_row (g, "emblem-shared-symbolic", "orange", "Sharing", "Phone name • Visibility", ns_page_sharing);
  /* printing needs CUPS, which a phone rarely has: GNOME Settings' page, only when it is there */
  g_autofree char *cups = g_find_program_in_path ("lpstat");
  if (cups)
    page_row (g, "printer-symbolic", "grey", "Printers", NULL, ns_page_printers);
  ns_push (page);
}

/* ---- Display ---- */

/* Night light is a gamma table the compositor gives the display: a CRTC with a GAMMA_LUT (or a legacy gamma ramp)
 * can take one. None that can (the S10e's DECON): no row. No card to ask: assume it can. */
static gboolean
display_has_gamma (void)
{
  static int known = -1;
  if (known >= 0)
    return known;
  gboolean asked = FALSE, gamma = FALSE;
  for (int i = 0; i < 8 && !gamma; i++) {
    g_autofree char *path = g_strdup_printf ("/dev/dri/card%d", i);
    int fd = open (path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      continue;
    drmModeRes *res = drmModeGetResources (fd);
    for (int c = 0; res && c < res->count_crtcs && !gamma; c++) {
      asked = TRUE;
      drmModeCrtc *crtc = drmModeGetCrtc (fd, res->crtcs[c]);
      gamma = crtc && crtc->gamma_size > 0;
      drmModeFreeCrtc (crtc);
      drmModeObjectProperties *props = drmModeObjectGetProperties (fd, res->crtcs[c], DRM_MODE_OBJECT_CRTC);
      for (guint p = 0; props && p < props->count_props && !gamma; p++) {
        drmModePropertyRes *pr = drmModeGetProperty (fd, props->props[p]);
        gamma = pr && !g_strcmp0 (pr->name, "GAMMA_LUT");
        drmModeFreeProperty (pr);
      }
      drmModeFreeObjectProperties (props);
    }
    drmModeFreeResources (res);
    close (fd);
  }
  known = gamma || !asked;
  return known;
}

typedef struct {
  char *name;
  int   max;
} Backlight;

static Backlight *
backlight (void)
{
  static Backlight *bl;
  static gboolean looked;
  if (looked)
    return bl;
  looked = TRUE;
  g_autoptr (GDir) d = g_dir_open ("/sys/class/backlight", 0, NULL);
  const char *n = d ? g_dir_read_name (d) : NULL;
  if (!n)
    return NULL;
  g_autofree char *path = g_build_filename ("/sys/class/backlight", n, "max_brightness", NULL);
  g_autofree char *t = NULL;
  if (!g_file_get_contents (path, &t, NULL, NULL))
    return NULL;
  bl = g_new0 (Backlight, 1);
  bl->name = g_strdup (n);
  bl->max = MAX (1, atoi (t));
  return bl;
}

static double
brightness_get (Backlight *bl)
{
  g_autofree char *path = g_build_filename ("/sys/class/backlight", bl->name, "brightness", NULL);
  g_autofree char *t = NULL;
  return g_file_get_contents (path, &t, NULL, NULL) ? 100.0 * atoi (t) / bl->max : 50;
}

/* logind lets the session's user set it, no root needed */
static void
brightness_set (GtkRange *r, Backlight *bl)
{
  guint v = MAX (1, (guint) (gtk_range_get_value (r) * bl->max / 100 + 0.5));
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                            "org.freedesktop.login1.Session", "SetBrightness",
                            g_variant_new ("(ssu)", "backlight", bl->name, v), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                            NULL, NULL, NULL);
}

static gboolean
dark_get (GValue *value, GVariant *v, gpointer data)
{
  g_value_set_boolean (value, !g_strcmp0 (g_variant_get_string (v, NULL), "prefer-dark"));
  return TRUE;
}

static GVariant *
dark_set (const GValue *value, const GVariantType *type, gpointer data)
{
  return g_variant_new_string (g_value_get_boolean (value) ? "prefer-dark" : "default");
}

static GtkWidget *
dark_row (GtkWidget *g, GSettings *iface)
{
  GtkWidget *r = ns_row_switch (g, "Dark mode", "Dark backgrounds for apps and the system", NULL, NULL);
  g_settings_bind_with_mapping (iface, "color-scheme", ns_row_get_switch (r), "active", G_SETTINGS_BIND_DEFAULT,
                                dark_get, dark_set, NULL, NULL);
  return r;
}

static const double text_scales[] = { 0.9, 1.0, 1.15, 1.3 };
static const char *const text_labels[] = { "Small", "Default", "Large", "Larger", NULL };
static const guint timeouts[] = { 30, 60, 120, 300, 600, 0 };
static const char *const timeout_labels[] = { "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes",
                                              "Never", NULL };

static void
text_picked (GtkWidget *row, guint i, gpointer iface)
{
  g_settings_set_double (iface, "text-scaling-factor", text_scales[i]);
}

static void
timeout_picked (GtkWidget *row, guint i, gpointer session)
{
  g_settings_set_uint (session, "idle-delay", timeouts[i]);
}

void
ns_page_display (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Display", &c);
  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");

  GtkWidget *g = ns_group (c, NULL);
  dark_row (g, iface);
  GSettings *color = ns_page_settings (page, "org.gnome.settings-daemon.plugins.color");
  if (color && display_has_gamma ())
    ns_row_switch (g, "Night light", "Warmer colours in the evening", color, "night-light-enabled");

  Backlight *bl = backlight ();
  if (bl) {
    g = ns_group (c, NULL);
    GtkWidget *scale;
    ns_row_scale (g, "Brightness", "display-brightness-symbolic", 1, 100, 1, &scale);
    gtk_range_set_value (GTK_RANGE (scale), brightness_get (bl));
    g_signal_connect (scale, "value-changed", G_CALLBACK (brightness_set), bl);
  }

  g = ns_group (c, NULL);
  double f = g_settings_get_double (iface, "text-scaling-factor");
  guint ti = 1;
  for (guint i = 0; i < G_N_ELEMENTS (text_scales); i++)
    if (ABS (text_scales[i] - f) < 0.01)
      ti = i;
  ns_row_pick (g, "Font size", text_labels, ti, text_picked, iface);
  ns_page_scale_row (g);

  GSettings *session = ns_page_settings (page, "org.gnome.desktop.session");
  guint idle = g_settings_get_uint (session, "idle-delay"), sel = 1;
  for (guint i = 0; i < G_N_ELEMENTS (timeouts); i++)
    if (timeouts[i] == idle)
      sel = i;
  g = ns_group (c, NULL);
  ns_row_pick (g, "Screen timeout", timeout_labels, sel, timeout_picked, session);
  GSettings *touch = ns_page_settings (page, "org.gnome.settings-daemon.peripherals.touchscreen");
  if (touch) {
    GtkWidget *r = ns_row_switch (g, "Auto rotate", "Turn the screen with the phone", NULL, NULL);
    g_settings_bind (touch, "orientation-lock", ns_row_get_switch (r), "active", G_SETTINGS_BIND_INVERT_BOOLEAN);
  }
  ns_push (page);
}

/* ---- Battery ---- */

static char *
duration (gint64 s)
{
  if (s <= 0)
    return NULL;
  return s >= 3600 ? g_strdup_printf ("%d h %d min", (int) (s / 3600), (int) (s % 3600 / 60))
                   : g_strdup_printf ("%d min", (int) (s / 60));
}

typedef struct {
  GtkWidget *big, *bar, *state;
} Charge;

static void
charge_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Charge *ch = g_object_get_data (page, "charge");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  if (v)
    g_variant_get (v, "(@a{sv})", &props);
  double p = 0;
  guint32 st = 0;
  gint64 empty = 0, full = 0;
  if (props) {
    g_variant_lookup (props, "Percentage", "d", &p);
    g_variant_lookup (props, "State", "u", &st);
    g_variant_lookup (props, "TimeToEmpty", "x", &empty);
    g_variant_lookup (props, "TimeToFull", "x", &full);
  }
  g_autofree char *big = g_strdup_printf ("%.0f%%", p);
  gtk_label_set_text (GTK_LABEL (ch->big), big);
  gtk_level_bar_set_value (GTK_LEVEL_BAR (ch->bar), p);
  g_autofree char *left = duration (empty);
  g_autofree char *tofull = duration (full);
  g_autofree char *say = st == 1 ? (tofull ? g_strdup_printf ("Charging • full in %s", tofull) : g_strdup ("Charging"))
                       : st == 4 ? g_strdup ("Fully charged")
                       : left ? g_strdup_printf ("About %s left", left) : g_strdup ("On battery");
  gtk_label_set_text (GTK_LABEL (ch->state), props ? say : "No battery information");
  g_object_unref (page);
}

void
ns_page_battery (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Battery", &c);
  Charge *ch = g_new0 (Charge, 1);
  g_object_set_data_full (G_OBJECT (page), "charge", ch, g_free);

  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  ch->big = gtk_label_new ("–");
  gtk_label_set_xalign (GTK_LABEL (ch->big), 0);
  gtk_widget_add_css_class (ch->big, "ns-huge");
  gtk_box_append (GTK_BOX (v), ch->big);
  ch->bar = gtk_level_bar_new_for_interval (0, 100);
  gtk_box_append (GTK_BOX (v), ch->bar);
  ch->state = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (ch->state), 0);
  gtk_widget_add_css_class (ch->state, "ns-sub");
  gtk_box_append (GTK_BOX (v), ch->state);
  ns_row_widget (g, NULL, v);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.UPower", "/org/freedesktop/UPower/devices/DisplayDevice",
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", "org.freedesktop.UPower.Device"), G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, charge_got, g_object_ref (page));

  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");
  g = ns_group (c, NULL);
  ns_row_switch (g, "Show battery percentage", "In the status bar", iface, "show-battery-percentage");
  page_row (g, NULL, NULL, "Power and screen blank", "Automatic suspend • Power button", ns_page_power);
  ns_push (page);
}

/* ---- Wallpaper and style ---- */

static const char *const accents[][2] = {
  { "blue", "#3584e4" }, { "teal", "#2190a4" }, { "green", "#3a944a" }, { "yellow", "#c88800" },
  { "orange", "#ed5b00" }, { "red", "#e62d42" }, { "pink", "#d56199" }, { "purple", "#9141ac" },
  { "slate", "#6f8396" },
};

static void
accent_toggled (GtkToggleButton *b, GSettings *iface)
{
  if (gtk_toggle_button_get_active (b))
    g_settings_set_enum (iface, "accent-color", GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "ns-accent")));
}

static GtkWidget *
accent_picker (GSettings *iface)
{
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 5);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 9);
  int cur = g_settings_get_enum (iface, "accent-color");
  GtkWidget *first = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (accents); i++) {
    GtkWidget *b = gtk_toggle_button_new ();
    gtk_widget_set_tooltip_text (b, accents[i][0]);
    g_autofree char *cls = g_strconcat ("ns-accent-", accents[i][0], NULL);
    gtk_widget_add_css_class (b, "ns-accent-dot");
    gtk_widget_add_css_class (b, cls);
    gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
    if (first)
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b), GTK_TOGGLE_BUTTON (first));
    else
      first = b;
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b), (int) i == cur);
    g_object_set_data (G_OBJECT (b), "ns-accent", GINT_TO_POINTER (i));
    g_signal_connect (b, "toggled", G_CALLBACK (accent_toggled), iface);
    gtk_flow_box_append (GTK_FLOW_BOX (flow), b);
  }
  return flow;
}

static void
picture_changed (GSettings *bg, const char *key, GtkWidget *row)
{
  g_autofree char *uri = g_settings_get_string (bg, "picture-uri");
  g_autoptr (GFile) f = g_file_new_for_uri (uri);
  g_autofree char *base = g_file_get_basename (f);
  ns_row_set_subtitle (row, base);
}

void
ns_page_wallpaper (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Wallpaper and style", &c);
  GSettings *bg = ns_page_settings (page, "org.gnome.desktop.background");
  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");

  GSettings *neo = ns_page_settings (page, "de.yesman.neo");
  if (neo) {
    GtkWidget *g = ns_group (c, "Parallax wallpaper");
    GtkWidget *hint = gtk_label_new ("Layers that move as you tilt the phone. Off shows the picture below.");
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_xalign (GTK_LABEL (hint), 0);
    gtk_widget_add_css_class (hint, "ns-sub");
    GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
    gtk_box_append (GTK_BOX (v), hint);
    gtk_box_append (GTK_BOX (v), ns_parallax_grid (neo));
    ns_row_widget (g, NULL, v);
  }

  GtkWidget *g = ns_group (c, "Picture");
  GtkWidget *r = page_row (g, "preferences-desktop-wallpaper-symbolic", "pink", "Wallpapers", NULL,
                           ns_page_wallpapers);
  picture_changed (bg, NULL, r);
  g_signal_connect_object (bg, "changed::picture-uri", G_CALLBACK (picture_changed), r, 0);

  g = ns_group (c, "Style");
  dark_row (g, iface);
  ns_row_widget (g, "Accent colour", accent_picker (iface));
  ns_push (page);
}

/* ---- Home screen (Neo) ---- */

static const char *const shape_values[] = { "system", "circle", "squircle", "rounded", "square", "teardrop",
                                            "cupertino", "cylinder", "egg", "octagon", "hexagon", "diamond", NULL };
static const char *const shape_labels[] = { "Pack / system", "Circle", "Squircle", "Rounded square", "Square",
                                            "Teardrop", "Cupertino", "Cylinder", "Egg", "Octagon", "Hexagon",
                                            "Diamond", NULL };
static const char *const launcher_values[] = { "neo", "phosh", NULL };
static const char *const launcher_labels[] = { "Neo", "Phosh's app grid", NULL };

void
ns_page_home (GtkWidget *row, gpointer data)
{
  GSettings *neo = ns_settings ("de.yesman.neo");
  if (!neo)
    return;
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Home screen", &c);
  g_object_set_data_full (G_OBJECT (page), "neo", neo, g_object_unref);

  /* what fits, the launcher's own rule (neo_home_max_columns/rows): a column is 66 px, a row 84 px below ≈ 200 px of
   * status bar, widgets, page dots, dock and home bar */
  int w = 360, h = 760;
  GListModel *mons = gdk_display_get_monitors (gdk_display_get_default ());
  g_autoptr (GdkMonitor) m = g_list_model_get_n_items (mons) ? g_list_model_get_item (mons, 0) : NULL;
  if (m) {
    GdkRectangle r;
    gdk_monitor_get_geometry (m, &r);
    w = MIN (r.width, r.height);
    h = MAX (r.width, r.height);
  }
  int cols = MAX (3, w / 66), rows = MAX (3, (h - 200) / 84);

  GtkWidget *g = ns_group (c, "Layout");
  ns_row_spin (g, "Columns", neo, "desktop-grid-columns", 3, cols);
  ns_row_spin (g, "Rows", neo, "desktop-grid-rows", 3, rows);
  ns_row_spin (g, "Dock icons", neo, "dock-num-icons", 2, cols);
  ns_row_spin (g, "App drawer columns", neo, "drawer-grid-columns", 3, cols);

  g = ns_group (c, NULL);
  ns_row_switch (g, "Weather and clock", "On the first page", neo, "home-widgets");
  ns_row_switch (g, "Hide app names", NULL, neo, "desktop-hide-app-labels");
  ns_row_switch (g, "Search in the app drawer", NULL, neo, "search-drawer-enabled");
  /* read by the patched Phosh: what the home gesture opens */
  if (ns_has_key (neo, "launcher"))
    ns_row_choice (g, "Home gesture opens", neo, "launcher", launcher_values, launcher_labels);

  /* icon packs: what is installed */
  GStrvBuilder *vb = g_strv_builder_new (), *lb = g_strv_builder_new ();
  g_strv_builder_add (vb, "");
  g_strv_builder_add (lb, "None (theme icons)");
  g_autoptr (GDir) d = g_dir_open ("/usr/share/neo-launcher/iconpacks", 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    g_strv_builder_add (vb, n);
    g_autofree char *t = g_strdup (n);
    t[0] = g_ascii_toupper (t[0]);
    g_strv_builder_add (lb, t);
  }
  char **values = g_strv_builder_end (vb), **labels = g_strv_builder_end (lb);
  g_strv_builder_unref (vb);
  g_strv_builder_unref (lb);
  g_object_set_data_full (G_OBJECT (page), "packs", values, (GDestroyNotify) g_strfreev);
  g_object_set_data_full (G_OBJECT (page), "pack-labels", labels, (GDestroyNotify) g_strfreev);
  g = ns_group (c, "Icons");
  ns_row_choice (g, "Icon pack", neo, "icon-pack", (const char *const *) values, (const char *const *) labels);
  ns_row_choice (g, "Icon shape", neo, "icon-shape", shape_values, shape_labels);
  ns_row_switch (g, "Wrap unthemed icons", "Icons the pack does not cover get its background", neo,
                 "icon-wrap-unthemed");
  if (ns_has_key (neo, "icon-shape-legacy"))
    ns_row_switch (g, "Shape legacy icons", "Icons without pack art on a shaped background", neo,
                   "icon-shape-legacy");

  g = ns_group (c, NULL);
  page_row (g, "preferences-desktop-wallpaper-symbolic", "pink", "Wallpaper and style", "Parallax • Picture",
            ns_page_wallpaper);
  ns_push (page);
}

/* ---- General management ---- */

void
ns_page_general (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("General management", &c);
  GtkWidget *g = ns_group (c, NULL);
  page_row (g, "preferences-desktop-locale-symbolic", "violet", "Language and region", "Language • Formats",
            ns_page_language);
  page_row (g, "preferences-system-time-symbolic", "blue", "Date and time", "Automatic time • Time zone",
            ns_page_datetime);
  page_row (g, "input-keyboard-symbolic", "grey", "Keyboard", "On-screen keyboard • Layouts", ns_page_keyboard);
  page_row (g, "input-mouse-symbolic", "grey", "Mouse and touchpad", "Pointer speed • Hardware keyboard",
            ns_page_mouse);
  g = ns_group (c, NULL);
  page_row (g, "system-users-symbolic", "orange", "Users", "Your account • Password", ns_page_users);
  page_row (g, "view-grid-symbolic", "blue", "Default apps", "Browser • Email • Music", ns_page_default_apps);
  ns_push (page);
}

/* ---- About phone ---- */

static char *
first_line_after (const char *file, const char *key)
{
  g_autofree char *t = NULL;
  if (!g_file_get_contents (file, &t, NULL, NULL))
    return NULL;
  g_auto (GStrv) lines = g_strsplit (t, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    if (g_str_has_prefix (lines[i], key)) {
      char *v = g_strstrip (g_strdup (lines[i] + strlen (key)));
      if (*v == '"' && v[strlen (v) - 1] == '"') {
        v[strlen (v) - 1] = 0;
        memmove (v, v + 1, strlen (v));
      }
      return v;
    }
  return NULL;
}

static void
pretty_got (GObject *src, GAsyncResult *res, gpointer name)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) hn = NULL;
  if (v)
    g_variant_get (v, "(v)", &hn);
  const char *pretty = hn ? g_variant_get_string (hn, NULL) : "";
  if (*pretty)
    gtk_label_set_text (GTK_LABEL (name), pretty);
  g_object_unref (name);
}

void
ns_page_about (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("About phone", &c);

  g_autofree char *model = NULL;
  g_file_get_contents ("/sys/firmware/devicetree/base/model", &model, NULL, NULL);
  if (model)
    g_strstrip (model);

  /* the head: the phone's name and model, big */
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (head, "ns-about-head");
  GtkWidget *name = gtk_label_new (g_get_host_name ());
  gtk_widget_add_css_class (name, "ns-big");
  GtkWidget *ml = gtk_label_new (model ? model : "");
  gtk_widget_add_css_class (ml, "ns-sub");
  gtk_box_append (GTK_BOX (head), name);
  gtk_box_append (GTK_BOX (head), ml);
  gtk_box_append (GTK_BOX (c), head);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.hostname1", "/org/freedesktop/hostname1",
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", "org.freedesktop.hostname1", "PrettyHostname"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, pretty_got,
                            g_object_ref (name));

  GtkWidget *g = ns_group (c, NULL);
  g_autofree char *os = first_line_after ("/etc/os-release", "PRETTY_NAME=");
  struct utsname u;
  uname (&u);
  ns_row_info (g, "Operating system", os);
  ns_row_info (g, "Kernel", u.release);
  g_autofree char *mem = first_line_after ("/proc/meminfo", "MemTotal:");
  g_autofree char *memh = mem ? g_format_size_full ((guint64) g_ascii_strtoull (mem, NULL, 10) * 1024,
                                                    G_FORMAT_SIZE_IEC_UNITS) : NULL;
  ns_row_info (g, "Memory", memh);
  g_autoptr (GFile) root = g_file_new_for_path ("/");
  g_autoptr (GFileInfo) fi = g_file_query_filesystem_info (root, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE ","
                                                           G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, NULL);
  g_autofree char *st = NULL;
  if (fi) {
    g_autofree char *size = g_format_size (g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));
    g_autofree char *free_ = g_format_size (g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
    st = g_strdup_printf ("%s free of %s", free_, size);
  }
  ns_row_info (g, "Storage", st);
  g_autofree char *cpus = g_strdup_printf ("%u cores", g_get_num_processors ());
  ns_row_info (g, "Processor", cpus);

  g = ns_group (c, NULL);
  page_row (g, NULL, NULL, "Software information", "Phone name • Versions", ns_page_software);
  ns_push (page);
}
