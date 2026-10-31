/* Neo Weather: the page behind the home screen's weather widget, laid out like One UI's Weather: the place and the
 * temperature over the sky, the next 24 hours as a curve, today against yesterday, the week, and the details.
 * GTK4, so scrolling is the GPU moving what is already drawn.
 *
 * Where: the place the widget shows (~/.cache/neo-launcher/weather.json: lat, lon, place).
 * What: one Open-Meteo forecast (no key) with yesterday in it (past_days=1), fetched in curl and kept in
 * ~/.cache/neo-launcher/forecast.json; the page shows that at once and asks again when it is 15 minutes old. */
#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <math.h>

#define STALE_S (15 * 60)
#define UA "neo-weather/1.0 (Phosh)"
#define HOURS 24
#define COL_W 66              /* px, one hour in the hourly strip */

typedef struct {
  GtkWidget *win, *body, *spinner;
  double     lat, lon;
  char      *place;
} Page;

/* ---- what the weather codes (WMO) look like and are called ---- */

static const char *
wx_icon (int code, gboolean day)
{
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

static const char *
wx_name (int code)
{
  static const struct { int upto; const char *name; } names[] = {
    { 0, "Clear" }, { 1, "Mostly clear" }, { 2, "Partly cloudy" }, { 3, "Cloudy" }, { 48, "Fog" },
    { 55, "Drizzle" }, { 57, "Freezing drizzle" }, { 63, "Rain" }, { 65, "Heavy rain" }, { 67, "Freezing rain" },
    { 75, "Snow" }, { 77, "Snow grains" }, { 81, "Showers" }, { 82, "Heavy showers" }, { 86, "Snow showers" },
    { 99, "Thunderstorm" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (code <= names[i].upto)
      return names[i].name;
  return "Cloudy";
}

/* the sky: a class on the window, its gradient is in style.css */
static const char *
wx_sky (int code, gboolean day)
{
  if (!day)
    return "wx-night";
  if (code <= 1)
    return "wx-clear";
  if (code == 45 || code == 48)
    return "wx-fog";
  if (code >= 95)
    return "wx-storm";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    return "wx-snow";
  if (code >= 51)
    return "wx-rain";
  return "wx-cloudy";
}

/* the sun yellow, the moon pale, everything else white (style.css) */
static GtkWidget *
wx_image (const char *icon, int size)
{
  GtkWidget *img = gtk_image_new_from_icon_name (icon);
  gtk_image_set_pixel_size (GTK_IMAGE (img), size);
  if (g_str_has_prefix (icon, "weather-clear-symbolic") || g_str_has_prefix (icon, "daytime-"))
    gtk_widget_add_css_class (img, "wx-sun");
  else if (strstr (icon, "night"))
    gtk_widget_add_css_class (img, "wx-moon");
  return img;
}

/* ---- the forecast, read ---- */

static double
num (JsonArray *a, guint i)
{
  if (!a || i >= json_array_get_length (a))
    return NAN;
  JsonNode *n = json_array_get_element (a, i);
  return JSON_NODE_HOLDS_VALUE (n) ? json_node_get_double (n) : NAN;
}

static const char *
str (JsonArray *a, guint i)
{
  if (!a || i >= json_array_get_length (a))
    return NULL;
  JsonNode *n = json_array_get_element (a, i);
  return JSON_NODE_HOLDS_VALUE (n) ? json_node_get_string (n) : NULL;
}

static JsonArray *
arr (JsonObject *o, const char *name)
{
  return o && json_object_has_member (o, name) ? json_object_get_array_member (o, name) : NULL;
}

static double
cur (JsonObject *c, const char *name)
{
  return c && json_object_has_member (c, name) ? json_object_get_double_member (c, name) : NAN;
}

static GDateTime *
parse_local (const char *t)
{
  if (!t)
    return NULL;
  g_autoptr (GTimeZone) tz = g_time_zone_new_local ();
  g_autofree char *full = strlen (t) == 16 ? g_strconcat (t, ":00", NULL) : g_strdup (t);
  return g_date_time_new_from_iso8601 (full, tz);
}

static char *
fmt_temp (double t)
{
  if (isnan (t))
    return g_strdup ("–");
  double r = round (t);
  return g_strdup_printf ("%.0f°", r == 0 ? 0 : r);     /* never "-0°" */
}

static GtkWidget *
label (const char *text, const char *cls, float xalign)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), xalign);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  if (cls)
    gtk_widget_add_css_class (l, cls);
  return l;
}

