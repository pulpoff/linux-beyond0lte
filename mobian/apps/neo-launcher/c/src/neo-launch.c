/* Launching. Neo stays on screen until the app's window is (Phosh's WindowShown), so the app opened before never
 * flashes up in between; it lets go of the keyboard meanwhile, or the new window could not take the focus. */
#include "neo-launch.h"
#include "neo-home.h"

#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>

void neo_home_release_keyboard (NeoHome *self);

struct _NeoLaunch {
  NeoHome *home;
  char    *app_id;          /* the launch in progress (lower case), or NULL */
  guint    timeout;
  char    *call_win;        /* the Calls window last raised */
};

void
neo_phosh_call (const char *method, GVariant *params)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, PHOSH_NEO_BUS, PHOSH_NEO_PATH, PHOSH_NEO_BUS, method, params, NULL,
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
}

void
neo_launch_finish (NeoLaunch *self)
{
  if (!self->app_id)
    return;
  g_clear_pointer (&self->app_id, g_free);
  g_clear_handle_id (&self->timeout, g_source_remove);
  neo_home_launch_end (self->home);
}

static gboolean
launch_timeout (gpointer data)
{
  NeoLaunch *self = data;
  self->timeout = 0;
  neo_launch_finish (self);
  return G_SOURCE_REMOVE;
}

static void
begin (NeoLaunch *self, const char *app_id)
{
  g_clear_handle_id (&self->timeout, g_source_remove);
  g_free (self->app_id);
  self->app_id = g_ascii_strdown (app_id, -1);
  neo_home_release_keyboard (self->home);
  /* an Android app's first window can take long: Waydroid's session may still be starting */
  self->timeout = g_timeout_add (g_str_has_prefix (self->app_id, "waydroid.") ? 30000 : 6000, launch_timeout, self);
}

static void
on_window_shown (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *signal,
                 GVariant *params, gpointer data)
{
  NeoLaunch *self = data;
  const char *app_id;
  if (!self->app_id)
    return;
  g_variant_get (params, "(&s)", &app_id);
  /* giving up the keyboard re-activates the app shown before, and Phosh reports that one too: wait for ours */
  g_autofree char *a = g_ascii_strdown (app_id ?: "", -1);
  if (!*a || !(g_str_equal (a, self->app_id) || strstr (a, self->app_id) || strstr (self->app_id, a)))
    return;
  neo_launch_finish (self);
}

static char *
app_id_of (GAppInfo *info)
{
  char *id = g_strdup (g_app_info_get_id (info));
  if (g_str_has_suffix (id, ".desktop"))
    id[strlen (id) - strlen (".desktop")] = '\0';
  return id;
}

/* Starts the app in a scope of its own: launched directly it lived in neo-launcher.service, and restarting Neo
 * killed every app opened from it. `gio launch` handles Exec lines and D-Bus activation alike. */
static void
spawn_app (GAppInfo *info)
{
  const char *file = G_IS_DESKTOP_APP_INFO (info) ? g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (info)) : NULL;
  /* D-Bus activated apps (Console, Camera, …) are started by the bus and only show a window on the Activate call
   * that follows: `gio launch` exited before that call went out, so Neo makes the call itself */
  gboolean dbus = G_IS_DESKTOP_APP_INFO (info) && g_desktop_app_info_get_boolean (G_DESKTOP_APP_INFO (info), "DBusActivatable");
  g_autofree char *systemd_run = g_find_program_in_path ("systemd-run");
  g_autoptr (GError) err = NULL;
  if (!file || dbus || !systemd_run) {
    g_autoptr (GdkAppLaunchContext) ctx = gdk_display_get_app_launch_context (gdk_display_get_default ());
    if (!g_app_info_launch (info, NULL, G_APP_LAUNCH_CONTEXT (ctx), &err))
      g_warning ("launching %s: %s", g_app_info_get_id (info), err->message);
    return;
  }
  g_autofree char *raw = app_id_of (info);
  g_autofree char *id = g_strdup (raw);
  for (char *c = id; *c; c++)
    if (!g_ascii_isalnum (*c) && *c != '_' && *c != '.')
      *c = '_';
  g_autofree char *unit = g_strdup_printf ("--unit=app-neo-%s-%" G_GINT64_FORMAT, id, g_get_monotonic_time ());
  g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  /* systemd hands neo-launcher.service its own memory.pressure to watch; an app inheriting that watched Neo's
   * cgroup, and GLib's PSI monitor spun at 90 % CPU in Compass. Apps get GLib's polling monitor instead. */
  g_subprocess_launcher_unsetenv (l, "MEMORY_PRESSURE_WATCH");
  g_subprocess_launcher_unsetenv (l, "MEMORY_PRESSURE_WRITE");
  g_subprocess_launcher_setenv (l, "GIO_USE_MEMORY_MONITOR", "poll", TRUE);
  /* an Android app (waydroid.<package>.desktop): full screen in a window of its own */
  g_autofree char *waydroid_open = g_find_program_in_path ("waydroid-open");
  g_autoptr (GSubprocess) p = NULL;
  if (g_str_has_prefix (raw, "waydroid.") && waydroid_open)
    p = g_subprocess_launcher_spawn (l, &err, "systemd-run", "--user", "--scope", "--collect", "--quiet",
                                     "--slice=app.slice", unit, "waydroid-open", raw + strlen ("waydroid."), NULL);
  else
    p = g_subprocess_launcher_spawn (l, &err, "systemd-run", "--user", "--scope", "--collect", "--quiet",
                                     "--slice=app.slice", unit, "gio", "launch", file, NULL);
  if (!p)
    g_warning ("launching %s: %s", raw, err->message);
}

