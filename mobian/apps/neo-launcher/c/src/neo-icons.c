/* Launcher icons. Every custom icon is rendered once into ~/.cache/neo-launcher/icons/<key>.png; the key covers the
 * pack, the shape, the two switches, the app and the pixel size, so a settings change simply misses the cache and
 * renders again. Keys, shapes and colours are the GJS Neo's (iconrender.js), so its renders are reused. */
#include "neo-icons.h"

#include <json-glib/json-glib.h>
#include <math.h>

#define LEGACY_FG 0.62   /* the "legacy treatment": the old icon sits at 62 % inside the shaped background */

/* Android apps whose pack icon would repeat a GNOME app's: desktop id -> pack drawable */
static const char *distinct[][2] = {
  { "waydroid.com.android.settings.desktop", "flatdroid" },
};

typedef struct {
  char       *dir;
  GHashTable *map;          /* desktop id -> drawable */
  char       *shape;        /* the pack's own shape, or NULL */
  double      scale;
  GPtrArray  *iconback, *iconmask, *iconupon;
} Pack;

struct _NeoIcons {
  GSettings *settings;
  char      *pack_id;       /* NULL: not read yet */
  Pack      *pack;
  char      *cache;
};

static void
pack_free (Pack *p)
{
  if (!p)
    return;
  g_free (p->dir);
  g_hash_table_unref (p->map);
  g_free (p->shape);
  g_ptr_array_unref (p->iconback);
  g_ptr_array_unref (p->iconmask);
  g_ptr_array_unref (p->iconupon);
  g_free (p);
}

static JsonNode *
read_json (const char *path)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root (parser);
  return root ? json_node_copy (root) : NULL;
}

static char **
pack_dirs (void)
{
  char **dirs = g_new0 (char *, 3);
  dirs[0] = g_build_filename (g_get_user_data_dir (), "neo-launcher", "iconpacks", NULL);
  dirs[1] = g_strdup ("/usr/share/neo-launcher/iconpacks");
  return dirs;
}

static void
neo_pack_free (NeoPack *p)
{
  g_free (p->id);
  g_free (p->title);
  g_free (p->dir);
  g_free (p);
}

static gint
pack_cmp (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate ((*(NeoPack **) a)->title, (*(NeoPack **) b)->title);
}

GPtrArray *
neo_icons_list_packs (void)
{
  g_auto (GStrv) dirs = pack_dirs ();
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  GPtrArray *packs = g_ptr_array_new_with_free_func ((GDestroyNotify) neo_pack_free);

  for (int d = 0; dirs[d]; d++) {           /* the user's dir first: it wins */
    g_autoptr (GDir) dir = g_dir_open (dirs[d], 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name (dir))) {
      if (g_hash_table_contains (seen, name))
        continue;
      g_autofree char *path = g_build_filename (dirs[d], name, NULL);
      g_autofree char *meta_path = g_build_filename (path, "pack.json", NULL);
      g_autofree char *map_path = g_build_filename (path, "gnome-map.json", NULL);
      if (!g_file_test (map_path, G_FILE_TEST_EXISTS))
        continue;
      JsonNode *meta = read_json (meta_path);
      if (!meta)
        continue;
      NeoPack *p = g_new0 (NeoPack, 1);
      p->id = g_strdup (name);
      JsonObject *o = JSON_NODE_HOLDS_OBJECT (meta) ? json_node_get_object (meta) : NULL;
      const char *title = o && json_object_has_member (o, "title") ? json_object_get_string_member (o, "title") : NULL;
      p->title = g_strdup (title ?: name);
      p->dir = g_strdup (path);
      json_node_unref (meta);
      g_hash_table_add (seen, p->id);
      g_ptr_array_add (packs, p);
    }
  }
  g_ptr_array_sort (packs, pack_cmp);
  return packs;
}

static GPtrArray *
str_list (JsonObject *o, const char *member)
{
  GPtrArray *list = g_ptr_array_new_with_free_func (g_free);
  JsonNode *n = o ? json_object_get_member (o, member) : NULL;
  if (n && JSON_NODE_HOLDS_ARRAY (n)) {
    JsonArray *a = json_node_get_array (n);
    for (guint i = 0; i < json_array_get_length (a); i++)
      if (json_node_get_value_type (json_array_get_element (a, i)) == G_TYPE_STRING)
        g_ptr_array_add (list, g_strdup (json_array_get_string_element (a, i)));
  }
  return list;
}

