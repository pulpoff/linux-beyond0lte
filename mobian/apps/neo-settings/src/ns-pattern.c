/*
 * "Set pattern" / "Set PIN" (Lock screen): draw an unlock pattern, draw it again to confirm, and it becomes
 * the user's password: the dots joined in order as digits ("1478"), which the patched Phosh lock screen sends to
 * PAM when the same pattern is drawn there. As on Android the current pattern (or PIN/password) comes first,
 * checked through PAM like the lock screen does; the password is then set through AccountsService (a polkit rule
 * of ours lets the active local user do that without polkit's own text prompt, which wanted the pattern typed
 * as digits). The login keyring then gets the same password (neo-keyring-rekey), so that unlocking the lock
 * screen unlocks it too (pam_gnome_keyring) and apps find their secrets. A PIN (or a password) goes the same
 * way, typed instead of drawn.
 *
 * Taken from the Mobile Settings plugin (gnome/neo-phosh/ms-plugin-surya). libcrypt is opened at run time: the
 * phone has the library but not its header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */



#include "ns-pattern.h"

#include <adwaita.h>
#include <math.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIN_DOTS 4
#define MIN_PIN 4

/* libcrypt's crypt_gensalt and crypt, found at run time */
static char *
hash_password (const char *password)
{
  static GModule *lib;
  static char *(*gensalt) (const char *, unsigned long, const char *, int);
  static char *(*crypt_) (const char *, const char *);
  if (!lib) {
    lib = g_module_open ("libcrypt.so.1", G_MODULE_BIND_LAZY);
    if (!lib || !g_module_symbol (lib, "crypt_gensalt", (gpointer *) &gensalt)
        || !g_module_symbol (lib, "crypt", (gpointer *) &crypt_))
      return NULL;
  }
  /* yescrypt, as passwd writes it */
  const char *salt = gensalt ("$y$", 0, NULL, 0);
  const char *h = salt ? crypt_ (password, salt) : NULL;
  return h && h[0] != '*' ? g_strdup (h) : NULL;
}

typedef enum {
  STAGE_CURRENT,
  STAGE_DRAW,
  STAGE_CONFIRM,
} Stage;

typedef struct {
  AdwDialog      *dialog;
  GtkWidget      *area;
  GtkWidget      *entry;        /* the current PIN/password, when the lock is not a pattern now */
  gboolean        current_is_pattern;
  char            current[128];
  GtkWidget      *message;
  GtkWidget      *cancel;
  GtkWidget      *next;
  Stage           stage;
  gboolean        pin;          /* a PIN typed, not a pattern drawn */
  char            seq[10];
  guint           len;
  char            first[128];
  double          x, y;
  gboolean        drawing;
  gboolean        error;
  gboolean        done;
  NsPatternDone   callback;
  gpointer        user_data;
} PatternDialog;


static void
geometry (GtkWidget *w, double *x0, double *y0, double *cell)
{
  double width = gtk_widget_get_width (w), height = gtk_widget_get_height (w);
  double side = MIN (width, height);

  *cell = side / 3.0;
  *x0 = (width - side) / 2.0;
  *y0 = (height - side) / 2.0;
}


static void
dot_center (GtkWidget *w, guint dot, double *cx, double *cy)
{
  double x0, y0, cell;

  geometry (w, &x0, &y0, &cell);
  *cx = x0 + cell * (dot % 3) + cell / 2.0;
  *cy = y0 + cell * (dot / 3) + cell / 2.0;
}


static gboolean
has_dot (PatternDialog *pd, guint dot)
{
  for (guint i = 0; i < pd->len; i++) {
    if (pd->seq[i] == '1' + dot)
      return TRUE;
  }
  return FALSE;
}


static void
append_dot (PatternDialog *pd, guint dot)
{
  if (pd->len >= 9 || has_dot (pd, dot))
    return;
  pd->seq[pd->len++] = '1' + dot;
  pd->seq[pd->len] = '\0';
}


/* as on the lock screen (and Android): a dot passed over on the way joins the pattern first */
static void
hit (PatternDialog *pd, double x, double y)
{
  double x0, y0, cell;

  geometry (pd->area, &x0, &y0, &cell);
  for (guint dot = 0; dot < 9; dot++) {
    double cx, cy;

    dot_center (pd->area, dot, &cx, &cy);
    if (hypot (x - cx, y - cy) > cell * 0.3 || has_dot (pd, dot))
      continue;
    if (pd->len > 0) {
      guint last = pd->seq[pd->len - 1] - '1';
      int dr = (int)(dot / 3) - (int)(last / 3), dc = (int)(dot % 3) - (int)(last % 3);

      if (dr % 2 == 0 && dc % 2 == 0)
        append_dot (pd, last + (dr / 2) * 3 + dc / 2);
    }
    append_dot (pd, dot);
    return;
  }
}


