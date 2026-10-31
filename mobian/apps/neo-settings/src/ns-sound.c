/* Sounds and vibration. Volumes and the output device through libpulse (PipeWire's pulse server here), connected
 * once at start (ns_sound_init) and followed by its subscription; the sound mode, vibration and the LED through
 * feedbackd's settings; ringtones the way Phosh's Mobile Settings sets them: a link per event in the "__custom"
 * sound theme in ~/.local/share/sounds. */
#include "ns-pages.h"
#include "ns-sound.h"
#include "ns-ui.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <unistd.h>

static pa_context *ctx;
static gboolean ready;

typedef struct {
  GtkWidget *media, *mic, *outs;    /* two scales, the output card */
  char      *sink, *source;         /* the defaults */
  guint8     sink_ch, source_ch;
  char      *sink_list;             /* the sinks last listed, to rebuild the card only when they change */
  GPtrArray *names;
  gboolean   syncing;
} Sound;

static Sound *shown;                /* the open page's, or NULL */

static void refresh (void);

static void
subscribed (pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *data)
{
  refresh ();
}

static void
state (pa_context *c, void *data)
{
  pa_context_state_t s = pa_context_get_state (c);
  if (s == PA_CONTEXT_READY) {
    ready = TRUE;
    pa_context_set_subscribe_callback (c, subscribed, NULL);
    pa_operation *o = pa_context_subscribe (c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE
                                               | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL);
    if (o)
      pa_operation_unref (o);
    refresh ();
  } else if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) {
    ready = FALSE;
  }
}

void
ns_sound_init (void)
{
  pa_glib_mainloop *ml = pa_glib_mainloop_new (NULL);
  pa_proplist *p = pa_proplist_new ();
  pa_proplist_sets (p, PA_PROP_APPLICATION_ID, "de.yesman.NeoSettings");
  ctx = pa_context_new_with_proplist (pa_glib_mainloop_get_api (ml), "Settings", p);
  pa_proplist_free (p);
  pa_context_set_state_callback (ctx, state, NULL);
  pa_context_connect (ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
}

static void
op (pa_operation *o)
{
  if (o)
    pa_operation_unref (o);
}

static double
percent (const pa_cvolume *v, int mute)
{
  return mute ? 0 : 100.0 * pa_cvolume_avg (v) / PA_VOLUME_NORM;
}

static void
show_scale (GtkWidget *scale, double v)
{
  /* not while a finger is on it */
  if (gtk_widget_has_focus (scale) && gtk_widget_get_state_flags (scale) & GTK_STATE_FLAG_ACTIVE)
    return;
  shown->syncing = TRUE;
  gtk_range_set_value (GTK_RANGE (scale), v);
  shown->syncing = FALSE;
}

static void
sink_got (pa_context *c, const pa_sink_info *i, int eol, void *data)
{
  if (eol || !i || !shown)
    return;
  shown->sink_ch = i->volume.channels;
  show_scale (shown->media, percent (&i->volume, i->mute));
}

static void
source_got (pa_context *c, const pa_source_info *i, int eol, void *data)
{
  if (eol || !i || !shown)
    return;
  shown->source_ch = i->volume.channels;
  show_scale (shown->mic, percent (&i->volume, i->mute));
  gtk_widget_set_visible (gtk_widget_get_ancestor (shown->mic, GTK_TYPE_LIST_BOX_ROW), TRUE);
}

static void
out_picked (GtkWidget *row, guint i, gpointer data)
{
  if (shown && i < shown->names->len)
    op (pa_context_set_default_sink (ctx, shown->names->pdata[i], NULL, NULL));
}

/* the output card: a picker when there is more than one place for the sound to go */
static void
sinks_got (pa_context *c, const pa_sink_info *i, int eol, void *data)
{
  static GString *names;
  static GPtrArray *labels;
  if (!names) {
    names = g_string_new (NULL);
    labels = g_ptr_array_new_with_free_func (g_free);
  }
  if (!eol && i) {
    g_string_append_printf (names, "%s\n", i->name);
    g_ptr_array_add (labels, g_strdup (i->description ? i->description : i->name));
    return;
  }
  if (shown && g_strcmp0 (shown->sink_list, names->str)) {
    g_free (shown->sink_list);
    shown->sink_list = g_strdup (names->str);
    g_ptr_array_set_size (shown->names, 0);
    g_auto (GStrv) n = g_strsplit (names->str, "\n", -1);
    guint sel = 0;
    for (guint k = 0; n[k] && *n[k]; k++) {
      if (!g_strcmp0 (n[k], shown->sink))
        sel = k;
      g_ptr_array_add (shown->names, g_strdup (n[k]));
    }
    g_ptr_array_add (labels, NULL);
    ns_group_clear (shown->outs);
    if (shown->names->len > 1)
      ns_row_pick (shown->outs, "Play sound on", (const char *const *) labels->pdata, sel, out_picked, NULL);
    else if (labels->len > 1)
      ns_row_info (shown->outs, "Play sound on", labels->pdata[0]);
    ns_group_set_visible (shown->outs, labels->len > 1);
  }
  g_string_truncate (names, 0);
  g_ptr_array_set_size (labels, 0);
}

static void
server_got (pa_context *c, const pa_server_info *i, void *data)
{
  if (!shown || !i)
    return;
  gboolean moved = g_strcmp0 (shown->sink, i->default_sink_name) != 0;
  g_free (shown->sink);
  g_free (shown->source);
  shown->sink = g_strdup (i->default_sink_name);
  shown->source = g_strdup (i->default_source_name);
  if (moved)
    g_clear_pointer (&shown->sink_list, g_free);
  if (shown->sink)
    op (pa_context_get_sink_info_by_name (c, shown->sink, sink_got, NULL));
  if (shown->source)
    op (pa_context_get_source_info_by_name (c, shown->source, source_got, NULL));
  op (pa_context_get_sink_info_list (c, sinks_got, NULL));
}

static void
refresh (void)
{
  if (ready && shown)
    op (pa_context_get_server_info (ctx, server_got, NULL));
}

static void
volume_set (GtkRange *r, gpointer is_mic)
{
  if (!shown || shown->syncing || !ready)
    return;
  const char *name = is_mic ? shown->source : shown->sink;
  guint8 ch = is_mic ? shown->source_ch : shown->sink_ch;
  if (!name || !ch)
    return;
  double p = gtk_range_get_value (r);
  pa_cvolume v;
  pa_cvolume_set (&v, ch, (pa_volume_t) (PA_VOLUME_NORM * p / 100.0));
  if (is_mic) {
    op (pa_context_set_source_volume_by_name (ctx, name, &v, NULL, NULL));
    op (pa_context_set_source_mute_by_name (ctx, name, p < 0.5, NULL, NULL));
  } else {
    op (pa_context_set_sink_volume_by_name (ctx, name, &v, NULL, NULL));
    op (pa_context_set_sink_mute_by_name (ctx, name, p < 0.5, NULL, NULL));
  }
}

static void
sound_free (Sound *s)
{
  if (shown == s)
    shown = NULL;
  g_free (s->sink);
  g_free (s->source);
  g_free (s->sink_list);
  g_ptr_array_unref (s->names);
  g_free (s);
}

/* ---- ringtones ---- */

static const char *const events[][2] = {
  { "phone-incoming-call", "Ringtone" },
  { "message-new-sms", "Message sound" },
  { "message-new-instant", "Chat sound" },
  { "alarm-clock-elapsed", "Alarm sound" },
};

static GPtrArray *
sound_files (void)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (g_free);
  const char *dirs[] = { "/usr/share/sounds/freedesktop/stereo", "/usr/share/sounds/gnome/default/alerts", NULL };
  for (guint d = 0; dirs[d]; d++) {
    g_autoptr (GDir) dir = g_dir_open (dirs[d], 0, NULL);
    for (const char *n; dir && (n = g_dir_read_name (dir));)
      if (g_str_has_suffix (n, ".oga") || g_str_has_suffix (n, ".ogg"))
        g_ptr_array_add (files, g_build_filename (dirs[d], n, NULL));
  }
  return files;
}

