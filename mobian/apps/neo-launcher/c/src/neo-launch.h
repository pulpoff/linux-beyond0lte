/* Starting apps from Neo, and what Phosh tells Neo about windows (de.yesman.PhoshNeo). */
#pragma once

#include <gtk/gtk.h>

typedef struct _NeoHome NeoHome;
typedef struct _NeoLaunch NeoLaunch;

#define PHOSH_NEO_BUS  "de.yesman.PhoshNeo"
#define PHOSH_NEO_PATH "/de/yesman/PhoshNeo"

NeoLaunch *neo_launch_new    (NeoHome *home);
void       neo_launch_app    (NeoLaunch *self, GAppInfo *info, GtkWidget *from);   /* from: the tapped cell */
void       neo_launch_run    (NeoLaunch *self, const char * const *argv);   /* a helper program; Neo steps aside */
void       neo_launch_finish (NeoLaunch *self);     /* a launch in progress ends now */
void       neo_phosh_call    (const char *method, GVariant *params);