static void
draw_func (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  PatternDialog *pd = data;
  GtkWidget *w = GTK_WIDGET (area);
  double x0, y0, cell, cx, cy;
  GdkRGBA fg;

  geometry (w, &x0, &y0, &cell);
  gtk_widget_get_color (w, &fg);
  if (pd->len > 0) {
    if (pd->error)
      cairo_set_source_rgba (cr, 0.88, 0.11, 0.14, 0.9);
    else
      cairo_set_source_rgba (cr, 0.21, 0.52, 0.89, 0.9);
    cairo_set_line_width (cr, cell * 0.05);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
    for (guint i = 0; i < pd->len; i++) {
      dot_center (w, pd->seq[i] - '1', &cx, &cy);
      if (i == 0)
        cairo_move_to (cr, cx, cy);
      else
        cairo_line_to (cr, cx, cy);
    }
    if (pd->drawing)
      cairo_line_to (cr, pd->x, pd->y);
    cairo_stroke (cr);
  }
  for (guint dot = 0; dot < 9; dot++) {
    gboolean on = has_dot (pd, dot);

    dot_center (w, dot, &cx, &cy);
    if (on && pd->error)
      cairo_set_source_rgba (cr, 0.88, 0.11, 0.14, 1.0);
    else if (on)
      cairo_set_source_rgba (cr, 0.21, 0.52, 0.89, 1.0);
    else
      cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.85);
    cairo_arc (cr, cx, cy, cell * (on ? 0.08 : 0.055), 0, 2 * G_PI);
    cairo_fill (cr);
  }
}


static void
set_stage (PatternDialog *pd, Stage stage)
{
  pd->stage = stage;
  pd->len = 0;
  pd->seq[0] = '\0';
  pd->error = FALSE;
  gboolean draw = stage == STAGE_CURRENT ? pd->current_is_pattern : !pd->pin;
  gtk_widget_set_visible (pd->area, draw);
  gtk_widget_set_visible (pd->entry, !draw);
  gtk_editable_set_text (GTK_EDITABLE (pd->entry), "");
  /* the password entry's text: digits for a new PIN */
  g_object_set (gtk_widget_get_first_child (pd->entry), "input-purpose",
                stage != STAGE_CURRENT && pd->pin ? GTK_INPUT_PURPOSE_PIN : GTK_INPUT_PURPOSE_PASSWORD, NULL);
  if (stage == STAGE_CURRENT) {
    gtk_label_set_text (GTK_LABEL (pd->message), pd->current_is_pattern ? "Draw your current pattern."
                                                                         : "Enter your current PIN or password.");
    gtk_button_set_label (GTK_BUTTON (pd->next), "Continue");
    if (!pd->current_is_pattern)
      gtk_widget_grab_focus (pd->entry);
  } else if (stage == STAGE_DRAW) {
    gtk_label_set_text (GTK_LABEL (pd->message),
                        pd->pin ? "Enter a new PIN of at least 4 digits. It becomes your password."
                                : "Draw your unlock pattern. It becomes your password: connect at least 4 dots.");
    gtk_button_set_label (GTK_BUTTON (pd->next), "Continue");
    if (pd->pin)
      gtk_widget_grab_focus (pd->entry);
  } else {
    gtk_label_set_text (GTK_LABEL (pd->message), pd->pin ? "Enter the PIN again to confirm."
                                                         : "Draw the pattern again to confirm.");
    gtk_button_set_label (GTK_BUTTON (pd->next), "Confirm");
    if (pd->pin)
      gtk_widget_grab_focus (pd->entry);
  }
  gtk_widget_set_sensitive (pd->next, FALSE);
  gtk_widget_queue_draw (pd->area);
}


static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, PatternDialog *pd)
{
  pd->len = 0;
  pd->seq[0] = '\0';
  pd->error = FALSE;
  pd->drawing = TRUE;
  pd->x = x;
  pd->y = y;
  /* the touch is the pattern's: the dialog's own drag (a bottom sheet swipes down to close) took it after the
   * first dot and the pattern ended there */
  gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_widget_set_sensitive (pd->next, FALSE);
  hit (pd, x, y);
  gtk_widget_queue_draw (pd->area);
}


