/* Neo Settings' building blocks, One UI style: pages with a large title, groups as rounded cards, rows with a
 * coloured round icon, a title and a "this • that" subtitle. */
#pragma once

#include <adwaita.h>

typedef void (*NsActivate) (GtkWidget *row, gpointer data);

/* the app's navigation: pages are pushed onto it */
void       ns_ui_init (AdwNavigationView *nav);
void       ns_push (AdwNavigationPage *page);

/* a sub page: back arrow and the title in the bar, `content` is the column to add groups to */
AdwNavigationPage *ns_page_new (const char *title, GtkWidget **content);

/* a page that is one long list (a GtkListView, which makes rows only for what is on screen): the list scrolls */
AdwNavigationPage *ns_page_new_list (const char *title, GtkWidget *list);

/* a card of rows, inset lines between them; `title` (or NULL) goes above it, small and dim */
GtkWidget *ns_group (GtkWidget *content, const char *title);
/* `icon` NULL: no icon; `color` one of blue, violet, purple, green, lime, orange, red, pink, yellow, grey */
GtkWidget *ns_row (GtkWidget *group, const char *icon, const char *color, const char *title, const char *subtitle);
void       ns_row_on_activate (GtkWidget *row, NsActivate cb, gpointer data);
void       ns_row_suffix (GtkWidget *row, GtkWidget *suffix);
void       ns_row_set_subtitle (GtkWidget *row, const char *subtitle);
GtkWidget *ns_row_switch (GtkWidget *group, const char *title, const char *subtitle, GSettings *s, const char *key);
/* a row of its own widget (a slider, a grid) under an optional title */
GtkWidget *ns_row_widget (GtkWidget *group, const char *title, GtkWidget *child);
/* an integer key as a −/+ stepper */
GtkWidget *ns_row_spin (GtkWidget *group, const char *title, GSettings *s, const char *key, int lo, int hi);
/* a string key from a fixed list: a row that shows the choice and opens a picker page */
GtkWidget *ns_row_choice (GtkWidget *group, const char *title, GSettings *s, const char *key,
                          const char *const *values, const char *const *labels);

/* a switch row with an icon, for a switch the caller drives (ns_switch_quiet sets it without its handler acting) */
GtkWidget *ns_row_toggle (GtkWidget *group, const char *icon, const char *color, const char *title,
                          const char *subtitle);
void       ns_switch_quiet (GtkWidget *sw, gboolean on);
gboolean   ns_switch_syncing (GtkWidget *sw);
/* the row's switch (of ns_row_switch, ns_row_toggle) */
GtkWidget *ns_row_get_switch (GtkWidget *row);
/* one flag of a flags key (or one member of a string list) as a switch */
GtkWidget *ns_row_flag (GtkWidget *group, const char *title, const char *subtitle, GSettings *s, const char *key,
                        const char *flag);
/* a row with an app's own icon in place of the round one */
GtkWidget *ns_row_app (GtkWidget *group, GAppInfo *app, const char *subtitle);
/* a value: the title with the value under it, not tappable */
GtkWidget *ns_row_info (GtkWidget *group, const char *title, const char *value);
/* an action in a card: the title alone, in the accent colour (red when `destructive`) */
GtkWidget *ns_row_button (GtkWidget *group, const char *title, gboolean destructive, NsActivate cb, gpointer data);
/* a slider from `lo` to `hi` with an icon in front; the GtkScale is returned in `scale` */
GtkWidget *ns_row_scale (GtkWidget *group, const char *title, const char *icon, double lo, double hi, double step,
                         GtkWidget **scale);
/* a choice by position: a row that shows labels[selected] and opens a picker page (the labels are copied);
 * `picked` gets the position */
typedef void (*NsPicked) (GtkWidget *row, guint index, gpointer data);
GtkWidget *ns_row_pick (GtkWidget *group, const char *title, const char *const *labels, guint selected,
                        NsPicked picked, gpointer data);
void       ns_row_pick_select (GtkWidget *row, guint selected);
/* a string or integer key, typed in a dialog */
GtkWidget *ns_row_entry (GtkWidget *group, const char *title, GSettings *s, const char *key);

typedef void (*NsEntered) (const char *text, gpointer data);
/* a form field the caller keeps: shows the value ("Not set", dots when `secret`), a tap asks for a new one and
 * `changed` (or NULL) gets it */
GtkWidget  *ns_row_value (GtkWidget *group, const char *title, const char *value, gboolean secret,
                          NsEntered changed, gpointer data);
const char *ns_row_value_get (GtkWidget *row);
void        ns_row_value_set (GtkWidget *row, const char *value);

/* a dim line of text below the cards so far */
GtkWidget *ns_note (GtkWidget *content, const char *text);
/* removes a card's rows (a list that is rebuilt); shows or hides a card with its title */
void       ns_group_clear (GtkWidget *group);
void       ns_group_set_visible (GtkWidget *group, gboolean visible);

/* dialogs over `parent`'s window. ns_ask: a line of text, `done` gets it, or NULL when cancelled */
void       ns_ask (GtkWidget *parent, const char *heading, const char *body, const char *initial, gboolean secret,
                   const char *ok, NsEntered done, gpointer data);
typedef void (*NsAnswered) (gboolean yes, gpointer data);
AdwDialog *ns_confirm (GtkWidget *parent, const char *heading, const char *body, const char *yes,
                       gboolean destructive, NsAnswered done, gpointer data);

/* a page whose lists follow a service: ns_later (page) fills it again 250 ms after the last call */
typedef void (*NsFill) (AdwNavigationPage *page);
void       ns_later_init (AdwNavigationPage *page, NsFill fill);
void       ns_later (AdwNavigationPage *page);

void       ns_spawn (const char *const *argv);
/* a settings schema if it is installed, else NULL */
GSettings *ns_settings (const char *schema);
gboolean   ns_settings_exists (const char *schema);
/* the same, owned by the page (freed with it) */
GSettings *ns_page_settings (AdwNavigationPage *page, const char *schema);
/* whether the installed schema has `key` (an older one may not) */
gboolean   ns_has_key (GSettings *s, const char *key);
/* an app id as GSettings paths take it (GNOME Settings', feedbackd's and phoc's way): no .desktop, lower case,
 * anything but letters, digits and '-' made '-' */
char      *ns_munge_app_id (const char *id);
/* a texture of a pixbuf (gdk_texture_new_for_pixbuf is deprecated); any thread */
GdkTexture *ns_texture_for_pixbuf (GdkPixbuf *pb);
/* the first line a program prints, or NULL */
char      *ns_run_line (const char *const *argv);