static char *
sound_label (const char *path)
{
  g_autofree char *base = g_path_get_basename (path);
  char *dot = strrchr (base, '.');
  if (dot)
    *dot = 0;
  g_strdelimit (base, "-_", ' ');
  base[0] = g_ascii_toupper (base[0]);
  return g_strdup (base);
}

static char *
custom_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "sounds", "__custom", NULL);
}

static void
play (const char *file)
{
  g_autofree char *pw = g_find_program_in_path ("pw-play");
  const char *argv[] = { pw ? "pw-play" : "paplay", file, NULL };
  ns_spawn (argv);
}

static void
tone_picked (GtkWidget *row, guint i, gpointer data)
{
  const char *event = data;
  GPtrArray *files = g_object_get_data (G_OBJECT (row), "ns-files");
  g_autofree char *dir = custom_dir ();
  g_autofree char *link = g_strconcat (dir, "/", event, ".ogg", NULL);
  g_mkdir_with_parents (dir, 0700);
  g_unlink (link);        /* our own link, replaced */
  if (i == 0)
    return;            /* the theme's own sound again */
  const char *file = files->pdata[i - 1];
  if (symlink (file, link) != 0)
    g_warning ("%s: %s", link, g_strerror (errno));
  /* the custom theme inherits the one it replaced, so the sounds not set here stay */
  g_autofree char *index = g_build_filename (dir, "index.theme", NULL);
  if (!g_file_test (index, G_FILE_TEST_EXISTS)) {
    GSettings *snd = g_object_get_data (G_OBJECT (row), "ns-snd");
    g_autofree char *was = g_settings_get_string (snd, "theme-name");
    g_autofree char *t = g_strdup_printf ("[Sound Theme]\nName=Custom\nInherits=%s\nDirectories=.\n",
                                          g_strcmp0 (was, "__custom") ? was : "freedesktop");
    g_file_set_contents (index, t, -1, NULL);
  }
  g_settings_set_string (g_object_get_data (G_OBJECT (row), "ns-snd"), "theme-name", "__custom");
  play (file);
}