static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, PatternDialog *pd)
{
  double sx, sy;

  gtk_gesture_drag_get_start_point (drag, &sx, &sy);
  pd->x = sx + dx;
  pd->y = sy + dy;
  hit (pd, pd->x, pd->y);
  gtk_widget_queue_draw (pd->area);
}


static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, PatternDialog *pd)
{
  pd->drawing = FALSE;
  if (pd->len == 0)
    return;
  if (pd->stage == STAGE_CURRENT) {
    gtk_widget_set_sensitive (pd->next, TRUE);
  } else if (pd->len < MIN_DOTS) {
    pd->error = TRUE;
    gtk_label_set_text (GTK_LABEL (pd->message), "Connect at least 4 dots. Try again.");
  } else if (pd->stage == STAGE_CONFIRM && g_strcmp0 (pd->seq, pd->first) != 0) {
    pd->error = TRUE;
    gtk_label_set_text (GTK_LABEL (pd->message), "That was a different pattern. Try again.");
  } else {
    gtk_label_set_text (GTK_LABEL (pd->message),
                        pd->stage == STAGE_DRAW ? "Pattern recorded." : "Pattern confirmed.");
    gtk_widget_set_sensitive (pd->next, TRUE);
  }
  gtk_widget_queue_draw (pd->area);
}


static void
finish (PatternDialog *pd, gboolean ok)
{
  if (pd->done)
    return;
  pd->done = TRUE;
  if (pd->callback)
    pd->callback (ok, pd->user_data);
  adw_dialog_force_close (pd->dialog);
}


/* --- setting the password through AccountsService ------------------------------------------------------------- */

/* The account password is set either way; a keyring left on the old password only means apps ask for it. */
static void
on_keyring_rekeyed (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *err_out = NULL;

  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL, &err_out, &error))
    g_warning ("neo-keyring-rekey: %s", error->message);
  else if (!g_subprocess_get_successful (G_SUBPROCESS (source)))
    g_warning ("The login keyring kept its old password: %s", err_out ? err_out : "");
  finish (pd, TRUE);
}


static void
rekey_keyring (PatternDialog *pd)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) proc = NULL;
  /* the new password, then the current one: with it the keyring's password is changed in place */
  g_autofree char *line = g_strconcat (pd->first, "\n", pd->current, "\n", NULL);

  gtk_label_set_text (GTK_LABEL (pd->message), "Moving the keyring to the new password…");
  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                           G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
                           "/usr/libexec/neo-keyring-rekey", NULL);
  if (!proc) {
    g_warning ("neo-keyring-rekey: %s", error->message);
    finish (pd, TRUE);
    return;
  }
  g_subprocess_communicate_utf8_async (proc, line, NULL, on_keyring_rekeyed, pd);
}


static void
on_password_set (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (!ret) {
    g_warning ("Setting the password failed: %s", error->message);
    gtk_label_set_text (GTK_LABEL (pd->message), "The password was not changed (not authorized?). Try again.");
    gtk_widget_set_sensitive (pd->next, TRUE);
    gtk_widget_set_sensitive (pd->cancel, TRUE);
    return;
  }
  rekey_keyring (pd);
}


static void
on_user_found (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  g_autofree char *hashed = NULL;
  const char *path;

  if (!ret) {
    g_warning ("AccountsService has no user: %s", error->message);
    gtk_label_set_text (GTK_LABEL (pd->message), "AccountsService is not available: the password was not changed.");
    gtk_widget_set_sensitive (pd->cancel, TRUE);
    return;
  }
  g_variant_get (ret, "(&o)", &path);
  hashed = hash_password (pd->first);
  if (!hashed) {
    gtk_label_set_text (GTK_LABEL (pd->message), "Could not hash the password.");
    gtk_widget_set_sensitive (pd->cancel, TRUE);
    return;
  }
  /* AccountsService's polkit policy asks for the current password (Phosh's prompt) */
  g_dbus_connection_call (G_DBUS_CONNECTION (source), "org.freedesktop.Accounts", path,
                          "org.freedesktop.Accounts.User", "SetPassword",
                          g_variant_new ("(ss)", hashed, ""), NULL,
                          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 120000, NULL,
                          on_password_set, pd);
}


