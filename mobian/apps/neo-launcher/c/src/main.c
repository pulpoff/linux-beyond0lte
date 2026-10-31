/* Neo Launcher for Phosh: the home screen (pages of app icons, page dots, a dock) and the app drawer, drawn as
 * layer-shell surfaces over the app windows. Phosh (phosh 0.58.0-1+neo1, launcher=neo in de.yesman.neo) calls
 * de.yesman.NeoLauncher.Home.Show() on the home gesture instead of unfolding its own app grid. Lock screen,
 * notifications, quick settings and the power dialog stay Phosh's. C and GTK 3, like Phosh. */
#include <gtk/gtk.h>
#include <handy.h>

#include "neo-edges.h"
#include "neo-home.h"
#include "neo-recents.h"
#include "neo-wayland.h"

#define APP_ID "de.yesman.NeoLauncher"

static const char home_xml[] =
  "<node><interface name='de.yesman.NeoLauncher.Home'>"
  "  <method name='Show'/>"
  "  <method name='Hide'/>"
  "  <property name='Visible' type='b' access='read'/>"
  "</interface></node>";

#define NEO_TYPE_APP (neo_app_get_type ())
G_DECLARE_FINAL_TYPE (NeoApp, neo_app, NEO, APP, GtkApplication)

struct _NeoApp {
  GtkApplication   parent;
  GSettings       *settings;
  NeoHome         *home;
  NeoRecents      *recents;
  NeoEdges        *edges;
  gboolean         pending_show;
  GDBusConnection *bus;
  char            *path;
  guint            reg_id;
};

G_DEFINE_TYPE (NeoApp, neo_app, GTK_TYPE_APPLICATION)

static void
emit_visible (gpointer data, gboolean visible)
{
  NeoApp *self = data;
  if (!self->bus || !self->path)
    return;
  GVariantBuilder changed;
  g_variant_builder_init (&changed, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&changed, "{sv}", "Visible", g_variant_new_boolean (visible));
  g_dbus_connection_emit_signal (self->bus, NULL, self->path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                 g_variant_new ("(sa{sv}as)", "de.yesman.NeoLauncher.Home", &changed, NULL), NULL);
}

static void
method_call (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
             GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
  NeoApp *self = data;
  if (!g_strcmp0 (method, "Show")) {
    /* Phosh calls as soon as the bus name appears, which may be before startup has built the windows */
    if (self->home)
      neo_home_show (self->home);
    else
      self->pending_show = TRUE;
  } else if (!g_strcmp0 (method, "Hide")) {
    if (self->home)
      neo_home_hide (self->home);
  }
  g_dbus_method_invocation_return_value (inv, NULL);
}

static GVariant *
get_property (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *prop,
              GError **err, gpointer data)
{
  NeoApp *self = data;
  return g_variant_new_boolean (self->home && neo_home_visible (self->home));
}

static const GDBusInterfaceVTable vtable = { method_call, get_property, NULL };

static gboolean
neo_app_dbus_register (GApplication *app, GDBusConnection *bus, const char *path, GError **err)
{
  NeoApp *self = NEO_APP (app);
  g_autoptr (GDBusNodeInfo) info = g_dbus_node_info_new_for_xml (home_xml, NULL);
  if (!G_APPLICATION_CLASS (neo_app_parent_class)->dbus_register (app, bus, path, err))
    return FALSE;
  self->bus = g_object_ref (bus);
  self->path = g_strdup (path);
  self->reg_id = g_dbus_connection_register_object (bus, path, info->interfaces[0], &vtable, self, NULL, err);
  return self->reg_id != 0;
}

static void
neo_app_dbus_unregister (GApplication *app, GDBusConnection *bus, const char *path)
{
  NeoApp *self = NEO_APP (app);
  if (self->reg_id)
    g_dbus_connection_unregister_object (bus, self->reg_id);
  self->reg_id = 0;
  G_APPLICATION_CLASS (neo_app_parent_class)->dbus_unregister (app, bus, path);
}

static void
neo_app_startup (GApplication *app)
{
  NeoApp *self = NEO_APP (app);
  G_APPLICATION_CLASS (neo_app_parent_class)->startup (app);
  g_application_hold (app);                 /* resident: the windows come and go, the service stays */
  hdy_init ();
  neo_wayland_init ();
  self->settings = g_settings_new ("de.yesman.neo");
  self->home = neo_home_new (GTK_APPLICATION (app), self->settings);
  neo_home_set_visible_cb (self->home, emit_visible, self);
  self->recents = neo_recents_new (GTK_APPLICATION (app), self->home);
  self->edges = neo_edges_new (GTK_APPLICATION (app), self->home, self->recents);
  if (self->pending_show) {
    self->pending_show = FALSE;
    neo_home_show (self->home);
  }
}

static void
neo_app_activate (GApplication *app)
{
  /* started by the session: stay hidden until Phosh asks */
}

static void
neo_app_class_init (NeoAppClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);
  app_class->dbus_register = neo_app_dbus_register;
  app_class->dbus_unregister = neo_app_dbus_unregister;
  app_class->startup = neo_app_startup;
  app_class->activate = neo_app_activate;
}

static void
neo_app_init (NeoApp *self)
{
}

int
main (int argc, char **argv)
{
  g_autoptr (NeoApp) app = g_object_new (NEO_TYPE_APP, "application-id", APP_ID, "flags", G_APPLICATION_DEFAULT_FLAGS, NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
