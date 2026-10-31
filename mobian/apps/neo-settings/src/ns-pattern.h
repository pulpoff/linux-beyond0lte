/* "Set pattern" / "Set PIN": the new unlock pattern or PIN becomes the user's password (see ns-pattern.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <gtk/gtk.h>

/* ok: the pattern (or PIN) was confirmed and is now the user's password */
typedef void (*NsPatternDone) (gboolean ok, gpointer user_data);

void ns_pattern_dialog_present (GtkWidget *parent, gboolean pin, NsPatternDone callback, gpointer user_data);
