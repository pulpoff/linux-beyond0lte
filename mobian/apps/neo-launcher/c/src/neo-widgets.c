/* The home screen widgets (see neo-widgets.h).
 * Weather: where the phone is comes from GeoClue (city accuracy), the place's name from OpenStreetMap's Nominatim,
 * the weather from Open-Meteo (no key). Both web requests run in curl, away from the main loop; the last answer is
 * kept in ~/.cache/neo-launcher/weather.json, so the widget shows at once after a restart and refreshes every 30
 * minutes while the home screen shows.
 * Clock: drawn with cairo, redrawn once a second while it shows. */
#include "neo-widgets.h"

#include <gio/gdesktopappinfo.h>
#include <json-glib/json-glib.h>
#include <math.h>

#define REFRESH_S (30 * 60)
#define UA "neo-launcher/2.0 (Phosh home screen)"

typedef struct {
  void     (*open) (const char *id, gpointer data);
  gpointer   data;
  GtkWidget *strip, *box, *clock;   /* the current strip's (the home is rebuilt on settings changes) */
  GtkWidget *icon, *temp, *place, *pin;
  guint      tick, refresh;
  /* the weather */
  double     lat, lon, temp_c;
  int        code;
  gboolean   day, have;
  char      *place_name;
  gint64     fetched;          /* µs, wall clock */
  gboolean   busy;
  gboolean   named;            /* the place has its name already (GNOME Weather's city) */
  char      *geo_client;
} Widgets;

/* ---- the weather: what it looks like ---- */

static const char *
weather_icon (int code, gboolean day)
{
  /* WMO weather codes */
  if (code == 0)
    return day ? "weather-clear-symbolic" : "weather-clear-night-symbolic";
  if (code <= 2)
    return day ? "weather-few-clouds-symbolic" : "weather-few-clouds-night-symbolic";
  if (code == 3)
    return "weather-overcast-symbolic";
  if (code == 45 || code == 48)
    return "weather-fog-symbolic";
  if ((code >= 51 && code <= 57) || (code >= 80 && code <= 82))
    return "weather-showers-scattered-symbolic";
  if (code >= 61 && code <= 67)
    return "weather-showers-symbolic";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    return "weather-snow-symbolic";
  if (code >= 95)
    return "weather-storm-symbolic";
  return "weather-overcast-symbolic";
}

static void
show_weather (Widgets *w)
{
  if (!w->box)
    return;
  gtk_widget_set_visible (w->box, w->have);
  if (!w->have)
    return;
  gtk_image_set_from_icon_name (GTK_IMAGE (w->icon), weather_icon (w->code, w->day), GTK_ICON_SIZE_DIALOG);
  g_autofree char *t = g_strdup_printf ("%.0f°", w->temp_c);
  gtk_label_set_text (GTK_LABEL (w->temp), t);
  gtk_label_set_text (GTK_LABEL (w->place), w->place_name ? w->place_name : "");
  gtk_widget_set_visible (w->pin, w->place_name && *w->place_name);
}

static char *
cache_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "neo-launcher", "weather.json", NULL);
}

static void
save_cache (Widgets *w)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "lat"); json_builder_add_double_value (b, w->lat);
  json_builder_set_member_name (b, "lon"); json_builder_add_double_value (b, w->lon);
  json_builder_set_member_name (b, "temp"); json_builder_add_double_value (b, w->temp_c);
  json_builder_set_member_name (b, "code"); json_builder_add_int_value (b, w->code);
  json_builder_set_member_name (b, "day"); json_builder_add_boolean_value (b, w->day);
  json_builder_set_member_name (b, "place"); json_builder_add_string_value (b, w->place_name ? w->place_name : "");
  json_builder_set_member_name (b, "fetched"); json_builder_add_int_value (b, w->fetched);
  json_builder_end_object (b);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  g_autofree char *path = cache_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  json_generator_to_file (gen, path, NULL);
}

static void
load_cache (Widgets *w)
{
  g_autofree char *path = cache_path ();
  g_autoptr (JsonParser) p = json_parser_new ();
  if (!json_parser_load_from_file (p, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p)))
    return;
  JsonObject *o = json_node_get_object (json_parser_get_root (p));
  w->lat = json_object_get_double_member_with_default (o, "lat", 0);
  w->lon = json_object_get_double_member_with_default (o, "lon", 0);
  w->temp_c = json_object_get_double_member_with_default (o, "temp", 0);
  w->code = json_object_get_int_member_with_default (o, "code", 3);
  w->day = json_object_get_boolean_member_with_default (o, "day", TRUE);
  w->place_name = g_strdup (json_object_get_string_member_with_default (o, "place", ""));
  w->fetched = json_object_get_int_member_with_default (o, "fetched", 0);
  w->have = TRUE;
}

