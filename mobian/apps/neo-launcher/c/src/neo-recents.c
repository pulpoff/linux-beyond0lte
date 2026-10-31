/* The task view (see neo-recents.h). Everything that moves is a NeoSurf: the wallpaper, the dim over it, each
 * card (the window's thumbnail, rounded once into its buffer), each card's chip (icon, name, memory) and the
 * panel under the cards (the memory bar and Close all). The window itself is transparent and only takes the
 * touches; a frame of an animation or a drag places the surfaces and commits, and phoc draws them where they
 * are: nothing is painted per frame. */
#include "neo-recents.h"

#include <gtk-layer-shell.h>
#include <gio/gdesktopappinfo.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <math.h>

#include "neo-apps.h"
#include "neo-home.h"
#include "neo-launch.h"
#include "neo-surf.h"

#define FINAL 0.62          /* a settled card's width, of the screen's */
#define GAP 22              /* px between settled cards */
#define CHIP 44             /* px above a card for its icon and name */
#define CLOSE_AT 0.18       /* of the screen: a card pushed up this far closes on release */
#define FLING 0.6           /* px/ms */
#define RADIUS 34           /* a card's corners at full size (One UI: ~21 px settled) */
#define DIM 0.45
#define PILL_H 44
#define PILL_BOTTOM 28
#define TRACK 0.42          /* of the screen's height: a pull this far up shrinks the app to its card */

typedef struct {
  char     *id, *app_id, *title;
  GAppInfo *info;
  NeoSurf  *card, *chip;
  double    dy, fade;          /* pushed up to close (px, up < 0), and its opacity then */
  guint64   mem;
  gboolean  thumb;
} Card;

/* an animation of up to four doubles from where they are to where they go, ease-out (or in) cubic */
typedef struct {
  double  *v[4];
  double   from[4], to[4];
  int      n;
  gint64   start;
  int      ms;
  gboolean ease_in;
  void   (*done) (gpointer data, gpointer card);
  gpointer card;
} Anim;

struct _NeoRecents {
  NeoHome   *home;
  GtkWidget *win, *area;
  GtkGesture *drag;
  int        W, H, scale;
  GPtrArray *cards;
  NeoSurf   *wall, *dim, *panel;
  double     pill_x, pill_y, pill_w;   /* Close all, in window px */
  double     panel_y;

  double     shrink;          /* 0 the whole screen … 1 a settled card */
  double     reveal;          /* the other cards, chips and the panel: 0 hidden … 1 shown */
  double     scroll;          /* which card is in the middle (fractional while scrolling) */
  int        zoom_index;      /* a tapped card growing back to the whole screen, or -1 */
  double     zoom_t;
  double     out;             /* fading out: 0 … 1 */

  gboolean   active, settled, decided, shown, swallow, held, listed;
  double     track;           /* how far up the finger is, px, before the hold */
  gint64     began;           /* when the swipe began, for the timing log */
  int        late;            /* frames that took longer than 25 ms while it opened */
  gint64     last_frame;
  guint      session;
  int        focus;
  GPtrArray *anims;           /* Anim */
  Anim      *main_anim;       /* the view's own (shrink/reveal/scroll/zoom), replaced by the next */
  gboolean   frame_pending, dirty;

  /* the drag in the settled view */
  int        mode;            /* 0 undecided, 1 scroll, 2 close, 3 nothing */
  int        drag_card;
  double     drag_start_scroll;
  gint64     last_t;
  double     last_dx, last_dy, vx, vy;

  guint64    mem_total, mem_used, mem_cache, mem_free;
  gboolean   measuring;
};

static double lerp (double a, double z, double t) { return a + (z - a) * t; }

static char *
format_bytes (guint64 n)
{
  if (n >= (guint64) 1 << 30)
    return g_strdup_printf ("%.1f GB", n / (double) ((guint64) 1 << 30));
  return g_strdup_printf ("%.0f MB", n / (double) (1 << 20));
}

static void request_frame (NeoRecents *self);
static void layout (NeoRecents *self);

/* ---- drawing the pictures, once each ---- */

static PangoFontDescription *
font (NeoRecents *self, int px, gboolean bold)
{
  PangoContext *ctx = gtk_widget_get_pango_context (self->win);
  PangoFontDescription *d = pango_font_description_copy (pango_context_get_font_description (ctx));
  pango_font_description_set_absolute_size (d, px * PANGO_SCALE);
  pango_font_description_set_weight (d, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  return d;
}

static void
rounded (cairo_t *cr, double x, double y, double w, double h, double r)
{
  r = MIN (r, MIN (w, h) / 2);
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2, 0);
  cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI / 2);
  cairo_arc (cr, x + r, y + h - r, r, G_PI / 2, G_PI);
  cairo_arc (cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
  cairo_close_path (cr);
}

/* text with One UI's soft shadow under it */
static void
shadowed (cairo_t *cr, PangoLayout *l, double x, double y, double r, double g, double b, double a)
{
  cairo_move_to (cr, x, y + 1);
  cairo_set_source_rgba (cr, 0, 0, 0, 0.6 * a);
  pango_cairo_show_layout (cr, l);
  cairo_move_to (cr, x, y);
  cairo_set_source_rgba (cr, r, g, b, a);
  pango_cairo_show_layout (cr, l);
}