/* ---- the hourly strip: a column per slot (time, icon, value, chance of rain) over the temperature curve ---- */

#define CURVE_H 34

typedef struct {
  GArray *temps;               /* double per slot */
  GArray *dots;                /* gboolean per slot */
  double  lo, hi;
} Curve;

static void
curve_free (Curve *c)
{
  g_array_unref (c->temps);
  g_array_unref (c->dots);
  g_free (c);
}

static void
draw_curve (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Curve *c = data;
  guint n = c->temps->len;
  double span = MAX (c->hi - c->lo, 1);
  #define CY(t) (h - 6 - ((t) - c->lo) / span * (h - 12))
  #define CX(i) ((i) * COL_W + COL_W / 2.0)
  cairo_set_line_width (cr, 1.6);
  cairo_set_source_rgba (cr, 1, 1, 1, 0.75);
  for (guint i = 0; i < n; i++) {
    double t = g_array_index (c->temps, double, i);
    if (i == 0) {
      cairo_move_to (cr, CX (i), CY (t));
      continue;
    }
    /* smooth: each segment leaves and arrives level */
    double p = g_array_index (c->temps, double, i - 1), mx = (CX (i - 1) + CX (i)) / 2;
    cairo_curve_to (cr, mx, CY (p), mx, CY (t), CX (i), CY (t));
  }
  cairo_stroke (cr);
  cairo_set_source_rgb (cr, 1, 1, 1);
  for (guint i = 0; i < n; i++)
    if (g_array_index (c->dots, gboolean, i)) {
      cairo_arc (cr, CX (i), CY (g_array_index (c->temps, double, i)), 4, 0, 2 * G_PI);
      cairo_fill (cr);
    }
}