/* ---- the weather: asking the web, in curl ---- */

typedef void (*Got) (Widgets *w, JsonObject *o);

typedef struct {
  Widgets *w;
  Got      got;
} Req;

static void
curl_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Req *r = data;
  g_autofree char *out = NULL;
  JsonObject *o = NULL;
  g_autoptr (JsonParser) p = json_parser_new ();
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out &&
      json_parser_load_from_data (p, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p)))
    o = json_node_get_object (json_parser_get_root (p));
  r->got (r->w, o);
  g_free (r);
}

static void
get_json (Widgets *w, const char *url, Got got)
{
  const char *argv[] = { "curl", "-sf", "--max-time", "15", "-A", UA, url, NULL };
  GSubprocess *p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  Req *r = g_new0 (Req, 1);
  r->w = w;
  r->got = got;
  if (!p) {
    got (w, NULL);
    g_free (r);
    return;
  }
  g_subprocess_communicate_utf8_async (p, NULL, NULL, curl_done, r);
  g_object_unref (p);
}

static void
got_place (Widgets *w, JsonObject *o)
{
  JsonObject *a = o && json_object_has_member (o, "address") ? json_object_get_object_member (o, "address") : NULL;
  const char *keys[] = { "suburb", "city_district", "village", "town", "city", "municipality", "county" };
  for (guint i = 0; a && i < G_N_ELEMENTS (keys); i++)
    if (json_object_has_member (a, keys[i])) {
      g_free (w->place_name);
      w->place_name = g_strdup (json_object_get_string_member (a, keys[i]));
      break;
    }
  w->busy = FALSE;
  w->have = TRUE;
  g_message ("weather: %.0f° code %d", w->temp_c, w->code);
  save_cache (w);
  show_weather (w);
}

static void
got_weather (Widgets *w, JsonObject *o)
{
  JsonObject *c = o && json_object_has_member (o, "current") ? json_object_get_object_member (o, "current") : NULL;
  if (!c) {
    g_message ("weather: Open-Meteo gave no answer");
    w->busy = FALSE;
    return;
  }
  w->temp_c = json_object_get_double_member_with_default (c, "temperature_2m", 0);
  w->code = json_object_get_int_member_with_default (c, "weather_code", 3);
  w->day = json_object_get_int_member_with_default (c, "is_day", 1) != 0;
  w->fetched = g_get_real_time ();
  if (w->named) {
    got_place (w, NULL);
    return;
  }
  char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (lat, sizeof lat, "%.4f", w->lat);
  g_ascii_formatd (lon, sizeof lon, "%.4f", w->lon);
  const char *const *langs = g_get_language_names ();
  g_autofree char *lang = g_strndup (langs[0], 2);
  g_autofree char *url = g_strdup_printf ("https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=14"
                                          "&lat=%s&lon=%s&accept-language=%s", lat, lon,
                                          g_strcmp0 (lang, "C") ? lang : "en");
  get_json (w, url, got_place);
}

static void
fetch_weather (Widgets *w)
{
  char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (lat, sizeof lat, "%.4f", w->lat);
  g_ascii_formatd (lon, sizeof lon, "%.4f", w->lon);
  g_autofree char *url = g_strdup_printf ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
                                          "&current=temperature_2m,weather_code,is_day", lat, lon);
  get_json (w, url, got_weather);
}

/* ---- where the phone is: GeoClue, once per refresh ---- */

#define GC "org.freedesktop.GeoClue2"

static void
stop_geo (Widgets *w, GDBusConnection *bus)
{
  if (!w->geo_client)
    return;
  g_dbus_connection_call (bus, GC, w->geo_client, GC ".Client", "Stop", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, NULL, NULL);
  g_clear_pointer (&w->geo_client, g_free);
}

static double
location_prop (GDBusConnection *bus, const char *path, const char *name)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_sync (bus, GC, path, "org.freedesktop.DBus.Properties", "Get",
                                                        g_variant_new ("(ss)", GC ".Location", name),
                                                        G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
                                                        NULL);
  g_autoptr (GVariant) inner = NULL;
  if (!v)
    return NAN;
  g_variant_get (v, "(v)", &inner);
  return g_variant_get_double (inner);
}