/* the card: the thumbnail cropped to the screen's shape with rounded corners; without one the app's icon */
static void
draw_card (NeoRecents *self, Card *c, const guint8 *data, int tw, int th, int stride)
{
  int bw, bh;
  if (data) {
    bw = tw;
    bh = (int) round ((double) tw * self->H / self->W);
  } else {
    bw = self->W * self->scale / 2;          /* a plain card: half resolution is plenty */
    bh = self->H * self->scale / 2;
  }
  cairo_surface_t *img = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, bw, bh);
  cairo_t *cr = cairo_create (img);
  rounded (cr, 0, 0, bw, bh, RADIUS * (double) bw / self->W);
  cairo_clip (cr);
  cairo_set_source_rgb (cr, 0x1c / 255.0, 0x1f / 255.0, 0x22 / 255.0);
  cairo_paint (cr);
  if (data) {
    cairo_surface_t *src = cairo_image_surface_create_for_data ((guint8 *) data, CAIRO_FORMAT_ARGB32, tw, th, stride);
    double k = MAX ((double) bw / tw, (double) bh / th);
    cairo_translate (cr, (bw - tw * k) / 2, 0);        /* the top of the window stays: its header */
    cairo_scale (cr, k, k);
    cairo_set_source_surface (cr, src, 0, 0);
    cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
    cairo_paint (cr);
    cairo_surface_destroy (src);
  } else if (c->info) {
    int px = 96 * self->scale / 2;
    g_autoptr (GdkPixbuf) icon = neo_home_app_icon (self->home, c->info, px);
    if (icon) {
      gdk_cairo_set_source_pixbuf (cr, icon, (bw - gdk_pixbuf_get_width (icon)) / 2.0,
                                   (bh - gdk_pixbuf_get_height (icon)) / 2.0);
      cairo_paint (cr);
    }
  }
  cairo_destroy (cr);
  neo_surf_set_image (c->card, img);
  cairo_surface_destroy (img);
  c->thumb = data != NULL;
}

static void
draw_chip (NeoRecents *self, Card *c)
{
  int s = self->scale;
  g_autoptr (PangoFontDescription) bold = font (self, 15 * s, TRUE);
  g_autoptr (PangoFontDescription) small = font (self, 13 * s, FALSE);
  const char *name = c->info ? neo_app_name (c->info) : NULL;
  if (!name || !*name)
    name = *c->title ? c->title : c->app_id;

  cairo_surface_t *probe = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *pc = cairo_create (probe);
  PangoLayout *l = pango_cairo_create_layout (pc);
  pango_layout_set_font_description (l, bold);
  pango_layout_set_text (l, name, -1);
  pango_layout_set_ellipsize (l, PANGO_ELLIPSIZE_END);
  pango_layout_set_width (l, (int) (self->W * FINAL * 0.62 * s) * PANGO_SCALE);
  pango_layout_set_single_paragraph_mode (l, TRUE);
  int lw, lh, mw = 0, mh = 0;
  pango_layout_get_pixel_size (l, &lw, &lh);
  PangoLayout *m = NULL;
  if (c->mem) {
    g_autofree char *mem = format_bytes (c->mem);
    m = pango_cairo_create_layout (pc);
    pango_layout_set_font_description (m, small);
    pango_layout_set_text (m, mem, -1);
    pango_layout_get_pixel_size (m, &mw, &mh);
  }
  int icon = 28 * s, gap = 8 * s;
  int w = icon + gap + lw + (m ? 2 * s + gap + mw : 0), h = (CHIP - 8) * s;
  cairo_surface_t *img = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, MAX (w, 1), h);
  cairo_t *cr = cairo_create (img);
  if (c->info) {
    g_autoptr (GdkPixbuf) pb = neo_home_app_icon (self->home, c->info, icon);
    if (pb) {
      gdk_cairo_set_source_pixbuf (cr, pb, 0, (h - gdk_pixbuf_get_height (pb)) / 2.0);
      cairo_paint (cr);
    }
  }
  shadowed (cr, l, icon + gap, (h - lh) / 2.0, 1, 1, 1, 1);
  if (m)
    shadowed (cr, m, icon + gap + lw + 2 * s + gap, (h - mh) / 2.0, 1, 1, 1, 0.7);
  cairo_destroy (cr);
  neo_surf_set_image (c->chip, img);
  cairo_surface_destroy (img);
  g_object_unref (l);
  if (m)
    g_object_unref (m);
  cairo_destroy (pc);
  cairo_surface_destroy (probe);
}

/* /proc/meminfo: used (apps, the kernel, GPU buffers: what cannot be dropped), cache (file pages the kernel can
 * drop at once), free. Shared memory counts as used: here that is mostly GPU buffers. */
static void
read_meminfo (NeoRecents *self)
{
  g_autofree char *text = NULL;
  if (!g_file_get_contents ("/proc/meminfo", &text, NULL, NULL))
    return;
  guint64 total = 0, free_ = 0, buffers = 0, cached = 0, srec = 0, shmem = 0;
  for (char *line = strtok (text, "\n"); line; line = strtok (NULL, "\n")) {
    char key[32];
    guint64 kb;
    if (sscanf (line, "%31[^:]: %" G_GUINT64_FORMAT, key, &kb) != 2)
      continue;
    kb *= 1024;
    if (!strcmp (key, "MemTotal")) total = kb;
    else if (!strcmp (key, "MemFree")) free_ = kb;
    else if (!strcmp (key, "Buffers")) buffers = kb;
    else if (!strcmp (key, "Cached")) cached = kb;
    else if (!strcmp (key, "SReclaimable")) srec = kb;
    else if (!strcmp (key, "Shmem")) shmem = kb;
  }
  if (!total)
    return;
  gint64 cache = (gint64) (buffers + cached + srec) - (gint64) shmem;
  self->mem_total = total;
  self->mem_free = free_;
  self->mem_cache = MAX (cache, 0);
  self->mem_used = total > free_ + self->mem_cache ? total - free_ - self->mem_cache : 0;
}