static GtkWidget *
drop_label (double pop, gboolean small)
{
  GtkWidget *b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
  if (isnan (pop))
    return b;
  GtkWidget *d = gtk_image_new_from_icon_name ("weather-showers-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (d), small ? 10 : 12);
  gtk_widget_add_css_class (d, "wx-dim");
  g_autofree char *t = g_strdup_printf ("%.0f%%", pop);
  gtk_box_append (GTK_BOX (b), d);
  gtk_box_append (GTK_BOX (b), label (t, "wx-small", 0));
  return b;
}

typedef struct {
  GtkWidget *row;              /* the columns */
  Curve     *curve;
  GPtrArray *pops;
} Strip;

static void
add_slot (Strip *st, const char *time, const char *icon, const char *value, gboolean big, double temp, double pop,
          gboolean dot)
{
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_size_request (col, COL_W, -1);
  gtk_box_append (GTK_BOX (col), label (time, "wx-hour", 0.5));
  GtkWidget *img = wx_image (icon, 28);
  gtk_box_append (GTK_BOX (col), img);
  gtk_box_append (GTK_BOX (col), label (value, big ? "wx-hour-temp" : "wx-hour-event", 0.5));
  gtk_box_append (GTK_BOX (st->row), col);
  g_ptr_array_add (st->pops, drop_label (pop, TRUE));
  g_array_append_val (st->curve->temps, temp);
  g_array_append_val (st->curve->dots, dot);
  if (!isnan (temp)) {
    st->curve->lo = MIN (st->curve->lo, temp);
    st->curve->hi = MAX (st->curve->hi, temp);
  }
}

/* the next 24 hours from `first`, with today's and tomorrow's sunrise and sunset in between */
static GtkWidget *
hourly_strip (JsonObject *hourly, guint first, JsonArray *sunrise, JsonArray *sunset)
{
  Strip st = { gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0), g_new0 (Curve, 1), g_ptr_array_new () };
  st.curve->temps = g_array_new (FALSE, FALSE, sizeof (double));
  st.curve->dots = g_array_new (FALSE, FALSE, sizeof (gboolean));
  st.curve->lo = G_MAXDOUBLE;
  st.curve->hi = -G_MAXDOUBLE;
  JsonArray *time = arr (hourly, "time"), *temp = arr (hourly, "temperature_2m");
  JsonArray *code = arr (hourly, "weather_code"), *pop = arr (hourly, "precipitation_probability");
  JsonArray *isday = arr (hourly, "is_day");

  g_autoptr (GPtrArray) events = g_ptr_array_new_with_free_func ((GDestroyNotify) g_date_time_unref);
  g_autoptr (GPtrArray) kinds = g_ptr_array_new ();
  for (guint d = 0; sunrise && d < json_array_get_length (sunrise); d++) {
    GDateTime *r = parse_local (str (sunrise, d)), *s = parse_local (str (sunset, d));
    if (r) { g_ptr_array_add (events, r); g_ptr_array_add (kinds, "Sunrise"); }
    if (s) { g_ptr_array_add (events, s); g_ptr_array_add (kinds, "Sunset"); }
  }

  for (guint i = first; time && i < first + HOURS && i < json_array_get_length (time); i++) {
    g_autoptr (GDateTime) t = parse_local (str (time, i));
    if (!t)
      continue;
    double tc = num (temp, i);
    g_autofree char *hl = g_date_time_format (t, "%H:%M");
    g_autofree char *value = fmt_temp (tc);
    add_slot (&st, hl, wx_icon ((int) num (code, i), num (isday, i) != 0), value, TRUE, tc, num (pop, i), TRUE);
    /* a sunrise or sunset within this hour follows it */
    g_autoptr (GDateTime) next = g_date_time_add_hours (t, 1);
    for (guint e = 0; e < events->len; e++) {
      GDateTime *at = events->pdata[e];
      if (g_date_time_compare (at, t) > 0 && g_date_time_compare (at, next) < 0) {
        g_autofree char *el = g_date_time_format (at, "%H:%M");
        double frac = g_date_time_difference (at, t) / (double) G_TIME_SPAN_HOUR, tn = num (temp, i + 1);
        gboolean rise = !g_strcmp0 (kinds->pdata[e], "Sunrise");
        add_slot (&st, el, rise ? "daytime-sunrise-symbolic" : "daytime-sunset-symbolic", kinds->pdata[e], FALSE,
                  isnan (tn) ? tc : tc + (tn - tc) * frac, NAN, FALSE);
      }
    }
  }

  GtkWidget *curve = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (curve), CURVE_H);
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (curve), st.curve->temps->len * COL_W);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (curve), draw_curve, st.curve, (GDestroyNotify) curve_free);
  GtkWidget *pops = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  for (guint i = 0; i < st.pops->len; i++) {
    gtk_widget_set_size_request (st.pops->pdata[i], COL_W, -1);
    gtk_box_append (GTK_BOX (pops), st.pops->pdata[i]);
  }
  g_ptr_array_unref (st.pops);

  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (v), st.row);
  gtk_box_append (GTK_BOX (v), curve);
  gtk_box_append (GTK_BOX (v), pops);
  GtkWidget *sc = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), v);
  return sc;
}

/* ---- the page ---- */

static GtkWidget *
card (void)
{
  GtkWidget *c = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (c, "wx-card");
  return c;
}

static const char *
uv_word (double uv)
{
  return uv < 3 ? "Low" : uv < 6 ? "Moderate" : uv < 8 ? "High" : uv < 11 ? "Very high" : "Extreme";
}

static GtkWidget *
detail (const char *title, const char *value, const char *sub)
{
  GtkWidget *b = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (b, "wx-detail");
  gtk_widget_set_hexpand (b, TRUE);
  gtk_box_append (GTK_BOX (b), label (title, "wx-dim", 0));
  gtk_box_append (GTK_BOX (b), label (value, "wx-big", 0));
  if (sub)
    gtk_box_append (GTK_BOX (b), label (sub, "wx-small", 0));
  return b;
}

static void
set_sky (Page *p, const char *sky)
{
  const char *skies[] = { "wx-night", "wx-clear", "wx-fog", "wx-storm", "wx-snow", "wx-rain", "wx-cloudy" };
  for (guint i = 0; i < G_N_ELEMENTS (skies); i++)
    gtk_widget_remove_css_class (p->win, skies[i]);
  gtk_widget_add_css_class (p->win, sky);
}