static void
on_located (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
            GVariant *params, gpointer data)
{
  Widgets *w = data;
  const char *old, *now;
  g_variant_get (params, "(&o&o)", &old, &now);
  double lat = location_prop (bus, now, "Latitude"), lon = location_prop (bus, now, "Longitude");
  stop_geo (w, bus);
  g_message ("weather: located (accuracy: city)");
  if (isnan (lat) || isnan (lon)) {
    w->busy = FALSE;
    return;
  }
  w->lat = lat;
  w->lon = lon;
  fetch_weather (w);
}

static gboolean
geo_timeout (gpointer data)
{
  Widgets *w = data;
  if (w->busy && w->geo_client) {
    g_message ("weather: no location after 30 s");
    g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
    stop_geo (w, bus);
    /* no fix: the last place still has weather */
    if (w->have && (w->lat || w->lon))
      fetch_weather (w);
    else
      w->busy = FALSE;
  }
  return G_SOURCE_REMOVE;
}

static void
locate (Widgets *w)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  g_autoptr (GVariant) c = bus ? g_dbus_connection_call_sync (bus, GC, "/org/freedesktop/GeoClue2/Manager",
                                                              GC ".Manager", "GetClient", NULL,
                                                              G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, 3000,
                                                              NULL, NULL) : NULL;
  if (!c) {
    g_message ("weather: no GeoClue");
    geo_timeout (w);
    return;
  }
  g_variant_get (c, "(o)", &w->geo_client);
  const char *props = "org.freedesktop.DBus.Properties";
  g_dbus_connection_call_sync (bus, GC, w->geo_client, props, "Set",
                               g_variant_new ("(ssv)", GC ".Client", "DesktopId",
                                              g_variant_new_string ("de.yesman.NeoLauncher")),
                               NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  g_dbus_connection_call_sync (bus, GC, w->geo_client, props, "Set",
                               g_variant_new ("(ssv)", GC ".Client", "RequestedAccuracyLevel",
                                              g_variant_new_uint32 (4)),      /* city */
                               NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  static guint sub;
  if (!sub)
    sub = g_dbus_connection_signal_subscribe (bus, GC, GC ".Client", "LocationUpdated", NULL, NULL,
                                              G_DBUS_SIGNAL_FLAGS_NONE, on_located, w, NULL);
  g_dbus_connection_call (bus, GC, w->geo_client, GC ".Client", "Start", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                          NULL, NULL, NULL);
  g_timeout_add_seconds (30, geo_timeout, w);
}

/* The first city in GNOME Weather (org.gnome.Weather locations: serialized GWeatherLocations, (uv) with
 * (name, code, current, [(lat, lon)], [(parent lat, lon)]), radians): the widget shows the place the app shows. */
static gboolean
weather_app_city (Widgets *w)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = src ? g_settings_schema_source_lookup (src, "org.gnome.Weather", TRUE) : NULL;
  if (!schema || !g_settings_schema_has_key (schema, "locations"))
    return FALSE;
  static GSettings *settings;
  if (!settings)
    settings = g_settings_new_full (schema, NULL, NULL);
  g_autoptr (GVariant) all = g_settings_get_value (settings, "locations");
  if (g_variant_n_children (all) == 0)
    return FALSE;
  g_autoptr (GVariant) first = g_variant_get_child_value (all, 0);     /* v */
  g_autoptr (GVariant) loc = g_variant_get_variant (first);              /* (uv) */
  if (!g_variant_is_of_type (loc, G_VARIANT_TYPE ("(uv)")))
    return FALSE;
  g_autoptr (GVariant) inner = NULL;
  guint32 version;
  g_variant_get (loc, "(uv)", &version, &inner);
  if (!g_variant_is_of_type (inner, G_VARIANT_TYPE ("(ssba(dd)a(dd))")))
    return FALSE;
  const char *name, *code;
  gboolean current;
  g_autoptr (GVariantIter) own = NULL, parent = NULL;
  g_variant_get (inner, "(&s&sba(dd)a(dd))", &name, &code, &current, &own, &parent);
  double lat, lon;
  if (!g_variant_iter_next (own, "(dd)", &lat, &lon) && !g_variant_iter_next (parent, "(dd)", &lat, &lon))
    return FALSE;
  w->lat = lat * 180 / G_PI;
  w->lon = lon * 180 / G_PI;
  g_free (w->place_name);
  w->place_name = g_strdup (name);
  w->named = TRUE;
  return TRUE;
}

static void
refresh (Widgets *w)
{
  if (w->busy)
    return;
  if (w->have && g_get_real_time () - w->fetched < (REFRESH_S - 60) * G_USEC_PER_SEC)
    return;
  w->busy = TRUE;
  if (weather_app_city (w)) {
    g_message ("weather: %s (GNOME Weather's city)", w->place_name);
    fetch_weather (w);
    return;
  }
  w->named = FALSE;
  g_message ("weather: locating");
  locate (w);
}

