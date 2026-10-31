/* Wi-Fi, Mobile network, Network and VPN: NetworkManager through libnm. The client is made once, at start, in the
 * background (ns_net_init); a page opened before it is ready fills itself when it is. The lists are rebuilt a
 * moment after NetworkManager says something changed, so a burst of signals is one rebuild. */
#include "ns-net.h"
#include "ns-pages.h"
#include "ns-ui.h"

#include <NetworkManager.h>

static NMClient *client;
static GPtrArray *waiting;       /* pages to fill once the client is there, and how */


static void
client_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  client = nm_client_new_finish (res, &err);
  if (!client)
    g_warning ("NetworkManager: %s", err->message);
  for (guint i = 0; waiting && i + 1 < waiting->len; i += 2) {
    AdwNavigationPage *page = waiting->pdata[i];
    NsFill fill = (NsFill) waiting->pdata[i + 1];
    if (client)
      fill (page);
    g_object_unref (page);
  }
  g_clear_pointer (&waiting, g_ptr_array_unref);
}

void
ns_net_init (void)
{
  nm_client_new_async (NULL, client_ready, NULL);
}

static void
with_client (AdwNavigationPage *page, NsFill fill)
{
  if (client) {
    fill (page);
    return;
  }
  if (!waiting)
    waiting = g_ptr_array_new ();
  g_ptr_array_add (waiting, g_object_ref (page));
  g_ptr_array_add (waiting, (gpointer) fill);
}

static void
set_nm_property (const char *name, gboolean on)
{
  nm_client_dbus_set_property (client, NM_DBUS_PATH, NM_DBUS_INTERFACE, name, g_variant_new_boolean (on), -1, NULL,
                               NULL, NULL);
}

/* the error of an async call, on the row that started it */
static void
row_error (GtkWidget *row, const char *what, GError *err)
{
  g_autofree char *t = g_strdup_printf ("%s: %s", what, err->message);
  ns_row_set_subtitle (row, t);
}

/* ---- Wi-Fi ---- */

typedef struct {
  NMDeviceWifi *dev;
  GtkWidget    *power, *cur, *avail, *saved, *none;
  guint         scan;
} Wifi;

static const char *
signal_icon (guint8 s)
{
  return s > 75 ? "network-wireless-signal-excellent-symbolic" : s > 50 ? "network-wireless-signal-good-symbolic"
       : s > 25 ? "network-wireless-signal-ok-symbolic" : "network-wireless-signal-weak-symbolic";
}

static char *
ap_ssid (NMAccessPoint *ap)
{
  GBytes *b = nm_access_point_get_ssid (ap);
  return b ? nm_utils_ssid_to_utf8 (g_bytes_get_data (b, NULL), g_bytes_get_size (b)) : NULL;
}

static gboolean
ap_secured (NMAccessPoint *ap)
{
  return (nm_access_point_get_flags (ap) & NM_802_11_AP_FLAGS_PRIVACY) || nm_access_point_get_wpa_flags (ap)
         || nm_access_point_get_rsn_flags (ap);
}

static gboolean
ap_enterprise (NMAccessPoint *ap)
{
  return (nm_access_point_get_wpa_flags (ap) | nm_access_point_get_rsn_flags (ap)) & NM_802_11_AP_SEC_KEY_MGMT_802_1X;
}

static NMRemoteConnection *
ap_saved (NMDeviceWifi *dev, NMAccessPoint *ap)
{
  const GPtrArray *all = nm_client_get_connections (client);
  g_autoptr (GPtrArray) fit = nm_device_filter_connections (NM_DEVICE (dev), all);
  g_autoptr (GPtrArray) mine = nm_access_point_filter_connections (ap, fit);
  return mine->len ? mine->pdata[0] : NULL;
}

static const char *
security_name (NMAccessPoint *ap)
{
  NM80211ApSecurityFlags f = nm_access_point_get_wpa_flags (ap) | nm_access_point_get_rsn_flags (ap);
  if (f & NM_802_11_AP_SEC_KEY_MGMT_802_1X)
    return "WPA Enterprise";
  if (f & NM_802_11_AP_SEC_KEY_MGMT_SAE)
    return "WPA3";
  if (nm_access_point_get_rsn_flags (ap))
    return "WPA2";
  if (nm_access_point_get_wpa_flags (ap))
    return "WPA";
  return nm_access_point_get_flags (ap) & NM_802_11_AP_FLAGS_PRIVACY ? "WEP" : "Open";
}

static void
activated (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (NMActiveConnection) ac = nm_client_activate_connection_finish (NM_CLIENT (src), res, &err);
  if (!ac)
    row_error (row, "Could not connect", err);
  g_object_unref (row);
}

static void
added_activated (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (NMActiveConnection) ac = nm_client_add_and_activate_connection_finish (NM_CLIENT (src), res, &err);
  if (!ac)
    row_error (row, "Could not connect", err);
  g_object_unref (row);
}

typedef struct {
  GtkWidget *row;
  char      *ap_path, *ssid;
  gboolean   sae;
  NMDevice  *dev;
} Join;

static void
join_free (Join *j)
{
  g_object_unref (j->row);
  g_object_unref (j->dev);
  g_free (j->ap_path);
  g_free (j->ssid);
  g_free (j);
}

static void
password_entered (const char *text, gpointer data)
{
  Join *j = data;
  if (text && *text) {
    NMConnection *conn = nm_simple_connection_new ();
    NMSetting *sec = nm_setting_wireless_security_new ();
    g_object_set (sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, j->sae ? "sae" : "wpa-psk",
                  NM_SETTING_WIRELESS_SECURITY_PSK, text, NULL);
    nm_connection_add_setting (conn, sec);
    ns_row_set_subtitle (j->row, "Connecting…");
    nm_client_add_and_activate_connection_async (client, conn, j->dev, j->ap_path, NULL, added_activated,
                                                 g_object_ref (j->row));
    g_object_unref (conn);
  }
  join_free (j);
}

static void wifi_details (GtkWidget *row, gpointer data);
static void join_form (NMDevice *dev, const char *ap_path, const char *ssid);
static void hidden_tapped (GtkWidget *row, gpointer data);

static void
ap_tapped (GtkWidget *row, gpointer data)
{
  AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Wifi *w = g_object_get_data (G_OBJECT (page), "wifi");
  const char *path = g_object_get_data (G_OBJECT (row), "ns-ap");
  NMAccessPoint *ap = NULL;
  const GPtrArray *aps = nm_device_wifi_get_access_points (w->dev);
  for (guint i = 0; i < aps->len && !ap; i++)
    if (!g_strcmp0 (nm_object_get_path (aps->pdata[i]), path))
      ap = aps->pdata[i];
  if (!ap)
    return;
  NMRemoteConnection *saved = ap_saved (w->dev, ap);
  if (saved) {
    ns_row_set_subtitle (row, "Connecting…");
    nm_client_activate_connection_async (client, NM_CONNECTION (saved), NM_DEVICE (w->dev), path, NULL, activated,
                                         g_object_ref (row));
    return;
  }
  if (ap_enterprise (ap)) {
    g_autofree char *ssid = ap_ssid (ap);
    join_form (NM_DEVICE (w->dev), path, ssid);
    return;
  }
  if (!ap_secured (ap)) {
    ns_row_set_subtitle (row, "Connecting…");
    nm_client_add_and_activate_connection_async (client, NULL, NM_DEVICE (w->dev), path, NULL, added_activated,
                                                 g_object_ref (row));
    return;
  }
  Join *j = g_new0 (Join, 1);
  j->row = g_object_ref (row);
  j->dev = g_object_ref (NM_DEVICE (w->dev));
  j->ap_path = g_strdup (path);
  j->ssid = ap_ssid (ap);
  j->sae = (nm_access_point_get_rsn_flags (ap) & NM_802_11_AP_SEC_KEY_MGMT_SAE)
           && !(nm_access_point_get_rsn_flags (ap) & NM_802_11_AP_SEC_KEY_MGMT_PSK);
  g_autofree char *head = g_strdup_printf ("Connect to %s", j->ssid ? j->ssid : "the network");
  ns_ask (row, head, "Enter the network's password.", NULL, TRUE, "Connect", password_entered, j);
}

static GtkWidget *
ap_row (GtkWidget *g, AdwNavigationPage *page, NMAccessPoint *ap, const char *sub)
{
  g_autofree char *ssid = ap_ssid (ap);
  GtkWidget *r = ns_row (g, signal_icon (nm_access_point_get_strength (ap)), "blue", ssid, sub);
  if (ap_secured (ap)) {
    GtkWidget *lock = gtk_image_new_from_icon_name ("system-lock-screen-symbolic");
    gtk_widget_set_opacity (lock, 0.5);
    ns_row_suffix (r, lock);
  }
  g_object_set_data (G_OBJECT (r), "ns-page", page);
  g_object_set_data_full (G_OBJECT (r), "ns-ap", g_strdup (nm_object_get_path (NM_OBJECT (ap))), g_free);
  return r;
}

