/* The parallax wallpaper, moved by the compositor: every layer is a Wayland subsurface of the wallpaper window,
 * holding a buffer drawn once (the layer cropped to the screen plus as far as it moves on every side). A tilt only changes
 * each layer's wp_viewport source rectangle, in fractions of a pixel, and commits it: no pixels are drawn per
 * frame, phoc samples the layers where they now are. The tilt is the accelerometer's (IIO, read once per frame),
 * measured against a slowly following rest position, so the picture sits centred however the phone is held. */
#define _GNU_SOURCE            /* memfd_create */
#include "neo-parallax.h"

#include <gdk/gdkwayland.h>
#include <json-glib/json-glib.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "viewporter-client-protocol.h"

#define ACCEL_DIR "/sys/bus/iio/devices"
#define TILT_FULL 0.20      /* of g: a tilt this far moves the front layer its whole range */
/* Per frame at 60 Hz. 0.20/0.22 were tuned while the sensor gave 10 Hz, so the filters saw a new value every sixth
 * frame; at 60 Hz the same figures followed every tremor of the hand (owner: "too responsive, it was smoother").
 * These give time constants of ≈ 0.25 s and ≈ 0.15 s: the picture glides after the phone. */
#define SMOOTH 0.065        /* the low-pass on the sensor, against its noise */
#define EASE 0.11           /* how quickly the picture follows that, a second stage so it glides */
#define RATE_RUN 60         /* Hz asked of the accelerometer while shown, and back to RATE_IDLE */
#define RATE_IDLE 10
#define REST 0.004          /* per frame: how quickly the rest position follows the way the phone is held */
#define IDLE_MS 100         /* sensor polling while nothing moves */

typedef struct {
  char   *file;
  double  depth;
  double  margin;          /* logical px of the layer beyond each screen edge, in its buffer */
  int     for_w, for_h, for_scale;     /* the screen its buffer was drawn for */
  struct wl_surface    *surface;
  struct wl_subsurface *sub;
  struct wp_viewport   *viewport;
  struct wl_buffer     *buffer;
  int     bw, bh;
} Layer;

struct _NeoParallax {
  GtkWidget  *win;
  GPtrArray  *layers;
  double      range;               /* logical px a depth-1 layer moves either way */
  double      tilt;                /* the tilt (of g) that moves it that far */
  int         w, h, scale;
  guint       gen;                 /* the size's generation: a drawing for an older one is dropped */
  gboolean    running;
  char       *accel[3];
  char       *rate;                /* its sampling_frequency, if it has one */
  double      g[3], rest[3];       /* filtered gravity, and the rest position */
  gboolean    have_g;
  double      ox, oy;              /* the front layer's offset, -1…1 */
  double      tx, ty;              /* where it is heading */
  struct wl_callback *frame;
  gint64      frame_asked;
  guint       idle;
  int         still;
};

static struct wl_compositor    *compositor;
static struct wl_subcompositor *subcompositor;
static struct wl_shm           *shm;
static struct wp_viewporter    *viewporter;

static void
global (void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
  if (!g_strcmp0 (iface, wl_compositor_interface.name))
    compositor = wl_registry_bind (reg, name, &wl_compositor_interface, MIN (version, 4));
  else if (!g_strcmp0 (iface, wl_subcompositor_interface.name))
    subcompositor = wl_registry_bind (reg, name, &wl_subcompositor_interface, 1);
  else if (!g_strcmp0 (iface, wl_shm_interface.name))
    shm = wl_registry_bind (reg, name, &wl_shm_interface, 1);
  else if (!g_strcmp0 (iface, wp_viewporter_interface.name))
    viewporter = wl_registry_bind (reg, name, &wp_viewporter_interface, 1);
}

static void
global_remove (void *data, struct wl_registry *reg, uint32_t name)
{
}

static const struct wl_registry_listener reg_listener = { global, global_remove };

static gboolean
bind_globals (void)
{
  if (compositor)
    return subcompositor && shm && viewporter;
  struct wl_display *wl = gdk_wayland_display_get_wl_display (gdk_display_get_default ());
  struct wl_registry *reg = wl_display_get_registry (wl);
  wl_registry_add_listener (reg, &reg_listener, NULL);
  wl_display_roundtrip (wl);
  return compositor && subcompositor && shm && viewporter;
}