static gboolean
refresh_tick (gpointer data)
{
  refresh (data);
  return G_SOURCE_CONTINUE;
}

/* ---- the clock ---- */

static gboolean
draw_clock (GtkWidget *area, cairo_t *cr, Widgets *w)
{
  int aw = gtk_widget_get_allocated_width (area), ah = gtk_widget_get_allocated_height (area);
  double side = MIN (aw, ah), x0 = aw - side, y0 = (ah - side) / 2, r = side / 2, cx = x0 + r, cy = y0 + r;
  /* One UI's square black face */
  double rr = side * 0.06;
  cairo_new_sub_path (cr);
  cairo_arc (cr, x0 + side - rr, y0 + rr, rr, -G_PI / 2, 0);
  cairo_arc (cr, x0 + side - rr, y0 + side - rr, rr, 0, G_PI / 2);
  cairo_arc (cr, x0 + rr, y0 + side - rr, rr, G_PI / 2, G_PI);
  cairo_arc (cr, x0 + rr, y0 + rr, rr, G_PI, 1.5 * G_PI);
  cairo_close_path (cr);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_fill (cr);

  PangoLayout *l = gtk_widget_create_pango_layout (area, NULL);
  PangoFontDescription *f = pango_font_description_from_string ("Sans");
  pango_font_description_set_absolute_size (f, side * 0.085 * PANGO_SCALE);
  pango_layout_set_font_description (l, f);
  cairo_set_source_rgb (cr, 1, 1, 1);
  for (int h = 1; h <= 12; h++) {
    char n[4];
    g_snprintf (n, sizeof n, "%d", h);
    pango_layout_set_text (l, n, -1);
    int tw, th;
    pango_layout_get_pixel_size (l, &tw, &th);
    double a = h * G_PI / 6 - G_PI / 2;
    cairo_move_to (cr, cx + cos (a) * r * 0.78 - tw / 2.0, cy + sin (a) * r * 0.78 - th / 2.0);
    pango_cairo_show_layout (cr, l);
  }
  pango_font_description_free (f);
  g_object_unref (l);

  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  double s = g_date_time_get_second (now), m = g_date_time_get_minute (now) + s / 60;
  double h = g_date_time_get_hour (now) % 12 + m / 60;
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  struct { double turn, len, width, rgb[3]; } hands[] = {
    { h / 12, 0.48, side * 0.04, { 1, 1, 1 } },
    { m / 60, 0.66, side * 0.03, { 1, 1, 1 } },
    { s / 60, 0.70, side * 0.012, { 0.9, 0.15, 0.15 } },
  };
  for (guint i = 0; i < G_N_ELEMENTS (hands); i++) {
    double a = hands[i].turn * 2 * G_PI - G_PI / 2;
    cairo_set_source_rgb (cr, hands[i].rgb[0], hands[i].rgb[1], hands[i].rgb[2]);
    cairo_set_line_width (cr, hands[i].width);
    cairo_move_to (cr, cx - cos (a) * r * 0.08, cy - sin (a) * r * 0.08);
    cairo_line_to (cr, cx + cos (a) * r * hands[i].len, cy + sin (a) * r * hands[i].len);
    cairo_stroke (cr);
  }
  cairo_arc (cr, cx, cy, side * 0.025, 0, 2 * G_PI);
  cairo_set_source_rgb (cr, 0.9, 0.15, 0.15);
  cairo_fill (cr);
  return TRUE;
}

static gboolean
clock_tick (gpointer data)
{
  Widgets *w = data;
  gtk_widget_queue_draw (w->clock);
  return G_SOURCE_CONTINUE;
}

/* ---- the strip ---- */

static void
on_map (GtkWidget *strip, Widgets *w)
{
  if (w->strip != strip)
    return;
  if (!w->tick)
    w->tick = g_timeout_add_seconds (1, clock_tick, w);
  if (!w->refresh)
    w->refresh = g_timeout_add_seconds (REFRESH_S, refresh_tick, w);
  refresh (w);
}

static void
on_unmap (GtkWidget *strip, Widgets *w)
{
  if (w->strip != strip)
    return;
  g_clear_handle_id (&w->tick, g_source_remove);
  g_clear_handle_id (&w->refresh, g_source_remove);
}

static void
on_destroy (GtkWidget *strip, Widgets *w)
{
  if (w->strip != strip)
    return;
  on_unmap (strip, w);
  w->strip = w->box = w->clock = NULL;
}

