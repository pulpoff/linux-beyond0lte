/* Bluetooth and Sharing: BlueZ over D-Bus (its ObjectManager, made in the background at start), and hostnamed
 * for the phone's name. While the Bluetooth page is open the phone looks for devices, is visible to them, and
 * answers pairing requests: it is BlueZ's agent then, and asks in a dialog when a code has to be compared or
 * typed. */
#include "ns-bt.h"
#include "ns-pages.h"
#include "ns-ui.h"

#define AGENT_PATH "/de/yesman/NeoSettings/agent"

static GDBusObjectManager *bluez;
static GPtrArray *waiting;

static void
bluez_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  bluez = g_dbus_object_manager_client_new_for_bus_finish (res, &err);
  if (!bluez)
    g_warning ("BlueZ: %s", err->message);
  for (guint i = 0; waiting && i + 1 < waiting->len; i += 2) {
    if (bluez)
      ((NsFill) waiting->pdata[i + 1]) (waiting->pdata[i]);
    g_object_unref (waiting->pdata[i]);
  }
  g_clear_pointer (&waiting, g_ptr_array_unref);
}

void
ns_bt_init (void)
{
  g_dbus_object_manager_client_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez",
                                            "/", NULL, NULL, NULL, NULL, bluez_ready, NULL);
}

static void
with_bluez (AdwNavigationPage *page, NsFill fill)
{
  if (bluez) {
    fill (page);
    return;
  }
  if (!waiting)
    waiting = g_ptr_array_new ();
  g_ptr_array_add (waiting, g_object_ref (page));
  g_ptr_array_add (waiting, (gpointer) fill);
}

static GDBusProxy *
adapter (void)
{
  g_autolist (GDBusObject) objs = bluez ? g_dbus_object_manager_get_objects (bluez) : NULL;
  for (GList *l = objs; l; l = l->next) {
    GDBusInterface *i = g_dbus_object_get_interface (l->data, "org.bluez.Adapter1");
    if (i)
      return G_DBUS_PROXY (i);
  }
  return NULL;
}

static gboolean
prop_bool (GDBusProxy *p, const char *name)
{
  g_autoptr (GVariant) v = p ? g_dbus_proxy_get_cached_property (p, name) : NULL;
  return v && g_variant_get_boolean (v);
}

static char *
prop_string (GDBusProxy *p, const char *name)
{
  g_autoptr (GVariant) v = p ? g_dbus_proxy_get_cached_property (p, name) : NULL;
  return v ? g_variant_dup_string (v, NULL) : NULL;
}