static gint
by_strength (gconstpointer a, gconstpointer b)
{
  return nm_access_point_get_strength (*(NMAccessPoint **) b) - nm_access_point_get_strength (*(NMAccessPoint **) a);
}

static void
wifi_fill (AdwNavigationPage *page)
{
  Wifi *w = g_object_get_data (G_OBJECT (page), "wifi");
  if (!w->dev) {
    const GPtrArray *devs = nm_client_get_devices (client);
    for (guint i = 0; i < devs->len && !w->dev; i++)
      if (NM_IS_DEVICE_WIFI (devs->pdata[i]))
        w->dev = g_object_ref (devs->pdata[i]);
    if (!w->dev) {
      gtk_widget_set_visible (w->none, TRUE);
      gtk_widget_set_sensitive (w->power, FALSE);
      return;
    }
    g_signal_connect_object (client, "notify::wireless-enabled", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (client, "connection-added", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (client, "connection-removed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (w->dev, "access-point-added", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (w->dev, "access-point-removed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (w->dev, "notify::active-access-point", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (w->dev, "state-changed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    nm_device_wifi_request_scan_async (w->dev, NULL, NULL, NULL);
  }
  gboolean on = nm_client_wireless_get_enabled (client);
  ns_switch_quiet (ns_row_get_switch (w->power), on);
  ns_row_set_subtitle (w->power, on ? "On" : "Off");

  ns_group_clear (w->cur);
  ns_group_clear (w->avail);
  NMAccessPoint *active = nm_device_wifi_get_active_access_point (w->dev);
  NMDeviceState st = nm_device_get_state (NM_DEVICE (w->dev));
  if (active && on) {
    const char *say = st == NM_DEVICE_STATE_ACTIVATED ? "Connected"
                    : st == NM_DEVICE_STATE_NEED_AUTH ? "Needs a password" : "Connecting…";
    GtkWidget *r = ap_row (w->cur, page, active, say);
    ns_row_on_activate (r, wifi_details, NULL);
  }
  ns_group_set_visible (w->cur, active && on);

  g_autoptr (GPtrArray) aps = g_ptr_array_new ();
  const GPtrArray *all = nm_device_wifi_get_access_points (w->dev);
  for (guint i = 0; on && i < all->len; i++)
    g_ptr_array_add (aps, all->pdata[i]);
  g_ptr_array_sort (aps, by_strength);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (active) {
    char *s = ap_ssid (active);
    if (s)
      g_hash_table_add (seen, s);
  }
  for (guint i = 0; i < aps->len; i++) {
    NMAccessPoint *ap = aps->pdata[i];
    char *ssid = ap_ssid (ap);
    if (!ssid || !*ssid || g_hash_table_contains (seen, ssid)) {
      g_free (ssid);
      continue;
    }
    g_hash_table_add (seen, ssid);
    const char *sub = ap_saved (w->dev, ap) ? "Saved" : ap_secured (ap) ? NULL : "Open";
    GtkWidget *r = ap_row (w->avail, page, ap, sub);
    ns_row_on_activate (r, ap_tapped, NULL);
  }
  ns_group_set_visible (w->avail, g_hash_table_size (seen) > (active ? 1u : 0u));
}

static void
wifi_power (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  if (!ns_switch_syncing (GTK_WIDGET (sw)) && client)
    set_nm_property ("WirelessEnabled", gtk_switch_get_active (sw));
}

static gboolean
wifi_scan (gpointer page)
{
  Wifi *w = g_object_get_data (page, "wifi");
  if (w->dev && client && nm_client_wireless_get_enabled (client))
    nm_device_wifi_request_scan_async (w->dev, NULL, NULL, NULL);
  return G_SOURCE_CONTINUE;
}

static void
wifi_free (Wifi *w)
{
  g_source_remove (w->scan);
  g_clear_object (&w->dev);
  g_free (w);
}

/* the network now in use, or a saved one */
typedef struct {
  NMRemoteConnection *conn;
  NMDevice           *dev;
} Net;

static void
net_free (Net *n)
{
  g_clear_object (&n->conn);
  g_clear_object (&n->dev);
  g_free (n);
}

static void
forgotten (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  if (!nm_remote_connection_delete_finish (NM_REMOTE_CONNECTION (src), res, &err))
    row_error (row, "Could not forget it", err);
  g_object_unref (row);
}

static void
forget_answered (gboolean yes, gpointer row)
{
  Net *n = g_object_get_data (G_OBJECT (row), "ns-net");
  if (yes && n->conn) {
    nm_remote_connection_delete_async (n->conn, NULL, forgotten, g_object_ref (row));
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
  }
}

static void
forget (GtkWidget *row, gpointer data)
{
  ns_confirm (row, "Forget this network?", "Its password is deleted and the phone no longer joins it by itself.",
              "Forget", TRUE, forget_answered, row);
}

static void
disconnected (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  if (!nm_device_disconnect_finish (NM_DEVICE (src), res, &err))
    row_error (row, "Could not disconnect", err);
  g_object_unref (row);
}

static void
disconnect (GtkWidget *row, gpointer data)
{
  Net *n = g_object_get_data (G_OBJECT (row), "ns-net");
  if (n->dev) {
    nm_device_disconnect_async (n->dev, NULL, disconnected, g_object_ref (row));
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
  }
}

static void
autoconnect_set (GtkSwitch *sw, GParamSpec *ps, NMRemoteConnection *conn)
{
  NMSettingConnection *s = nm_connection_get_setting_connection (NM_CONNECTION (conn));
  if (!s || nm_setting_connection_get_autoconnect (s) == gtk_switch_get_active (sw))
    return;
  g_object_set (s, NM_SETTING_CONNECTION_AUTOCONNECT, gtk_switch_get_active (sw), NULL);
  nm_remote_connection_commit_changes_async (conn, TRUE, NULL, NULL, NULL);
}

static void
net_page (const char *title, NMRemoteConnection *conn, NMDevice *dev, NMAccessPoint *ap)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (title, &c);
  Net *n = g_new0 (Net, 1);
  n->conn = conn ? g_object_ref (conn) : NULL;
  n->dev = dev ? g_object_ref (dev) : NULL;
  g_object_set_data_full (G_OBJECT (page), "ns-net", n, (GDestroyNotify) net_free);

  GtkWidget *g;
  if (ap || dev) {
    g = ns_group (c, NULL);
    if (ap) {
      g_autofree char *sig = g_strdup_printf ("%u%%", nm_access_point_get_strength (ap));
      ns_row_info (g, "Signal strength", sig);
      g_autofree char *fr = g_strdup_printf ("%.1f GHz", nm_access_point_get_frequency (ap) / 1000.0);
      ns_row_info (g, "Frequency", fr);
      ns_row_info (g, "Security", security_name (ap));
    }
    NMIPConfig *ip4 = dev ? nm_device_get_ip4_config (dev) : NULL;
    GPtrArray *addrs = ip4 ? nm_ip_config_get_addresses (ip4) : NULL;
    if (addrs && addrs->len)
      ns_row_info (g, "IP address", nm_ip_address_get_address (addrs->pdata[0]));
    if (ip4 && nm_ip_config_get_gateway (ip4))
      ns_row_info (g, "Router", nm_ip_config_get_gateway (ip4));
    if (dev && nm_device_get_hw_address (dev))
      ns_row_info (g, "MAC address", nm_device_get_hw_address (dev));
  }
  g = ns_group (c, NULL);
  NMSettingConnection *s = conn ? nm_connection_get_setting_connection (NM_CONNECTION (conn)) : NULL;
  if (s) {
    GtkWidget *r = ns_row_switch (g, "Auto reconnect", "Join this network when it is in reach", NULL, NULL);
    GtkWidget *sw = ns_row_get_switch (r);
    gtk_switch_set_active (GTK_SWITCH (sw), nm_setting_connection_get_autoconnect (s));
    g_signal_connect_object (sw, "notify::active", G_CALLBACK (autoconnect_set), conn, 0);
  }
  if (dev) {
    GtkWidget *r = ns_row_button (g, "Disconnect", FALSE, disconnect, NULL);
    g_object_set_data (G_OBJECT (r), "ns-net", n);
  }
  if (conn) {
    GtkWidget *r = ns_row_button (g, "Forget", TRUE, forget, NULL);
    g_object_set_data (G_OBJECT (r), "ns-net", n);
  }
  ns_push (page);
}

static void
wifi_details (GtkWidget *row, gpointer data)
{
  AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Wifi *w = g_object_get_data (G_OBJECT (page), "wifi");
  NMAccessPoint *ap = nm_device_wifi_get_active_access_point (w->dev);
  NMActiveConnection *ac = nm_device_get_active_connection (NM_DEVICE (w->dev));
  if (!ap)
    return;
  g_autofree char *ssid = ap_ssid (ap);
  net_page (ssid, ac ? nm_active_connection_get_connection (ac) : NULL, NM_DEVICE (w->dev), ap);
}

static void
saved_tapped (GtkWidget *row, gpointer conn)
{
  net_page (nm_connection_get_id (conn), conn, NULL, NULL);
}

static void
saved_fill (AdwNavigationPage *page)
{
  GtkWidget *g = g_object_get_data (G_OBJECT (page), "ns-list");
  ns_group_clear (g);
  const GPtrArray *all = nm_client_get_connections (client);
  guint n = 0;
  for (guint i = 0; i < all->len; i++) {
    NMConnection *conn = all->pdata[i];
    if (g_strcmp0 (nm_connection_get_connection_type (conn), NM_SETTING_WIRELESS_SETTING_NAME))
      continue;
    GtkWidget *r = ns_row (g, "network-wireless-symbolic", "blue", nm_connection_get_id (conn), NULL);
    g_object_set_data_full (G_OBJECT (r), "ns-conn", g_object_ref (conn), g_object_unref);
    ns_row_on_activate (r, saved_tapped, conn);
    n++;
  }
  ns_group_set_visible (g, n > 0);
  gtk_widget_set_visible (g_object_get_data (G_OBJECT (page), "ns-empty"), n == 0);
}

static void
saved_page (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Saved networks", &c);
  GtkWidget *g = ns_group (c, NULL);
  g_object_set_data (G_OBJECT (page), "ns-list", g);
  GtkWidget *empty = ns_note (c, "No saved networks.");
  gtk_widget_set_visible (empty, FALSE);
  g_object_set_data (G_OBJECT (page), "ns-empty", empty);
  ns_later_init (page, saved_fill);
  g_signal_connect_object (client, "connection-added", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  g_signal_connect_object (client, "connection-removed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  saved_fill (page);
  ns_push (page);
}

void
ns_page_wifi (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Wi-Fi", &c);
  Wifi *w = g_new0 (Wifi, 1);
  g_object_set_data_full (G_OBJECT (page), "wifi", w, (GDestroyNotify) wifi_free);
  ns_later_init (page, wifi_fill);

  GtkWidget *g = ns_group (c, NULL);
  w->power = ns_row_toggle (g, NULL, NULL, "Wi-Fi", NULL);
  g_signal_connect (ns_row_get_switch (w->power), "notify::active", G_CALLBACK (wifi_power), NULL);
  w->none = ns_note (c, "This phone has no Wi-Fi adapter.");
  gtk_widget_set_visible (w->none, FALSE);
  w->cur = ns_group (c, "Current network");
  ns_group_set_visible (w->cur, FALSE);
  w->avail = ns_group (c, "Available networks");
  ns_group_set_visible (w->avail, FALSE);
  g = ns_group (c, NULL);
  w->saved = ns_row (g, NULL, NULL, "Saved networks", "Forget • Auto reconnect");
  ns_row_on_activate (w->saved, saved_page, NULL);
  GtkWidget *add = ns_row (g, NULL, NULL, "Add network", "A hidden network, or one not in reach");
  g_object_set_data (G_OBJECT (add), "ns-page", page);
  ns_row_on_activate (add, hidden_tapped, NULL);
  ns_row_on_activate (ns_row (g, NULL, NULL, "Mobile hotspot", "Share the mobile data over Wi-Fi"), ns_page_hotspot,
                      NULL);
  w->scan = g_timeout_add_seconds (20, wifi_scan, page);
  with_client (page, wifi_fill);
  ns_push (page);
}

/* ---- Joining by hand: a hidden network, or WPA Enterprise (802.1X) ---- */

static const char *const sec_labels[] = { "None", "WPA/WPA2 Personal", "WPA3 Personal", "WPA/WPA2 Enterprise", NULL };
enum { SEC_NONE, SEC_PSK, SEC_SAE, SEC_EAP };
static const char *const eap_labels[] = { "PEAP", "TTLS", "TLS", "PWD", NULL };
static const char *const eap_values[] = { "peap", "ttls", "tls", "pwd" };
enum { EAP_PEAP, EAP_TTLS, EAP_TLS, EAP_PWD };
static const char *const phase2_labels[] = { "MSCHAPv2", "PAP", "MSCHAP", "GTC", NULL };
static const char *const phase2_values[] = { "mschapv2", "pap", "mschap", "gtc" };
static const char *const ca_labels[] = { "System certificates", "Choose a file…", "Do not validate", NULL };
enum { CA_SYSTEM, CA_FILE, CA_NONE };

typedef struct {
  NMDevice  *dev;
  char      *ap_path;
  GtkWidget *name, *sec, *pass, *eap, *phase2, *ident, *anon, *ca, *domain, *cert, *key, *keypass, *status, *certs;
  guint      sec_i, eap_i, phase2_i, ca_i;
  char      *ca_path, *cert_path, *key_path;
} Form;

static void
form_free (Form *f)
{
  g_clear_object (&f->dev);
  g_free (f->ap_path);
  g_free (f->ca_path);
  g_free (f->cert_path);
  g_free (f->key_path);
  g_free (f);
}

static void
form_update (Form *f)
{
  gboolean eap = f->sec_i == SEC_EAP, inner = eap && (f->eap_i == EAP_PEAP || f->eap_i == EAP_TTLS);
  gboolean tls = eap && f->eap_i == EAP_TLS;
  gtk_widget_set_visible (f->pass, (f->sec_i == SEC_PSK || f->sec_i == SEC_SAE) || (eap && !tls));
  gtk_widget_set_visible (f->eap, eap);
  gtk_widget_set_visible (f->phase2, inner);
  gtk_widget_set_visible (f->ident, eap);
  gtk_widget_set_visible (f->anon, inner);
  gtk_widget_set_visible (f->ca, eap && f->eap_i != EAP_PWD);
  gtk_widget_set_visible (f->domain, eap && f->eap_i != EAP_PWD && f->ca_i != CA_NONE);
  gtk_widget_set_visible (f->cert, tls);
  gtk_widget_set_visible (f->key, tls);
  gtk_widget_set_visible (f->keypass, tls);
  ns_group_set_visible (f->certs, eap && f->eap_i != EAP_PWD);
  gtk_list_box_invalidate_headers (GTK_LIST_BOX (gtk_widget_get_parent (f->pass)));
  gtk_list_box_invalidate_headers (GTK_LIST_BOX (f->certs));
}

static void
sec_picked (GtkWidget *row, guint i, gpointer f)
{
  ((Form *) f)->sec_i = i;
  form_update (f);
}

static void
eap_picked (GtkWidget *row, guint i, gpointer f)
{
  ((Form *) f)->eap_i = i;
  form_update (f);
}

static void
phase2_picked (GtkWidget *row, guint i, gpointer f)
{
  ((Form *) f)->phase2_i = i;
}

/* a file for a certificate or key row: its name in the subtitle, its path kept in `slot` */
typedef struct {
  GtkWidget *row;
  char     **slot;
  Form      *f;
} FilePick;

static void
file_chosen (GObject *src, GAsyncResult *res, gpointer data)
{
  FilePick *p = data;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, NULL);
  if (file) {
    g_free (*p->slot);
    *p->slot = g_file_get_path (file);
    g_autofree char *base = g_file_get_basename (file);
    ns_row_set_subtitle (p->row, base);
  } else if (p->f && p->slot == &p->f->ca_path && !p->f->ca_path) {
    /* no file after all: back to the system's certificates */
    p->f->ca_i = CA_SYSTEM;
    ns_row_pick_select (p->f->ca, CA_SYSTEM);
    form_update (p->f);
  }
  g_object_unref (p->row);
  g_free (p);
}

static void
choose_file (GtkWidget *row, char **slot, Form *f, const char *title)
{
  FilePick *p = g_new0 (FilePick, 1);
  p->row = g_object_ref (row);
  p->slot = slot;
  p->f = f;
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (d, title);
  gtk_file_dialog_open (d, GTK_WINDOW (gtk_widget_get_root (row)), NULL, file_chosen, p);
}

static void
ca_picked (GtkWidget *row, guint i, gpointer data)
{
  Form *f = data;
  f->ca_i = i;
  if (i == CA_FILE)
    choose_file (row, &f->ca_path, f, "CA certificate");
  form_update (f);
}

static void
cert_tapped (GtkWidget *row, gpointer f)
{
  choose_file (row, &((Form *) f)->cert_path, NULL, "Your certificate");
}

static void
key_tapped (GtkWidget *row, gpointer f)
{
  choose_file (row, &((Form *) f)->key_path, NULL, "Private key");
}

static void
form_added (GObject *src, GAsyncResult *res, gpointer status)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (NMActiveConnection) ac = nm_client_add_and_activate_connection_finish (NM_CLIENT (src), res, &err);
  if (ac) {
    AdwNavigationView *nav = ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (status, ADW_TYPE_NAVIGATION_VIEW));
    if (nav)
      adw_navigation_view_pop (nav);
  } else {
    row_error (status, "Could not connect", err);
  }
  g_object_unref (status);
}

static const char *
value_or_null (GtkWidget *row)
{
  const char *v = ns_row_value_get (row);
  return *v ? v : NULL;
}

static void
form_connect (GtkWidget *row, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  const char *ssid = f->name ? value_or_null (f->name) : NULL;
  if (f->name && !ssid) {
    ns_row_set_subtitle (f->status, "The network's name is missing");
    return;
  }
  const char *pass = value_or_null (f->pass);
  if ((f->sec_i == SEC_PSK || f->sec_i == SEC_SAE) && (!pass || strlen (pass) < 8)) {
    ns_row_set_subtitle (f->status, "The password has at least 8 characters");
    return;
  }

  NMConnection *conn = nm_simple_connection_new ();
  NMSetting *wifi = nm_setting_wireless_new ();
  if (ssid) {
    g_autoptr (GBytes) b = g_bytes_new (ssid, strlen (ssid));
    g_object_set (wifi, NM_SETTING_WIRELESS_SSID, b, NM_SETTING_WIRELESS_HIDDEN, TRUE, NULL);
    NMSetting *s = nm_setting_connection_new ();
    g_object_set (s, NM_SETTING_CONNECTION_ID, ssid, NULL);
    nm_connection_add_setting (conn, s);
  }
  nm_connection_add_setting (conn, wifi);
  if (f->sec_i != SEC_NONE) {
    NMSetting *sec = nm_setting_wireless_security_new ();
    const char *mgmt = f->sec_i == SEC_EAP ? "wpa-eap" : f->sec_i == SEC_SAE ? "sae" : "wpa-psk";
    g_object_set (sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, mgmt, NULL);
    if (f->sec_i != SEC_EAP)
      g_object_set (sec, NM_SETTING_WIRELESS_SECURITY_PSK, pass, NULL);
    nm_connection_add_setting (conn, sec);
  }
  if (f->sec_i == SEC_EAP) {
    NMSetting8021x *x = NM_SETTING_802_1X (nm_setting_802_1x_new ());
    nm_setting_802_1x_add_eap_method (x, eap_values[f->eap_i]);
    g_object_set (x, NM_SETTING_802_1X_IDENTITY, value_or_null (f->ident), NULL);
    if (f->eap_i != EAP_TLS)
      g_object_set (x, NM_SETTING_802_1X_PASSWORD, pass, NULL);
    if (f->eap_i == EAP_PEAP || f->eap_i == EAP_TTLS)
      g_object_set (x, NM_SETTING_802_1X_PHASE2_AUTH, phase2_values[f->phase2_i],
                    NM_SETTING_802_1X_ANONYMOUS_IDENTITY, value_or_null (f->anon), NULL);
    if (f->eap_i != EAP_PWD) {
      if (f->ca_i == CA_SYSTEM)
        g_object_set (x, NM_SETTING_802_1X_SYSTEM_CA_CERTS, TRUE, NULL);
      else if (f->ca_i == CA_FILE && f->ca_path &&
               !nm_setting_802_1x_set_ca_cert (x, f->ca_path, NM_SETTING_802_1X_CK_SCHEME_PATH, NULL, &err)) {
        row_error (f->status, "CA certificate", err);
        g_object_unref (x);
        g_object_unref (conn);
        return;
      }
      if (f->ca_i != CA_NONE && value_or_null (f->domain))
        g_object_set (x, NM_SETTING_802_1X_DOMAIN_SUFFIX_MATCH, value_or_null (f->domain), NULL);
    }
    if (f->eap_i == EAP_TLS) {
      if (!f->cert_path || !f->key_path) {
        ns_row_set_subtitle (f->status, "TLS needs your certificate and its private key");
        g_object_unref (x);
        g_object_unref (conn);
        return;
      }
      if (!nm_setting_802_1x_set_client_cert (x, f->cert_path, NM_SETTING_802_1X_CK_SCHEME_PATH, NULL, &err) ||
          !nm_setting_802_1x_set_private_key (x, f->key_path, value_or_null (f->keypass),
                                              NM_SETTING_802_1X_CK_SCHEME_PATH, NULL, &err)) {
        row_error (f->status, "Certificate", err);
        g_object_unref (x);
        g_object_unref (conn);
        return;
      }
    }
    nm_connection_add_setting (conn, NM_SETTING (x));
  }
  ns_row_set_subtitle (f->status, "Connecting…");
  nm_client_add_and_activate_connection_async (client, conn, f->dev, f->ap_path, NULL, form_added,
                                               g_object_ref (f->status));
  g_object_unref (conn);
}

/* `ap_path` NULL: a hidden network, its name and security typed in; else that access point (an enterprise one) */
static void
join_form (NMDevice *dev, const char *ap_path, const char *ssid)
{
  GtkWidget *c;
  g_autofree char *title = ssid ? g_strdup (ssid) : g_strdup ("Add network");
  AdwNavigationPage *page = ns_page_new (title, &c);
  Form *f = g_new0 (Form, 1);
  f->dev = g_object_ref (dev);
  f->ap_path = g_strdup (ap_path);
  g_object_set_data_full (G_OBJECT (page), "ns-form", f, (GDestroyNotify) form_free);

  GtkWidget *g = ns_group (c, NULL);
  if (!ap_path) {
    f->name = ns_row_value (g, "Network name", NULL, FALSE, NULL, NULL);
    f->sec_i = SEC_PSK;
    f->sec = ns_row_pick (g, "Security", sec_labels, f->sec_i, sec_picked, f);
  } else {
    f->sec_i = SEC_EAP;
  }
  f->eap = ns_row_pick (g, "Method", eap_labels, f->eap_i, eap_picked, f);
  f->phase2 = ns_row_pick (g, "Inner authentication", phase2_labels, f->phase2_i, phase2_picked, f);
  f->ident = ns_row_value (g, "Identity", NULL, FALSE, NULL, NULL);
  f->anon = ns_row_value (g, "Anonymous identity", NULL, FALSE, NULL, NULL);
  f->pass = ns_row_value (g, "Password", NULL, TRUE, NULL, NULL);

  g = f->certs = ns_group (c, NULL);
  f->ca = ns_row_pick (g, "CA certificate", ca_labels, f->ca_i, ca_picked, f);
  f->domain = ns_row_value (g, "Domain", NULL, FALSE, NULL, NULL);
  f->cert = ns_row (g, NULL, NULL, "Your certificate", "Choose a file");
  ns_row_on_activate (f->cert, cert_tapped, f);
  f->key = ns_row (g, NULL, NULL, "Private key", "Choose a file");
  ns_row_on_activate (f->key, key_tapped, f);
  f->keypass = ns_row_value (g, "Private key password", NULL, TRUE, NULL, NULL);

  g = ns_group (c, NULL);
  f->status = ns_row_button (g, "Connect", FALSE, form_connect, f);
  form_update (f);
  ns_push (page);
}

static void
hidden_tapped (GtkWidget *row, gpointer data)
{
  AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Wifi *w = g_object_get_data (G_OBJECT (page), "wifi");
  if (w->dev)
    join_form (NM_DEVICE (w->dev), NULL, NULL);
}

/* ---- Mobile hotspot ---- */

/* the same connection Phosh's hotspot tile uses: the first access-point Wi-Fi connection the device can take, made
 * the way Phosh makes it ("Phosh Hotspot", WPA2, shared IPv4) when there is none yet */
typedef struct {
  NMDeviceWifi       *dev;
  NMRemoteConnection *conn;
  GtkWidget          *power, *name, *pass, *band, *clients, *none;
  guint               poll;
} Hot;

static const char *const band_labels[] = { "Automatic", "2.4 GHz", "5 GHz", NULL };
static const char *const band_values[] = { NULL, "bg", "a" };

static void
hot_free (Hot *h)
{
  if (h->poll)
    g_source_remove (h->poll);
  g_clear_object (&h->dev);
  g_clear_object (&h->conn);
  g_free (h);
}

static NMRemoteConnection *
hot_conn (NMDeviceWifi *dev)
{
  const GPtrArray *all = nm_client_get_connections (client);
  for (guint i = 0; i < all->len; i++) {
    NMSettingWireless *s = nm_connection_get_setting_wireless (all->pdata[i]);
    if (s && !g_strcmp0 (nm_setting_wireless_get_mode (s), NM_SETTING_WIRELESS_MODE_AP) &&
        nm_device_connection_compatible (NM_DEVICE (dev), all->pdata[i], NULL))
      return all->pdata[i];
  }
  return NULL;
}

static NMActiveConnection *
hot_active (Hot *h)
{
  NMActiveConnection *ac = h->dev ? nm_device_get_active_connection (NM_DEVICE (h->dev)) : NULL;
  return ac && h->conn && nm_active_connection_get_connection (ac) == h->conn ? ac : NULL;
}

static char *
random_key (void)
{
  static const char chars[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  char k[11];
  for (guint i = 0; i < 10; i++)
    k[i] = chars[g_random_int_range (0, sizeof chars - 1)];
  k[10] = 0;
  return g_strdup (k);
}

static void
secrets_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = nm_remote_connection_get_secrets_finish (NM_REMOTE_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) sec = v ? g_variant_lookup_value (v, NM_SETTING_WIRELESS_SECURITY_SETTING_NAME,
                                                         G_VARIANT_TYPE_VARDICT) : NULL;
  const char *psk = NULL;
  if (sec && g_variant_lookup (sec, NM_SETTING_WIRELESS_SECURITY_PSK, "&s", &psk))
    ns_row_value_set (row, psk);
  g_object_unref (row);
}

/* who is on it: dnsmasq's leases, which NetworkManager keeps per interface */
static gboolean
hot_clients (gpointer page)
{
  Hot *h = g_object_get_data (page, "hot");
  ns_group_clear (h->clients);
  guint n = 0;
  if (hot_active (h)) {
    g_autofree char *path = g_strdup_printf ("/var/lib/NetworkManager/dnsmasq-%s.leases",
                                             nm_device_get_iface (NM_DEVICE (h->dev)));
    g_autofree char *t = NULL;
    g_file_get_contents (path, &t, NULL, NULL);
    g_auto (GStrv) lines = g_strsplit (t ? t : "", "\n", -1);
    for (guint i = 0; lines[i]; i++) {
      g_auto (GStrv) f = g_strsplit (lines[i], " ", 5);
      if (g_strv_length (f) < 4)
        continue;
      ns_row (h->clients, "computer-symbolic", "grey", g_strcmp0 (f[3], "*") ? f[3] : f[2], f[2]);
      n++;
    }
  }
  ns_group_set_visible (h->clients, n > 0);
  return G_SOURCE_CONTINUE;
}

static void
hot_fill (AdwNavigationPage *page)
{
  Hot *h = g_object_get_data (G_OBJECT (page), "hot");
  if (!h->dev) {
    const GPtrArray *devs = nm_client_get_devices (client);
    for (guint i = 0; i < devs->len && !h->dev; i++)
      if (NM_IS_DEVICE_WIFI (devs->pdata[i]) &&
          nm_device_wifi_get_capabilities (devs->pdata[i]) & NM_WIFI_DEVICE_CAP_AP)
        h->dev = g_object_ref (devs->pdata[i]);
    if (!h->dev) {
      gtk_widget_set_visible (h->none, TRUE);
      gtk_widget_set_sensitive (h->power, FALSE);
      return;
    }
    g_signal_connect_object (h->dev, "state-changed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (client, "notify::active-connections", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (client, "connection-added", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
    g_signal_connect_object (client, "connection-removed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  }
  NMRemoteConnection *conn = hot_conn (h->dev);
  if (conn != h->conn) {
    g_set_object (&h->conn, conn);
    if (conn) {
      NMSettingWireless *s = nm_connection_get_setting_wireless (NM_CONNECTION (conn));
      GBytes *b = nm_setting_wireless_get_ssid (s);
      g_autofree char *ssid = b ? nm_utils_ssid_to_utf8 (g_bytes_get_data (b, NULL), g_bytes_get_size (b)) : NULL;
      ns_row_value_set (h->name, ssid);
      const char *band = nm_setting_wireless_get_band (s);
      guint bi = !g_strcmp0 (band, "bg") ? 1 : !g_strcmp0 (band, "a") ? 2 : 0;
      ns_row_pick_select (h->band, bi);
      g_object_set_data (G_OBJECT (h->band), "ns-band", GUINT_TO_POINTER (bi));
      nm_remote_connection_get_secrets_async (conn, NM_SETTING_WIRELESS_SECURITY_SETTING_NAME, NULL, secrets_got,
                                              g_object_ref (h->pass));
    }
  }
  NMActiveConnection *ac = hot_active (h);
  NMActiveConnectionState st = ac ? nm_active_connection_get_state (ac) : NM_ACTIVE_CONNECTION_STATE_UNKNOWN;
  ns_switch_quiet (ns_row_get_switch (h->power), ac != NULL);
  ns_row_set_subtitle (h->power, st == NM_ACTIVE_CONNECTION_STATE_ACTIVATED ? "On"
                                 : st == NM_ACTIVE_CONNECTION_STATE_ACTIVATING ? "Starting…" : "Off");
  hot_clients (page);
}

static NMConnection *
hot_new (Hot *h)
{
  NMConnection *conn = nm_simple_connection_new ();
  NMSetting *s = nm_setting_connection_new ();
  g_object_set (s, NM_SETTING_CONNECTION_ID, "Phosh Hotspot", NM_SETTING_CONNECTION_AUTOCONNECT, FALSE, NULL);
  nm_connection_add_setting (conn, s);
  const char *name = ns_row_value_get (h->name);
  g_autoptr (GBytes) ssid = g_bytes_new (name, strlen (name));
  s = nm_setting_wireless_new ();
  guint bi = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (h->band), "ns-band"));
  g_object_set (s, NM_SETTING_WIRELESS_MODE, NM_SETTING_WIRELESS_MODE_AP, NM_SETTING_WIRELESS_SSID, ssid,
                NM_SETTING_WIRELESS_BAND, band_values[bi], NULL);
  nm_connection_add_setting (conn, s);
  NMSettingWirelessSecurity *sec = NM_SETTING_WIRELESS_SECURITY (nm_setting_wireless_security_new ());
  nm_setting_wireless_security_add_proto (sec, "rsn");
  nm_setting_wireless_security_add_pairwise (sec, "ccmp");
  nm_setting_wireless_security_add_group (sec, "ccmp");
  g_object_set (sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk", NM_SETTING_WIRELESS_SECURITY_PSK,
                ns_row_value_get (h->pass), NULL);
  nm_connection_add_setting (conn, NM_SETTING (sec));
  s = nm_setting_ip4_config_new ();
  g_object_set (s, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_SHARED, NULL);
  nm_connection_add_setting (conn, s);
  s = nm_setting_ip6_config_new ();
  g_object_set (s, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_IGNORE, NULL);
  nm_connection_add_setting (conn, s);
  return conn;
}

static void
hot_power (GtkSwitch *sw, GParamSpec *ps, gpointer page)
{
  Hot *h = g_object_get_data (page, "hot");
  if (ns_switch_syncing (GTK_WIDGET (sw)) || !h->dev)
    return;
  NMActiveConnection *ac = hot_active (h);
  if (!gtk_switch_get_active (sw)) {
    if (ac)
      nm_client_deactivate_connection_async (client, ac, NULL, NULL, NULL);
    return;
  }
  ns_row_set_subtitle (h->power, "Starting…");
  if (h->conn) {
    nm_client_activate_connection_async (client, NM_CONNECTION (h->conn), NM_DEVICE (h->dev), NULL, NULL,
                                         activated, g_object_ref (h->power));
  } else {
    g_autoptr (NMConnection) conn = hot_new (h);
    nm_client_add_and_activate_connection_async (client, conn, NM_DEVICE (h->dev), NULL, NULL, added_activated,
                                                 g_object_ref (h->power));
  }
}

/* a change to the saved hotspot; if it is on, it starts again with it */
static void
hot_committed (GObject *src, GAsyncResult *res, gpointer page)
{
  Hot *h = g_object_get_data (page, "hot");
  g_autoptr (GError) err = NULL;
  if (!nm_remote_connection_commit_changes_finish (NM_REMOTE_CONNECTION (src), res, &err))
    row_error (h->power, "Could not save it", err);
  else if (hot_active (h))
    nm_client_activate_connection_async (client, NM_CONNECTION (h->conn), NM_DEVICE (h->dev), NULL, NULL,
                                         activated, g_object_ref (h->power));
  g_object_unref (page);
}

static void
hot_save (AdwNavigationPage *page)
{
  Hot *h = g_object_get_data (G_OBJECT (page), "hot");
  if (!h->conn)
    return;
  NMConnection *c = NM_CONNECTION (h->conn);
  const char *name = ns_row_value_get (h->name);
  g_autoptr (GBytes) ssid = g_bytes_new (name, strlen (name));
  guint bi = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (h->band), "ns-band"));
  g_object_set (nm_connection_get_setting_wireless (c), NM_SETTING_WIRELESS_SSID, ssid, NM_SETTING_WIRELESS_BAND,
                band_values[bi], NULL);
  NMSettingWirelessSecurity *sec = nm_connection_get_setting_wireless_security (c);
  if (sec)
    g_object_set (sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk", NM_SETTING_WIRELESS_SECURITY_PSK,
                  ns_row_value_get (h->pass), NULL);
  nm_remote_connection_commit_changes_async (h->conn, TRUE, NULL, hot_committed, g_object_ref (page));
}

static void
hot_name_changed (const char *text, gpointer page)
{
  Hot *h = g_object_get_data (page, "hot");
  if (!*text) {
    g_autofree char *host = g_hostname_to_ascii (g_get_host_name ());
    ns_row_value_set (h->name, host);
  }
  hot_save (page);
}

static void
hot_pass_changed (const char *text, gpointer page)
{
  Hot *h = g_object_get_data (page, "hot");
  if (strlen (text) < 8 || strlen (text) > 63) {
    ns_row_set_subtitle (h->pass, "8 to 63 characters: not changed");
    if (h->conn)
      nm_remote_connection_get_secrets_async (h->conn, NM_SETTING_WIRELESS_SECURITY_SETTING_NAME, NULL,
                                              secrets_got, g_object_ref (h->pass));
    return;
  }
  hot_save (page);
}

static void
hot_band_picked (GtkWidget *row, guint i, gpointer page)
{
  g_object_set_data (G_OBJECT (row), "ns-band", GUINT_TO_POINTER (i));
  hot_save (page);
}

void
ns_page_hotspot (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Mobile hotspot", &c);
  Hot *h = g_new0 (Hot, 1);
  g_object_set_data_full (G_OBJECT (page), "hot", h, (GDestroyNotify) hot_free);
  ns_later_init (page, hot_fill);

  GtkWidget *g = ns_group (c, NULL);
  h->power = ns_row_toggle (g, NULL, NULL, "Mobile hotspot", "Off");
  g_signal_connect (ns_row_get_switch (h->power), "notify::active", G_CALLBACK (hot_power), page);
  h->none = ns_note (c, "This phone's Wi-Fi cannot be an access point.");
  gtk_widget_set_visible (h->none, FALSE);

  g = ns_group (c, NULL);
  g_autofree char *host = g_hostname_to_ascii (g_get_host_name ());
  h->name = ns_row_value (g, "Network name", host, FALSE, hot_name_changed, page);
  g_autofree char *key = random_key ();
  h->pass = ns_row_value (g, "Password", key, FALSE, hot_pass_changed, page);
  h->band = ns_row_pick (g, "Band", band_labels, 0, hot_band_picked, page);

  h->clients = ns_group (c, "Connected devices");
  ns_group_set_visible (h->clients, FALSE);
  ns_note (c, "Other devices use the phone's mobile data. Wi-Fi is off while the hotspot is on.");
  h->poll = g_timeout_add_seconds (5, hot_clients, page);
  with_client (page, hot_fill);
  ns_push (page);
}

/* ---- Mobile network ---- */

typedef struct {
  GtkWidget *data, *apns, *info;
} Mobile;

static NMDevice *
modem (void)
{
  const GPtrArray *devs = nm_client_get_devices (client);
  for (guint i = 0; i < devs->len; i++)
    if (NM_IS_DEVICE_MODEM (devs->pdata[i]))
      return devs->pdata[i];
  return NULL;
}

static void
apn_tapped (GtkWidget *row, gpointer conn)
{
  NMDevice *dev = modem ();
  ns_row_set_subtitle (row, "Connecting…");
  nm_client_activate_connection_async (client, conn, dev, NULL, NULL, activated, g_object_ref (row));
}

static void
mobile_fill (AdwNavigationPage *page)
{
  Mobile *m = g_object_get_data (G_OBJECT (page), "mobile");
  gboolean on = nm_client_wwan_get_enabled (client);
  ns_switch_quiet (ns_row_get_switch (m->data), on);
  NMDevice *dev = modem ();
  NMActiveConnection *ac = dev ? nm_device_get_active_connection (dev) : NULL;
  NMRemoteConnection *cur = ac ? nm_active_connection_get_connection (ac) : NULL;
  ns_row_set_subtitle (m->data, !dev ? "No modem" : cur ? "Connected" : on ? "On" : "Off");

  ns_group_clear (m->apns);
  const GPtrArray *all = nm_client_get_connections (client);
  guint n = 0;
  for (guint i = 0; i < all->len; i++) {
    NMConnection *conn = all->pdata[i];
    NMSettingGsm *gsm = nm_connection_get_setting_gsm (conn);
    if (!gsm)
      continue;
    const char *apn = nm_setting_gsm_get_apn (gsm);
    GtkWidget *r = ns_row (m->apns, NULL, NULL, nm_connection_get_id (conn),
                           cur == all->pdata[i] ? "In use" : apn && *apn ? apn : "Automatic APN");
    GtkWidget *mark = gtk_image_new_from_icon_name ("object-select-symbolic");
    gtk_widget_add_css_class (mark, "ns-accent");
    gtk_widget_set_opacity (mark, cur == all->pdata[i]);
    ns_row_suffix (r, mark);
    g_object_set_data_full (G_OBJECT (r), "ns-conn", g_object_ref (conn), g_object_unref);
    if (dev)
      ns_row_on_activate (r, apn_tapped, conn);
    n++;
  }
  ns_group_set_visible (m->apns, n > 0);
}

static void
mobile_data (GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
  if (!ns_switch_syncing (GTK_WIDGET (sw)) && client)
    set_nm_property ("WwanEnabled", gtk_switch_get_active (sw));
}

static const char *
generation (guint32 tech)
{
  return tech & (1u << 15) ? "5G" : tech & (1u << 14) ? "4G" : tech & 0x3e0 ? "3G" : tech & 0x1e ? "2G" : NULL;
}

/* ModemManager's own answer: operator, signal, number */
static void
modems_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Mobile *m = g_object_get_data (page, "mobile");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) objs = NULL;
  if (v)
    objs = g_variant_get_child_value (v, 0);
  GVariantIter it;
  const char *path;
  GVariant *ifaces;
  gboolean any = FALSE;
  if (objs) {
    g_variant_iter_init (&it, objs);
    while (!any && g_variant_iter_next (&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
      g_autoptr (GVariant) mm = g_variant_lookup_value (ifaces, "org.freedesktop.ModemManager1.Modem",
                                                        G_VARIANT_TYPE_VARDICT);
      g_autoptr (GVariant) gpp = g_variant_lookup_value (ifaces, "org.freedesktop.ModemManager1.Modem.Modem3gpp",
                                                         G_VARIANT_TYPE_VARDICT);
      g_variant_unref (ifaces);
      if (!mm)
        continue;
      any = TRUE;
      const char *op = NULL, *sim = NULL, *imei = NULL, *model = NULL;
      guint32 tech = 0, q = 0;
      gboolean recent;
      g_autofree const char **nums = NULL;
      if (gpp) {
        g_variant_lookup (gpp, "OperatorName", "&s", &op);
        g_variant_lookup (gpp, "Imei", "&s", &imei);
      }
      g_variant_lookup (mm, "Sim", "&o", &sim);
      g_variant_lookup (mm, "Model", "&s", &model);
      g_variant_lookup (mm, "AccessTechnologies", "u", &tech);
      g_variant_lookup (mm, "SignalQuality", "(ub)", &q, &recent);
      g_variant_lookup (mm, "OwnNumbers", "^a&s", &nums);
      gboolean has_sim = sim && g_strcmp0 (sim, "/");
      ns_row_info (m->info, "SIM", has_sim ? "Inserted" : "No SIM card");
      if (op && *op)
        ns_row_info (m->info, "Network operator", op);
      if (generation (tech)) {
        g_autofree char *t = g_strdup_printf ("%s • %u%% signal", generation (tech), q);
        ns_row_info (m->info, "Network", t);
      }
      if (nums && nums[0])
        ns_row_info (m->info, "Phone number", nums[0]);
      if (model)
        ns_row_info (m->info, "Modem", model);
      if (imei && *imei)
        ns_row_info (m->info, "IMEI", imei);
    }
  }
  if (!any)
    ns_row_info (m->info, "Modem", "None found");
  ns_group_set_visible (m->info, TRUE);
  g_object_unref (page);
}

static void
mobile_connect (AdwNavigationPage *page)
{
  g_signal_connect_object (client, "notify::wwan-enabled", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  g_signal_connect_object (client, "notify::active-connections", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  g_signal_connect_object (client, "connection-added", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  g_signal_connect_object (client, "connection-removed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
}

/* mobile_fill runs once the client is there; its signals are hooked up the first time */
static void
mobile_first (AdwNavigationPage *page)
{
  mobile_connect (page);
  mobile_fill (page);
}

void
ns_page_mobile (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Mobile network", &c);
  Mobile *m = g_new0 (Mobile, 1);
  g_object_set_data_full (G_OBJECT (page), "mobile", m, g_free);
  ns_later_init (page, mobile_fill);

  GtkWidget *g = ns_group (c, NULL);
  m->data = ns_row_toggle (g, NULL, NULL, "Mobile data", NULL);
  g_signal_connect (ns_row_get_switch (m->data), "notify::active", G_CALLBACK (mobile_data), NULL);
  GSettings *wwan = ns_page_settings (page, "org.gnome.settings-daemon.plugins.wwan");
  if (ns_has_key (wwan, "unlock-sim"))
    ns_row_switch (g, "Ask for the SIM PIN", "When the phone starts", wwan, "unlock-sim");
  m->apns = ns_group (c, "Access point names");
  ns_group_set_visible (m->apns, FALSE);
  m->info = ns_group (c, "SIM and network");
  ns_group_set_visible (m->info, FALSE);

  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.ModemManager1", "/org/freedesktop/ModemManager1",
                            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                            G_VARIANT_TYPE ("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, modems_got,
                            g_object_ref (page));
  with_client (page, mobile_first);
  ns_push (page);
}

/* ---- Network and VPN ---- */

typedef struct {
  GtkWidget *vpn, *other, *novpn;
} Network;

static NMActiveConnection *
active_for (NMConnection *conn)
{
  const GPtrArray *acs = nm_client_get_active_connections (client);
  for (guint i = 0; i < acs->len; i++)
    if ((NMConnection *) nm_active_connection_get_connection (acs->pdata[i]) == conn)
      return acs->pdata[i];
  return NULL;
}

static void
conn_switched (GtkSwitch *sw, GParamSpec *ps, NMConnection *conn)
{
  if (ns_switch_syncing (GTK_WIDGET (sw)))
    return;
  NMActiveConnection *ac = active_for (conn);
  if (gtk_switch_get_active (sw) && !ac)
    nm_client_activate_connection_async (client, conn, NULL, NULL, NULL, activated,
                                         g_object_ref (gtk_widget_get_ancestor (GTK_WIDGET (sw),
                                                                                GTK_TYPE_LIST_BOX_ROW)));
  else if (!gtk_switch_get_active (sw) && ac)
    nm_client_deactivate_connection_async (client, ac, NULL, NULL, NULL);
}

static void
network_fill (AdwNavigationPage *page)
{
  Network *n = g_object_get_data (G_OBJECT (page), "network");
  ns_group_clear (n->vpn);
  ns_group_clear (n->other);
  const GPtrArray *all = nm_client_get_connections (client);
  guint vpns = 0, others = 0;
  for (guint i = 0; i < all->len; i++) {
    NMConnection *conn = all->pdata[i];
    const char *type = nm_connection_get_connection_type (conn);
    gboolean vpn = !g_strcmp0 (type, NM_SETTING_VPN_SETTING_NAME) || !g_strcmp0 (type, NM_SETTING_WIREGUARD_SETTING_NAME);
    gboolean wired = !g_strcmp0 (type, NM_SETTING_WIRED_SETTING_NAME);
    if (!vpn && !wired)
      continue;
    NMActiveConnection *ac = active_for (conn);
    NMActiveConnectionState st = ac ? nm_active_connection_get_state (ac) : NM_ACTIVE_CONNECTION_STATE_UNKNOWN;
    GtkWidget *r = ns_row_toggle (vpn ? n->vpn : n->other, vpn ? "network-vpn-symbolic" : "network-wired-symbolic",
                               vpn ? "violet" : "grey", nm_connection_get_id (conn),
                               st == NM_ACTIVE_CONNECTION_STATE_ACTIVATED ? "Connected"
                               : st == NM_ACTIVE_CONNECTION_STATE_ACTIVATING ? "Connecting…"
                               : vpn ? "VPN" : "Wired");
    GtkWidget *sw = ns_row_get_switch (r);
    ns_switch_quiet (sw, ac != NULL);
    g_object_set_data_full (G_OBJECT (r), "ns-conn", g_object_ref (conn), g_object_unref);
    g_signal_connect (sw, "notify::active", G_CALLBACK (conn_switched), conn);
    vpn ? vpns++ : others++;
  }
  gtk_widget_set_visible (n->novpn, vpns == 0);
  ns_group_set_visible (n->vpn, vpns > 0);
  ns_group_set_visible (n->other, others > 0);
}

static const char *const proxy_modes[] = { "none", "auto", "manual", NULL };
static const char *const proxy_labels[] = { "Off", "Automatic (configuration URL)", "Manual", NULL };

static void
proxy_mode (GSettings *s, const char *key, GtkWidget *group)
{
  g_autofree char *m = g_settings_get_string (s, "mode");
  for (GtkWidget *r = gtk_widget_get_first_child (group); r; r = gtk_widget_get_next_sibling (r)) {
    const char *only = g_object_get_data (G_OBJECT (r), "ns-mode");
    if (only)
      gtk_widget_set_visible (r, !g_strcmp0 (only, m));
  }
  gtk_list_box_invalidate_headers (GTK_LIST_BOX (group));
}

static void
proxy_rows (AdwNavigationPage *page, GtkWidget *c)
{
  GSettings *proxy = ns_page_settings (page, "org.gnome.system.proxy");
  GSettings *http = ns_page_settings (page, "org.gnome.system.proxy.http");
  GSettings *https = ns_page_settings (page, "org.gnome.system.proxy.https");
  if (!proxy)
    return;
  GtkWidget *g = ns_group (c, "Proxy");
  ns_row_choice (g, "Proxy", proxy, "mode", proxy_modes, proxy_labels);
  GtkWidget *r = ns_row_entry (g, "Configuration URL", proxy, "autoconfig-url");
  g_object_set_data (G_OBJECT (r), "ns-mode", "auto");
  if (http) {
    r = ns_row_entry (g, "HTTP proxy", http, "host");
    g_object_set_data (G_OBJECT (r), "ns-mode", "manual");
    r = ns_row_entry (g, "HTTP port", http, "port");
    g_object_set_data (G_OBJECT (r), "ns-mode", "manual");
  }
  if (https) {
    r = ns_row_entry (g, "HTTPS proxy", https, "host");
    g_object_set_data (G_OBJECT (r), "ns-mode", "manual");
    r = ns_row_entry (g, "HTTPS port", https, "port");
    g_object_set_data (G_OBJECT (r), "ns-mode", "manual");
  }
  proxy_mode (proxy, NULL, g);
  g_signal_connect_object (proxy, "changed::mode", G_CALLBACK (proxy_mode), g, 0);
}

/* a WireGuard .conf (NetworkManager's own), or an OpenVPN .ovpn when its plugin is installed: nmcli imports them */
static gboolean
has_openvpn (void)
{
  return g_file_test ("/usr/lib/NetworkManager/VPN/nm-openvpn-service.name", G_FILE_TEST_EXISTS);
}

static void
imported (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autofree char *out = NULL, *err_text = NULL;
  g_autoptr (GError) err = NULL;
  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, &err_text, &err))
    row_error (row, "Could not import it", err);
  else if (!g_subprocess_get_successful (G_SUBPROCESS (src)))
    ns_row_set_subtitle (row, err_text && *err_text ? g_strstrip (err_text) : "Could not import it");
  else
    ns_row_set_subtitle (row, "Imported");
  g_object_unref (row);
}

static void
vpn_chosen (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, NULL);
  g_autofree char *path = file ? g_file_get_path (file) : NULL;
  g_autofree char *t = NULL;
  if (!path || !g_file_get_contents (path, &t, NULL, NULL)) {
    g_object_unref (row);
    return;
  }
  const char *type = strstr (t, "[Interface]") ? "wireguard" : has_openvpn () ? "openvpn" : NULL;
  if (!type) {
    ns_row_set_subtitle (row, "Not a WireGuard file (OpenVPN needs network-manager-openvpn)");
    g_object_unref (row);
    return;
  }
  g_autoptr (GError) err = NULL;
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                                &err, "nmcli", "connection", "import", "type", type, "file", path,
                                                NULL);
  if (!p) {
    row_error (row, "Could not import it", err);
    g_object_unref (row);
    return;
  }
  ns_row_set_subtitle (row, "Importing…");
  g_subprocess_communicate_utf8_async (p, NULL, NULL, imported, row);
}

static void
vpn_import (GtkWidget *row, gpointer data)
{
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (d, "VPN file");
  gtk_file_dialog_open (d, GTK_WINDOW (gtk_widget_get_root (row)), NULL, vpn_chosen, g_object_ref (row));
}

static void
network_connect (AdwNavigationPage *page)
{
  g_signal_connect_object (client, "notify::active-connections", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  g_signal_connect_object (client, "connection-added", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  g_signal_connect_object (client, "connection-removed", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  network_fill (page);
}

void
ns_page_network (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Network and VPN", &c);
  Network *n = g_new0 (Network, 1);
  g_object_set_data_full (G_OBJECT (page), "network", n, g_free);
  ns_later_init (page, network_fill);
  n->vpn = ns_group (c, "VPN");
  ns_group_set_visible (n->vpn, FALSE);
  n->novpn = ns_note (c, "No VPN is set up yet.");
  GtkWidget *imp = ns_group (c, NULL);
  ns_row_on_activate (ns_row (imp, "document-open-symbolic", "violet", "Import VPN file",
                              has_openvpn () ? "WireGuard (.conf) • OpenVPN (.ovpn)" : "WireGuard (.conf)"),
                      vpn_import, NULL);
  n->other = ns_group (c, "Wired and USB");
  ns_group_set_visible (n->other, FALSE);
  proxy_rows (page, c);
  with_client (page, network_connect);
  ns_push (page);
}

/* ---- Printers ---- */

/* CUPS: the printers from lpstat (the ones added, and the driverless ones it finds on the network), the
 * administrative steps through cups-pk-helper's D-Bus mechanism (polkit asks), the way GNOME Settings does them */

static void
pk_done (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  const char *msg = NULL;
  if (v)
    g_variant_get (v, "(&s)", &msg);
  if (!v)
    row_error (row, "Did not work", err);
  else if (msg && *msg)
    ns_row_set_subtitle (row, msg);
  else {
    ns_row_set_subtitle (row, "Done");
    AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-list-page");
    if (page)
      ns_later (page);
  }
  g_object_unref (row);
}

static void
pk_call (GtkWidget *row, const char *method, GVariant *args)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (!bus)
    return;
  ns_row_set_subtitle (row, "Working…");
  g_dbus_connection_call (bus, "org.opensuse.CupsPkHelper.Mechanism", "/", "org.opensuse.CupsPkHelper.Mechanism",
                          method, args, G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                          120000, NULL, pk_done, g_object_ref (row));
}

static void
run_done (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autofree char *out = NULL, *err_text = NULL;
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, &err_text, NULL) &&
      g_subprocess_get_successful (G_SUBPROCESS (src)))
    ns_row_set_subtitle (row, g_object_get_data (G_OBJECT (row), "ns-ok"));
  else
    ns_row_set_subtitle (row, err_text && *err_text ? g_strstrip (err_text) : "Did not work");
  g_object_unref (row);
}

static void
run_on_row (GtkWidget *row, const char *ok, const char *const *argv)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GSubprocess) p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                                 &err);
  if (!p) {
    row_error (row, "Did not work", err);
    return;
  }
  g_object_set_data_full (G_OBJECT (row), "ns-ok", g_strdup (ok), g_free);
  g_subprocess_communicate_utf8_async (p, NULL, NULL, run_done, g_object_ref (row));
}

