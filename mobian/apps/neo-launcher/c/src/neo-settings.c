/* The launcher settings window (see neo-settings.h), libhandy rows bound to de.yesman.neo. */
#include "neo-settings.h"

#include <handy.h>

#include "neo-home.h"
#include "neo-icons.h"

static const char *shapes[][2] = {
  { "system", "Pack / system" }, { "circle", "Circle" }, { "squircle", "Squircle" }, { "rounded", "Rounded square" },
  { "square", "Square" }, { "teardrop", "Teardrop" }, { "cupertino", "Cupertino" }, { "cylinder", "Cylinder" },
  { "egg", "Egg" }, { "octagon", "Octagon" }, { "hexagon", "Hexagon" }, { "diamond", "Diamond" },
};

static GtkWidget *current;

static GtkWidget *
spin (GSettings *s, const char *key, const char *title, int min, int max)
{
  GtkWidget *row = hdy_action_row_new ();
  hdy_preferences_row_set_title (HDY_PREFERENCES_ROW (row), title);
  GtkWidget *b = gtk_spin_button_new_with_range (min, max, 1);
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  g_settings_bind (s, key, b, "value", G_SETTINGS_BIND_DEFAULT);
  gtk_container_add (GTK_CONTAINER (row), b);
  return row;
}

static GtkWidget *
toggle (GSettings *s, const char *key, const char *title, const char *subtitle)
{
  GtkWidget *row = hdy_action_row_new ();
  hdy_preferences_row_set_title (HDY_PREFERENCES_ROW (row), title);
  if (subtitle)
    hdy_action_row_set_subtitle (HDY_ACTION_ROW (row), subtitle);
  GtkWidget *sw = gtk_switch_new ();
  gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);
  g_settings_bind (s, key, sw, "active", G_SETTINGS_BIND_DEFAULT);
  gtk_container_add (GTK_CONTAINER (row), sw);
  hdy_action_row_set_activatable_widget (HDY_ACTION_ROW (row), sw);
  return row;
}

/* a combo row over string choices (value, label) bound to a string key */
typedef struct {
  GSettings *s;
  char      *key;
  GPtrArray *values;
  GtkWidget *row;
  gulong     changed;
} Choice;

static void
choice_free (gpointer data)
{
  Choice *c = data;
  g_signal_handler_disconnect (c->s, c->changed);
  g_ptr_array_unref (c->values);
  g_free (c->key);
  g_free (c);
}

static void
choice_sync (GSettings *s, const char *key, Choice *c)
{
  g_autofree char *v = g_settings_get_string (c->s, c->key);
  guint i = 0;
  if (!g_ptr_array_find_with_equal_func (c->values, v, g_str_equal, &i))
    i = 0;
  if (hdy_combo_row_get_selected_index (HDY_COMBO_ROW (c->row)) != (int) i)
    hdy_combo_row_set_selected_index (HDY_COMBO_ROW (c->row), i);
}

static void
choice_picked (GtkWidget *row, GParamSpec *ps, Choice *c)
{
  int i = hdy_combo_row_get_selected_index (HDY_COMBO_ROW (row));
  if (i < 0 || i >= (int) c->values->len)
    return;
  g_autofree char *v = g_settings_get_string (c->s, c->key);
  if (g_strcmp0 (v, c->values->pdata[i]))
    g_settings_set_string (c->s, c->key, c->values->pdata[i]);
}

static char *
value_name (gpointer item, gpointer data)
{
  return hdy_value_object_dup_string (item);
}

/* values and labels alternate in `pairs` */
static GtkWidget *
choice (GSettings *s, const char *key, const char *title, GPtrArray *pairs)
{
  Choice *c = g_new0 (Choice, 1);
  c->s = s;
  c->key = g_strdup (key);
  c->values = g_ptr_array_new_with_free_func (g_free);
  GListStore *model = g_list_store_new (HDY_TYPE_VALUE_OBJECT);
  for (guint i = 0; i + 1 < pairs->len; i += 2) {
    g_ptr_array_add (c->values, g_strdup (pairs->pdata[i]));
    g_autoptr (HdyValueObject) o = hdy_value_object_new_collect (G_TYPE_STRING, pairs->pdata[i + 1]);
    g_list_store_append (model, o);
  }
  GtkWidget *row = c->row = hdy_combo_row_new ();
  hdy_preferences_row_set_title (HDY_PREFERENCES_ROW (row), title);
  hdy_combo_row_bind_name_model (HDY_COMBO_ROW (row), G_LIST_MODEL (model),
                                 value_name, NULL, NULL);
  g_object_unref (model);
  choice_sync (s, key, c);
  g_autofree char *sig = g_strdup_printf ("changed::%s", key);
  c->changed = g_signal_connect (s, sig, G_CALLBACK (choice_sync), c);
  g_signal_connect (row, "notify::selected-index", G_CALLBACK (choice_picked), c);
  g_object_set_data_full (G_OBJECT (row), "neo-choice", c, choice_free);
  return row;
}