/* --- the current password, checked as the lock screen checks it ---------------------------------------------- */

static int
pam_answer (int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
  struct pam_response *r = calloc (n, sizeof *r);

  if (!r)
    return PAM_BUF_ERR;
  for (int i = 0; i < n; i++) {
    if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
      r[i].resp = strdup (data);
  }
  *resp = r;
  return PAM_SUCCESS;
}


static void
check_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  struct pam_conv conv = { pam_answer, data };
  pam_handle_t *handle = NULL;
  /* the lock screen's own PAM service ("phosh"), so the same password works in both */
  int rc = pam_start ("phosh", g_get_user_name (), &conv, &handle);

  if (rc == PAM_SUCCESS)
    rc = pam_authenticate (handle, 0);
  pam_end (handle, rc);
  g_task_return_boolean (task, rc == PAM_SUCCESS);
}


static void
on_checked (GObject *source, GAsyncResult *res, gpointer data)
{
  PatternDialog *pd = data;

  adw_dialog_set_can_close (pd->dialog, TRUE);
  gtk_widget_set_sensitive (pd->cancel, TRUE);
  if (g_task_propagate_boolean (G_TASK (res), NULL)) {
    set_stage (pd, STAGE_DRAW);
    return;
  }
  memset (pd->current, 0, sizeof pd->current);
  pd->error = TRUE;
  gtk_label_set_text (GTK_LABEL (pd->message), pd->current_is_pattern ? "That is not your current pattern. Try again."
                                                                      : "That is not your current PIN or password.");
  gtk_editable_set_text (GTK_EDITABLE (pd->entry), "");
  gtk_widget_queue_draw (pd->area);
}


static void
check_current (PatternDialog *pd)
{
  g_autoptr (GTask) task = g_task_new (NULL, NULL, on_checked, pd);

  g_strlcpy (pd->current, pd->current_is_pattern ? pd->seq : gtk_editable_get_text (GTK_EDITABLE (pd->entry)),
             sizeof pd->current);
  gtk_widget_set_sensitive (pd->next, FALSE);
  gtk_widget_set_sensitive (pd->cancel, FALSE);
  gtk_label_set_text (GTK_LABEL (pd->message), "Checking…");
  /* the dialog stays until the answer is in: on_checked writes to it */
  adw_dialog_set_can_close (pd->dialog, FALSE);
  g_task_set_task_data (task, g_strdup (pd->current), g_free);
  g_task_run_in_thread (task, check_thread);
}


static void
on_entry_changed (PatternDialog *pd)
{
  const char *t = gtk_editable_get_text (GTK_EDITABLE (pd->entry));
  if (pd->stage == STAGE_CURRENT && !pd->current_is_pattern)
    gtk_widget_set_sensitive (pd->next, *t != '\0');
  else if (pd->stage != STAGE_CURRENT && pd->pin) {
    gboolean digits = strlen (t) >= MIN_PIN && strlen (t) < sizeof pd->first;
    for (const char *c = t; *c; c++)
      digits = digits && g_ascii_isdigit (*c);
    gtk_widget_set_sensitive (pd->next, digits);
  }
}


static void
on_next (PatternDialog *pd)
{
  g_autoptr (GDBusConnection) bus = NULL;

  if (pd->stage == STAGE_CURRENT) {
    if (gtk_widget_get_sensitive (pd->next))
      check_current (pd);
    return;
  }
  if (!gtk_widget_get_sensitive (pd->next))
    return;
  if (pd->stage == STAGE_DRAW) {
    g_strlcpy (pd->first, pd->pin ? gtk_editable_get_text (GTK_EDITABLE (pd->entry)) : pd->seq, sizeof pd->first);
    set_stage (pd, STAGE_CONFIRM);
    return;
  }
  if (pd->pin && g_strcmp0 (pd->first, gtk_editable_get_text (GTK_EDITABLE (pd->entry)))) {
    gtk_label_set_text (GTK_LABEL (pd->message), "That was a different PIN. Try again.");
    gtk_editable_set_text (GTK_EDITABLE (pd->entry), "");
    return;
  }
  gtk_widget_set_sensitive (pd->next, FALSE);
  gtk_widget_set_sensitive (pd->cancel, FALSE);
  gtk_label_set_text (GTK_LABEL (pd->message), "Setting the pattern as your password…");
  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (!bus) {
    gtk_label_set_text (GTK_LABEL (pd->message), "No system bus: the password was not changed.");
    gtk_widget_set_sensitive (pd->cancel, TRUE);
    return;
  }
  g_dbus_connection_call (bus, "org.freedesktop.Accounts", "/org/freedesktop/Accounts",
                          "org.freedesktop.Accounts", "FindUserById",
                          g_variant_new ("(x)", (gint64) getuid ()), G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_user_found, pd);
}