static void
printer_default (GtkWidget *row, gpointer name)
{
  const char *argv[] = { "lpoptions", "-d", name, NULL };
  run_on_row (row, "This is now the default printer", argv);
}

static void
printer_test (GtkWidget *row, gpointer name)
{
  const char *page = g_file_test ("/usr/share/cups/data/testprint", G_FILE_TEST_EXISTS)
                     ? "/usr/share/cups/data/testprint" : "/etc/os-release";
  const char *argv[] = { "lp", "-d", name, page, NULL };
  run_on_row (row, "Sent to the printer", argv);
}

static void
printer_removed (gboolean yes, gpointer row)
{
  if (yes)
    pk_call (row, "PrinterDelete", g_variant_new ("(s)", (char *) g_object_get_data (G_OBJECT (row), "ns-name")));
}

static void
printer_remove (GtkWidget *row, gpointer name)
{
  ns_confirm (row, "Remove this printer?", NULL, "Remove", TRUE, printer_removed, row);
}

static void
printer_page (GtkWidget *row, gpointer data)
{
  const char *name = g_object_get_data (G_OBJECT (row), "ns-name");
  gboolean added = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "ns-added"));
  AdwNavigationPage *list = g_object_get_data (G_OBJECT (row), "ns-list-page");
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new (name, &c);
  g_object_set_data_full (G_OBJECT (page), "ns-name", g_strdup (name), g_free);
  name = g_object_get_data (G_OBJECT (page), "ns-name");
  GtkWidget *g = ns_group (c, NULL);
  ns_row_info (g, "Status", g_object_get_data (G_OBJECT (row), "ns-state"));
  g = ns_group (c, NULL);
  GtkWidget *r = ns_row_button (g, "Set as default", FALSE, printer_default, (gpointer) name);
  r = ns_row_button (g, "Print a test page", FALSE, printer_test, (gpointer) name);
  if (added) {
    r = ns_row_button (g, "Remove printer", TRUE, printer_remove, (gpointer) name);
    g_object_set_data_full (G_OBJECT (r), "ns-name", g_strdup (name), g_free);
    g_object_set_data (G_OBJECT (r), "ns-list-page", list);
  }
  ns_push (page);
}