/* the panel under the cards: the memory bar with its legend (the total as a badge on the left, the three parts
 * on the right), and the Close all pill at the bottom */
static void
draw_panel (NeoRecents *self)
{
  int s = self->scale, W = self->W, H = self->H;
  double card_bottom = H * 0.46 + H * FINAL / 2;
  double top = card_bottom;
  int ph = (int) ceil (H - top);
  if (ph <= 0)
    return;
  self->panel_y = top;
  cairo_surface_t *img = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, W * s, ph * s);
  cairo_t *cr = cairo_create (img);
  cairo_scale (cr, s, s);
  g_autoptr (PangoFontDescription) bold = font (self, 15, TRUE);
  g_autoptr (PangoFontDescription) f12 = font (self, 12, FALSE);
  g_autoptr (PangoFontDescription) f12b = font (self, 12, TRUE);

  /* Close all */
  PangoLayout *l = pango_cairo_create_layout (cr);
  pango_layout_set_font_description (l, bold);
  pango_layout_set_text (l, "Close all", -1);
  int lw, lh;
  pango_layout_get_pixel_size (l, &lw, &lh);
  double pw = lw + 56, py = H - PILL_H - PILL_BOTTOM - top, px = (W - pw) / 2;
  rounded (cr, px, py, pw, PILL_H, PILL_H / 2.0);
  cairo_set_source_rgba (cr, 0x2a / 255.0, 0x2d / 255.0, 0x31 / 255.0, 0.92);
  cairo_fill (cr);
  cairo_move_to (cr, px + 28, py + (PILL_H - lh) / 2.0);
  cairo_set_source_rgb (cr, 1, 1, 1);
  pango_cairo_show_layout (cr, l);
  self->pill_x = px;
  self->pill_y = py + top;
  self->pill_w = pw;

  if (self->mem_total) {
    static const double colors[3][3] = { { 0xef, 0x53, 0x50 }, { 0xff, 0xb7, 0x4d }, { 0x66, 0xbb, 0x6a } };
    static const char *names[3] = { "Used", "Cache", "Free" };
    guint64 parts[3] = { self->mem_used, self->mem_cache, self->mem_free };
    double rw = round (W * FINAL), rx = (W - rw) / 2;
    /* the legend's height: three lines */
    PangoLayout *items[3];
    int iw = 0, ih = 0;
    for (int i = 0; i < 3; i++) {
      g_autofree char *v = format_bytes (parts[i]);
      g_autofree char *t = g_strdup_printf ("<span foreground=\"#%02x%02x%02x\">●</span> %s %s", (int) colors[i][0],
                                            (int) colors[i][1], (int) colors[i][2], names[i], v);
      items[i] = pango_cairo_create_layout (cr);
      pango_layout_set_font_description (items[i], f12);
      pango_layout_set_markup (items[i], t, -1);
      int w, h;
      pango_layout_get_pixel_size (items[i], &w, &h);
      iw = MAX (iw, w);
      ih = h;
    }
    double legend_h = 3 * ih + 2 * 2, block = 10 + 6 + legend_h;
    double by = (card_bottom + (H - PILL_H - PILL_BOTTOM) - block) / 2 - top;
    /* the bar: 10 px, the three parts in one rounded track */
    cairo_save (cr);
    rounded (cr, rx, by, rw, 10, 5);
    cairo_clip (cr);
    double x = rx;
    for (int i = 0; i < 3; i++) {
      double ww = rw * parts[i] / self->mem_total;
      cairo_set_source_rgb (cr, colors[i][0] / 255, colors[i][1] / 255, colors[i][2] / 255);
      cairo_rectangle (cr, x, by, ww + 0.5, 10);
      cairo_fill (cr);
      x += ww;
    }
    cairo_restore (cr);
    /* the badge: the phone's whole memory, outlined */
    g_autofree char *tot = format_bytes (self->mem_total);
    PangoLayout *t = pango_cairo_create_layout (cr);
    pango_layout_set_font_description (t, f12b);
    pango_layout_set_text (t, tot, -1);
    int tw, th;
    pango_layout_get_pixel_size (t, &tw, &th);
    double bw = tw + 16, bh = th + 2, ly = by + 16;
    double lx = (W - (bw + 14 + iw)) / 2;
    rounded (cr, lx + 0.5, ly + (legend_h - bh) / 2 + 0.5, bw - 1, bh - 1, bh / 2);
    cairo_set_source_rgba (cr, 1, 1, 1, 0.6);
    cairo_set_line_width (cr, 1);
    cairo_stroke (cr);
    shadowed (cr, t, lx + 8, ly + (legend_h - th) / 2, 1, 1, 1, 1);
    g_object_unref (t);
    for (int i = 0; i < 3; i++) {
      shadowed (cr, items[i], lx + bw + 14, ly + i * (ih + 2), 1, 1, 1, 0.85);
      g_object_unref (items[i]);
    }
  }
  g_object_unref (l);
  cairo_destroy (cr);
  neo_surf_set_image (self->panel, img);
  cairo_surface_destroy (img);
}

static void
draw_wall (NeoRecents *self)
{
  g_autoptr (GdkPixbuf) pb = neo_home_wallpaper (self->home, self->W * self->scale, self->H * self->scale);
  if (!pb) {
    neo_surf_set_solid (self->wall, 0, 0, 0);
    return;
  }
  cairo_surface_t *img = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, gdk_pixbuf_get_width (pb),
                                                     gdk_pixbuf_get_height (pb));
  cairo_t *cr = cairo_create (img);
  gdk_cairo_set_source_pixbuf (cr, pb, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);
  neo_surf_set_image (self->wall, img);
  cairo_surface_destroy (img);
}