static void
pair (GPtrArray *a, const char *value, const char *label)
{
  g_ptr_array_add (a, g_strdup (value));
  g_ptr_array_add (a, g_strdup (label));
}

static GtkWidget *
group (const char *title, ...)
{
  GtkWidget *g = hdy_preferences_group_new ();
  hdy_preferences_group_set_title (HDY_PREFERENCES_GROUP (g), title);
  va_list ap;
  va_start (ap, title);
  for (GtkWidget *r; (r = va_arg (ap, GtkWidget *));)
    gtk_container_add (GTK_CONTAINER (g), r);
  va_end (ap);
  return g;
}

/* parallax wallpapers: directories with a parallax.json, the user's and the system's */
static GPtrArray *
parallax_choices (void)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  pair (a, "", "Off (the GNOME wallpaper)");
  g_autofree char *user = g_build_filename (g_get_user_data_dir (), "neo-launcher", "parallax", NULL);
  const char *roots[] = { user, "/usr/share/neo-launcher/parallax" };
  for (guint r = 0; r < G_N_ELEMENTS (roots); r++) {
    g_autoptr (GDir) d = g_dir_open (roots[r], 0, NULL);
    for (const char *n; d && (n = g_dir_read_name (d));) {
      g_autofree char *dir = g_build_filename (roots[r], n, NULL);
      g_autofree char *json = g_build_filename (dir, "parallax.json", NULL);
      if (g_file_test (json, G_FILE_TEST_EXISTS)) {
        g_autofree char *label = g_strdup (n);
        label[0] = g_ascii_toupper (label[0]);
        pair (a, dir, label);
      }
    }
  }
  return a;
}