static void
tone_rows (AdwNavigationPage *page, GtkWidget *c, GSettings *snd)
{
  GPtrArray *files = sound_files ();
  g_object_set_data_full (G_OBJECT (page), "ns-files", files, (GDestroyNotify) g_ptr_array_unref);
  if (!files->len)
    return;
  g_autoptr (GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (labels, g_strdup ("Default"));
  for (guint i = 0; i < files->len; i++)
    g_ptr_array_add (labels, sound_label (files->pdata[i]));
  g_ptr_array_add (labels, NULL);
  g_autofree char *dir = custom_dir ();
  GtkWidget *g = ns_group (c, "Ringtones and alerts");
  for (guint e = 0; e < G_N_ELEMENTS (events); e++) {
    g_autofree char *link = g_strconcat (dir, "/", events[e][0], ".ogg", NULL);
    g_autofree char *to = g_file_read_link (link, NULL);
    guint sel = 0;
    for (guint i = 0; to && i < files->len; i++)
      if (!g_strcmp0 (to, files->pdata[i]))
        sel = i + 1;
    GtkWidget *r = ns_row_pick (g, events[e][1], (const char *const *) labels->pdata, sel, tone_picked,
                                (gpointer) events[e][0]);
    g_object_set_data (G_OBJECT (r), "ns-files", files);
    g_object_set_data (G_OBJECT (r), "ns-snd", snd);
  }
}

/* ---- the page ---- */

static const char *const profiles[] = { "full", "quiet", "silent", NULL };
static const char *const profile_labels[] = { "Sound", "Vibrate", "Mute", NULL };

static void
haptic_set (GtkRange *r, GSettings *fb)
{
  g_settings_set_double (fb, "max-haptic-strength", gtk_range_get_value (r) / 100.0);
}

void
ns_page_sound (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Sounds and vibration", &c);
  Sound *s = g_new0 (Sound, 1);
  s->names = g_ptr_array_new_with_free_func (g_free);
  g_object_set_data_full (G_OBJECT (page), "sound", s, (GDestroyNotify) sound_free);
  shown = s;

  GSettings *fb = ns_page_settings (page, "org.sigxcpu.feedbackd");
  GtkWidget *g;
  if (fb) {
    g = ns_group (c, NULL);
    ns_row_choice (g, "Sound mode", fb, "profile", profiles, profile_labels);
  }

  GSettings *snd = ns_page_settings (page, "org.gnome.desktop.sound");
  double top = snd && ns_has_key (snd, "allow-volume-above-100-percent")
               && g_settings_get_boolean (snd, "allow-volume-above-100-percent") ? 150 : 100;
  g = ns_group (c, "Volume");
  ns_row_scale (g, "Media", "audio-volume-high-symbolic", 0, top, 1, &s->media);
  g_signal_connect (s->media, "value-changed", G_CALLBACK (volume_set), NULL);
  GtkWidget *mr = ns_row_scale (g, "Microphone", "audio-input-microphone-symbolic", 0, 100, 1, &s->mic);
  gtk_widget_set_visible (mr, FALSE);
  g_signal_connect (s->mic, "value-changed", G_CALLBACK (volume_set), GINT_TO_POINTER (1));
  s->outs = ns_group (c, NULL);
  ns_group_set_visible (s->outs, FALSE);

  if (fb) {
    g = ns_group (c, "Vibration");
    if (ns_has_key (fb, "max-haptic-strength")) {
      GtkWidget *scale;
      ns_row_scale (g, "Vibration intensity", "phone-symbolic", 0, 100, 10, &scale);
      gtk_range_set_value (GTK_RANGE (scale), 100 * g_settings_get_double (fb, "max-haptic-strength"));
      g_signal_connect (scale, "value-changed", G_CALLBACK (haptic_set), fb);
    }
    g_autoptr (GDir) leds = g_dir_open ("/sys/class/leds", 0, NULL);
    if (ns_has_key (fb, "prefer-flash") && leds && g_dir_read_name (leds))
      ns_row_switch (g, "Flash for notifications", "The camera flash instead of the status light", fb,
                     "prefer-flash");
    GSettings *phosh = ns_page_settings (page, "sm.puri.phosh");
    if (ns_has_key (phosh, "quick-silent"))
      ns_row_switch (g, "Volume down silences calls", "When locked, an incoming call stops ringing", phosh,
                     "quick-silent");
  }

  if (snd) {
    tone_rows (page, c, snd);
    /* event and touch sounds: only GTK 3 and GNOME Shell play them, nothing on Phosh does; feedbackd's profile
     * (Sound mode) is what sounds here */
    g = ns_group (c, NULL);
    if (ns_has_key (snd, "allow-volume-above-100-percent"))
      ns_row_switch (g, "Louder than 100%", "Can distort the sound", snd, "allow-volume-above-100-percent");
  }
  refresh ();
  ns_push (page);
}