static void
open_weather (GtkButton *b, Widgets *w)
{
  /* our own weather page (neo-weather, GTK4) once there is weather to show, else GNOME Weather */
  g_autoptr (GDesktopAppInfo) page = g_desktop_app_info_new ("de.yesman.NeoWeather.desktop");
  w->open (w->have && page ? "de.yesman.NeoWeather.desktop" : "org.gnome.Weather.desktop", w->data);
}

static void
open_clock (GtkButton *b, Widgets *w)
{
  w->open ("org.gnome.clocks.desktop", w->data);
}

static GtkWidget *
flat_button (GtkWidget *child, GCallback cb, Widgets *w)
{
  GtkWidget *b = gtk_button_new ();
  gtk_button_set_relief (GTK_BUTTON (b), GTK_RELIEF_NONE);
  gtk_style_context_add_class (gtk_widget_get_style_context (b), "neo-widget");
  gtk_container_add (GTK_CONTAINER (b), child);
  g_signal_connect (b, "clicked", cb, w);
  return b;
}

/* one weather for the launcher's life: GeoClue's signal and curl's answers find it after a rebuild */
static Widgets *the;

GtkWidget *
neo_widgets_new (gboolean compact, void (*open) (const char *desktop_id, gpointer data), gpointer data)
{
  gboolean first = !the;
  if (!the)
    the = g_new0 (Widgets, 1);
  Widgets *w = the;
  if (w->strip)
    on_unmap (w->strip, w);
  w->open = open;
  w->data = data;
  /* the weather on the left taking the slack, the clock at the right edge. Not homogeneous columns: those made
   * every column as wide as the clock, and 5 or 6 of them overflowed the screen */
  GtkWidget *strip = w->strip = gtk_grid_new ();
  gtk_style_context_add_class (gtk_widget_get_style_context (strip), "neo-widgets");

  /* the weather: icon, then the temperature over the place */
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  w->icon = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (w->icon), compact ? 32 : 44);
  gtk_style_context_add_class (gtk_widget_get_style_context (w->icon), "neo-weather-icon");
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (col, GTK_ALIGN_CENTER);
  w->temp = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (w->temp), 0);
  gtk_style_context_add_class (gtk_widget_get_style_context (w->temp), "neo-weather-temp");
  GtkWidget *place_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
  w->pin = gtk_image_new_from_icon_name ("mark-location-symbolic", GTK_ICON_SIZE_MENU);
  gtk_image_set_pixel_size (GTK_IMAGE (w->pin), 12);
  w->place = gtk_label_new ("");
  gtk_label_set_ellipsize (GTK_LABEL (w->place), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (w->place), 18);
  gtk_style_context_add_class (gtk_widget_get_style_context (w->place), "neo-weather-place");
  gtk_style_context_add_class (gtk_widget_get_style_context (w->pin), "neo-weather-place");
  gtk_container_add (GTK_CONTAINER (place_row), w->pin);
  gtk_container_add (GTK_CONTAINER (place_row), w->place);
  gtk_container_add (GTK_CONTAINER (col), w->temp);
  gtk_container_add (GTK_CONTAINER (col), place_row);
  gtk_container_add (GTK_CONTAINER (row), w->icon);
  gtk_container_add (GTK_CONTAINER (row), col);
  GtkWidget *weather = flat_button (row, G_CALLBACK (open_weather), w);
  gtk_widget_set_halign (weather, GTK_ALIGN_START);
  gtk_widget_set_valign (weather, GTK_ALIGN_START);
  w->box = row;

  w->clock = gtk_drawing_area_new ();
  /* compact (landscape): a row there is ~78 px, and the full clock with its button padding took 80 */
  gtk_widget_set_size_request (w->clock, compact ? 54 : 72, compact ? 54 : 72);
  g_signal_connect (w->clock, "draw", G_CALLBACK (draw_clock), w);
  GtkWidget *clock = flat_button (w->clock, G_CALLBACK (open_clock), w);

  gtk_widget_set_hexpand (weather, TRUE);
  gtk_grid_attach (GTK_GRID (strip), weather, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (strip), clock, 1, 0, 1, 1);
  gtk_widget_set_valign (clock, GTK_ALIGN_START);
  g_signal_connect (strip, "map", G_CALLBACK (on_map), w);
  g_signal_connect (strip, "unmap", G_CALLBACK (on_unmap), w);
  g_signal_connect (strip, "destroy", G_CALLBACK (on_destroy), w);
  gtk_widget_show_all (strip);
  if (first)
    load_cache (w);
  show_weather (w);
  return strip;
}