/* lpstat's lines into a table: `lpstat -e` the names, -v the added ones, -p their state, -d the default */
static GHashTable *
lpstat (const char *flag, guint field)
{
  GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  const char *argv[] = { "lpstat", flag, NULL };
  g_autofree char *out = NULL;
  g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                NULL, NULL);
  g_auto (GStrv) lines = g_strsplit (out ? out : "", "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    g_auto (GStrv) w = g_strsplit_set (lines[i], " :", -1);
    if (g_strv_length (w) > field && *w[field])
      g_hash_table_insert (t, g_strdup (w[field]), g_strdup (lines[i]));
  }
  return t;
}

typedef struct {
  GPtrArray *names;
  GHashTable *added, *states;
  char *def;
} Printers;

static void
printers_free (Printers *p)
{
  g_ptr_array_unref (p->names);
  g_hash_table_unref (p->added);
  g_hash_table_unref (p->states);
  g_free (p->def);
  g_free (p);
}

static void
printers_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  Printers *p = g_new0 (Printers, 1);
  p->names = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GHashTable) all = lpstat ("-e", 0);
  GHashTableIter it;
  gpointer k;
  g_hash_table_iter_init (&it, all);
  while (g_hash_table_iter_next (&it, &k, NULL))
    g_ptr_array_add (p->names, g_strdup (k));
  g_ptr_array_sort (p->names, (GCompareFunc) g_strcmp0);
  p->added = lpstat ("-v", 2);
  p->states = lpstat ("-p", 1);
  g_autoptr (GHashTable) d = lpstat ("-d", 4);
  g_hash_table_iter_init (&it, d);
  if (g_hash_table_iter_next (&it, &k, NULL))
    p->def = g_strdup (k);
  g_task_return_pointer (task, p, (GDestroyNotify) printers_free);
}