static void
map_from (GHashTable *map, const char *path, const char *prefix, gboolean keep_existing)
{
  JsonNode *root = read_json (path);
  if (!root)
    return;
  if (JSON_NODE_HOLDS_OBJECT (root)) {
    JsonObject *o = json_node_get_object (root);
    g_autoptr (GList) members = json_object_get_members (o);
    for (GList *l = members; l; l = l->next) {
      JsonNode *v = json_object_get_member (o, l->data);
      if (json_node_get_value_type (v) != G_TYPE_STRING)
        continue;
      char *key = prefix ? g_strdup_printf ("%s%s.desktop", prefix, (char *) l->data) : g_strdup (l->data);
      if (keep_existing && g_hash_table_contains (map, key)) {
        g_free (key);
        continue;
      }
      g_hash_table_replace (map, key, g_strdup (json_node_get_string (v)));
    }
  }
  json_node_unref (root);
}

static Pack *
current_pack (NeoIcons *self)
{
  g_autofree char *id = g_settings_get_string (self->settings, "icon-pack");
  if (self->pack_id && !g_strcmp0 (id, self->pack_id))
    return self->pack;
  g_free (self->pack_id);
  self->pack_id = g_strdup (id);
  g_clear_pointer (&self->pack, pack_free);
  if (!*id)
    return NULL;

  g_autoptr (GPtrArray) packs = neo_icons_list_packs ();
  NeoPack *info = NULL;
  for (guint i = 0; i < packs->len; i++)
    if (!g_strcmp0 (((NeoPack *) packs->pdata[i])->id, id))
      info = packs->pdata[i];
  if (!info)
    return NULL;

  Pack *p = g_new0 (Pack, 1);
  p->dir = g_strdup (info->dir);
  p->map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autofree char *map_path = g_build_filename (p->dir, "gnome-map.json", NULL);
  g_autofree char *filter_path = g_build_filename (p->dir, "appfilter.json", NULL);
  map_from (p->map, map_path, NULL, FALSE);
  /* Android apps (Waydroid's waydroid.<package>.desktop): the pack's own Android mapping. A GNOME entry wins. */
  map_from (p->map, filter_path, "waydroid.", TRUE);
  /* Android's settings are not GNOME's: the robot, not the same gear twice (when the pack has it) */
  for (guint i = 0; i < G_N_ELEMENTS (distinct); i++) {
    g_autofree char *png = g_strdup_printf ("%s.png", distinct[i][1]);
    g_autofree char *path = g_build_filename (p->dir, "icons", png, NULL);
    if (g_file_test (path, G_FILE_TEST_EXISTS))
      g_hash_table_replace (p->map, g_strdup (distinct[i][0]), g_strdup (distinct[i][1]));
  }
  g_autofree char *meta_path = g_build_filename (p->dir, "pack.json", NULL);
  JsonNode *meta = read_json (meta_path);
  JsonObject *o = meta && JSON_NODE_HOLDS_OBJECT (meta) ? json_node_get_object (meta) : NULL;
  p->scale = 1;
  if (o && json_object_has_member (o, "scale")) {
    JsonNode *s = json_object_get_member (o, "scale");
    if (JSON_NODE_HOLDS_VALUE (s) && json_node_get_value_type (s) == G_TYPE_STRING)
      p->scale = g_ascii_strtod (json_node_get_string (s), NULL);
    else if (JSON_NODE_HOLDS_VALUE (s))
      p->scale = json_node_get_double (s);
    if (p->scale <= 0)
      p->scale = 1;
  }
  if (o && json_object_has_member (o, "shape") && !json_object_get_null_member (o, "shape"))
    p->shape = g_strdup (json_object_get_string_member (o, "shape"));
  p->iconback = str_list (o, "iconback");
  p->iconmask = str_list (o, "iconmask");
  p->iconupon = str_list (o, "iconupon");
  if (meta)
    json_node_unref (meta);
  self->pack = p;
  return p;
}

