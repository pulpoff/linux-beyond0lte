/* The parallax picker (see ns-parallax.h). A preview is the set's preview.jpg if it has one, else its layers laid
 * over each other at preview size, made in a thread and kept in ~/.cache/neo-settings. */
#include "ns-parallax.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>

#define THUMB_W 150
#define THUMB_H 300
/* on screen: three in a row fit a 360 px wide phone */
#define PICK_W 88
#define PICK_H 176

typedef struct {
  GSettings *neo;
  GtkWidget *flow;
} Grid;

static char *
thumb_cache (const char *dir)
{
  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_MD5, dir, -1);
  g_autofree char *name = g_strconcat ("parallax-", sum, ".png", NULL);
  return g_build_filename (g_get_user_cache_dir (), "neo-settings", name, NULL);
}

/* the layers in parallax.json order, each scaled to cover the preview, centred */
static GdkPixbuf *
compose (const char *dir)
{
  g_autofree char *json = g_build_filename (dir, "parallax.json", NULL);
  g_autoptr (JsonParser) p = json_parser_new ();
  if (!json_parser_load_from_file (p, json, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p)))
    return NULL;
  JsonObject *o = json_node_get_object (json_parser_get_root (p));
  JsonArray *layers = json_object_has_member (o, "layers") ? json_object_get_array_member (o, "layers") : NULL;
  GdkPixbuf *out = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, THUMB_W, THUMB_H);
  gdk_pixbuf_fill (out, 0x000000ff);
  for (guint i = 0; layers && i < json_array_get_length (layers); i++) {
    JsonObject *l = json_array_get_object_element (layers, i);
    const char *file = l ? json_object_get_string_member_with_default (l, "file", NULL) : NULL;
    if (!file)
      continue;
    g_autofree char *path = g_build_filename (dir, file, NULL);
    int w, h;
    if (!gdk_pixbuf_get_file_info (path, &w, &h) || w <= 0 || h <= 0)
      continue;
    double k = MAX ((double) THUMB_W / w, (double) THUMB_H / h);
    int sw = MAX (THUMB_W, (int) (w * k + 0.5)), sh = MAX (THUMB_H, (int) (h * k + 0.5));
    g_autoptr (GdkPixbuf) pb = gdk_pixbuf_new_from_file_at_scale (path, sw, sh, FALSE, NULL);
    if (!pb)
      continue;
    gdk_pixbuf_composite (pb, out, 0, 0, THUMB_W, THUMB_H, -(sw - THUMB_W) / 2.0, -(sh - THUMB_H) / 2.0, 1, 1,
                          GDK_INTERP_BILINEAR, 255);
  }
  return out;
}

static void
thumb_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  const char *dir = data;
  g_autofree char *preview = g_build_filename (dir, "preview.jpg", NULL);
  g_autofree char *cache = thumb_cache (dir);
  GdkPixbuf *pb = NULL;
  if (g_file_test (preview, G_FILE_TEST_EXISTS))
    pb = gdk_pixbuf_new_from_file_at_scale (preview, THUMB_W, THUMB_H, TRUE, NULL);
  if (!pb)
    pb = gdk_pixbuf_new_from_file (cache, NULL);
  if (!pb && (pb = compose (dir))) {
    g_autofree char *cdir = g_path_get_dirname (cache);
    g_mkdir_with_parents (cdir, 0700);
    gdk_pixbuf_save (pb, cache, "png", NULL, NULL);
  }
  if (!pb) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "no preview");
    return;
  }
  GdkTexture *t = ns_texture_for_pixbuf (pb);
  g_object_unref (pb);
  g_task_return_pointer (task, t, g_object_unref);
}

static void
thumb_done (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GdkTexture) t = g_task_propagate_pointer (G_TASK (res), NULL);
  GtkWidget *pic = GTK_WIDGET (src);
  if (t)
    gtk_picture_set_paintable (GTK_PICTURE (pic), GDK_PAINTABLE (t));
}

/* ---- the grid ---- */