/* ---- cards ---- */

static GAppInfo *
app_info (const char *app_id)
{
  if (!app_id || !*app_id)
    return NULL;
  g_autofree char *a = g_strdup_printf ("%s.desktop", app_id);
  g_autofree char *lower = g_ascii_strdown (a, -1);
  GDesktopAppInfo *i = g_desktop_app_info_new (a);
  if (!i)
    i = g_desktop_app_info_new (lower);
  return i ? G_APP_INFO (i) : NULL;
}

static void
card_free (Card *c)
{
  neo_surf_free (c->card);
  neo_surf_free (c->chip);
  g_clear_object (&c->info);
  g_free (c->id);
  g_free (c->app_id);
  g_free (c->title);
  g_free (c);
}

static void
load_thumb (NeoRecents *self, Card *c, const char *path, guint w, guint h, guint stride)
{
  g_autofree char *data = NULL;
  gsize len = 0;
  if (path && *path && w && h && g_file_get_contents (path, &data, &len, NULL) && len >= (gsize) stride * h) {
    draw_card (self, c, (const guint8 *) data, w, h, stride);
    return;
  }
  if (!neo_surf_width (c->card))
    draw_card (self, c, NULL, 0, 0, 0);
}

/* the stacking: wallpaper, dim, the cards, their chips, the panel */
static void
restack (NeoRecents *self)
{
  NeoSurf *below = self->dim;
  neo_surf_place_above (self->dim, self->wall);
  for (guint i = 0; i < self->cards->len; i++) {
    Card *c = self->cards->pdata[i];
    neo_surf_place_above (c->card, below);
    below = c->card;
  }
  for (guint i = 0; i < self->cards->len; i++) {
    Card *c = self->cards->pdata[i];
    neo_surf_place_above (c->chip, below);
    below = c->chip;
  }
  neo_surf_place_above (self->panel, below);
}

static gboolean
mapped (NeoRecents *self)
{
  return neo_surf_attached (self->wall);
}

static void
attach_card (NeoRecents *self, Card *c)
{
  if (mapped (self)) {
    neo_surf_attach (c->card, self->win);
    neo_surf_attach (c->chip, self->win);
  }
}

typedef struct {
  NeoRecents *self;
  guint       session;
  gboolean    first;
} FetchCtx;

static void measure (NeoRecents *self);
static void track_show (NeoRecents *self);

static gboolean
debug_timing (void)
{
  static int on = -1;
  if (on < 0)
    on = g_getenv ("NEO_DEBUG_RECENTS") != NULL;
  return on;
}

static void
got_windows (GObject *src, GAsyncResult *res, gpointer data)
{
  FetchCtx *ctx = data;
  NeoRecents *self = ctx->self;
  g_autoptr (GVariant) out = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (!self->active || self->session != ctx->session) {
    g_free (ctx);
    return;
  }
  g_autoptr (GPtrArray) old = self->cards;
  self->cards = g_ptr_array_new_with_free_func ((GDestroyNotify) card_free);
  int focus = -1;
  if (out) {
    g_autoptr (GVariantIter) it = NULL;
    const char *id, *app_id, *title, *path;
    guint w, h, stride;
    gboolean focused;
    g_variant_get (out, "(a(ssssuuub))", &it);
    while (g_variant_iter_next (it, "(&s&s&s&suuub)", &id, &app_id, &title, &path, &w, &h, &stride, &focused)) {
      Card *c = NULL;
      for (guint i = 0; old && i < old->len; i++) {
        Card *o = old->pdata[i];
        if (!g_strcmp0 (o->id, id)) {
          c = g_ptr_array_steal_index (old, i);
          break;
        }
      }
      if (!c) {
        c = g_new0 (Card, 1);
        c->id = g_strdup (id);
        c->app_id = g_strdup (app_id);
        c->title = g_strdup (title);
        c->info = app_info (app_id);
        c->fade = 1;
        c->card = neo_surf_new ();
        c->chip = neo_surf_new ();
        draw_chip (self, c);
        attach_card (self, c);
      }
      if (!c->thumb)
        load_thumb (self, c, path, w, h, stride);
      if (focused)
        focus = self->cards->len;
      g_ptr_array_add (self->cards, c);
    }
  }
  if (ctx->first) {
    self->focus = focus;
    self->scroll = focus >= 0 ? focus : MAX (0, (int) self->cards->len - 1);
    self->listed = TRUE;
    measure (self);
    if (debug_timing ())
      g_message ("recents: windows listed %d ms after the swipe began",
                 (int) ((g_get_monotonic_time () - self->began) / 1000));
    if (self->track > 0)
      track_show (self);
  }
  self->scroll = CLAMP (self->scroll, 0, MAX (0, (int) self->cards->len - 1));
  if (mapped (self))
    restack (self);
  request_frame (self);
  g_free (ctx);
}

static void
fetch (NeoRecents *self, gboolean first)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  FetchCtx *ctx = g_new0 (FetchCtx, 1);
  ctx->self = self;
  ctx->session = self->session;
  ctx->first = first;
  g_dbus_connection_call (bus, PHOSH_NEO_BUS, PHOSH_NEO_PATH, PHOSH_NEO_BUS, "GetWindows", NULL,
                          G_VARIANT_TYPE ("(a(ssssuuub))"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, NULL,
                          got_windows, ctx);
}

