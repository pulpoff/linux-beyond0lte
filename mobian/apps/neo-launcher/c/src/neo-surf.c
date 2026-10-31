/* Subsurfaces moved by the compositor (see neo-surf.h). They stay synchronized with the parent: a frame's
 * positions (which apply on the parent's commit anyway), sizes and opacities all land in one parent commit, so a
 * frame never shows half placed. */
#define _GNU_SOURCE            /* memfd_create */
#include "neo-surf.h"

#include <gdk/gdkwayland.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "alpha-modifier-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

struct _NeoSurf {
  struct wl_surface    *surface;
  struct wl_subsurface *sub;
  struct wp_viewport   *viewport;
  struct wp_alpha_modifier_surface_v1 *alpha_mod;
  struct wl_buffer     *buffer;
  int       bw, bh;
  gboolean  shown;              /* the buffer is attached (alpha > 0) */
  int       x, y, w, h;         /* the last placement, to send only what changed */
  double    alpha;
  gboolean  placed;
  gboolean  busy;               /* the compositor may still read the buffer (attached, not released yet) */
  guint8   *map;                /* a canvas: the buffer's memory, kept mapped, and cairo drawing straight into it */
  size_t    map_size;
  cairo_surface_t *canvas;
};

static struct wl_compositor    *compositor;
static struct wl_subcompositor *subcompositor;
static struct wl_shm           *shm;
static struct wp_viewporter    *viewporter;
static struct wp_alpha_modifier_v1 *alpha_modifier;
static struct wp_single_pixel_buffer_manager_v1 *single_pixel;

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
  else if (!g_strcmp0 (iface, wp_alpha_modifier_v1_interface.name))
    alpha_modifier = wl_registry_bind (reg, name, &wp_alpha_modifier_v1_interface, 1);
  else if (!g_strcmp0 (iface, wp_single_pixel_buffer_manager_v1_interface.name))
    single_pixel = wl_registry_bind (reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
}

static void
global_remove (void *data, struct wl_registry *reg, uint32_t name)
{
}

static const struct wl_registry_listener reg_listener = { global, global_remove };

gboolean
neo_surf_init (void)
{
  static gboolean done;
  if (!done) {
    GdkDisplay *display = gdk_display_get_default ();
    done = TRUE;
    if (!GDK_IS_WAYLAND_DISPLAY (display))
      return FALSE;
    struct wl_display *wl = gdk_wayland_display_get_wl_display (display);
    struct wl_registry *reg = wl_display_get_registry (wl);
    wl_registry_add_listener (reg, &reg_listener, NULL);
    wl_display_roundtrip (wl);
    if (!alpha_modifier)
      g_message ("neo-surf: no wp_alpha_modifier_v1, nothing fades");
  }
  return compositor && subcompositor && shm && viewporter;
}

NeoSurf *
neo_surf_new (void)
{
  NeoSurf *self = g_new0 (NeoSurf, 1);
  self->surface = wl_compositor_create_surface (compositor);
  self->viewport = wp_viewporter_get_viewport (viewporter, self->surface);
  if (alpha_modifier)
    self->alpha_mod = wp_alpha_modifier_v1_get_surface (alpha_modifier, self->surface);
  /* no input: touches go to the window under it */
  struct wl_region *empty = wl_compositor_create_region (compositor);
  wl_surface_set_input_region (self->surface, empty);
  wl_region_destroy (empty);
  self->alpha = 1;
  return self;
}

static void
canvas_drop (NeoSurf *self)
{
  g_clear_pointer (&self->canvas, cairo_surface_destroy);
  if (self->map)
    munmap (self->map, self->map_size);
  self->map = NULL;
  self->map_size = 0;
}

void
neo_surf_free (NeoSurf *self)
{
  if (!self)
    return;
  canvas_drop (self);
  g_clear_pointer (&self->sub, wl_subsurface_destroy);
  g_clear_pointer (&self->alpha_mod, wp_alpha_modifier_surface_v1_destroy);
  g_clear_pointer (&self->viewport, wp_viewport_destroy);
  g_clear_pointer (&self->surface, wl_surface_destroy);
  g_clear_pointer (&self->buffer, wl_buffer_destroy);
  g_free (self);
}

static void
use_buffer (NeoSurf *self, struct wl_buffer *buf, int w, int h)
{
  g_clear_pointer (&self->buffer, wl_buffer_destroy);
  self->buffer = buf;
  self->bw = w;
  self->bh = h;
  self->placed = FALSE;          /* the source crop and destination are sent again */
  self->busy = FALSE;
  if (self->shown && buf) {
    wl_surface_attach (self->surface, buf, 0, 0);
    self->busy = TRUE;
    wl_surface_damage_buffer (self->surface, 0, 0, w, h);
    wp_viewport_set_source (self->viewport, wl_fixed_from_int (-1), wl_fixed_from_int (-1),
                            wl_fixed_from_int (-1), wl_fixed_from_int (-1));
  }
}

