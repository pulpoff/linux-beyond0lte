/* The home screen's widgets (One UI / the S22's Neo): the weather where the phone is, and an analog clock. */
#pragma once

#include <gtk/gtk.h>

#define NEO_WIDGET_ROWS 2      /* the strip takes the first two rows of the first page */

/* a row the width of the page (`cols` cells): the weather over all but the last, the clock in the last; a tap opens
 * the app behind each (`open` gets its desktop id) */
GtkWidget *neo_widgets_new (gboolean compact, void (*open) (const char *desktop_id, gpointer data), gpointer data);