NeoIcons *
neo_icons_new (GSettings *settings)
{
  NeoIcons *self = g_new0 (NeoIcons, 1);
  self->settings = g_object_ref (settings);
  self->cache = g_build_filename (g_get_user_cache_dir (), "neo-launcher", "icons", NULL);
  g_mkdir_with_parents (self->cache, 0755);
  return self;
}

void
neo_icons_free (NeoIcons *self)
{
  if (!self)
    return;
  g_object_unref (self->settings);
  g_free (self->pack_id);
  pack_free (self->pack);
  g_free (self->cache);
  g_free (self);
}

void
neo_icons_reset (NeoIcons *self)
{
  g_clear_pointer (&self->pack_id, g_free);
  g_clear_pointer (&self->pack, pack_free);
}

/* ---- stock icons ---- */

static GdkPixbuf *
load_at (const char *path, int px)
{
  return path ? gdk_pixbuf_new_from_file_at_scale (path, px, px, TRUE, NULL) : NULL;
}

/* the app's own icon as a pixbuf of about px (theme lookup, or the file a FileIcon names); *missing (if given) is
 * set when the theme has no such icon and the generic one stands in */
static GdkPixbuf *
source_pixbuf (GAppInfo *info, int px, gboolean *missing)
{
  GIcon *gicon = g_app_info_get_icon (info);
  g_autoptr (GtkIconInfo) ii = NULL;
  if (gicon)
    ii = gtk_icon_theme_lookup_by_gicon (gtk_icon_theme_get_default (), gicon, px, GTK_ICON_LOOKUP_FORCE_SIZE);
  if (missing)
    *missing = !ii;
  if (!ii)
    ii = gtk_icon_theme_lookup_icon (gtk_icon_theme_get_default (), "application-x-executable", px, GTK_ICON_LOOKUP_FORCE_SIZE);
  return ii ? gtk_icon_info_load_icon (ii, NULL) : NULL;
}

/* Scale into a px² square, aspect kept, centred on transparent. */
static GdkPixbuf *
fit (GdkPixbuf *pb, int px)
{
  int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
  if (w == px && h == px)
    return g_object_ref (pb);
  double f = MIN ((double) px / w, (double) px / h);
  int sw = MAX (1, (int) round (w * f)), sh = MAX (1, (int) round (h * f));
  GdkPixbuf *out = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, px, px);
  gdk_pixbuf_fill (out, 0);
  gdk_pixbuf_scale (pb, out, (px - sw) / 2, (px - sh) / 2, sw, sh, (px - sw) / 2.0, (px - sh) / 2.0, f, f,
                    GDK_INTERP_BILINEAR);
  return out;
}

GdkPixbuf *
neo_icons_stock (GAppInfo *info, int px)
{
  g_autoptr (GdkPixbuf) pb = source_pixbuf (info, px, NULL);
  if (!pb) {
    GdkPixbuf *empty = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, px, px);
    gdk_pixbuf_fill (empty, 0);
    return empty;
  }
  return fit (pb, px);
}

/* ---- cache keys ---- */

static char *
cache_key (NeoIcons *self, GAppInfo *info, int px)
{
  g_autofree char *shape = g_settings_get_string (self->settings, "icon-shape");
  gboolean wrap = g_settings_get_boolean (self->settings, "icon-wrap-unthemed");
  gboolean legacy = g_settings_get_boolean (self->settings, "icon-shape-legacy");
  const char *id = g_app_info_get_id (info);
  /* 'b' marks Android icons drawn since they take the pack's appfilter */
  const char *android = g_str_has_prefix (id, "waydroid.") ? "b" : "";
  g_autofree char *safe = g_strdup (id);
  for (char *c = safe; *c; c++)
    if (!g_ascii_isalnum (*c) && *c != '.' && *c != '_' && *c != '-')
      *c = '_';
  /* 'f': icons with their own background take the pack's shape at full size (since 2.0); 's': wrapped icons take
   * the shape picked, not the backplate's */
  return g_strdup_printf ("%s_%s_%d%d%sfs_%s_%d", self->pack_id && *self->pack_id ? self->pack_id : "-", shape,
                          wrap ? 1 : 0, legacy ? 1 : 0, android, safe, px);
}