/* a shm buffer filled by `fill` (BGRA premultiplied rows of w * 4 bytes) */
static struct wl_buffer *
shm_buffer (int w, int h, void (*fill) (guint8 *dst, gpointer data), gpointer data)
{
  int stride = w * 4, size = stride * h;
  int fd = memfd_create ("neo-surf", MFD_CLOEXEC);
  if (fd < 0 || ftruncate (fd, size) < 0) {
    if (fd >= 0)
      close (fd);
    return NULL;
  }
  guint8 *dst = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (dst == MAP_FAILED) {
    close (fd);
    return NULL;
  }
  fill (dst, data);
  munmap (dst, size);
  struct wl_shm_pool *pool = wl_shm_create_pool (shm, fd, size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer (pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);
  return buf;
}

typedef struct {
  const guint8 *src;
  int w, h, stride;
} Rows;

static void
copy_rows (guint8 *dst, gpointer data)
{
  Rows *r = data;
  for (int y = 0; y < r->h; y++)
    memcpy (dst + y * r->w * 4, r->src + y * r->stride, r->w * 4);
}

void
neo_surf_set_bgra (NeoSurf *self, const guint8 *data, int w, int h, int stride)
{
  Rows r = { data, w, h, stride };
  canvas_drop (self);
  if (w > 0 && h > 0)
    use_buffer (self, shm_buffer (w, h, copy_rows, &r), w, h);
}

void
neo_surf_set_image (NeoSurf *self, cairo_surface_t *image)
{
  cairo_surface_flush (image);
  /* cairo's ARGB32 is native-endian premultiplied: on a little-endian phone that is wl_shm's ARGB8888 */
  neo_surf_set_bgra (self, cairo_image_surface_get_data (image), cairo_image_surface_get_width (image),
                     cairo_image_surface_get_height (image), cairo_image_surface_get_stride (image));
}

void
neo_surf_set_solid (NeoSurf *self, double r, double g, double b)
{
  if (single_pixel) {
    use_buffer (self, wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer (single_pixel,
                  (uint32_t) (r * UINT32_MAX), (uint32_t) (g * UINT32_MAX), (uint32_t) (b * UINT32_MAX), UINT32_MAX),
                1, 1);
    return;
  }
  guint8 px[4] = { (guint8) (b * 255), (guint8) (g * 255), (guint8) (r * 255), 255 };
  neo_surf_set_bgra (self, px, 1, 1, 4);
}

/* ---- a canvas: drawn in place, never copied ---- */

static void
buffer_release (void *data, struct wl_buffer *buf)
{
  NeoSurf *self = data;
  if (buf == self->buffer)
    self->busy = FALSE;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

cairo_surface_t *
neo_surf_canvas (NeoSurf *self, int w, int h)
{
  if (w <= 0 || h <= 0)
    return NULL;
  if (self->canvas && self->bw == w && self->bh == h && !self->busy)
    return self->canvas;
  int stride = w * 4;
  size_t size = (size_t) stride * h;
  int fd = memfd_create ("neo-canvas", MFD_CLOEXEC);
  if (fd < 0 || ftruncate (fd, (off_t) size) < 0) {
    if (fd >= 0)
      close (fd);
    return NULL;
  }
  guint8 *map = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    close (fd);
    return NULL;
  }
  struct wl_shm_pool *pool = wl_shm_create_pool (shm, fd, (int32_t) size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer (pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);
  wl_buffer_add_listener (buf, &buffer_listener, self);
  /* the compositor still holds the old buffer: draw on in a new one, starting from what the old one shows */
  if (self->canvas && self->bw == w && self->bh == h)
    memcpy (map, self->map, size);
  canvas_drop (self);
  self->map = map;
  self->map_size = size;
  self->canvas = cairo_image_surface_create_for_data (map, CAIRO_FORMAT_ARGB32, w, h, stride);
  use_buffer (self, buf, w, h);
  return self->canvas;
}

void
neo_surf_canvas_changed (NeoSurf *self, int x, int y, int w, int h)
{
  if (!self->canvas)
    return;
  cairo_surface_flush (self->canvas);
  if (self->shown && self->buffer) {
    wl_surface_attach (self->surface, self->buffer, 0, 0);
    wl_surface_damage_buffer (self->surface, x, y, w, h);
    wl_surface_commit (self->surface);
    self->busy = TRUE;
  }
}

int neo_surf_width (NeoSurf *self) { return self->bw; }
int neo_surf_height (NeoSurf *self) { return self->bh; }

void
neo_surf_attach (NeoSurf *self, GtkWidget *parent_window)
{
  GdkWindow *gw = gtk_widget_get_window (parent_window);
  struct wl_surface *parent = gw ? gdk_wayland_window_get_wl_surface (gw) : NULL;
  if (!parent || self->sub)
    return;
  self->sub = wl_subcompositor_get_subsurface (subcompositor, self->surface, parent);
  self->placed = FALSE;
  self->shown = FALSE;
  /* nothing shows until placed */
  wl_surface_attach (self->surface, NULL, 0, 0);
  wl_surface_commit (self->surface);
}

void
neo_surf_detach (NeoSurf *self)
{
  g_clear_pointer (&self->sub, wl_subsurface_destroy);
  if (self->shown) {
    wl_surface_attach (self->surface, NULL, 0, 0);
    wl_surface_commit (self->surface);
  }
  self->shown = FALSE;
}

gboolean neo_surf_attached (NeoSurf *self) { return self->sub != NULL; }

void
neo_surf_place_above (NeoSurf *self, NeoSurf *below)
{
  if (!self->sub)
    return;
  if (below && below->surface)
    wl_subsurface_place_above (self->sub, below->surface);
}

void
neo_surf_crop (NeoSurf *self, double x, double y, double w, double h)
{
  if (w <= 0)
    wp_viewport_set_source (self->viewport, wl_fixed_from_int (-1), wl_fixed_from_int (-1),
                            wl_fixed_from_int (-1), wl_fixed_from_int (-1));
  else
    wp_viewport_set_source (self->viewport, wl_fixed_from_double (x), wl_fixed_from_double (y),
                            wl_fixed_from_double (w), wl_fixed_from_double (h));
  if (self->shown) {
    /* phoc repaints a surface only where it is damaged: a new crop alone left the old picture on screen (the
     * drawer's list did not move with the finger) */
    wl_surface_damage_buffer (self->surface, 0, 0, self->bw, self->bh);
    wl_surface_commit (self->surface);
  }
}

void
neo_surf_place (NeoSurf *self, double x, double y, double w, double h, double alpha)
{
  if (!self->sub || !self->buffer)
    return;
  int ix = (int) lround (x), iy = (int) lround (y);
  int iw = MAX (1, (int) lround (w)), ih = MAX (1, (int) lround (h));
  gboolean show = alpha > 0.004 && w >= 1 && h >= 1;
  gboolean commit = FALSE;

  if (show != self->shown) {
    self->shown = show;
    if (show) {
      wl_surface_attach (self->surface, self->buffer, 0, 0);
      self->busy = TRUE;
      wl_surface_damage_buffer (self->surface, 0, 0, self->bw, self->bh);
      self->placed = FALSE;
    } else {
      wl_surface_attach (self->surface, NULL, 0, 0);
    }
    commit = TRUE;
  }
  if (!show) {
    if (commit)
      wl_surface_commit (self->surface);
    return;
  }
  if (!self->placed || ix != self->x || iy != self->y)
    wl_subsurface_set_position (self->sub, ix, iy);
  if (!self->placed || iw != self->w || ih != self->h) {
    wp_viewport_set_destination (self->viewport, iw, ih);
    commit = TRUE;
  }
  alpha = CLAMP (alpha, 0, 1);
  if (self->alpha_mod && (!self->placed || fabs (alpha - self->alpha) > 0.002)) {
    wp_alpha_modifier_surface_v1_set_multiplier (self->alpha_mod, (uint32_t) (alpha * UINT32_MAX));
    commit = TRUE;
  }
  self->x = ix;
  self->y = iy;
  self->w = iw;
  self->h = ih;
  self->alpha = alpha;
  self->placed = TRUE;
  if (commit)
    wl_surface_commit (self->surface);
}

/* ---- the parent's commit, and a frame callback on it ---- */

typedef struct {
  struct wl_callback *cb;
  void (*done) (gpointer data);
  gpointer data;
} Frame;

static void
frame_done (void *data, struct wl_callback *cb, uint32_t time)
{
  Frame *f = data;
  wl_callback_destroy (cb);
  f->cb = NULL;
  if (f->done)
    f->done (f->data);
}

static const struct wl_callback_listener frame_listener = { frame_done };

void
neo_surf_commit_parent (GtkWidget *parent_window, void (*done) (gpointer data), gpointer data)
{
  GdkWindow *gw = gtk_widget_get_window (parent_window);
  struct wl_surface *parent = gw ? gdk_wayland_window_get_wl_surface (gw) : NULL;
  if (!parent)
    return;
  Frame *f = g_object_get_data (G_OBJECT (parent_window), "neo-surf-frame");
  if (!f) {
    f = g_new0 (Frame, 1);
    g_object_set_data_full (G_OBJECT (parent_window), "neo-surf-frame", f, g_free);
  }
  if (done) {
    if (!f->cb) {
      f->cb = wl_surface_frame (parent);
      wl_callback_add_listener (f->cb, &frame_listener, f);
    }
    f->done = done;
    f->data = data;
  }
  wl_surface_commit (parent);
}

void
neo_surf_cancel_frame (GtkWidget *parent_window)
{
  Frame *f = g_object_get_data (G_OBJECT (parent_window), "neo-surf-frame");
  if (f && f->cb) {
    wl_callback_destroy (f->cb);
    f->cb = NULL;
  }
}