static void
wallpaper_picture (GtkWidget *b, gpointer data)
{
  const char *argv[] = { "neo-settings", "--page", "wallpaper", NULL };
  g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void
on_destroy (GtkWidget *w, NeoHome *home)
{
  current = NULL;
}

/* back (Alt+Left from Neo's edge gesture) and Escape close it, and Neo comes back */
static gboolean
on_key (GtkWidget *w, GdkEventKey *e, NeoHome *home)
{
  if (e->keyval == GDK_KEY_Escape || (e->keyval == GDK_KEY_Left && (e->state & GDK_MOD1_MASK))) {
    gtk_widget_destroy (w);
    neo_home_show (home);
    return TRUE;
  }
  return FALSE;
}

static void
close_clicked (GtkWidget *b, NeoHome *home)
{
  gtk_widget_destroy (gtk_widget_get_toplevel (b));
  neo_home_show (home);
}

void
neo_settings_show (GtkApplication *app, GSettings *s, NeoHome *home)
{
  /* Neo Settings' Home screen page, the phone's own settings style; this window only when it is not installed */
  g_autofree char *ns = g_find_program_in_path ("neo-settings");
  if (ns) {
    const char *argv[] = { ns, "--page", "home", NULL };
    g_autoptr (GError) err = NULL;
    if (g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &err)) {
      neo_home_hide (home);
      return;
    }
    g_warning ("neo-settings: %s", err->message);
  }
  if (current) {
    gtk_window_present (GTK_WINDOW (current));
    neo_home_hide (home);
    return;
  }
  /* a plain window with its own ×: the title buttons of a preferences window showed no close on Phosh */
  GtkWidget *win = current = hdy_application_window_new ();
  gtk_window_set_application (GTK_WINDOW (win), app);
  gtk_window_set_title (GTK_WINDOW (win), "Launcher settings");
  gtk_window_set_default_size (GTK_WINDOW (win), 360, 720);
  g_signal_connect (win, "destroy", G_CALLBACK (on_destroy), home);
  g_signal_connect (win, "key-press-event", G_CALLBACK (on_key), home);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = hdy_header_bar_new ();
  hdy_header_bar_set_title (HDY_HEADER_BAR (header), "Launcher settings");
  hdy_header_bar_set_show_close_button (HDY_HEADER_BAR (header), FALSE);
  GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (close, "Close");
  gtk_style_context_add_class (gtk_widget_get_style_context (close), "circular");
  g_signal_connect (close, "clicked", G_CALLBACK (close_clicked), home);
  hdy_header_bar_pack_end (HDY_HEADER_BAR (header), close);
  gtk_container_add (GTK_CONTAINER (box), header);

  GtkWidget *page = hdy_preferences_page_new ();
  gtk_widget_set_vexpand (page, TRUE);
  gtk_container_add (GTK_CONTAINER (page), group ("Home screen",
    spin (s, "desktop-grid-columns", "Columns", 3, neo_home_max_columns (home)),
    spin (s, "desktop-grid-rows", "Rows", 3, neo_home_max_rows (home)),
    toggle (s, "desktop-hide-app-labels", "Hide app labels", NULL),
    toggle (s, "home-widgets", "Widgets", "The weather and the clock over the first page"), NULL));
  gtk_container_add (GTK_CONTAINER (page), group ("Dock", spin (s, "dock-num-icons", "Icons", 2, neo_home_max_columns (home)), NULL));
  gtk_container_add (GTK_CONTAINER (page), group ("App drawer",
    spin (s, "drawer-grid-columns", "Columns", 3, neo_home_max_columns (home)),
    toggle (s, "search-drawer-enabled", "Search field", NULL), NULL));

  g_autoptr (GPtrArray) packs = g_ptr_array_new_with_free_func (g_free);
  pair (packs, "", "None (theme icons)");
  g_autoptr (GPtrArray) found = neo_icons_list_packs ();
  for (guint i = 0; found && i < found->len; i++) {
    NeoPack *p = found->pdata[i];
    pair (packs, p->id, p->title);
  }
  g_autoptr (GPtrArray) shape_list = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < G_N_ELEMENTS (shapes); i++)
    pair (shape_list, shapes[i][0], shapes[i][1]);
  gtk_container_add (GTK_CONTAINER (page), group ("Icons",
    choice (s, "icon-pack", "Icon pack", packs),
    choice (s, "icon-shape", "Icon shape", shape_list),
    toggle (s, "icon-wrap-unthemed", "Wrap unthemed icons", "Icons the pack does not cover get its background"),
    toggle (s, "icon-shape-legacy", "Shape legacy icons", "Icons without pack art on a shaped background"), NULL));

  g_autoptr (GPtrArray) parallax = parallax_choices ();
  GtkWidget *pic = hdy_action_row_new ();
  hdy_preferences_row_set_title (HDY_PREFERENCES_ROW (pic), "Picture");
  hdy_action_row_set_subtitle (HDY_ACTION_ROW (pic), "The still wallpaper, in GNOME Settings");
  GtkWidget *go = gtk_button_new_from_icon_name ("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_valign (go, GTK_ALIGN_CENTER);
  gtk_style_context_add_class (gtk_widget_get_style_context (go), "flat");
  g_signal_connect (go, "clicked", G_CALLBACK (wallpaper_picture), NULL);
  gtk_container_add (GTK_CONTAINER (pic), go);
  hdy_action_row_set_activatable_widget (HDY_ACTION_ROW (pic), go);
  gtk_container_add (GTK_CONTAINER (page), group ("Wallpaper",
    choice (s, "wallpaper-parallax", "Parallax", parallax), pic, NULL));

  g_autoptr (GPtrArray) launchers = g_ptr_array_new_with_free_func (g_free);
  pair (launchers, "neo", "Neo");
  pair (launchers, "phosh", "Phosh app grid");
  gtk_container_add (GTK_CONTAINER (page), group ("Launcher", choice (s, "launcher", "Home screen", launchers), NULL));

  gtk_container_add (GTK_CONTAINER (box), page);
  gtk_container_add (GTK_CONTAINER (win), box);
  gtk_widget_show_all (win);
  neo_home_hide (home);
}