static void
fill (Page *p, JsonObject *o)
{
  JsonObject *c = o && json_object_has_member (o, "current") ? json_object_get_object_member (o, "current") : NULL;
  JsonObject *h = o && json_object_has_member (o, "hourly") ? json_object_get_object_member (o, "hourly") : NULL;
  JsonObject *d = o && json_object_has_member (o, "daily") ? json_object_get_object_member (o, "daily") : NULL;
  if (!c || !h || !d)
    return;
  for (GtkWidget *ch; (ch = gtk_widget_get_first_child (p->body));)
    gtk_box_remove (GTK_BOX (p->body), ch);

  int code = (int) cur (c, "weather_code");
  gboolean day = cur (c, "is_day") != 0;
  set_sky (p, wx_sky (code, day));
  JsonArray *dmax = arr (d, "temperature_2m_max"), *dmin = arr (d, "temperature_2m_min");
  JsonArray *dcode = arr (d, "weather_code"), *dpop = arr (d, "precipitation_probability_max");
  JsonArray *dtime = arr (d, "time"), *rise = arr (d, "sunrise"), *set = arr (d, "sunset"), *uv = arr (d, "uv_index_max");
  /* daily[0] is yesterday, daily[1] today */

  /* the head: place, temperature, what it is, today's high and low, feels like */
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (head, "wx-head");
  GtkWidget *place = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *pin = gtk_image_new_from_icon_name ("mark-location-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (pin), 22);
  gtk_box_append (GTK_BOX (place), pin);
  gtk_box_append (GTK_BOX (place), label (p->place ? p->place : "", "wx-place", 0));
  gtk_box_append (GTK_BOX (head), place);
  g_autofree char *now = fmt_temp (cur (c, "temperature_2m"));
  gtk_box_append (GTK_BOX (head), label (now, "wx-now", 0));
  gtk_box_append (GTK_BOX (head), label (wx_name (code), "wx-cond", 0));
  g_autofree char *hi = fmt_temp (num (dmax, 1)), *lo = fmt_temp (num (dmin, 1));
  g_autofree char *feels = fmt_temp (cur (c, "apparent_temperature"));
  g_autofree char *range = g_strdup_printf ("↑%s / ↓%s\nFeels like %s", hi, lo, feels);
  GtkWidget *rl = label (range, "wx-range", 0);
  gtk_widget_set_margin_top (rl, 70);
  gtk_box_append (GTK_BOX (head), rl);
  gtk_box_append (GTK_BOX (p->body), head);

  /* the next 24 hours */
  GtkWidget *hours = card ();
  g_autofree char *summary = g_strdup_printf ("%s. High %s, low %s.", wx_name ((int) num (dcode, 1)), hi, lo);
  gtk_box_append (GTK_BOX (hours), label (summary, "wx-summary", 0));
  gtk_box_append (GTK_BOX (hours), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  JsonArray *htime = arr (h, "time");
  g_autoptr (GDateTime) dnow = g_date_time_new_now_local ();
  g_autofree char *hour_now = g_date_time_format (dnow, "%Y-%m-%dT%H:00");
  guint first = 0;
  for (guint i = 0; htime && i < json_array_get_length (htime); i++)
    if (!g_strcmp0 (str (htime, i), hour_now)) {
      first = i;
      break;
    }
  gtk_box_append (GTK_BOX (hours), hourly_strip (h, first, rise, set));
  gtk_box_append (GTK_BOX (p->body), hours);

  /* now against this hour yesterday */
  double then = num (arr (h, "temperature_2m"), first >= 24 ? first - 24 : G_MAXUINT);
  double diff = cur (c, "temperature_2m") - then;
  if (!isnan (diff)) {
    GtkWidget *cmp = card ();
    GtkWidget *t = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append (GTK_BOX (t), gtk_image_new_from_icon_name ("temperature-symbolic"));
    gtk_box_append (GTK_BOX (t), label ("Today's temperature", "wx-dim", 0));
    gtk_box_append (GTK_BOX (cmp), t);
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    const char *say = fabs (diff) < 1 ? "About the same as yesterday"
                    : diff < 0 ? (diff > -4 ? "Temperatures a little lower than yesterday"
                                            : "Much colder than yesterday")
                               : (diff < 4 ? "Temperatures a little higher than yesterday"
                                           : "Much warmer than yesterday");
    GtkWidget *sl = label (say, "wx-big", 0);
    gtk_widget_set_hexpand (sl, TRUE);
    gtk_box_append (GTK_BOX (row), sl);
    if (fabs (diff) >= 1) {
      g_autofree char *dd = g_strdup_printf ("%.0f°%s", fabs (diff), diff < 0 ? "↓" : "↑");
      gtk_box_append (GTK_BOX (row), label (dd, "wx-delta", 1));
    }
    gtk_box_append (GTK_BOX (cmp), row);
    gtk_box_append (GTK_BOX (p->body), cmp);
  }

  /* the week: yesterday dim, then today and on */
  GtkWidget *week = card ();
  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 18);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  for (guint i = 0; dtime && i < json_array_get_length (dtime); i++) {
    g_autofree char *noon = g_strconcat (str (dtime, i) ? str (dtime, i) : "", "T12:00", NULL);
    g_autoptr (GDateTime) dt = parse_local (noon);
    g_autofree char *name = i == 0 ? g_strdup ("Yesterday") : i == 1 ? g_strdup ("Today")
                          : dt ? g_date_time_format (dt, "%a") : g_strdup ("");
    g_autofree char *mx = fmt_temp (num (dmax, i)), *mn = fmt_temp (num (dmin, i));
    g_autofree char *temps = g_strdup_printf ("%s  %s", mx, mn);
    const char *cls = i == 0 ? "wx-yesterday" : "wx-day";
    GtkWidget *n = label (name, cls, 0);
    gtk_widget_set_hexpand (n, TRUE);
    gtk_grid_attach (GTK_GRID (grid), n, 0, i, 1, 1);
    if (i > 0) {
      int dc = (int) num (dcode, i);
      gtk_grid_attach (GTK_GRID (grid), drop_label (num (dpop, i), FALSE), 1, i, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), wx_image (wx_icon (dc, TRUE), 26), 2, i, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), wx_image (wx_icon (dc, FALSE), 26), 3, i, 1, 1);
    }
    gtk_grid_attach (GTK_GRID (grid), label (temps, cls, 1), 4, i, 1, 1);
  }
  gtk_box_append (GTK_BOX (week), grid);
  gtk_box_append (GTK_BOX (p->body), week);

  /* the details */
  GtkWidget *more = card ();
  GtkWidget *dg = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (dg), 16);
  gtk_grid_set_column_homogeneous (GTK_GRID (dg), TRUE);
  g_autoptr (GDateTime) r = parse_local (str (rise, 1));
  g_autoptr (GDateTime) s = parse_local (str (set, 1));
  g_autofree char *rs = r ? g_date_time_format (r, "%H:%M") : g_strdup ("–");
  g_autofree char *ss = s ? g_date_time_format (s, "%H:%M") : g_strdup ("–");
  g_autofree char *ssub = g_strdup_printf ("Sunset %s", ss);
  double u = num (uv, 1);
  g_autofree char *uvs = isnan (u) ? g_strdup ("–") : g_strdup_printf ("%.0f", u);
  g_autofree char *hum = g_strdup_printf ("%.0f%%", cur (c, "relative_humidity_2m"));
  g_autofree char *wind = g_strdup_printf ("%.0f km/h", cur (c, "wind_speed_10m"));
  g_autofree char *pres = g_strdup_printf ("%.0f hPa", cur (c, "pressure_msl"));
  gtk_grid_attach (GTK_GRID (dg), detail ("Sunrise", rs, ssub), 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail ("UV index", uvs, isnan (u) ? NULL : uv_word (u)), 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail ("Humidity", hum, NULL), 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail ("Wind", wind, NULL), 1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail ("Pressure", pres, NULL), 0, 2, 1, 1);
  gtk_box_append (GTK_BOX (more), dg);
  gtk_box_append (GTK_BOX (p->body), more);

  gtk_box_append (GTK_BOX (p->body), label ("Open-Meteo.com", "wx-credit", 0.5));
}