typedef struct {
  NeoLaunch *self;
  GAppInfo  *info;
} LaunchCtx;

/* an app that already runs only presents its window on a second launch, and Phosh raises no window on such a
 * request: switch to its window instead (the newest one when it has several) */
static void
got_windows (GObject *src, GAsyncResult *res, gpointer data)
{
  LaunchCtx *ctx = data;
  g_autoptr (GVariant) out = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autofree char *app_id = app_id_of (ctx->info);
  g_autofree char *running = NULL;
  if (out) {
    g_autoptr (GVariantIter) it = NULL;
    const char *id, *wapp;
    g_variant_get (out, "(a(ssssuuub))", &it);
    while (g_variant_iter_next (it, "(&s&s&s&suuub)", &id, &wapp, NULL, NULL, NULL, NULL, NULL, NULL))
      if (!g_ascii_strcasecmp (wapp, app_id)) {
        g_free (running);
        running = g_strdup (id);
      }
  }
  g_message ("launch %s: running %s", app_id, running ?: "no");
  begin (ctx->self, app_id);
  if (running)
    neo_phosh_call ("Activate", g_variant_new ("(s)", running));
  else
    spawn_app (ctx->info);
  g_object_unref (ctx->info);
  g_free (ctx);
}

void
neo_launch_app (NeoLaunch *self, GAppInfo *info, GtkWidget *from)
{
  neo_home_launch_card (self->home, info, from);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  LaunchCtx *ctx = g_new0 (LaunchCtx, 1);
  ctx->self = self;
  ctx->info = g_object_ref (info);
  g_dbus_connection_call (bus, PHOSH_NEO_BUS, PHOSH_NEO_PATH, PHOSH_NEO_BUS, "GetWindows", NULL,
                          G_VARIANT_TYPE ("(a(ssssuuub))"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, got_windows, ctx);
}

void
neo_launch_run (NeoLaunch *self, const char * const *argv)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GSubprocess) p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_NONE, &err);
  if (!p) {
    g_warning ("%s: %s", argv[0], err->message);
    return;
  }
  neo_home_hide (self->home);
}

/* gnome-calls presents its window on an incoming call, but phoc does not focus a window that asks without an
 * activation token: it mapped under Neo and the call rang with no way to answer it. Bring the newest Calls window
 * up, a few times over two seconds: the call window maps a moment after the call appears on the bus. */
typedef struct {
  NeoLaunch *self;
  int        tries;
} CallCtx;

static void raise_call (NeoLaunch *self, int tries);

static gboolean
raise_call_again (gpointer data)
{
  CallCtx *c = data;
  raise_call (c->self, c->tries);
  g_free (c);
  return G_SOURCE_REMOVE;
}

static void
got_call_windows (GObject *src, GAsyncResult *res, gpointer data)
{
  CallCtx *c = data;
  g_autoptr (GVariant) out = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (c->tries < 6) {
    CallCtx *next = g_new0 (CallCtx, 1);
    next->self = c->self;
    next->tries = c->tries + 1;
    g_timeout_add (350, raise_call_again, next);
  }
  g_autofree char *win = NULL;
  if (out) {
    g_autoptr (GVariantIter) it = NULL;
    const char *id, *wapp;
    g_variant_get (out, "(a(ssssuuub))", &it);
    while (g_variant_iter_next (it, "(&s&s&s&suuub)", &id, &wapp, NULL, NULL, NULL, NULL, NULL, NULL))
      if (!g_ascii_strcasecmp (wapp, "org.gnome.Calls")) {
        g_free (win);
        win = g_strdup (id);
      }
  }
  if (win) {
    if (c->tries == 0 || g_strcmp0 (win, c->self->call_win))
      g_message ("incoming call: raising %s", win);
    g_free (c->self->call_win);
    c->self->call_win = g_strdup (win);
    neo_launch_finish (c->self);
    neo_home_hide (c->self->home);
    neo_phosh_call ("Activate", g_variant_new ("(s)", win));
  }
  g_free (c);
}

static void
raise_call (NeoLaunch *self, int tries)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  CallCtx *c = g_new0 (CallCtx, 1);
  c->self = self;
  c->tries = tries;
  g_dbus_connection_call (bus, PHOSH_NEO_BUS, PHOSH_NEO_PATH, PHOSH_NEO_BUS, "GetWindows", NULL,
                          G_VARIANT_TYPE ("(a(ssssuuub))"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL,
                          got_call_windows, c);
}

static void
on_call_added (GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *signal,
               GVariant *params, gpointer data)
{
  g_autoptr (GVariant) ifaces = g_variant_get_child_value (params, 1);
  g_autoptr (GVariant) call = g_variant_lookup_value (ifaces, "org.gnome.Calls.Call", G_VARIANT_TYPE_VARDICT);
  gboolean inbound = FALSE;
  if (call && g_variant_lookup (call, "Inbound", "b", &inbound) && inbound)
    raise_call (data, 0);
}

NeoLaunch *
neo_launch_new (NeoHome *home)
{
  NeoLaunch *self = g_new0 (NeoLaunch, 1);
  self->home = home;
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  g_dbus_connection_signal_subscribe (bus, PHOSH_NEO_BUS, PHOSH_NEO_BUS, "WindowShown", PHOSH_NEO_PATH, NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE, on_window_shown, self, NULL);
  g_dbus_connection_signal_subscribe (bus, "org.gnome.Calls", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                                      "/org/gnome/Calls", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_call_added, self, NULL);
  return self;
}