static void
printers_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Printers *p = g_task_propagate_pointer (G_TASK (res), NULL);
  GtkWidget *g = g_object_get_data (G_OBJECT (page), "ns-list");
  ns_group_clear (g);
  for (guint i = 0; p && i < p->names->len; i++) {
    const char *name = p->names->pdata[i];
    const char *st = g_hash_table_lookup (p->states, name);
    const char *say = !st ? "On the network" : strstr (st, "idle") ? "Ready" : strstr (st, "printing") ? "Printing"
                    : strstr (st, "disabled") ? "Paused" : "Ready";
    g_autofree char *sub = !g_strcmp0 (name, p->def) ? g_strdup_printf ("Default • %s", say) : g_strdup (say);
    GtkWidget *r = ns_row (g, "printer-symbolic", "grey", name, sub);
    g_object_set_data_full (G_OBJECT (r), "ns-name", g_strdup (name), g_free);
    g_object_set_data_full (G_OBJECT (r), "ns-state", g_strdup (say), g_free);
    g_object_set_data (G_OBJECT (r), "ns-added", GINT_TO_POINTER (g_hash_table_contains (p->added, name)));
    g_object_set_data (G_OBJECT (r), "ns-list-page", page);
    ns_row_on_activate (r, printer_page, NULL);
  }
  guint n = p ? p->names->len : 0;
  ns_group_set_visible (g, n > 0);
  gtk_widget_set_visible (g_object_get_data (G_OBJECT (page), "ns-empty"), n == 0);
  if (p)
    printers_free (p);
  g_object_unref (page);
}