GdkPixbuf *
neo_icons_lookup (NeoIcons *self, GAppInfo *info, int px, gboolean *pending)
{
  Pack *pack = current_pack (self);
  g_autofree char *shape = g_settings_get_string (self->settings, "icon-shape");
  gboolean legacy = g_settings_get_boolean (self->settings, "icon-shape-legacy");

  *pending = FALSE;
  if (!pack && !g_strcmp0 (shape, "system") && !legacy)
    return neo_icons_stock (info, px);                 /* the theme's icon */
  g_autofree char *key = cache_key (self, info, px);
  g_autofree char *png = g_strdup_printf ("%s/%s.png", self->cache, key);
  g_autofree char *none = g_strdup_printf ("%s/%s.none", self->cache, key);
  if (g_file_test (png, G_FILE_TEST_EXISTS)) {
    GdkPixbuf *pb = gdk_pixbuf_new_from_file (png, NULL);
    if (pb)
      return pb;
  }
  if (!g_file_test (none, G_FILE_TEST_EXISTS))         /* drawn before: nothing custom applies */
    *pending = TRUE;
  return neo_icons_stock (info, px);
}

/* ---- rendering (iconrender.js) ---- */

static void
rounded_rect (cairo_t *cr, double s, double tl, double tr, double br, double bl)
{
  cairo_new_sub_path (cr);
  cairo_arc (cr, s - tr * s, tr * s, tr * s, -G_PI / 2, 0);
  cairo_arc (cr, s - br * s, s - br * s, br * s, 0, G_PI / 2);
  cairo_arc (cr, bl * s, s - bl * s, bl * s, G_PI / 2, G_PI);
  cairo_arc (cr, tl * s, tl * s, tl * s, G_PI, 3 * G_PI / 2);
  cairo_close_path (cr);
}

/* |x|^nx + |y|^ny = 1, 0.5 px inset so the anti-aliased edge is not clipped */
static void
superellipse (cairo_t *cr, double s, double nx, double ny)
{
  double c = s / 2, r = s / 2 - 0.5;
  const int N = 128;
  for (int i = 0; i <= N; i++) {
    double t = (double) i / N * 2 * G_PI, ct = cos (t), st = sin (t);
    double x = copysign (pow (fabs (ct), 2 / nx), ct), y = copysign (pow (fabs (st), 2 / ny), st);
    if (i == 0)
      cairo_move_to (cr, c + r * x, c + r * y);
    else
      cairo_line_to (cr, c + r * x, c + r * y);
  }
  cairo_close_path (cr);
}

static void
polygon (cairo_t *cr, double s, int n, double rot)
{
  double c = s / 2, r = s / 2 - 0.5;
  for (int i = 0; i < n; i++) {
    double a = rot - G_PI / 2 + (double) i / n * 2 * G_PI;
    if (i == 0)
      cairo_move_to (cr, c + r * cos (a), c + r * sin (a));
    else
      cairo_line_to (cr, c + r * cos (a), c + r * sin (a));
  }
  cairo_close_path (cr);
}

static void
shape_path (cairo_t *cr, const char *shape, double s)
{
  double c = s / 2;
  if (!g_strcmp0 (shape, "circle"))
    cairo_arc (cr, c, c, c, 0, 2 * G_PI);
  else if (!g_strcmp0 (shape, "square"))
    rounded_rect (cr, s, 0.06, 0.06, 0.06, 0.06);
  else if (!g_strcmp0 (shape, "rounded"))
    rounded_rect (cr, s, 0.2, 0.2, 0.2, 0.2);
  else if (!g_strcmp0 (shape, "teardrop"))
    rounded_rect (cr, s, 0.5, 0.5, 0.12, 0.5);
  else if (!g_strcmp0 (shape, "cupertino"))
    superellipse (cr, s, 5, 5);
  else if (!g_strcmp0 (shape, "cylinder"))
    superellipse (cr, s, 2.2, 6);
  else if (!g_strcmp0 (shape, "egg")) {
    cairo_move_to (cr, c + 0.46 * s, c);
    for (int i = 1; i <= 96; i++) {
      double t = i / 96.0 * 2 * G_PI, sy = sin (t);
      cairo_line_to (cr, c + 0.46 * s * cos (t) * (1 + 0.14 * sy), c + 0.47 * s * sy);
    }
    cairo_close_path (cr);
  } else if (!g_strcmp0 (shape, "octagon"))
    polygon (cr, s, 8, G_PI / 8);
  else if (!g_strcmp0 (shape, "hexagon"))
    polygon (cr, s, 6, 0);
  else if (!g_strcmp0 (shape, "diamond"))
    polygon (cr, s, 4, 0);
  else
    superellipse (cr, s, 4, 4);   /* squircle */
}

