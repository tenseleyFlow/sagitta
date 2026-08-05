#ifndef SAG_UI_TABS_H
#define SAG_UI_TABS_H

/*
 * Sprint 23 §1: the tab model.
 *
 * THE DISCIPLINE THIS FILE EXISTS FOR: closing a tab compacts the array
 * and renumbers every index above it, so a saved INDEX silently comes
 * to mean a different tab.  In facsimile that let a closed pane's text
 * be written over its sibling's file — the index was still valid, it
 * just meant something else now.
 *
 * So anything that must refer to "this tab" across a mutation holds the
 * tab_id, never the position.  Ids are monotonic and never reused, so a
 * stale one resolves to nothing rather than to whatever took the slot.
 */

#include "ui/layout.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;

enum {
    /* A directory opened as a group routinely exceeds a small cap, and
     * the failure mode of a small one is worse than the memory. */
    SAG_TAB_MAX = 512,
    SAG_TAB_LABEL_MAX = 64
};

typedef struct Tab {
    u32 tab_id;   /* monotonic, never reused; 0 is invalid */
    u32 buffer_id;
    char *path;   /* canonical realpath; NULL when untitled */
    Pane *root;   /* this tab's pane tree */
    Pane *focus;  /* focused leaf within root */
    /* Sprint 24 animates these; 0 means ungrouped until then. */
    u32 group_id;
    u32 group_ordinal;
    /*
     * Placeholder so Sprint 25's schema work can proceed in parallel.
     * Real lazy hydration is Sprint 24; until then this is always false
     * and asserted so.
     */
    bool deferred;
} Tab;

VEC_DECL(TabVec, Tab);

typedef struct Tabs {
    TabVec v;         /* compacted on close: indices shift, ids never */
    u32 next_tab_id;  /* starts at 1 */
    int active;       /* INDEX into v; -1 when empty */
    int scroll;       /* first visible entry in the strip */
} Tabs;

void sag_tabs_init(Tabs *t);
void sag_tabs_free(Ed *ed);

/* Returns the index, or -1 when refused.  The return value is NOT
 * decoration: a silent cap failure in facsimile made callers load the
 * new file into the still-active tab, and the next save wrote it over
 * the old file's path. */
int sag_tab_open(Ed *ed, const char *path);
/* False when vetoed — a modified tab needs an answer first. */
bool sag_tab_close(Ed *ed, int idx);
int sag_tab_index_of_id(const Ed *ed, u32 id);
int sag_tab_find_by_path(const Ed *ed, const char *path);
void sag_tab_switch(Ed *ed, int idx);
/* Insertion, not swap: dragging a tab three places right leaves the two
 * it passed in their original relative order. */
void sag_tab_reorder(Ed *ed, int from, int to);
/* Derived from the undo tree every time it is asked.  There is no
 * stored boolean: a cached flag is a label that can lie, and undoing
 * back to the clean state has to clear the marker. */
bool sag_tab_modified(const Ed *ed, int idx);
u32 sag_tab_count(const Ed *ed);
Tab *sag_tab_at(Ed *ed, int idx);

/* Where index `i` lands when `from` moves to `to`. */
int sag_tab_shifted_index(int i, int from, int to);

/* Rows the strip needs; layout reserves them like the footer row. */
u32 sag_tab_strip_rows(const Ed *ed);
void sag_tab_strip_draw(Ed *ed, Rect rect);
/* True when the click was consumed. */
bool sag_tab_strip_click(Ed *ed, u16 x, u16 y);

#endif
