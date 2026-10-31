/* BlueZ's object manager, made in the background at start (see ns-bt.c) */
#pragma once

#include <gtk/gtk.h>

void       ns_bt_init (void);
/* "Phone name": hostnamed's pretty name, renamed in a dialog */
GtkWidget *ns_bt_name_row (GtkWidget *group);
