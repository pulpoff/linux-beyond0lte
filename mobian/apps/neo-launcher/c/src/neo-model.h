/* What sits where on the home screen: the dock and the pages, kept in ~/.config/neo-launcher/layout.json. */
#pragma once

#include <glib.h>

/* one icon's place: dock >= 0 for a dock slot, else page/index on the home pages */
typedef struct {
  int dock;
  int page;
  int index;
} NeoPos;

#define NEO_POS_NONE ((NeoPos) { -1, -1, -1 })

typedef struct {
  int        cols, rows, dock_size;
  int        reserved;    /* slots at the top of the first page that widgets take */
  GPtrArray *dock;        /* char* desktop ids */
  GPtrArray *pages;       /* GPtrArray* of char* desktop ids */
  GHashTable *hidden;     /* ids taken off the home screen (still in the drawer) */
} NeoModel;

NeoModel *neo_model_new        (int cols, int rows, int dock_size);
void      neo_model_free       (NeoModel *self);
int       neo_model_per_page   (NeoModel *self);
int       neo_model_capacity   (NeoModel *self, int page);   /* the first page less the widgets' slots */
void      neo_model_set_reserved (NeoModel *self, int slots);
void      neo_model_set_hidden (NeoModel *self, char **hidden);
void      neo_model_load       (NeoModel *self, GHashTable *apps);
gboolean  neo_model_reconcile  (NeoModel *self, GHashTable *apps);
gboolean  neo_model_on_home    (NeoModel *self, const char *id);
gboolean  neo_model_remove_one (NeoModel *self, const char *id, NeoPos from);
void      neo_model_add_shortcut (NeoModel *self, const char *id);
void      neo_model_move       (NeoModel *self, const char *id, NeoPos target, NeoPos from, gboolean copy);
void      neo_model_save       (NeoModel *self);