/* ---- the forecast, fetched (curl, away from the main loop) and kept ---- */

static char *
cache_path (const char *name)
{
  return g_build_filename (g_get_user_cache_dir (), "neo-launcher", name, NULL);
}

static void
fetched (GObject *src, GAsyncResult *res, gpointer data)
{
  Page *p = data;
  g_autofree char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out;
  gtk_spinner_stop (GTK_SPINNER (p->spinner));
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!ok || !json_parser_load_from_data (jp, out, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp))) {
    g_message ("Open-Meteo gave no answer");
    return;
  }
  g_autofree char *path = cache_path ("forecast.json");
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents (path, out, -1, NULL);
  fill (p, json_node_get_object (json_parser_get_root (jp)));
}

static void
fetch (Page *p)
{
  char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (lat, sizeof lat, "%.4f", p->lat);
  g_ascii_formatd (lon, sizeof lon, "%.4f", p->lon);
  g_autofree char *url = g_strdup_printf (
    "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto&past_days=1&forecast_days=8"
    "&current=temperature_2m,apparent_temperature,weather_code,is_day,relative_humidity_2m,wind_speed_10m,pressure_msl"
    "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,"
    "uv_index_max", lat, lon);
  const char *argv[] = { "curl", "-sf", "--max-time", "20", "-A", UA, url, NULL };
  GSubprocess *sp = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (!sp)
    return;
  gtk_spinner_start (GTK_SPINNER (p->spinner));
  g_subprocess_communicate_utf8_async (sp, NULL, NULL, fetched, p);
  g_object_unref (sp);
}