/* Each app's memory beside its name, read by appmem.py in a process of its own: reading /proc of a process stuck
 * in the kernel blocks the reader, and Neo's main loop (the system gestures) must never be that reader. */
typedef struct {
  NeoRecents *self;
  guint       session;
} MemCtx;

static void
measured (GObject *src, GAsyncResult *res, gpointer data)
{
  MemCtx *ctx = data;
  NeoRecents *self = ctx->self;
  g_autofree char *out = NULL;
  self->measuring = FALSE;
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out &&
      self->active && self->session == ctx->session) {
    g_autoptr (JsonParser) p = json_parser_new ();
    if (json_parser_load_from_data (p, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p))) {
      JsonObject *o = json_node_get_object (json_parser_get_root (p));
      for (guint i = 0; i < self->cards->len; i++) {
        Card *c = self->cards->pdata[i];
        if (c->app_id && json_object_has_member (o, c->app_id)) {
          c->mem = (guint64) json_object_get_double_member (o, c->app_id);
          draw_chip (self, c);
        }
      }
      request_frame (self);
    }
  }
  g_free (ctx);
}

static void
measure (NeoRecents *self)
{
  const char *helpers[] = { "/usr/libexec/neo-launcher/appmem.py", "/usr/share/neo-launcher/appmem.py" };
  const char *helper = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (helpers) && !helper; i++)
    if (g_file_test (helpers[i], G_FILE_TEST_EXISTS))
      helper = helpers[i];
  if (!helper || self->measuring || !self->cards->len)
    return;
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "timeout");
  g_ptr_array_add (argv, "-s");
  g_ptr_array_add (argv, "KILL");
  g_ptr_array_add (argv, "3");
  g_ptr_array_add (argv, "python3");
  g_ptr_array_add (argv, (char *) helper);
  for (guint i = 0; i < self->cards->len; i++) {
    Card *c = self->cards->pdata[i];
    if (c->app_id && *c->app_id)
      g_ptr_array_add (argv, c->app_id);
  }
  g_ptr_array_add (argv, NULL);
  GSubprocess *p = g_subprocess_newv ((const char * const *) argv->pdata,
                                      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (!p)
    return;
  MemCtx *ctx = g_new0 (MemCtx, 1);
  ctx->self = self;
  ctx->session = self->session;
  self->measuring = TRUE;
  g_subprocess_communicate_utf8_async (p, NULL, NULL, measured, ctx);
  g_object_unref (p);
}

/* ---- the frame: placing every surface ---- */

typedef struct {
  double s, cw, ch, step, cx, cy;
} Geo;

static Geo
geometry (NeoRecents *self)
{
  Geo g;
  g.s = lerp (1, FINAL, self->shrink);
  g.cw = self->W * g.s;
  g.ch = self->H * g.s;
  g.step = self->W * FINAL + GAP;
  g.cx = self->W / 2.0;
  g.cy = lerp (self->H / 2.0, self->H * 0.46, self->shrink);
  return g;
}

static int
card_at (NeoRecents *self, double x, double y)
{
  Geo g = geometry (self);
  for (guint i = 0; i < self->cards->len; i++) {
    double mx = g.cx + (i - self->scroll) * g.step;
    if (fabs (x - mx) <= g.cw / 2 && fabs (y - g.cy) <= g.ch / 2)
      return i;
  }
  return -1;
}

static void
layout (NeoRecents *self)
{
  if (!mapped (self))
    return;
  double W = self->W, H = self->H;
  double zoom = self->zoom_index >= 0 ? self->zoom_t : 0;
  double keep = (1 - zoom) * (1 - self->out);
  Geo g = geometry (self);

  /* the wallpaper shows behind the cards only once they have shrunk: before that the card covers it all */
  neo_surf_place (self->wall, 0, 0, W, H, CLAMP (self->shrink * 3, 0, 1) * (1 - self->out));
  neo_surf_place (self->dim, 0, 0, W, H, DIM * self->shrink * keep);
  int focus = (int) round (self->scroll);
  for (guint i = 0; i < self->cards->len; i++) {
    Card *c = self->cards->pdata[i];
    double s = g.s, x = g.cx + (i - self->scroll) * g.step, y = g.cy + c->dy, op = c->fade * (1 - self->out);
    if ((int) i != focus)
      op *= self->reveal;                       /* the neighbours come in with the hold */
    if (self->zoom_index >= 0) {
      if ((int) i == self->zoom_index) {
        s = lerp (g.s, 1, zoom);
        x = lerp (x, W / 2, zoom);
        y = lerp (y, H / 2, zoom);
      } else {
        op *= 1 - zoom;
      }
    }
    double cw = W * s, ch = H * s;
    if (x + cw / 2 < 0 || x - cw / 2 > W)
      op = 0;                                   /* off screen: not composited */
    neo_surf_place (c->card, x - cw / 2, y - ch / 2, cw, ch, op);
    double chip_w = MIN (neo_surf_width (c->chip) / (double) self->scale, g.cw);
    double chip_h = neo_surf_height (c->chip) / (double) self->scale;
    neo_surf_crop (c->chip, 0, 0, chip_w * self->scale, chip_h * self->scale);
    neo_surf_place (c->chip, x - chip_w / 2, y - ch / 2 - CHIP, chip_w, chip_h, op * self->reveal * (1 - zoom));
  }
  neo_surf_place (self->panel, 0, self->panel_y, W, neo_surf_height (self->panel) / (double) self->scale,
                  self->reveal * keep);
}

/* ---- animations ---- */