static void
on_cancel (PatternDialog *pd)
{
  finish (pd, FALSE);
}


static void
on_closed (PatternDialog *pd)
{
  if (!pd->done && pd->callback)
    pd->callback (FALSE, pd->user_data);
  memset (pd->current, 0, sizeof pd->current);
  memset (pd->first, 0, sizeof pd->first);
  g_free (pd);
}


void
ns_pattern_dialog_present (GtkWidget *parent, gboolean pin, NsPatternDone callback, gpointer user_data)
{
  PatternDialog *pd = g_new0 (PatternDialog, 1);
  GtkWidget *box, *buttons, *toolbar, *header;
  GtkGesture *drag;

  pd->callback = callback;
  pd->user_data = user_data;
  pd->pin = pin;
  {
    /* what unlocks the phone now: the dot grid for a pattern, a text field for a PIN or a password */
    GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                               "de.yesman.neo", TRUE);
    if (schema) {
      g_autoptr (GSettings) neo = g_settings_new ("de.yesman.neo");
      g_autofree char *type = g_settings_get_string (neo, "lock-type");

      pd->current_is_pattern = g_strcmp0 (type, "pattern") == 0;
      g_settings_schema_unref (schema);
    }
  }
  pd->dialog = ADW_DIALOG (adw_dialog_new ());
  /* floating, not a bottom sheet: a sheet is dragged down to close, the same stroke that draws a pattern */
  adw_dialog_set_presentation_mode (pd->dialog, ADW_DIALOG_FLOATING);
  adw_dialog_set_title (pd->dialog, pin ? "Set PIN" : "Set pattern");
  adw_dialog_set_content_width (pd->dialog, 360);
  adw_dialog_set_content_height (pd->dialog, 620);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 18);

  pd->message = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (pd->message), TRUE);
  gtk_label_set_justify (GTK_LABEL (pd->message), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (box), pd->message);

  pd->area = gtk_drawing_area_new ();
  gtk_widget_set_size_request (pd->area, 260, 260);
  gtk_widget_set_vexpand (pd->area, TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (pd->area), draw_func, pd, NULL);
  drag = gtk_gesture_drag_new ();
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), pd);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), pd);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), pd);
  gtk_widget_add_controller (pd->area, GTK_EVENT_CONTROLLER (drag));
  gtk_box_append (GTK_BOX (box), pd->area);

  pd->entry = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (pd->entry), TRUE);
  gtk_widget_set_valign (pd->entry, GTK_ALIGN_START);
  gtk_widget_set_vexpand (pd->entry, TRUE);
  g_signal_connect_swapped (pd->entry, "changed", G_CALLBACK (on_entry_changed), pd);
  g_signal_connect_swapped (pd->entry, "activate", G_CALLBACK (on_next), pd);
  gtk_box_append (GTK_BOX (box), pd->entry);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous (GTK_BOX (buttons), TRUE);
  pd->cancel = gtk_button_new_with_label ("Cancel");
  gtk_widget_add_css_class (pd->cancel, "pill");
  pd->next = gtk_button_new_with_label ("Continue");
  gtk_widget_add_css_class (pd->next, "pill");
  gtk_widget_add_css_class (pd->next, "suggested-action");
  g_signal_connect_swapped (pd->cancel, "clicked", G_CALLBACK (on_cancel), pd);
  g_signal_connect_swapped (pd->next, "clicked", G_CALLBACK (on_next), pd);
  gtk_box_append (GTK_BOX (buttons), pd->cancel);
  gtk_box_append (GTK_BOX (buttons), pd->next);
  gtk_box_append (GTK_BOX (box), buttons);

  toolbar = adw_toolbar_view_new ();
  header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
  adw_dialog_set_child (pd->dialog, toolbar);
  g_signal_connect_swapped (pd->dialog, "closed", G_CALLBACK (on_closed), pd);

  set_stage (pd, STAGE_CURRENT);
  adw_dialog_present (pd->dialog, parent);
}