static void
paint_pixbuf (cairo_t *cr, GdkPixbuf *pb, int size, double x, double y)
{
  g_autoptr (GdkPixbuf) f = fit (pb, size);
  gdk_cairo_set_source_pixbuf (cr, f, x, y);
  cairo_paint (cr);
}

/* The box holding the icon's pixels more opaque than `thr`; FALSE when there are none. */
static gboolean
alpha_box (GdkPixbuf *pb, int thr, int *bx, int *by, int *bw, int *bh)
{
  if (gdk_pixbuf_get_n_channels (pb) != 4)
    return FALSE;
  const guchar *px = gdk_pixbuf_read_pixels (pb);
  int stride = gdk_pixbuf_get_rowstride (pb), w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (px[y * stride + x * 4 + 3] > thr) {
        x0 = MIN (x0, x); x1 = MAX (x1, x);
        y0 = MIN (y0, y); y1 = MAX (y1, y);
      }
  if (x1 < 0)
    return FALSE;
  *bx = x0; *by = y0; *bw = x1 - x0 + 1; *bh = y1 - y0 + 1;
  return TRUE;
}

/* An icon with its own background (a filled square, squircle, rounded square): nearly all of its box is opaque.
 * A glyph on transparency, or a round icon, is not. */
static gboolean
full_bleed (GdkPixbuf *pb)
{
  int bx, by, bw, bh, solid = 0;
  if (!alpha_box (pb, 16, &bx, &by, &bw, &bh) || bw < 8 || bh < 8)
    return FALSE;
  const guchar *px = gdk_pixbuf_read_pixels (pb);
  int stride = gdk_pixbuf_get_rowstride (pb);
  for (int y = by; y < by + bh; y++)
    for (int x = bx; x < bx + bw; x++)
      solid += px[y * stride + x * 4 + 3] > 200;
  return solid >= 0.85 * bw * bh && fabs ((double) bw / bh - 1) < 0.15;
}

/* Average colour of the icon's opaque pixels, lifted towards white so the icon stays readable on it. */
static void
tint_of (GdkPixbuf *pb, double rgb[3])
{
  const guchar *px = gdk_pixbuf_read_pixels (pb);
  int n = gdk_pixbuf_get_n_channels (pb), stride = gdk_pixbuf_get_rowstride (pb);
  int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
  double r = 0, g = 0, b = 0;
  int c = 0;
  for (int y = 0; y < h; y += 3)
    for (int x = 0; x < w; x += 3) {
      const guchar *o = px + y * stride + x * n;
      if (n == 4 && o[3] < 128)
        continue;
      r += o[0]; g += o[1]; b += o[2]; c++;
    }
  if (!c) {
    rgb[0] = 0.91; rgb[1] = 0.92; rgb[2] = 0.96;
    return;
  }
  r /= c * 255.0; g /= c * 255.0; b /= c * 255.0;
  if (MAX (r, MAX (g, b)) - MIN (r, MIN (g, b)) < 0.08) {   /* grey icon: a neutral light background */
    rgb[0] = 0.91; rgb[1] = 0.92; rgb[2] = 0.96;
    return;
  }
  const double mix = 0.55;                                  /* pastel */
  rgb[0] = r + (1 - r) * mix; rgb[1] = g + (1 - g) * mix; rgb[2] = b + (1 - b) * mix;
}

/* The colour just inside the icon's outline (a ring at 90–97 % of the radius): what a shaped background must
 * match. FALSE when the ring is see-through. */
