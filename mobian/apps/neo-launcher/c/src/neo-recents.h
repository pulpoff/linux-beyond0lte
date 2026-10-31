/* The task view (One UI's recents), driven by Neo's home bar with the windows Phosh lists over de.yesman.PhoshNeo:
 * while the finger pulls up from the bottom edge the current app shrinks with it; when it rests the app settles into
 * its card and the other apps' cards come in beside it; on release it settles as the task view, and without the rest
 * it fades to home. Cards scroll sideways, a
 * tap switches to the app, a swipe up closes it, the pill closes them all, a tap beside the cards goes home. */
#pragma once

#include <gtk/gtk.h>

typedef struct _NeoHome NeoHome;
typedef struct _NeoRecents NeoRecents;

NeoRecents *neo_recents_new     (GtkApplication *app, NeoHome *home);
/* the home bar's swipe: begin, then hold (the finger rests), then release (to the task view after a hold) */
void        neo_recents_begin   (NeoRecents *self);
void        neo_recents_track   (NeoRecents *self, double up);    /* px the finger is up from where it began */
void        neo_recents_hold    (NeoRecents *self);
void        neo_recents_release (NeoRecents *self, gboolean to_recents);
void        neo_recents_hide    (NeoRecents *self);
gboolean    neo_recents_active  (NeoRecents *self);