static void
printers_fill (AdwNavigationPage *page)
{
  g_autoptr (GTask) t = g_task_new (NULL, NULL, printers_got, g_object_ref (page));
  g_task_run_in_thread (t, printers_thread);
}

static void
address_entered (const char *text, gpointer row)
{
  if (!text || !*text)
    return;
  g_autofree char *uri = strstr (text, "://") ? g_strdup (text) : g_strdup_printf ("ipp://%s/ipp/print", text);
  g_autoptr (GUri) u = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
  if (!u || !g_uri_get_host (u)) {
    ns_row_set_subtitle (row, "Not a printer address");
    return;
  }
  g_autofree char *name = g_strcanon (g_strdup (g_uri_get_host (u)),
                                      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_", '_');
  /* "everywhere": CUPS asks the printer itself (IPP Everywhere, driverless), no PPD needed */
  pk_call (row, "PrinterAdd", g_variant_new ("(sssss)", name, uri, "everywhere", name, ""));
}

static void
printer_add (GtkWidget *row, gpointer data)
{
  ns_ask (row, "Add a printer", "Its address, like printer.local or ipp://192.168.1.20/ipp/print. Network printers "
          "CUPS finds by itself are listed already.", NULL, FALSE, "Add", address_entered, row);
}

void
ns_page_printers (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Printers", &c);
  GtkWidget *g = ns_group (c, NULL);
  g_object_set_data (G_OBJECT (page), "ns-list", g);
  ns_group_set_visible (g, FALSE);
  GtkWidget *empty = ns_note (c, "No printers found.");
  gtk_widget_set_visible (empty, FALSE);
  g_object_set_data (G_OBJECT (page), "ns-empty", empty);
  g = ns_group (c, NULL);
  GtkWidget *add = ns_row_button (g, "Add printer", FALSE, printer_add, NULL);
  g_object_set_data (G_OBJECT (add), "ns-list-page", page);
  ns_later_init (page, printers_fill);
  printers_fill (page);
  ns_push (page);
}
