#ifndef YEW_UI_TABS_H
#define YEW_UI_TABS_H

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

#include "edit/cmd.h"
#include "term/input.h"
#include "ui/layout.h"
#include "ui/strip.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;
typedef struct Buffer Buffer;

enum {
    /* A directory opened as a group routinely exceeds a small cap, and
     * the failure mode of a small one is worse than the memory. */
    YEW_TAB_MAX = 512,
    YEW_TAB_LABEL_MAX = 64
};

typedef struct Tab {
    u32 tab_id;   /* monotonic, never reused; 0 is invalid */
    u32 buffer_id;
    char *path;   /* canonical realpath; NULL when untitled */
    char *display_path; /* workspace/logical spelling; never authoritative */
    Pane *root;   /* this tab's pane tree */
    Pane *focus;  /* focused leaf within root */
    /*
     * Membership.  `group_id` 0 means ungrouped and is authoritative;
     * `group_ordinal` is 1-based within the group.  See ui/groups.h for
     * why the group does not keep the other half of this.
     */
    u32 group_id;
    u32 group_ordinal;
    /*
     * There is deliberately NO `deferred` flag here.
     *
     * Sprint 23 carried one as a placeholder.  Sprint 24 deletes it:
     * residency is asked of the ALLOCATION (does this tab's buffer hold
     * a TextBuf), which is the same question every save path already
     * ends up asking.  A flag is a second answer that can disagree with
     * the first, and the disagreement is unrecoverable — a tab that
     * looks resident is never read, and the empty buffer is what gets
     * saved over the real file.
     *
     * There IS a flag for a missing file, and it is a different kind of
     * thing: "was this path absent when we restored" has no structural
     * answer and cannot be recomputed on a draw path without a stat per
     * tab per frame.  The name says when it was true so nobody mistakes
     * it for a live fact — Sprint 25 §6 checks the disk once, at
     * restore, and never again.
     */
    bool missing_at_restore;
} Tab;

VEC_DECL(TabVec, Tab);

typedef struct Tabs {
    TabVec v;         /* compacted on close: indices shift, ids never */
    u32 next_tab_id;  /* starts at 1 */
    int active;       /* INDEX into v; -1 when empty */
    int scroll;       /* first visible entry on row 1 */
    /* Row 2 scrolls independently: the member list and the row-1 list
     * have different lengths, so one shared offset would scroll a row
     * the user was not looking at. */
    int member_scroll;
} Tabs;

void yew_tabs_init(Tabs *t);
void yew_tabs_free(Ed *ed);

/* Returns the index, or -1 when refused.  The return value is NOT
 * decoration: a silent cap failure in facsimile made callers load the
 * new file into the still-active tab, and the next save wrote it over
 * the old file's path. */
int yew_tab_open(Ed *ed, const char *path);
/* False when vetoed — a modified tab needs an answer first. */
bool yew_tab_close(Ed *ed, int idx);
int yew_tab_index_of_id(const Ed *ed, u32 id);
/* Renames what a tab shows — the save-as path.  Canonicalized here, at
 * the one site that establishes a tab's name, so comparators never
 * touch the filesystem. */
void yew_tab_set_path(Ed *ed, int idx, const char *path);
int yew_tab_find_by_path(const Ed *ed, const char *path);
void yew_tab_switch(Ed *ed, int idx);
/* Insertion, not swap: dragging a tab three places right leaves the two
 * it passed in their original relative order. */
void yew_tab_reorder(Ed *ed, int from, int to);
/* Derived from the undo tree every time it is asked.  There is no
 * stored boolean: a cached flag is a label that can lie, and undoing
 * back to the clean state has to clear the marker. */
bool yew_tab_modified(const Ed *ed, int idx);
u32 yew_tab_count(const Ed *ed);
Tab *yew_tab_at(Ed *ed, int idx);
const char *yew_tab_display_path(const Tab *tab);

/*
 * Sprint 24 §3: lazy hydration.
 *
 * `is_resident` asks the allocation, never a flag.  `hydrate` runs at
 * the top of every switch-to-tab and returns immediately for a resident
 * tab, so opening a 40-file group costs ONE file read (D4) and the
 * other 39 tabs are a path and no buffer.
 */
bool yew_tab_is_resident(const Ed *ed, int tab_idx);
int yew_tab_hydrate(Ed *ed, int tab_idx); /* 0 ok; performs the read */
/*
 * Releases the text.  Refuses the ACTIVE tab: deferring what the user
 * is looking at leaves the window pointing at no text.
 *
 * NEVER copy a buffer into a non-resident tab to "prime" it.  The copy
 * allocates, which makes the tab look resident, after which the real
 * file is never read and the fabricated content is what a save writes.
 */