static Anim *
animate (NeoRecents *self, int ms, gboolean ease_in, void (*done) (gpointer, gpointer), gpointer card)
{
  Anim *a = g_new0 (Anim, 1);
  a->start = g_get_monotonic_time ();
  a->ms = ms;
  a->ease_in = ease_in;
  a->done = done;
  a->card = card;
  g_ptr_array_add (self->anims, a);
  request_frame (self);
  return a;
}

static void
anim_to (Anim *a, double *v, double to)
{
  a->v[a->n] = v;
  a->from[a->n] = *v;
  a->to[a->n] = to;
  a->n++;
}

static void
stop_main (NeoRecents *self)
{
  if (self->main_anim) {
    g_ptr_array_remove (self->anims, self->main_anim);
    self->main_anim = NULL;
  }
}

static Anim *
animate_view (NeoRecents *self, int ms, void (*done) (gpointer, gpointer))
{
  stop_main (self);
  return self->main_anim = animate (self, ms, FALSE, done, NULL);
}

static void
cancel_card_anims (NeoRecents *self, Card *c)
{
  for (guint i = 0; i < self->anims->len;) {
    Anim *a = self->anims->pdata[i];
    if (a->card == c && a != self->main_anim)
      g_ptr_array_remove_index (self->anims, i);
    else
      i++;
  }
}

/* steps every animation to now; the finished ones run their `done` after the frame is placed */
static GPtrArray *
step_anims (NeoRecents *self)
{
  gint64 now = g_get_monotonic_time ();
  GPtrArray *finished = NULL;
  for (guint i = 0; i < self->anims->len;) {
    Anim *a = self->anims->pdata[i];
    double t = CLAMP ((now - a->start) / (a->ms * 1000.0), 0, 1);
    double e = a->ease_in ? t * t * t : 1 - pow (1 - t, 3);
    for (int k = 0; k < a->n; k++)
      *a->v[k] = lerp (a->from[k], a->to[k], e);
    if (t >= 1) {
      if (!finished)
        finished = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (finished, g_ptr_array_steal_index (self->anims, i));
      if (a == self->main_anim)
        self->main_anim = NULL;
    } else {
      i++;
    }
  }
  return finished;
}

static void
on_frame (gpointer data)
{
  NeoRecents *self = data;
  self->frame_pending = FALSE;
  if (self->dirty || self->anims->len)
    request_frame (self);
}

/* One frame: step, place, commit, and ask phoc to say when it has been shown. Input in between only marks the
 * view dirty: one commit per frame, never more. */
static void
frame (NeoRecents *self)
{
  self->dirty = FALSE;
  gint64 now = g_get_monotonic_time ();
  if (debug_timing () && self->active && !self->settled && self->last_frame && now - self->last_frame > 25000)
    self->late++;
  self->last_frame = now;
  g_autoptr (GPtrArray) finished = step_anims (self);
  layout (self);
  if (mapped (self)) {
    self->frame_pending = TRUE;
    neo_surf_commit_parent (self->win, on_frame, self);
  }
  for (guint i = 0; finished && i < finished->len; i++) {
    Anim *a = finished->pdata[i];
    if (a->done)
      a->done (self, a->card);
  }
}

static void
request_frame (NeoRecents *self)
{
  self->dirty = TRUE;
  if (!self->frame_pending)
    frame (self);
}

/* ---- the window ---- */

static gboolean
draw_clear (GtkWidget *w, cairo_t *cr, NeoRecents *self)
{
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba (cr, 0, 0, 0, 0);
  cairo_paint (cr);
  return FALSE;
}

static void
on_map (GtkWidget *w, NeoRecents *self)
{
  neo_surf_attach (self->wall, self->win);
  neo_surf_attach (self->dim, self->win);
  for (guint i = 0; i < self->cards->len; i++)
    attach_card (self, self->cards->pdata[i]);
  neo_surf_attach (self->panel, self->win);
  restack (self);
  layout (self);            /* GTK's first commit (its first frame) shows them */
}

static void
on_unmap (GtkWidget *w, NeoRecents *self)
{
  neo_surf_cancel_frame (self->win);
  self->frame_pending = FALSE;
  neo_surf_detach (self->wall);
  neo_surf_detach (self->dim);
  neo_surf_detach (self->panel);
  for (guint i = 0; i < self->cards->len; i++) {
    Card *c = self->cards->pdata[i];
    neo_surf_detach (c->card);
    neo_surf_detach (c->chip);
  }
}

static void
on_size (GtkWidget *w, GdkRectangle *a, NeoRecents *self)
{
  int scale = gtk_widget_get_scale_factor (w);
  if (a->width <= 1 || (a->width == self->W && a->height == self->H && scale == self->scale))
    return;
  self->W = a->width;
  self->H = a->height;
  self->scale = scale;
  draw_wall (self);
  draw_panel (self);
  request_frame (self);
}

static void
show_window (NeoRecents *self)
{
  if (!self->W) {
    /* before the first configure: the monitor's size, so the pictures are ready for the first frame */
    GdkMonitor *m = gdk_display_get_primary_monitor (gdk_display_get_default ());
    if (!m)
      m = gdk_display_get_monitor (gdk_display_get_default (), 0);
    GdkRectangle geo = { 0, 0, 360, 760 };
    if (m)
      gdk_monitor_get_geometry (m, &geo);
    self->W = geo.width;
    self->H = geo.height;
    self->scale = m ? gdk_monitor_get_scale_factor (m) : 1;
    draw_wall (self);
  }
  draw_panel (self);
  gtk_widget_show (self->win);
}

/* ---- the settled view's input ---- */

static void ready (NeoRecents *self);
static void go_home (NeoRecents *self);
static void open_card (NeoRecents *self, int i);
static void close_card (NeoRecents *self, int i);