static void
prop_set (GDBusProxy *p, const char *iface, const char *name, GVariant *value)
{
  g_dbus_connection_call (g_dbus_proxy_get_connection (p), "org.bluez", g_dbus_proxy_get_object_path (p),
                          "org.freedesktop.DBus.Properties", "Set", g_variant_new ("(ssv)", iface, name, value),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
call_done (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_proxy_call_finish (G_DBUS_PROXY (src), res, &err);
  if (!v && !g_error_matches (err, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY)) {
    g_dbus_error_strip_remote_error (err);
    g_autofree char *t = g_strdup_printf ("Failed: %s", err->message);
    ns_row_set_subtitle (row, t);
  }
  g_object_unref (row);
}

/* ---- the pairing agent ---- */

static const char agent_xml[] =
  "<node><interface name='org.bluez.Agent1'>"
  "<method name='Release'/>"
  "<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
  "<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
  "<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
  "<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/>"
  "<arg type='q' direction='in'/></method>"
  "<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
  "<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
  "<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
  "<method name='Cancel'/>"
  "</interface></node>";

static guint agent_id;
static GtkWidget *agent_parent;      /* the Bluetooth page, for the dialogs */
static AdwDialog *agent_dialog;
static GDBusMethodInvocation *pending;

static char *
device_name (const char *path)
{
  g_autoptr (GDBusInterface) i = bluez ? g_dbus_object_manager_get_interface (bluez, path, "org.bluez.Device1") : NULL;
  char *n = i ? prop_string (G_DBUS_PROXY (i), "Alias") : NULL;
  return n ? n : g_strdup ("the device");
}

static void
agent_reply (GVariant *v)
{
  if (!pending)
    return;
  if (v)
    g_dbus_method_invocation_return_value (pending, v);
  else
    g_dbus_method_invocation_return_dbus_error (pending, "org.bluez.Error.Rejected", "Rejected by the user");
  pending = NULL;
  agent_dialog = NULL;
}

static void
confirmed (gboolean yes, gpointer data)
{
  agent_reply (yes ? g_variant_new ("()") : NULL);
}

static void
pin_entered (const char *text, gpointer passkey)
{
  if (!text)
    agent_reply (NULL);
  else if (passkey)
    agent_reply (g_variant_new ("(u)", (guint32) g_ascii_strtoull (text, NULL, 10)));
  else
    agent_reply (g_variant_new ("(s)", text));
}

static void
agent_call (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *method,
            GVariant *args, GDBusMethodInvocation *inv, gpointer data)
{
  const char *dev = NULL;
  if (g_variant_n_children (args))
    g_variant_get_child (args, 0, "&o", &dev);
  g_autofree char *name = dev ? device_name (dev) : NULL;

  if (!g_strcmp0 (method, "Cancel") || !g_strcmp0 (method, "Release")) {
    if (agent_dialog)
      adw_dialog_force_close (agent_dialog);
    agent_reply (NULL);
    g_dbus_method_invocation_return_value (inv, NULL);
    return;
  }
  if (!g_strcmp0 (method, "AuthorizeService")) {
    /* a paired device's own services: it was trusted at pairing */
    g_dbus_method_invocation_return_value (inv, NULL);
    return;
  }
  if (pending || !agent_parent) {
    g_dbus_method_invocation_return_dbus_error (inv, "org.bluez.Error.Rejected", "Busy");
    return;
  }
  pending = inv;
  g_autofree char *head = g_strdup_printf ("Pair with %s?", name);
  if (!g_strcmp0 (method, "RequestConfirmation")) {
    guint32 key;
    g_variant_get (args, "(&ou)", &dev, &key);
    g_autofree char *body = g_strdup_printf ("Make sure %s shows this code:\n\n%06u", name, key);
    agent_dialog = ns_confirm (agent_parent, head, body, "Pair", FALSE, confirmed, NULL);
  } else if (!g_strcmp0 (method, "RequestAuthorization")) {
    agent_dialog = ns_confirm (agent_parent, head, NULL, "Pair", FALSE, confirmed, NULL);
  } else if (!g_strcmp0 (method, "RequestPinCode") || !g_strcmp0 (method, "RequestPasskey")) {
    gboolean passkey = !g_strcmp0 (method, "RequestPasskey");
    ns_ask (agent_parent, head, "Type the PIN shown on the device (often 0000 or 1234).", NULL, FALSE, "Pair",
            pin_entered, GINT_TO_POINTER (passkey));
  } else if (!g_strcmp0 (method, "DisplayPinCode") || !g_strcmp0 (method, "DisplayPasskey")) {
    g_autofree char *code = NULL;
    if (!g_strcmp0 (method, "DisplayPinCode")) {
      const char *pin;
      g_variant_get (args, "(&o&s)", &dev, &pin);
      code = g_strdup (pin);
    } else {
      guint32 key;
      guint16 typed;
      g_variant_get (args, "(&ouq)", &dev, &key, &typed);
      code = g_strdup_printf ("%06u", key);
    }
    g_autofree char *body = g_strdup_printf ("Type this code on %s:\n\n%s", name, code);
    /* nothing to answer: the device sends the result */
    agent_reply (g_variant_new ("()"));
    agent_dialog = ns_confirm (agent_parent, head, body, "OK", FALSE, NULL, NULL);
    agent_dialog = NULL;
  } else {
    agent_reply (NULL);
  }
}

static void
agent_on (GtkWidget *page)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (!bus || agent_id)
    return;
  static GDBusNodeInfo *info;
  if (!info)
    info = g_dbus_node_info_new_for_xml (agent_xml, NULL);
  static const GDBusInterfaceVTable vt = { agent_call, NULL, NULL };
  agent_id = g_dbus_connection_register_object (bus, AGENT_PATH, info->interfaces[0], &vt, NULL, NULL, NULL);
  agent_parent = page;
  g_dbus_connection_call (bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
                          g_variant_new ("(os)", AGENT_PATH, "KeyboardDisplay"), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, NULL, NULL);
  g_dbus_connection_call (bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent",
                          g_variant_new ("(o)", AGENT_PATH), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
agent_off (void)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  agent_reply (NULL);
  agent_parent = NULL;
  if (!bus || !agent_id)
    return;
  g_dbus_connection_call (bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent",
                          g_variant_new ("(o)", AGENT_PATH), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  g_dbus_connection_unregister_object (bus, agent_id);
  agent_id = 0;
}

/* ---- Bluetooth ---- */

typedef struct {
  GtkWidget  *power, *paired, *avail, *visible, *off;
  GDBusProxy *adapter;
  gboolean    scanning, was_discoverable;
} Bt;

static const char *
device_icon (GDBusProxy *dev)
{
  g_autofree char *i = prop_string (dev, "Icon");
  if (!i)
    return "bluetooth-symbolic";
  return g_str_has_prefix (i, "audio-head") ? "audio-headphones-symbolic"
       : g_str_has_prefix (i, "audio") ? "audio-speakers-symbolic"
       : g_str_has_prefix (i, "input-keyboard") ? "input-keyboard-symbolic"
       : g_str_has_prefix (i, "input-mouse") ? "input-mouse-symbolic"
       : g_str_has_prefix (i, "input-gaming") ? "input-gaming-symbolic"
       : g_str_has_prefix (i, "phone") ? "phone-symbolic"
       : g_str_has_prefix (i, "computer") ? "computer-symbolic"
       : g_str_has_prefix (i, "camera") ? "camera-photo-symbolic"
       : "bluetooth-symbolic";
}

static void
connect_tapped (GtkWidget *row, gpointer data)
{
  GDBusProxy *dev = g_object_get_data (G_OBJECT (row), "ns-dev");
  gboolean on = prop_bool (dev, "Connected");
  ns_row_set_subtitle (row, on ? "Disconnecting…" : "Connecting…");
  g_dbus_proxy_call (dev, on ? "Disconnect" : "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, call_done,
                     g_object_ref (row));
}

static void
paired (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_proxy_call_finish (G_DBUS_PROXY (src), res, &err);
  if (v) {
    /* trusted: it may connect back by itself, as phones do */
    prop_set (G_DBUS_PROXY (src), "org.bluez.Device1", "Trusted", g_variant_new_boolean (TRUE));
    g_dbus_proxy_call (G_DBUS_PROXY (src), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, NULL, NULL);
  } else {
    g_dbus_error_strip_remote_error (err);
    g_autofree char *t = g_strdup_printf ("Not paired: %s", err->message);
    ns_row_set_subtitle (row, t);
  }
  g_object_unref (row);
}

static void
pair_tapped (GtkWidget *row, gpointer data)
{
  GDBusProxy *dev = g_object_get_data (G_OBJECT (row), "ns-dev");
  ns_row_set_subtitle (row, "Pairing…");
  g_dbus_proxy_call (dev, "Pair", NULL, G_DBUS_CALL_FLAGS_NONE, 60000, NULL, paired, g_object_ref (row));
}

/* ---- a paired device ---- */

static void
alias_entered (const char *text, gpointer dev)
{
  if (text && *text)
    prop_set (dev, "org.bluez.Device1", "Alias", g_variant_new_string (text));
  g_object_unref (dev);
}

static void
rename_tapped (GtkWidget *row, gpointer dev)
{
  g_autofree char *a = prop_string (dev, "Alias");
  ns_ask (row, "Rename", NULL, a, FALSE, "Rename", alias_entered, g_object_ref (dev));
}

static void
forget_answered (gboolean yes, gpointer row)
{
  GDBusProxy *dev = g_object_get_data (G_OBJECT (row), "ns-dev");
  GDBusProxy *ad = adapter ();
  if (yes && ad) {
    g_dbus_proxy_call (ad, "RemoveDevice", g_variant_new ("(o)", g_dbus_proxy_get_object_path (dev)),
                       G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
  }
  g_clear_object (&ad);
}

static void
forget_tapped (GtkWidget *row, gpointer data)
{
  ns_confirm (row, "Unpair this device?", "It has to be paired again to connect.", "Unpair", TRUE, forget_answered,
              row);
}

static void
device_fill (AdwNavigationPage *page)
{
  GDBusProxy *dev = g_object_get_data (G_OBJECT (page), "ns-dev");
  GtkWidget *conn = g_object_get_data (G_OBJECT (page), "ns-conn");
  gboolean on = prop_bool (dev, "Connected");
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (conn), "ns-title-label")),
                      on ? "Disconnect" : "Connect");
  ns_row_set_subtitle (conn, on ? "Connected" : NULL);
}

static void
device_page (GtkWidget *b, GDBusProxy *dev)
{
  GtkWidget *c;
  g_autofree char *alias = prop_string (dev, "Alias");
  AdwNavigationPage *page = ns_page_new (alias ? alias : "Device", &c);
  g_object_set_data_full (G_OBJECT (page), "ns-dev", g_object_ref (dev), g_object_unref);
  ns_later_init (page, device_fill);
  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *conn = ns_row_button (g, "Connect", FALSE, connect_tapped, NULL);
  g_object_set_data_full (G_OBJECT (conn), "ns-dev", g_object_ref (dev), g_object_unref);
  g_object_set_data (G_OBJECT (page), "ns-conn", conn);
  GtkWidget *r = ns_row (g, NULL, NULL, "Rename", alias);
  ns_row_on_activate (r, rename_tapped, dev);
  g = ns_group (c, NULL);
  g_autofree char *addr = prop_string (dev, "Address");
  ns_row_info (g, "Bluetooth address", addr);
  g = ns_group (c, NULL);
  r = ns_row_button (g, "Unpair", TRUE, forget_tapped, NULL);
  g_object_set_data_full (G_OBJECT (r), "ns-dev", g_object_ref (dev), g_object_unref);
  g_signal_connect_object (dev, "g-properties-changed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  device_fill (page);
  ns_push (page);
}

static gint
by_name (gconstpointer a, gconstpointer b)
{
  g_autofree char *x = prop_string (*(GDBusProxy **) a, "Alias");
  g_autofree char *y = prop_string (*(GDBusProxy **) b, "Alias");
  return g_strcmp0 (x, y);
}

static void
bt_fill (AdwNavigationPage *page)
{
  Bt *bt = g_object_get_data (G_OBJECT (page), "bt");
  if (!bt->adapter) {
    bt->adapter = adapter ();
    if (!bt->adapter) {
      gtk_widget_set_sensitive (bt->power, FALSE);
      ns_row_set_subtitle (bt->power, "No Bluetooth adapter");
      return;
    }
    g_signal_connect_object (bluez, "object-added", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (bluez, "object-removed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (bluez, "interface-proxy-properties-changed", G_CALLBACK (ns_later), page,
                             G_CONNECT_SWAPPED);
    bt->was_discoverable = prop_bool (bt->adapter, "Discoverable");
  }
  gboolean on = prop_bool (bt->adapter, "Powered");
  ns_switch_quiet (ns_row_get_switch (bt->power), on);
  ns_row_set_subtitle (bt->power, on ? "On" : "Off");
  if (on && !bt->scanning) {
    bt->scanning = TRUE;
    prop_set (bt->adapter, "org.bluez.Adapter1", "Discoverable", g_variant_new_boolean (TRUE));
    g_dbus_proxy_call (bt->adapter, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  } else if (!on) {
    bt->scanning = FALSE;
  }
  g_autofree char *alias = prop_string (bt->adapter, "Alias");
  g_autofree char *vis = g_strdup_printf ("Visible as “%s” to nearby devices while this page is open.",
                                          alias ? alias : "this phone");
  gtk_label_set_text (GTK_LABEL (bt->visible), vis);
  gtk_widget_set_visible (bt->visible, on);
  gtk_widget_set_visible (bt->off, !on);

  ns_group_clear (bt->paired);
  ns_group_clear (bt->avail);
  g_autoptr (GPtrArray) devs = g_ptr_array_new_with_free_func (g_object_unref);
  g_autolist (GDBusObject) objs = g_dbus_object_manager_get_objects (bluez);
  for (GList *l = objs; on && l; l = l->next) {
    GDBusInterface *i = g_dbus_object_get_interface (l->data, "org.bluez.Device1");
    if (i)
      g_ptr_array_add (devs, i);
  }
  g_ptr_array_sort (devs, by_name);
  guint np = 0, na = 0;
  for (guint i = 0; i < devs->len; i++) {
    GDBusProxy *dev = devs->pdata[i];
    g_autofree char *alias_ = prop_string (dev, "Alias");
    g_autoptr (GVariant) name = g_dbus_proxy_get_cached_property (dev, "Name");
    gboolean is_paired = prop_bool (dev, "Paired");
    if (!is_paired && !name)
      continue;        /* nameless ones are just addresses */
    gboolean conn = prop_bool (dev, "Connected");
    GtkWidget *r = ns_row (is_paired ? bt->paired : bt->avail, device_icon (dev), conn ? "blue" : "grey", alias_,
                           is_paired ? (conn ? "Connected" : NULL) : NULL);
    g_object_set_data_full (G_OBJECT (r), "ns-dev", g_object_ref (dev), g_object_unref);
    if (is_paired) {
      GtkWidget *gear = gtk_button_new_from_icon_name ("emblem-system-symbolic");
      gtk_widget_add_css_class (gear, "flat");
      gtk_widget_add_css_class (gear, "circular");
      g_signal_connect_data (gear, "clicked", G_CALLBACK (device_page), g_object_ref (dev),
                             (GClosureNotify) (void (*) (void)) g_object_unref, 0);
      ns_row_suffix (r, gear);
      ns_row_on_activate (r, connect_tapped, NULL);
      np++;
    } else {
      ns_row_on_activate (r, pair_tapped, NULL);
      na++;
    }
  }
  ns_group_set_visible (bt->paired, np > 0);
  ns_group_set_visible (bt->avail, na > 0);
}

static void
bt_power (GtkSwitch *sw, GParamSpec *ps, AdwNavigationPage *page)
{
  Bt *bt = g_object_get_data (G_OBJECT (page), "bt");
  if (!ns_switch_syncing (GTK_WIDGET (sw)) && bt->adapter)
    prop_set (bt->adapter, "org.bluez.Adapter1", "Powered", g_variant_new_boolean (gtk_switch_get_active (sw)));
}

static void
bt_free (Bt *bt)
{
  agent_off ();
  if (bt->adapter) {
    if (bt->scanning)
      g_dbus_proxy_call (bt->adapter, "StopDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    if (!bt->was_discoverable)
      prop_set (bt->adapter, "org.bluez.Adapter1", "Discoverable", g_variant_new_boolean (FALSE));
    g_object_unref (bt->adapter);
  }
  g_free (bt);
}

void
ns_page_bluetooth (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Bluetooth", &c);
  Bt *bt = g_new0 (Bt, 1);
  g_object_set_data_full (G_OBJECT (page), "bt", bt, (GDestroyNotify) bt_free);
  ns_later_init (page, bt_fill);

  GtkWidget *g = ns_group (c, NULL);
  bt->power = ns_row_toggle (g, NULL, NULL, "Bluetooth", NULL);
  g_signal_connect (ns_row_get_switch (bt->power), "notify::active", G_CALLBACK (bt_power), page);
  bt->visible = ns_note (c, "");
  gtk_widget_set_visible (bt->visible, FALSE);
  bt->off = ns_note (c, "Turn on Bluetooth to connect headphones, speakers, watches and more.");
  bt->paired = ns_group (c, "Paired devices");
  ns_group_set_visible (bt->paired, FALSE);
  bt->avail = ns_group (c, "Available devices");
  ns_group_set_visible (bt->avail, FALSE);
  agent_on (GTK_WIDGET (page));
  with_bluez (page, bt_fill);
  ns_push (page);
}

/* ---- Sharing: the phone's name ---- */

static void
pretty_set (const char *text, gpointer row)
{
  if (text && *text) {
    g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
    /* polkit may ask for the password: Phosh shows its prompt */
    if (bus)
      g_dbus_connection_call (bus, "org.freedesktop.hostname1", "/org/freedesktop/hostname1",
                              "org.freedesktop.hostname1", "SetPrettyHostname", g_variant_new ("(sb)", text, TRUE),
                              NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 60000, NULL, NULL, NULL);
    ns_row_set_subtitle (row, text);
  }
  g_object_unref (row);
}

static void
name_tapped (GtkWidget *row, gpointer data)
{
  const char *cur = g_object_get_data (G_OBJECT (row), "ns-name");
  ns_ask (row, "Phone name", "Shown to Bluetooth devices and on the network.", cur, FALSE, "Rename", pretty_set,
          g_object_ref (row));
}

static void
name_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) hn = NULL;
  if (v)
    g_variant_get (v, "(v)", &hn);
  const char *pretty = hn ? g_variant_get_string (hn, NULL) : "";
  const char *name = *pretty ? pretty : g_get_host_name ();
  ns_row_set_subtitle (row, name);
  g_object_set_data_full (G_OBJECT (row), "ns-name", g_strdup (name), g_free);
  g_object_unref (row);
}

GtkWidget *
ns_bt_name_row (GtkWidget *g)
{
  GtkWidget *r = ns_row (g, NULL, NULL, "Phone name", g_get_host_name ());
  ns_row_on_activate (r, name_tapped, NULL);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.hostname1", "/org/freedesktop/hostname1",
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", "org.freedesktop.hostname1", "PrettyHostname"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, name_got, g_object_ref (r));
  return r;
}

static void
visible_fill (AdwNavigationPage *page)
{
  GtkWidget *r = g_object_get_data (G_OBJECT (page), "ns-visible");
  g_autoptr (GDBusProxy) ad = adapter ();
  gtk_widget_set_visible (r, ad != NULL);
  if (!ad)
    return;
  ns_switch_quiet (ns_row_get_switch (r), prop_bool (ad, "Discoverable"));
  if (!g_object_get_data (G_OBJECT (page), "ns-hooked")) {
    g_object_set_data (G_OBJECT (page), "ns-hooked", GINT_TO_POINTER (1));
    g_signal_connect_object (bluez, "interface-proxy-properties-changed", G_CALLBACK (ns_later), page,
                             G_CONNECT_SWAPPED);
  }
}

static void
visible_set (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  g_autoptr (GDBusProxy) ad = adapter ();
  if (ad && !ns_switch_syncing (GTK_WIDGET (sw))) {
    /* 0: until switched off again */
    prop_set (ad, "org.bluez.Adapter1", "DiscoverableTimeout", g_variant_new_uint32 (0));
    prop_set (ad, "org.bluez.Adapter1", "Discoverable", g_variant_new_boolean (gtk_switch_get_active (sw)));
  }
}

void
ns_page_sharing (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Sharing", &c);
  ns_later_init (page, visible_fill);
  GtkWidget *g = ns_group (c, NULL);
  ns_bt_name_row (g);
  GtkWidget *r = ns_row_switch (g, "Visible to Bluetooth devices", "Others can find the phone and pair with it",
                                NULL, NULL);
  g_signal_connect (ns_row_get_switch (r), "notify::active", G_CALLBACK (visible_set), NULL);
  gtk_widget_set_visible (r, FALSE);
  g_object_set_data (G_OBJECT (page), "ns-visible", r);
  with_bluez (page, visible_fill);
  ns_push (page);
}