static void
find_accel (NeoParallax *self)
{
  g_autoptr (GDir) dir = g_dir_open (ACCEL_DIR, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir))) {
    g_autofree char *x = g_build_filename (ACCEL_DIR, name, "in_accel_x_raw", NULL);
    if (!g_file_test (x, G_FILE_TEST_EXISTS))
      continue;
    const char *axes[] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
      g_autofree char *f = g_strdup_printf ("in_accel_%s_raw", axes[i]);
      self->accel[i] = g_build_filename (ACCEL_DIR, name, f, NULL);
    }
    self->rate = g_build_filename (ACCEL_DIR, name, "in_accel_sampling_frequency", NULL);
    g_message ("parallax: accelerometer %s", name);
    return;
  }
  g_message ("parallax: no accelerometer, the wallpaper stays still");
}

/* the hub reports at 10 Hz unless asked: too coarse to follow a hand */
static void
set_rate (NeoParallax *self, int hz)
{
  char v[16];
  int fd = self->rate ? open (self->rate, O_WRONLY) : -1;     /* sysfs: no temp file and rename */
  if (fd < 0)
    return;
  int n = g_snprintf (v, sizeof v, "%d", hz);
  if (write (fd, v, n) != n)
    g_message ("parallax: the accelerometer rate stays");
  close (fd);
}

static gboolean
read_accel (NeoParallax *self, double g[3])
{
  for (int i = 0; i < 3; i++) {
    char buf[32];
    if (!self->accel[i])
      return FALSE;
    int fd = open (self->accel[i], O_RDONLY);
    if (fd < 0)
      return FALSE;
    ssize_t n = read (fd, buf, sizeof buf - 1);
    close (fd);
    if (n <= 0)
      return FALSE;
    buf[n] = '\0';
    g[i] = g_ascii_strtod (buf, NULL);
  }
  return TRUE;
}

/* ---- buffers ---- */

/* the pixels into a memfd, ARGB8888: no Wayland in here, it runs in the drawing thread; -1 if it failed */
static int
shm_fill (GdkPixbuf *pb, int w, int h)
{
  int stride = w * 4, size = stride * h;
  int fd = memfd_create ("neo-parallax", MFD_CLOEXEC);
  if (fd < 0 || ftruncate (fd, size) < 0) {
    if (fd >= 0)
      close (fd);
    return -1;
  }
  guint8 *dst = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (dst == MAP_FAILED) {
    close (fd);
    return -1;
  }
  /* GdkPixbuf RGB(A), straight alpha -> ARGB8888 little endian (B G R A), premultiplied */
  const guint8 *src = gdk_pixbuf_read_pixels (pb);
  int n = gdk_pixbuf_get_n_channels (pb), sstride = gdk_pixbuf_get_rowstride (pb);
  for (int y = 0; y < h; y++) {
    const guint8 *s = src + y * sstride;
    guint8 *d = dst + y * stride;
    for (int x = 0; x < w; x++, s += n, d += 4) {
      guint a = n == 4 ? s[3] : 255;
      d[0] = s[2] * a / 255;
      d[1] = s[1] * a / 255;
      d[2] = s[0] * a / 255;
      d[3] = a;
    }
  }
  munmap (dst, size);
  return fd;
}

/* the memfd as a buffer (main thread); takes the fd */
static struct wl_buffer *
shm_buffer (int fd, int w, int h)
{
  int stride = w * 4, size = stride * h;
  struct wl_shm_pool *pool = wl_shm_create_pool (shm, fd, size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer (pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);
  return buf;
}

/* ---- drawing, in a thread: decoding a 2340 px layer takes the better part of a second, and the main thread has
 * to answer phoc's configures meanwhile (a rotation that waits 3 s for them breaks phoc's layout) ---- */

typedef struct {
  char   *file;
  double  margin;
  int     fd, bw, bh;          /* the result */
} Draw;

typedef struct {
  NeoParallax *self;
  guint        gen;
  int          w, h, scale;
  GArray      *draws;          /* Draw, one per layer */
} Job;

static void
job_free (Job *j)
{
  for (guint i = 0; i < j->draws->len; i++) {
    Draw *d = &g_array_index (j->draws, Draw, i);
    g_free (d->file);
    if (d->fd >= 0)
      close (d->fd);
  }
  g_array_unref (j->draws);
  g_free (j);
}

/* each layer scaled to cover the screen plus as far as it moves on every side (range × |depth|), centred: a layer
 * that hardly moves is hardly zoomed */
static void
draw_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  Job *j = data;
  for (guint i = 0; i < j->draws->len; i++) {
    Draw *d = &g_array_index (j->draws, Draw, i);
    d->bw = (int) round ((j->w + 2 * d->margin) * j->scale);
    d->bh = (int) round ((j->h + 2 * d->margin) * j->scale);
    g_autoptr (GError) err = NULL;
    g_autoptr (GdkPixbuf) pixels = gdk_pixbuf_new_from_file (d->file, &err);
    if (!pixels) {
      g_message ("parallax: %s", err->message);
      continue;
    }
    int pw = gdk_pixbuf_get_width (pixels), ph = gdk_pixbuf_get_height (pixels);
    double k = MAX ((double) d->bw / pw, (double) d->bh / ph);
    int sw = (int) ceil (pw * k), sh = (int) ceil (ph * k);
    g_autoptr (GdkPixbuf) scaled = gdk_pixbuf_scale_simple (pixels, sw, sh, GDK_INTERP_BILINEAR);
    g_clear_object (&pixels);
    g_autoptr (GdkPixbuf) crop = gdk_pixbuf_new_subpixbuf (scaled, (sw - d->bw) / 2, (sh - d->bh) / 2, d->bw, d->bh);
    d->fd = shm_fill (crop, d->bw, d->bh);
  }
  g_task_return_boolean (task, TRUE);
}