static gboolean
edge_color_of (GdkPixbuf *pb, double rgb[3])
{
  const guchar *px = gdk_pixbuf_read_pixels (pb);
  int n = gdk_pixbuf_get_n_channels (pb), stride = gdk_pixbuf_get_rowstride (pb);
  int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
  double cx = w / 2.0, cy = h / 2.0, R = MIN (w, h) / 2.0, r = 0, g = 0, b = 0;
  int c = 0, seen = 0;
  const double fs[] = { 0.9, 0.94, 0.97 };
  for (int i = 0; i < 72; i++) {
    double a = i / 72.0 * 2 * G_PI;
    for (int k = 0; k < 3; k++) {
      int x = (int) round (cx + cos (a) * R * fs[k]), y = (int) round (cy + sin (a) * R * fs[k]);
      if (x < 0 || y < 0 || x >= w || y >= h)
        continue;
      const guchar *o = px + y * stride + x * n;
      seen++;
      if (n == 4 && o[3] < 200)
        continue;
      r += o[0]; g += o[1]; b += o[2]; c++;
    }
  }
  if (!seen || c < seen * 0.5)
    return FALSE;
  rgb[0] = r / c / 255; rgb[1] = g / c / 255; rgb[2] = b / c / 255;
  return TRUE;
}

static guint
hash_int (const char *s)
{
  gint32 h = 0;
  for (; *s; s++)
    h = (gint32) ((guint32) h * 31 + (guchar) *s);
  return (guint) ABS ((gint64) h);
}

static GdkPixbuf *
pack_pixbuf (Pack *pack, const char *name, int px)
{
  if (!name)
    return NULL;
  g_autofree char *png = g_strdup_printf ("%s.png", name);
  g_autofree char *path = g_build_filename (pack->dir, "icons", png, NULL);
  return load_at (path, px);
}

static const char *
pick (GPtrArray *list, const char *id)
{
  return list->len ? list->pdata[hash_int (id) % list->len] : NULL;
}

static GdkPixbuf *
surface_to_pixbuf (cairo_surface_t *surf, int px)
{
  cairo_surface_flush (surf);
  return gdk_pixbuf_get_from_surface (surf, 0, 0, px, px);
}

/* What is on `surf` (drawn by `cr`) becomes art in the chosen shape: over a background in the colour of its own
 * edge, clipped to the shape, as pack art is when a shape other than the pack's is picked. */