/* the cached forecast, if it is for here; TRUE when it is fresh too */
static gboolean
load_cache (Page *p)
{
  g_autofree char *path = cache_path ("forecast.json");
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!json_parser_load_from_file (jp, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp)))
    return FALSE;
  JsonObject *o = json_node_get_object (json_parser_get_root (jp));
  double lat = json_object_get_double_member_with_default (o, "latitude", 999);
  double lon = json_object_get_double_member_with_default (o, "longitude", 999);
  if (fabs (lat - p->lat) > 0.2 || fabs (lon - p->lon) > 0.2)
    return FALSE;
  fill (p, o);
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GFileInfo) info = g_file_query_info (f, G_FILE_ATTRIBUTE_TIME_MODIFIED, 0, NULL, NULL);
  return info && g_get_real_time () / G_USEC_PER_SEC -
                   (gint64) g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) < STALE_S;
}

/* where: what the home screen's widget last showed */
static gboolean
load_place (Page *p)
{
  g_autofree char *path = cache_path ("weather.json");
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!json_parser_load_from_file (jp, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp)))
    return FALSE;
  JsonObject *o = json_node_get_object (json_parser_get_root (jp));
  p->lat = json_object_get_double_member_with_default (o, "lat", 0);
  p->lon = json_object_get_double_member_with_default (o, "lon", 0);
  p->place = g_strdup (json_object_get_string_member_with_default (o, "place", ""));
  return p->lat || p->lon;
}

/* ---- the window ---- */

static void
refresh_clicked (GtkButton *b, Page *p)
{
  fetch (p);
}

/* back (Alt+Left from Neo's edge swipe) and Escape close the page, back to the home screen */
static void
add_back (GtkWidget *win)
{
  GtkEventController *keys = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (keys), GTK_SHORTCUT_SCOPE_GLOBAL);
  GtkShortcutAction *close = gtk_named_action_new ("window.close");
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_shortcut_trigger_parse_string ("Escape")),
                      close));
  gtk_widget_add_controller (win, keys);
}

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWindow *have = gtk_application_get_active_window (app);
  if (have) {
    gtk_window_present (have);
    return;
  }
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/de/yesman/NeoWeather/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);

  Page *p = g_new0 (Page, 1);
  GtkWidget *win = p->win = adw_application_window_new (app);
  g_object_set_data_full (G_OBJECT (win), "page", p, g_free);
  gtk_window_set_title (GTK_WINDOW (win), "Weather");
  gtk_window_set_default_size (GTK_WINDOW (win), 360, 720);
  gtk_widget_add_css_class (win, "wx");
  add_back (win);
  set_sky (p, "wx-cloudy");

  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *sc = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (sc, TRUE);
  p->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (p->body, "wx-body");
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), p->body);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), sc);

  /* reload floats over the sky, top right; back is the system's (the swipe, or closing the app) */
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (bar, GTK_ALIGN_START);
  gtk_widget_set_halign (bar, GTK_ALIGN_END);
  gtk_widget_add_css_class (bar, "wx-bar");
  p->spinner = gtk_spinner_new ();
  GtkWidget *reload = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_add_css_class (reload, "flat");
  gtk_widget_add_css_class (reload, "circular");
  g_signal_connect (reload, "clicked", G_CALLBACK (refresh_clicked), p);
  gtk_box_append (GTK_BOX (bar), p->spinner);
  gtk_box_append (GTK_BOX (bar), reload);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), bar);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), overlay);

  gtk_window_present (GTK_WINDOW (win));
  if (!load_place (p)) {
    gtk_box_append (GTK_BOX (p->body), label ("No weather yet: the home screen's widget has not found a place.",
                                              "wx-summary", 0));
    return;
  }
  if (!load_cache (p))
    fetch (p);
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = adw_application_new ("de.yesman.NeoWeather", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