static void place (NeoParallax *self);
static NeoParallax *alive;   /* the one parallax; a drawing that finishes after it is freed finds NULL here */

static void
drawn (GObject *src, GAsyncResult *res, gpointer data)
{
  Job *j = g_task_get_task_data (G_TASK (res));
  NeoParallax *self = j->self;
  if (self != alive || j->gen != self->gen)  /* freed, or the screen changed again: a newer drawing is coming */
    return;
  for (guint i = 0; i < j->draws->len && i < self->layers->len; i++) {
    Draw *d = &g_array_index (j->draws, Draw, i);
    Layer *l = self->layers->pdata[i];
    if (d->fd < 0)
      continue;
    struct wl_buffer *old = l->buffer;
    l->buffer = shm_buffer (d->fd, d->bw, d->bh);
    d->fd = -1;
    l->bw = d->bw;
    l->bh = d->bh;
    l->margin = d->margin;
    l->for_w = j->w;
    l->for_h = j->h;
    l->for_scale = j->scale;
    if (l->sub) {
      wl_surface_attach (l->surface, l->buffer, 0, 0);
      wl_surface_damage_buffer (l->surface, 0, 0, l->bw, l->bh);
    }
    if (old)
      wl_buffer_destroy (old);
  }
  place (self);
  if (self->win && gtk_widget_get_mapped (self->win))
    gtk_widget_queue_draw (self->win);       /* the parent's commit makes newly attached subsurfaces appear */
}

static void
draw_layers (NeoParallax *self)
{
  if (!self->w)
    return;
  Job *j = g_new0 (Job, 1);
  j->self = self;
  j->gen = ++self->gen;
  j->w = self->w;
  j->h = self->h;
  j->scale = self->scale;
  j->draws = g_array_new (FALSE, TRUE, sizeof (Draw));
  for (guint i = 0; i < self->layers->len; i++) {
    Layer *l = self->layers->pdata[i];
    Draw d = { g_strdup (l->file), ceil (self->range * fabs (l->depth)), -1, 0, 0 };
    g_array_append_val (j->draws, d);
  }
  GTask *t = g_task_new (NULL, NULL, drawn, NULL);
  g_task_set_task_data (t, j, (GDestroyNotify) job_free);
  g_task_run_in_thread (t, draw_thread);
  g_object_unref (t);
}

/* ---- placing ---- */

static void
place (NeoParallax *self)
{
  for (guint i = 0; i < self->layers->len; i++) {
    Layer *l = self->layers->pdata[i];
    if (!l->surface || !l->buffer)
      continue;
    /* moving a layer right means showing more of its left: the source window moves the other way */
    /* depth -1…1: the layers behind the screen plane (negative) move against the ones in front */
    if (l->for_w != self->w || l->for_h != self->h || l->for_scale != self->scale) {
      /* drawn for another size (a rotation): the whole buffer stretched, until this size's drawing arrives */
      wp_viewport_set_source (l->viewport, wl_fixed_from_int (0), wl_fixed_from_int (0),
                              wl_fixed_from_int (l->bw), wl_fixed_from_int (l->bh));
      wp_viewport_set_destination (l->viewport, self->w, self->h);
      wl_surface_commit (l->surface);
      continue;
    }
    double dx = -self->ox * self->range * l->depth, dy = -self->oy * self->range * l->depth;
    double sx = (l->margin + dx) * self->scale, sy = (l->margin + dy) * self->scale;
    wp_viewport_set_source (l->viewport, wl_fixed_from_double (sx), wl_fixed_from_double (sy),
                            wl_fixed_from_double (self->w * self->scale), wl_fixed_from_double (self->h * self->scale));
    wp_viewport_set_destination (l->viewport, self->w, self->h);
    wl_surface_commit (l->surface);
  }
}

