/* The wallpaper, decoded in full and scaled once to the screen's pixels with the best filter: a source of 4K and
 * more resampled on every frame cost page swipes their frame rate, and the decoder's own scaled decode left the
 * picture with a fraction of its colours (banding the lock screen does not show). Both shapes of the screen are
 * kept, so a rotation only swaps the picture. */
#include "neo-wall.h"

#include <gio/gio.h>
#include <math.h>
#include <string.h>

static GSettings  *bg;
static char       *wall_id;
static GHashTable *cache;         /* "WxH" -> GdkPixbuf */

static GSettings *
bg_settings (void)
{
  if (!bg)
    bg = g_settings_new ("org.gnome.desktop.background");
  return bg;
}

/* The picture a GNOME background XML (desktop-base's themes) names: the first file of its first slide, of its
 * sizes the one whose shape is nearest the screen's. */
static char *
xml_wallpaper (const char *path, double aspect)
{
  g_autofree char *xml = NULL;
  if (!g_file_get_contents (path, &xml, NULL, NULL))
    return NULL;
  const char *f0 = strstr (xml, "<file>"), *f1 = f0 ? strstr (f0, "</file>") : NULL;
  if (!f0 || !f1)
    return NULL;
  g_autofree char *file = g_strndup (f0 + 6, f1 - f0 - 6);
  g_autoptr (GRegex) re = g_regex_new ("<size\\s+width=\"(\\d+)\"\\s+height=\"(\\d+)\"\\s*>\\s*([^<]+?)\\s*</size>", 0, 0, NULL);
  g_autoptr (GMatchInfo) m = NULL;
  char *best = NULL;
  double best_d = G_MAXDOUBLE;
  g_regex_match (re, file, 0, &m);
  while (g_match_info_matches (m)) {
    g_autofree char *w = g_match_info_fetch (m, 1), *h = g_match_info_fetch (m, 2);
    double d = fabs (log ((g_ascii_strtod (w, NULL) / MAX (1, g_ascii_strtod (h, NULL))) / aspect));
    if (d < best_d) {
      best_d = d;
      g_free (best);
      best = g_match_info_fetch (m, 3);
    }
    g_match_info_next (m, NULL);
  }
  if (best)
    return best;
  g_strstrip (file);
  return *file ? g_strdup (file) : NULL;
}

static GdkPixbuf *
load_cover (const char *uri, int W, int H)
{
  g_autoptr (GFile) gf = g_file_new_for_uri (uri);
  g_autofree char *path = g_file_get_path (gf);
  if (!path)
    return NULL;
  if (g_str_has_suffix (path, ".xml")) {
    char *p = xml_wallpaper (path, (double) W / H);
    g_free (path);
    path = p;
    if (!path)
      return NULL;
  }
  g_autoptr (GdkPixbuf) pb = NULL;
  if (g_str_has_suffix (path, ".svg")) {        /* a vector picture is drawn straight at the size needed */
    int iw = 0, ih = 0;
    gdk_pixbuf_get_file_info (path, &iw, &ih);
    pb = iw && ih && (double) iw / ih > (double) W / H ? gdk_pixbuf_new_from_file_at_scale (path, -1, H, TRUE, NULL)
                                                         : gdk_pixbuf_new_from_file_at_scale (path, W, -1, TRUE, NULL);
  } else {
    pb = gdk_pixbuf_new_from_file (path, NULL);
  }
  if (!pb)
    return NULL;
  int pw = gdk_pixbuf_get_width (pb), ph = gdk_pixbuf_get_height (pb);
  double k = MAX ((double) W / pw, (double) H / ph);
  int cw = MIN (pw, (int) round (W / k)), ch = MIN (ph, (int) round (H / k));
  g_autoptr (GdkPixbuf) crop = gdk_pixbuf_new_subpixbuf (pb, (pw - cw) / 2, (ph - ch) / 2, cw, ch);
  return gdk_pixbuf_scale_simple (crop, W, H, GDK_INTERP_HYPER);
}

GdkPixbuf *
neo_wall_get (int w, int h)
{
  if (w <= 0 || h <= 0)
    return NULL;
  GSettings *s = bg_settings ();
  const char *keys[] = { "picture-uri-dark", "picture-uri" };
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    g_autofree char *uri = g_settings_get_string (s, keys[i]);
    if (!uri || !*uri)
      continue;
    g_autofree char *opts = g_settings_get_string (s, "picture-options");
    g_autofree char *id = g_strdup_printf ("%s %s", uri, opts);
    if (g_strcmp0 (id, wall_id)) {
      g_free (wall_id);
      wall_id = g_strdup (id);
      g_clear_pointer (&cache, g_hash_table_unref);
    }
    if (!cache)
      cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
    g_autofree char *key = g_strdup_printf ("%dx%d", w, h);
    GdkPixbuf *done = g_hash_table_lookup (cache, key);
    if (done)
      return g_object_ref (done);
    GdkPixbuf *pb = load_cover (uri, w, h);
    if (pb) {
      g_hash_table_replace (cache, g_steal_pointer (&key), g_object_ref (pb));
      return pb;
    }
    g_message ("wallpaper %s could not be loaded", uri);
  }
  return NULL;
}

typedef struct {
  void   (*changed) (gpointer data);
  gpointer data;
} Watch;

static void
on_bg_changed (GSettings *s, const char *key, Watch *w)
{
  if (g_str_has_prefix (key, "picture-uri") || !g_strcmp0 (key, "picture-options"))
    w->changed (w->data);
}

void
neo_wall_watch (void (*changed) (gpointer data), gpointer data)
{
  Watch *w = g_new0 (Watch, 1);
  w->changed = changed;
  w->data = data;
  g_signal_connect (bg_settings (), "changed", G_CALLBACK (on_bg_changed), w);
}
