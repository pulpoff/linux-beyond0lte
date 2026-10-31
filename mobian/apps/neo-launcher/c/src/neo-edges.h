/* Neo's system gestures on the screen edges: the home bar (a 15 px strip over Phosh's bar: swipe up = home, swipe
 * up and rest = the task view) and back (a pull in from the left or right edge). */
#pragma once

#include <gtk/gtk.h>

typedef struct _NeoHome NeoHome;
typedef struct _NeoRecents NeoRecents;
typedef struct _NeoEdges NeoEdges;

NeoEdges *neo_edges_new (GtkApplication *app, NeoHome *home, NeoRecents *recents);