/* One sensor reading: filters it, moves the rest position towards it, and places the layers if the picture
 * moved by more than a hair. TRUE when it did. */
static gboolean
step (NeoParallax *self)
{
  double g[3];
  if (!read_accel (self, g))
    return FALSE;
  double n = sqrt (g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
  if (n <= 0)
    return FALSE;
  for (int i = 0; i < 3; i++)
    g[i] /= n;
  if (!self->have_g) {
    memcpy (self->g, g, sizeof g);
    memcpy (self->rest, g, sizeof g);
    self->have_g = TRUE;
  }
  for (int i = 0; i < 3; i++) {
    self->g[i] += (g[i] - self->g[i]) * SMOOTH;
    self->rest[i] += (self->g[i] - self->rest[i]) * REST;
  }
  /* x: the right edge dipping; y: the top edge coming towards you */
  self->tx = CLAMP ((self->g[0] - self->rest[0]) / self->tilt, -1, 1);
  self->ty = CLAMP ((self->g[1] - self->rest[1]) / self->tilt, -1, 1);
  double ox = self->ox + (self->tx - self->ox) * EASE;
  double oy = self->oy + (self->ty - self->oy) * EASE;
  double moved = fabs (ox - self->ox) * self->range + fabs (oy - self->oy) * self->range;
  if (moved * self->scale < 0.02)
    return FALSE;
  self->ox = ox;
  self->oy = oy;
  place (self);
  return TRUE;
}

static const struct wl_callback_listener frame_listener;

/* One frame of following the tilt. The frame callback is asked for before the layers' commit: a commit that
 * changes nothing does not make phoc draw, so the callback only comes when the picture moved. */
static void
tick (NeoParallax *self)
{
  Layer *top = self->layers->len ? self->layers->pdata[self->layers->len - 1] : NULL;
  if (top && top->surface && !self->frame) {
    self->frame = wl_surface_frame (top->surface);
    self->frame_asked = g_get_monotonic_time ();
    wl_callback_add_listener (self->frame, &frame_listener, self);
  }
  if (step (self))
    self->still = 0;
  else
    self->still++;
}

static void
frame_done (void *data, struct wl_callback *cb, uint32_t time)
{
  NeoParallax *self = data;
  wl_callback_destroy (cb);
  self->frame = NULL;
  /* still for half a second: the idle poll takes over until the phone moves again */
  if (self->running && self->still < 30)
    tick (self);
}

static const struct wl_callback_listener frame_listener = { frame_done };

static gboolean
idle_poll (gpointer data)
{
  NeoParallax *self = data;
  if (!self->running) {
    self->idle = 0;
    return G_SOURCE_REMOVE;
  }
  /* a frame callback phoc never answers (the layer hidden under a window) would stop the loop for good */
  if (self->frame && g_get_monotonic_time () - self->frame_asked > 250000) {
    wl_callback_destroy (self->frame);
    self->frame = NULL;
  }
  if (self->still >= 30 || !self->frame)
    tick (self);
  return G_SOURCE_CONTINUE;
}

/* ---- surfaces, following the window's wl_surface (GTK 3 makes a new one at every map) ---- */

static void
attach_layers (NeoParallax *self)
{
  GdkWindow *gw = gtk_widget_get_window (self->win);
  struct wl_surface *parent = gw ? gdk_wayland_window_get_wl_surface (gw) : NULL;
  if (!parent || !self->w)
    return;
  for (guint i = 0; i < self->layers->len; i++) {
    Layer *l = self->layers->pdata[i];
    if (!l->surface) {
      l->surface = wl_compositor_create_surface (compositor);
      l->viewport = wp_viewporter_get_viewport (viewporter, l->surface);
      /* no input: touches go to the window below the layers */
      struct wl_region *empty = wl_compositor_create_region (compositor);
      wl_surface_set_input_region (l->surface, empty);
      wl_region_destroy (empty);
    }
    l->sub = wl_subcompositor_get_subsurface (subcompositor, l->surface, parent);
    wl_subsurface_set_position (l->sub, 0, 0);
    wl_subsurface_set_desync (l->sub);
    if (l->buffer) {         /* else it appears when its drawing arrives (drawn) */
      wl_surface_attach (l->surface, l->buffer, 0, 0);
      wl_surface_damage_buffer (l->surface, 0, 0, l->bw, l->bh);
    }
  }
  place (self);
  gtk_widget_queue_draw (self->win);       /* the parent's commit makes the subsurfaces appear */
}

static void
detach_layers (NeoParallax *self)
{
  if (self->frame) {
    wl_callback_destroy (self->frame);
    self->frame = NULL;
  }
  for (guint i = 0; i < self->layers->len; i++) {
    Layer *l = self->layers->pdata[i];
    g_clear_pointer (&l->sub, wl_subsurface_destroy);
  }
}

static void on_map (GtkWidget *w, NeoParallax *self) { attach_layers (self); }
static void on_unmap (GtkWidget *w, NeoParallax *self) { detach_layers (self); }

static void
layer_free (Layer *l)
{
  g_clear_pointer (&l->sub, wl_subsurface_destroy);
  g_clear_pointer (&l->viewport, wp_viewport_destroy);
  g_clear_pointer (&l->surface, wl_surface_destroy);
  g_clear_pointer (&l->buffer, wl_buffer_destroy);
  g_free (l->file);
  g_free (l);
}

NeoParallax *
neo_parallax_new (GtkWidget *wall_win, const char *dir)
{
  g_autofree char *json = g_build_filename (dir, "parallax.json", NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) err = NULL;
  if (!bind_globals ()) {
    g_message ("parallax: the compositor lacks subsurfaces, shm or wp_viewporter");
    return NULL;
  }
  if (!json_parser_load_from_file (parser, json, &err)) {
    g_message ("parallax: %s", err->message);
    return NULL;
  }
  JsonObject *o = json_node_get_object (json_parser_get_root (parser));
  JsonArray *arr = json_object_has_member (o, "layers") ? json_object_get_array_member (o, "layers") : NULL;
  if (!arr || !json_array_get_length (arr))
    return NULL;

  NeoParallax *self = g_new0 (NeoParallax, 1);
  self->win = wall_win;
  self->range = json_object_has_member (o, "range") ? json_object_get_double_member (o, "range") : 30;
  self->tilt = json_object_has_member (o, "tilt") ? json_object_get_double_member (o, "tilt") : TILT_FULL;
  self->layers = g_ptr_array_new_with_free_func ((GDestroyNotify) layer_free);
  for (guint i = 0; i < json_array_get_length (arr); i++) {
    JsonObject *lo = json_array_get_object_element (arr, i);
    Layer *l = g_new0 (Layer, 1);
    l->file = g_build_filename (dir, json_object_get_string_member (lo, "file"), NULL);
    l->depth = json_object_has_member (lo, "depth") ? json_object_get_double_member (lo, "depth") : 0;
    if (!g_file_test (l->file, G_FILE_TEST_EXISTS)) {
      g_message ("parallax: no %s", l->file);
      layer_free (l);
      continue;
    }
    g_ptr_array_add (self->layers, l);
  }
  find_accel (self);
  g_signal_connect_after (wall_win, "map", G_CALLBACK (on_map), self);
  g_signal_connect (wall_win, "unmap", G_CALLBACK (on_unmap), self);
  g_message ("parallax: %u layers from %s, range %.0f px", self->layers->len, dir, self->range);
  alive = self;
  return self;
}

void
neo_parallax_free (NeoParallax *self)
{
  if (!self)
    return;
  neo_parallax_run (self, FALSE);
  if (alive == self)
    alive = NULL;
  self->gen++;
  g_signal_handlers_disconnect_by_data (self->win, self);
  detach_layers (self);
  g_ptr_array_unref (self->layers);
  for (int i = 0; i < 3; i++)
    g_free (self->accel[i]);
  g_free (self->rate);
  g_free (self);
}

void
neo_parallax_resize (NeoParallax *self, int w, int h, int scale)
{
  if (w == self->w && h == self->h && scale == self->scale)
    return;
  self->w = w;
  self->h = h;
  self->scale = scale;
  draw_layers (self);
  if (gtk_widget_get_mapped (self->win)) {
    if (!self->layers->len || !((Layer *) self->layers->pdata[0])->sub)
      attach_layers (self);
    else
      place (self);          /* the old pictures stretched to the new size, at once */
  }
}

void
neo_parallax_run (NeoParallax *self, gboolean run)
{
  if (self->running == run)
    return;
  self->running = run;
  if (run) {
    self->have_g = FALSE;
    self->still = 0;
    set_rate (self, RATE_RUN);
    self->idle = g_timeout_add (IDLE_MS, idle_poll, self);
    tick (self);
  } else {
    g_clear_handle_id (&self->idle, g_source_remove);
    set_rate (self, RATE_IDLE);
  }
}