static void
mark_current (Grid *g)
{
  g_autofree char *cur = g_settings_get_string (g->neo, "wallpaper-parallax");
  for (GtkWidget *c = gtk_widget_get_first_child (g->flow); c; c = gtk_widget_get_next_sibling (c)) {
    GtkWidget *card = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (c));
    const char *dir = g_object_get_data (G_OBJECT (card), "ns-dir");
    gboolean on = !g_strcmp0 (dir, cur);
    if (on)
      gtk_widget_add_css_class (card, "ns-picked");
    else
      gtk_widget_remove_css_class (card, "ns-picked");
  }
}

static void
picked (GtkFlowBox *flow, GtkFlowBoxChild *child, Grid *g)
{
  GtkWidget *card = gtk_flow_box_child_get_child (child);
  g_settings_set_string (g->neo, "wallpaper-parallax", g_object_get_data (G_OBJECT (card), "ns-dir"));
}

static void
changed (GSettings *s, const char *key, Grid *g)
{
  mark_current (g);
}

static GtkWidget *
card_new (const char *dir, const char *name)
{
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (v, "ns-parallax-card");
  g_object_set_data_full (G_OBJECT (v), "ns-dir", g_strdup (dir), g_free);
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_overflow (frame, GTK_OVERFLOW_HIDDEN);
  gtk_widget_add_css_class (frame, "ns-parallax-thumb");
  GtkWidget *pic = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (pic), GTK_CONTENT_FIT_COVER);
  gtk_widget_set_size_request (pic, PICK_W, PICK_H);
  gtk_box_append (GTK_BOX (frame), pic);
  if (*dir) {
    GTask *t = g_task_new (pic, NULL, thumb_done, NULL);
    g_task_set_task_data (t, g_strdup (dir), g_free);
    g_task_run_in_thread (t, thumb_thread);
    g_object_unref (t);
  } else {
    GtkWidget *off = gtk_image_new_from_icon_name ("action-unavailable-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (off), 32);
    gtk_widget_set_vexpand (off, TRUE);
    gtk_widget_set_valign (off, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand (pic, FALSE);
    gtk_widget_set_size_request (pic, -1, 0);
    gtk_widget_set_size_request (frame, PICK_W, PICK_H);
    gtk_box_append (GTK_BOX (frame), off);
  }
  gtk_box_append (GTK_BOX (v), frame);
  GtkWidget *l = gtk_label_new (name);
  gtk_widget_add_css_class (l, "ns-sub");
  gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (v), l);
  return v;
}

static void
add_sets (GtkWidget *flow, const char *root, GHashTable *seen)
{
  g_autoptr (GDir) d = g_dir_open (root, 0, NULL);
  if (!d)
    return;
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  for (const char *n; (n = g_dir_read_name (d));)
    g_ptr_array_add (names, g_strdup (n));
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < names->len; i++) {
    const char *n = names->pdata[i];
    g_autofree char *dir = g_build_filename (root, n, NULL);
    g_autofree char *json = g_build_filename (dir, "parallax.json", NULL);
    if (!g_file_test (json, G_FILE_TEST_EXISTS) || g_hash_table_contains (seen, n))
      continue;
    g_hash_table_add (seen, g_strdup (n));       /* the user's own set of a name wins over the system's */
    g_autofree char *title = g_strdup (n);
    title[0] = g_ascii_toupper (title[0]);
    gtk_flow_box_append (GTK_FLOW_BOX (flow), card_new (dir, title));
  }
}

static void
grid_free (Grid *g)
{
  g_signal_handlers_disconnect_by_data (g->neo, g);
  g_free (g);
}

GtkWidget *
ns_parallax_grid (GSettings *neo)
{
  Grid *g = g_new0 (Grid, 1);
  g->neo = neo;
  GtkWidget *flow = g->flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 2);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 3);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 12);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_append (GTK_FLOW_BOX (flow), card_new ("", "Off"));
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autofree char *mine = g_build_filename (g_get_user_data_dir (), "neo-launcher", "parallax", NULL);
  add_sets (flow, mine, seen);
  add_sets (flow, "/usr/share/neo-launcher/parallax", seen);
  g_signal_connect (flow, "child-activated", G_CALLBACK (picked), g);
  g_signal_connect (neo, "changed::wallpaper-parallax", G_CALLBACK (changed), g);
  g_object_set_data_full (G_OBJECT (flow), "ns-grid", g, (GDestroyNotify) grid_free);
  mark_current (g);
  return flow;
}