void yew_tab_defer(Ed *ed, int tab_idx);
/* The buffer this tab shows; NULL when the tab has no view yet. */
Buffer *yew_tab_buffer(Ed *ed, int tab_idx);

/* Where index `i` lands when `from` moves to `to`. */
int yew_tab_shifted_index(int i, int from, int to);

/*
 * The ROW-1 ENTRY LIST — the one construction of it.
 *
 * A group is one entry, placed where its FIRST member sits, with
 * payload = −gid; ungrouped tabs are themselves, with payload = index.
 * Members never appear individually on row 1.
 *
 * The renderer and the walk-through (ui/groupnav.c) both call this
 * rather than each building the list.  Two constructions of "what is on
 * row 1" drift, and the drift shows up as left/right skipping an entry
 * the user can see, or as a click landing on the wrong one.
 */
int yew_tab_row1_entries(const Ed *ed, StripEntry *out, int cap);
/* Index into that list of the entry the active tab belongs to; -1 when
 * there is none. */
int yew_tab_row1_active(const Ed *ed, const StripEntry *entries, int n);

/*
 * Sprint 27 §4: the PRE-DRAG row-1 slot table.
 *
 * Rebuilt by every row-1 render.  A slot is a visual position on the
 * strip; the payload is what occupied that position BEFORE the drag's
 * preview permutation was applied.
 *
 * THE PITFALL THIS EXISTS FOR.  While a tab is being dragged the region
 * table describes the PREVIEWED strip — the held entry has been moved
 * under the pointer — so `yew_region_hit` always answers "you are
 * hovering the thing you are holding".  The dwell-over-a-group test
 * needs the other question: what was here before I picked this up.
 * That is this table, and nothing else may answer it.
 */
int yew_strip_slot_at(u16 x, u16 y);              /* -1 when off row 1 */
bool yew_strip_pre_payload(int slot, i32 *payload);
/* Slots the last row-1 render produced. */
int yew_strip_slot_count(void);
/* The cell just past the last rendered entry — where "the blank tail"
 * begins.  The drop target that carries a tab out of a sole group has
 * nothing else to aim at. */
u16 yew_strip_tail_x(void);

/* Rows the strip needs; layout reserves them like the footer row. */
u32 yew_tab_strip_rows(const Ed *ed);
void yew_tab_strip_draw(Ed *ed, Rect rect);
/*
 * Row 2: the members of `gid`.  Also the hover-preview renderer Sprint
 * 27 calls with a group the pointer is merely over — one function, so
 * the pinned row and the preview cannot disagree about placement.
 */
void yew_tab_member_strip_draw(Ed *ed, Rect rect, u32 gid);
/* True when the click was consumed. */
bool yew_tab_strip_click(Ed *ed, u16 x, u16 y);

/*
 * The dirty-close question.  It holds the tab_ID: another event can
 * compact the array while the prompt is up, and an index captured
 * beforehand would then answer for a different file.
 */
typedef struct TabPrompt {
    u32 tab_id;
    bool active;
} TabPrompt;

/* True when the key was consumed by the prompt. */
bool yew_tab_prompt_key(Ed *ed, u8 answer);

/*
 * Sprint 24 §7: the 500 ms digit-extension window.
 *
 * JUMP IMMEDIATELY, THEN ARM.  Waiting half a second to see whether a
 * second digit is coming would put that lag on the overwhelmingly
 * common single-digit case; superseding a jump already made costs
 * nothing.
 *
 * yew_tab_jump_key returns true when it CONSUMED the key.  A digit
 * arriving inside the window is part of a chord, so an out-of-range one
 * is swallowed rather than inserted into the document — a surprise edit
 * while navigating is worse than a dropped key.
 */
enum {
    YEW_JUMP_WINDOW_MS = 500
};

bool yew_tab_jump_key(Ed *ed, Key key);
void yew_tab_jump_clear(Ed *ed);
/* Test seam: how the window currently stands. */
bool yew_tab_jump_armed(void);

CmdStatus yew_tab_cmd_new(CmdCtx *cx);
CmdStatus yew_tab_cmd_open(CmdCtx *cx);
CmdStatus yew_tab_cmd_close(CmdCtx *cx);
CmdStatus yew_tab_cmd_next(CmdCtx *cx);
CmdStatus yew_tab_cmd_prev(CmdCtx *cx);
CmdStatus yew_tab_cmd_goto(CmdCtx *cx);
CmdStatus yew_tab_cmd_move(CmdCtx *cx);
/* Sprint 27 §5: the tab context menu's rows.  Commands, not menu-only
 * handlers, so the mouse and the keyboard reach the same code. */
CmdStatus yew_tab_cmd_close_others(CmdCtx *cx);
CmdStatus yew_tab_cmd_copy_path(CmdCtx *cx);

#endif