static void
reshape (cairo_t *cr, cairo_surface_t *surf, const char *shape, int px)
{
  g_autoptr (GdkPixbuf) art = surface_to_pixbuf (surf, px);
  double rgb[3];
  if (!edge_color_of (art, rgb))
    tint_of (art, rgb);
  cairo_save (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_restore (cr);
  cairo_save (cr);
  shape_path (cr, shape, px);
  cairo_clip (cr);
  cairo_set_source_rgb (cr, rgb[0], rgb[1], rgb[2]);
  cairo_paint (cr);
  paint_pixbuf (cr, art, px, 0, 0);
  cairo_restore (cr);
}

GdkPixbuf *
neo_icons_render (NeoIcons *self, GAppInfo *info, int px)
{
  Pack *pack = current_pack (self);
  g_autofree char *raw_shape = g_settings_get_string (self->settings, "icon-shape");
  gboolean wrap = g_settings_get_boolean (self->settings, "icon-wrap-unthemed");
  gboolean legacy = g_settings_get_boolean (self->settings, "icon-shape-legacy");
  const char *shape = g_strcmp0 (raw_shape, "system") ? raw_shape : (pack && pack->shape ? pack->shape : "squircle");
  const char *id = g_app_info_get_id (info);
  g_autofree char *key = cache_key (self, info, px);
  g_autofree char *png = g_strdup_printf ("%s/%s.png", self->cache, key);
  g_autofree char *none = g_strdup_printf ("%s/%s.none", self->cache, key);

  cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, px, px);
  cairo_t *cr = cairo_create (surf);
  gboolean drawn = FALSE, missing = FALSE;

  const char *drawable = pack ? g_hash_table_lookup (pack->map, id) : NULL;
  g_autoptr (GdkPixbuf) art = drawable ? pack_pixbuf (pack, drawable, px) : NULL;
  if (art) {
    /* "System" keeps the pack's own art. Any other shape re-wraps it: the pack icon over a background in the
     * colour of its own edge, clipped to the shape. */
    if (!g_strcmp0 (raw_shape, "system")) {
      paint_pixbuf (cr, art, px, 0, 0);
    } else {
      g_autoptr (GdkPixbuf) f = fit (art, px);
      double rgb[3];
      if (!edge_color_of (f, rgb))
        tint_of (f, rgb);
      shape_path (cr, shape, px);
      cairo_clip (cr);
      cairo_set_source_rgb (cr, rgb[0], rgb[1], rgb[2]);
      cairo_paint (cr);
      paint_pixbuf (cr, f, px, 0, 0);
    }
    drawn = TRUE;
  } else {
    g_autoptr (GdkPixbuf) src = source_pixbuf (info, px, &missing);
    g_autoptr (GdkPixbuf) back = src && wrap && pack && pack->iconback->len ?
                                 pack_pixbuf (pack, pick (pack->iconback, id), px) : NULL;
    g_autoptr (GdkPixbuf) whole = src ? fit (src, px) : NULL;
    int bx, by, bw, bh, sx, sy, sw, sh;
    if (back && whole && full_bleed (whole) && alpha_box (back, 16, &bx, &by, &bw, &bh) &&
        alpha_box (whole, 16, &sx, &sy, &sw, &sh)) {
      /* An icon with a background of its own sat shrunk on the pack's backplate: a frame in a frame, and smaller
       * than the pack's icons. Its art is cropped to that background, fills the backplate's box and takes the
       * backplate's outline: the pack's size and shape, the app's own picture. */
      g_autoptr (GdkPixbuf) art = gdk_pixbuf_new_subpixbuf (whole, sx, sy, sw, sh);
      g_autoptr (GdkPixbuf) big = gdk_pixbuf_scale_simple (art, bw, bh, GDK_INTERP_HYPER);
      cairo_surface_t *mask = gdk_cairo_surface_create_from_pixbuf (back, 1, NULL);
      cairo_push_group (cr);
      gdk_cairo_set_source_pixbuf (cr, big, bx, by);
      cairo_paint (cr);
      cairo_pop_group_to_source (cr);
      cairo_mask_surface (cr, mask, 0, 0);
      cairo_surface_destroy (mask);
      drawn = TRUE;
      if (g_strcmp0 (raw_shape, "system"))     /* the shape picked, not the backplate's */
        reshape (cr, surf, shape, px);
    } else if (back) {
      if (back)
        paint_pixbuf (cr, back, px, 0, 0);
      int inner = (int) round (px * pack->scale);
      paint_pixbuf (cr, src, inner, round ((px - inner) / 2.0), round ((px - inner) / 2.0));
      /* iconmask: opaque mask pixels are cut away (PorterDuff DST_OUT) */
      g_autoptr (GdkPixbuf) mask = pack_pixbuf (pack, pick (pack->iconmask, id), px);
      if (mask) {
        cairo_set_operator (cr, CAIRO_OPERATOR_DEST_OUT);
        paint_pixbuf (cr, mask, px, 0, 0);
        cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
      }
      g_autoptr (GdkPixbuf) upon = pack_pixbuf (pack, pick (pack->iconupon, id), px);
      if (upon)
        paint_pixbuf (cr, upon, px, 0, 0);
      drawn = TRUE;
      if (g_strcmp0 (raw_shape, "system"))
        reshape (cr, surf, shape, px);
    } else if (src && legacy) {
      g_autoptr (GdkPixbuf) f = fit (src, px);
      double rgb[3];
      tint_of (f, rgb);
      shape_path (cr, shape, px);
      cairo_clip (cr);
      cairo_set_source_rgb (cr, rgb[0], rgb[1], rgb[2]);
      cairo_paint (cr);
      int inner = (int) round (px * LEGACY_FG);
      paint_pixbuf (cr, src, inner, round ((px - inner) / 2.0), round ((px - inner) / 2.0));
      drawn = TRUE;
    }
  }
  cairo_destroy (cr);

  GdkPixbuf *out = NULL;
  /* The generic icon standing in is never kept: apt installs an app's icons before the theme's cache knows them
   * (icon-theme.cache is rebuilt at the end of the run), and the generic icon drawn then stayed for good. */
  if (drawn) {
    if (!missing)
      cairo_surface_write_to_png (surf, png);
    out = surface_to_pixbuf (surf, px);
  } else if (!missing) {
    g_file_set_contents (none, "", 0, NULL);
  }
  cairo_surface_destroy (surf);
  return out;
}