static void
drag_begin (GtkGestureDrag *g, double x, double y, NeoRecents *self)
{
  if (!self->settled) {
    gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  stop_main (self);
  self->mode = 0;
  self->drag_card = card_at (self, x, y);
  self->drag_start_scroll = self->scroll;
  self->last_t = g_get_monotonic_time ();
  self->last_dx = self->last_dy = self->vx = self->vy = 0;
}

static void
drag_update (GtkGestureDrag *g, double dx, double dy, NeoRecents *self)
{
  if (!self->settled)
    return;
  gint64 now = g_get_monotonic_time ();
  double dt = MAX (1, (now - self->last_t) / 1000.0);
  self->vx = (dx - self->last_dx) / dt;
  self->vy = (dy - self->last_dy) / dt;
  self->last_t = now;
  self->last_dx = dx;
  self->last_dy = dy;
  if (!self->mode && hypot (dx, dy) > 12)
    self->mode = fabs (dx) > fabs (dy) ? 1 : self->drag_card >= 0 && dy < 0 ? 2 : 3;
  Geo geo = geometry (self);
  if (self->mode == 1) {
    self->scroll = CLAMP (self->drag_start_scroll - dx / geo.step, -0.3, self->cards->len - 0.7);
  } else if (self->mode == 2) {
    Card *c = self->cards->pdata[self->drag_card];
    c->dy = MIN (0, dy);
    c->fade = 1 - CLAMP (-c->dy / (self->H * 0.5), 0, 0.8);
  }
  request_frame (self);
}

static void
drag_end (GtkGestureDrag *g, double dx, double dy, NeoRecents *self)
{
  if (!self->settled)
    return;
  if (self->mode == 1) {
    double to = round (self->scroll);
    if (fabs (self->vx) > FLING)
      to = self->vx < 0 ? ceil (self->drag_start_scroll + 0.01) : floor (self->drag_start_scroll - 0.01);
    Anim *a = animate_view (self, 240, NULL);
    anim_to (a, &self->scroll, CLAMP (to, 0, MAX (0, (int) self->cards->len - 1)));
  } else if (self->mode == 2) {
    Card *c = self->cards->pdata[self->drag_card];
    if (-c->dy > self->H * CLOSE_AT || self->vy < -FLING) {
      close_card (self, self->drag_card);
    } else {
      cancel_card_anims (self, c);
      Anim *a = animate (self, 200, FALSE, NULL, c);
      anim_to (a, &c->dy, 0);
      anim_to (a, &c->fade, 1);
    }
  } else if (!self->mode) {
    double sx, sy;
    gtk_gesture_drag_get_start_point (g, &sx, &sy);
    if (sy >= self->pill_y && sy <= self->pill_y + PILL_H && sx >= self->pill_x && sx <= self->pill_x + self->pill_w) {
      neo_phosh_call ("CloseAll", NULL);
      go_home (self);
      return;
    }
    int i = card_at (self, sx, sy);
    if (i >= 0)
      open_card (self, i);
    else
      go_home (self);
  }
  self->mode = 0;
}

/* ---- what the task view does ---- */

static void
closed_card (gpointer data, gpointer card)
{
  NeoRecents *self = data;
  Card *c = card;
  guint i;
  if (!g_ptr_array_find (self->cards, c, &i))
    return;
  neo_phosh_call ("Close", g_variant_new ("(s)", c->id));
  g_ptr_array_remove_index (self->cards, i);
  if (!self->cards->len) {
    go_home (self);
    return;
  }
  /* the memory comes back once the app has gone */
  read_meminfo (self);
  draw_panel (self);
  /* the next card slides in from the right; with none after it, the one before from the left */
  double old = round (self->scroll);
  if (i < self->cards->len && old >= i)
    self->scroll -= 1;
  Anim *a = animate_view (self, 220, NULL);
  anim_to (a, &self->scroll, CLAMP (old, 0, self->cards->len - 1));
}

static void
close_card (NeoRecents *self, int i)
{
  Card *c = self->cards->pdata[i];
  cancel_card_anims (self, c);
  Anim *a = animate (self, 180, TRUE, closed_card, c);
  anim_to (a, &c->dy, -self->H);
  anim_to (a, &c->fade, 0);
}

static void
hidden_done (gpointer data, gpointer card)
{
  neo_recents_hide (data);
}

static void
open_card (NeoRecents *self, int i)
{
  Card *c = self->cards->pdata[i];
  self->settled = FALSE;
  neo_phosh_call ("Activate", g_variant_new ("(s)", c->id));
  neo_home_hide (self->home);
  self->zoom_index = i;
  self->zoom_t = 0;
  Anim *a = animate_view (self, 240, hidden_done);
  anim_to (a, &self->zoom_t, 1);
}

static void
go_home (NeoRecents *self)
{
  neo_home_show (self->home);
  self->settled = FALSE;
  Anim *a = animate_view (self, 160, hidden_done);
  anim_to (a, &self->out, 1);
}

static void
ready (NeoRecents *self)
{
  if (!self->cards->len) {
    neo_recents_hide (self);
    neo_home_show (self->home);
    return;
  }
  self->settled = TRUE;
}

static void
held_done (gpointer data, gpointer card)
{
  NeoRecents *self = data;
  if (self->decided)
    ready (self);
}

void
neo_recents_begin (NeoRecents *self)
{
  self->swallow = FALSE;
  if (self->settled) {         /* in the task view a swipe up means home */
    self->swallow = TRUE;
    return;
  }
  stop_main (self);
  g_ptr_array_set_size (self->anims, 0);
  self->active = TRUE;
  self->settled = self->decided = self->shown = self->held = self->listed = FALSE;
  self->track = 0;
  self->began = g_get_monotonic_time ();
  self->late = 0;
  self->last_frame = 0;
  self->session++;
  self->shrink = self->reveal = self->scroll = self->zoom_t = self->out = 0;
  self->zoom_index = -1;
  g_ptr_array_set_size (self->cards, 0);
  read_meminfo (self);
  fetch (self, TRUE);
}

/* the app follows the finger, the way the drawer does: only the card's place changes per frame */
static void
track_show (NeoRecents *self)
{
  if (self->shown || self->focus < 0 || !self->cards->len)
    return;                    /* from the home screen there is no app to pull: only a hold shows the view */
  self->shown = TRUE;
  show_window (self);
  if (debug_timing ())
    g_message ("recents: shown %d ms after the swipe began",
               (int) ((g_get_monotonic_time () - self->began) / 1000));
}

void
neo_recents_track (NeoRecents *self, double up)
{
  if (self->swallow || !self->active || self->decided || self->held)
    return;
  self->track = up;
  if (!self->listed)
    return;
  track_show (self);
  if (!self->shown)
    return;
  self->shrink = CLAMP (up / (self->H * TRACK), 0, 1);
  request_frame (self);
}

void
neo_recents_hold (NeoRecents *self)
{
  if (self->swallow || !self->active || self->decided || self->held)
    return;
  self->held = TRUE;
  if (!self->shown) {
    self->shown = TRUE;
    show_window (self);
  }
  /* from wherever the finger has pulled it to */
  Anim *a = animate_view (self, 220, held_done);
  anim_to (a, &self->shrink, 1);
  anim_to (a, &self->reveal, 1);
}

void
neo_recents_release (NeoRecents *self, gboolean to_recents)
{
  if (self->swallow) {
    self->swallow = FALSE;
    go_home (self);
    return;
  }
  if (!self->active || self->decided)
    return;
  self->decided = TRUE;
  if (!to_recents && self->shown) {
    /* pulled but not held: home, which the edges show underneath, comes up as the card fades */
    go_home (self);
    return;
  }
  if (!to_recents || !self->shown) {
    neo_recents_hide (self);
    return;
  }
  if (debug_timing ())
    g_message ("recents: released, %d late frames while it opened", self->late);
  if (!self->main_anim)
    ready (self);
}

void
neo_recents_hide (NeoRecents *self)
{
  stop_main (self);
  g_ptr_array_set_size (self->anims, 0);
  self->active = self->settled = self->decided = FALSE;
  gtk_widget_hide (self->win);
  g_ptr_array_set_size (self->cards, 0);
  self->session++;
}

gboolean
neo_recents_active (NeoRecents *self)
{
  return self->active;
}

NeoRecents *
neo_recents_new (GtkApplication *app, NeoHome *home)
{
  if (!neo_surf_init ()) {
    g_message ("recents: the compositor lacks subsurfaces or wp_viewporter, no task view");
    return NULL;
  }
  NeoRecents *self = g_new0 (NeoRecents, 1);
  self->home = home;
  self->cards = g_ptr_array_new_with_free_func ((GDestroyNotify) card_free);
  self->anims = g_ptr_array_new_with_free_func (g_free);
  self->zoom_index = -1;
  self->wall = neo_surf_new ();
  self->dim = neo_surf_new ();
  neo_surf_set_solid (self->dim, 0, 0, 0);
  self->panel = neo_surf_new ();

  GtkWidget *win = self->win = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (win), "Neo Recents");
  gtk_window_set_decorated (GTK_WINDOW (win), FALSE);
  GdkScreen *screen = gtk_widget_get_screen (win);
  GdkVisual *rgba = gdk_screen_get_rgba_visual (screen);
  if (rgba)
    gtk_widget_set_visual (win, rgba);
  gtk_widget_set_app_paintable (win, TRUE);
  gtk_layer_init_for_window (GTK_WINDOW (win));
  gtk_layer_set_namespace (GTK_WINDOW (win), "neo-recents");
  /* above Neo's home (TOP) and the apps */
  gtk_layer_set_layer (GTK_WINDOW (win), GTK_LAYER_SHELL_LAYER_OVERLAY);
  for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++)
    gtk_layer_set_anchor (GTK_WINDOW (win), e, TRUE);
  gtk_layer_set_exclusive_zone (GTK_WINDOW (win), 0);
  gtk_layer_set_keyboard_mode (GTK_WINDOW (win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  gtk_style_context_add_class (gtk_widget_get_style_context (win), "neo-recents");

  self->area = gtk_drawing_area_new ();
  gtk_widget_set_app_paintable (self->area, TRUE);
  gtk_widget_add_events (self->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                     GDK_TOUCH_MASK);
  gtk_container_add (GTK_CONTAINER (win), self->area);
  g_signal_connect (win, "draw", G_CALLBACK (draw_clear), self);
  g_signal_connect_after (win, "map", G_CALLBACK (on_map), self);
  g_signal_connect (win, "unmap", G_CALLBACK (on_unmap), self);
  g_signal_connect (self->area, "size-allocate", G_CALLBACK (on_size), self);

  self->drag = gtk_gesture_drag_new (self->area);
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (self->drag), FALSE);
  g_signal_connect (self->drag, "drag-begin", G_CALLBACK (drag_begin), self);
  g_signal_connect (self->drag, "drag-update", G_CALLBACK (drag_update), self);
  g_signal_connect (self->drag, "drag-end", G_CALLBACK (drag_end), self);
  gtk_widget_show (self->area);
  return self;
}
