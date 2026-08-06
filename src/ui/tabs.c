/*
 * Sprint 23 §1/§2.  See tabs.h for the stable-id discipline.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ui/tabs.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/groupnav.h"
#include "ui/glyphs.h"
#include "ui/groups.h"
#include "ui/mouse.h"
#include "ui/message.h"
#include "ui/region.h"
#include "ui/strip.h"
#include "util/log.h"

void sag_tabs_init(Tabs *t)
{
    if (t == NULL)
        return;
    (void)memset(t, 0, sizeof(*t));
    t->next_tab_id = 1U; /* 0 is the invalid id */
    t->active = -1;
}

static void tab_destroy(Ed *ed, Tab *t)
{
    if (t == NULL)
        return;
    /*
     * The pane tree owns Wins, which the workspace also tracks; release
     * each leaf's view before freeing the nodes so neither side is left
     * holding a pointer the other freed.
     */
    if (t->root != NULL) {
        Pane *leaves[SAG_PANE_MAX_LEAVES * 2];
        u32 n = 0U;
        u32 i;

        sag_pane_collect_leaves(t->root, leaves,
                                SAG_ARRAY_LEN(leaves), &n);
        for (i = 0U; i < n; i++)
            sag_ed_win_release(ed, leaves[i]->win);
        sag_pane_free(ed, t->root);
    }
    free(t->path);
    (void)memset(t, 0, sizeof(*t));
}

void sag_tabs_free(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < ed->tabs.v.len; i++)
        tab_destroy(ed, &ed->tabs.v.data[i]);
    TabVec_free(&ed->tabs.v);
    sag_tabs_init(&ed->tabs);
}

u32 sag_tab_count(const Ed *ed)
{
    return ed == NULL ? 0U : (u32)ed->tabs.v.len;
}

Tab *sag_tab_at(Ed *ed, int idx)
{
    if (ed == NULL || idx < 0 || (size_t)idx >= ed->tabs.v.len)
        return NULL;
    return &ed->tabs.v.data[idx];
}

int sag_tab_index_of_id(const Ed *ed, u32 id)
{
    size_t i;

    if (ed == NULL || id == 0U)
        return -1;
    for (i = 0U; i < ed->tabs.v.len; i++) {
        if (ed->tabs.v.data[i].tab_id == id)
            return (int)i;
    }
    return -1; /* gone, rather than "whatever is there now" */
}

/*
 * Canonicalized at the ONE site where a tab's name is established, so
 * comparators — which sit on hot paths and get called per keystroke —
 * never have to touch the filesystem.
 */
static char *canonical_path(const char *path)
{
    char *resolved;
    char *out;
    size_t n;

    if (path == NULL || path[0] == '\0')
        return NULL;
    resolved = realpath(path, NULL);
    if (resolved != NULL)
        return resolved; /* realpath allocates; the Tab owns it */
    /* A path that does not exist yet is still a legitimate tab name;
     * keep what the user typed rather than refusing. */
    n = strlen(path) + 1U;
    out = sag_xmalloc(n);
    (void)memcpy(out, path, n);
    return out;
}

int sag_tab_find_by_path(const Ed *ed, const char *path)
{
    char *want;
    size_t i;
    int found = -1;

    if (ed == NULL || path == NULL)
        return -1;
    want = canonical_path(path);
    if (want == NULL)
        return -1;
    for (i = 0U; i < ed->tabs.v.len; i++) {
        const Tab *t = &ed->tabs.v.data[i];

        if (t->path != NULL && strcmp(t->path, want) == 0) {
            found = (int)i;
            break;
        }
    }
    free(want);
    return found;
}

void sag_tab_set_path(Ed *ed, int idx, const char *path)
{
    Tab *t = sag_tab_at(ed, idx);

    if (t == NULL)
        return;
    free(t->path);
    t->path = canonical_path(path);
    sag_state_mark_dirty(ed);
}

int sag_tab_open(Ed *ed, const char *path)
{
    Tab t;
    int existing;
    Win *win;
    Buffer *buf;

    if (ed == NULL)
        return -1;
    if (path != NULL) {
        /* One tab per file.  The same buffer in two PANES is fine; in
         * two tabs it is two claims on one save path. */
        existing = sag_tab_find_by_path(ed, path);
        if (existing >= 0) {
            sag_tab_switch(ed, existing);
            return existing;
        }
    }
    if (ed->tabs.v.len >= (size_t)SAG_TAB_MAX) {
        sag_msg(ed, SAG_MSG_ERROR, "too many tabs (max %d)", SAG_TAB_MAX);
        return -1;
    }
    win = sag_ed_win_clone(ed, ed->win);
    if (win == NULL) {
        sag_msg(ed, SAG_MSG_ERROR, "no room for another view");
        return -1;
    }
    (void)memset(&t, 0, sizeof(t));
    t.tab_id = ed->tabs.next_tab_id++;
    t.path = canonical_path(path);
    /*
     * The new tab gets its OWN buffer, and a file one is born
     * NON-RESIDENT: opening it costs no read at all.  The read happens
     * at the first switch, through sag_tab_hydrate.
     *
     * Sprint 23 pointed every tab at the window it cloned from, so all
     * tabs showed one buffer; that is why the save path could say
     * &ed->buffer and be right.  Both halves changed together.
     */
    buf = sag_ws_file_buf(ed, t.path);
    if (buf == NULL) {
        sag_ed_win_release(ed, win);
        free(t.path);
        sag_msg(ed, SAG_MSG_ERROR, "no room for another buffer");
        return -1;
    }
    sag_ed_win_set_buffer(ed, win, buf);
    t.root = sag_pane_new_leaf(win);
    t.focus = t.root;
    t.buffer_id = buf->id;
    TabVec_push(&ed->tabs.v, t);
    sag_state_mark_dirty(ed);
    return (int)ed->tabs.v.len - 1;
}

/*
 * Which tab should be active once `idx` is gone — decided by ID, and
 * decided BEFORE the compaction, because after the memmove the index
 * that names the neighbour has already changed meaning.
 */
static u32 pick_survivor_id(const Ed *ed, int idx)
{
    int active = ed->tabs.active;
    int n = (int)ed->tabs.v.len;

    if (active != idx)
        return active >= 0 && active < n
               ? ed->tabs.v.data[active].tab_id
               : 0U;
    if (idx + 1 < n)
        return ed->tabs.v.data[idx + 1].tab_id; /* the one to its right */
    if (idx - 1 >= 0)
        return ed->tabs.v.data[idx - 1].tab_id; /* else to its left */
    return 0U;
}

bool sag_tab_close(Ed *ed, int idx)
{
    u32 survivor;

    if (ed == NULL || idx < 0 || (size_t)idx >= ed->tabs.v.len)
        return false;
    survivor = pick_survivor_id(ed, idx);
    /*
     * Leave the group BEFORE the compaction, while `idx` still names
     * this tab.  Doing it after would compact the ordinals of whichever
     * tab slid into the slot, and auto-dissolve would count a group
     * that still has members.
     */
    sag_group_remove_member(ed, idx);
    tab_destroy(ed, &ed->tabs.v.data[idx]);
    (void)memmove(&ed->tabs.v.data[idx], &ed->tabs.v.data[idx + 1],
                  (ed->tabs.v.len - (size_t)idx - 1U) *
                      sizeof(*ed->tabs.v.data));
    ed->tabs.v.len--;
    /* Resolved AFTER compaction, from the id chosen before it. */
    ed->tabs.active = sag_tab_index_of_id(ed, survivor);
    if (ed->tabs.active < 0 && ed->tabs.v.len > 0U)
        ed->tabs.active = 0;
    sag_tab_switch(ed, ed->tabs.active);
    sag_state_mark_dirty(ed);
    return true;
}

void sag_tab_switch(Ed *ed, int idx)
{
    Tab *t;

    if (ed == NULL)
        return;
    t = sag_tab_at(ed, idx);
    if (t == NULL) {
        ed->tabs.active = ed->tabs.v.len == 0U ? -1 : ed->tabs.active;
        return;
    }
    ed->tabs.active = idx;
    /*
     * Hydrate FIRST.  Every route to a different tab comes through
     * here, so this is the one place the read can be guaranteed to
     * happen before anything tries to draw the text (§3).
     */
    (void)sag_tab_hydrate(ed, idx);
    /*
     * Swapping the visible tree is the whole switch.  Each Win owns its
     * cursors and viewport, so there is no save/restore choreography —
     * the state lives where it is used rather than in a global the
     * switch has to remember to sync.
     */
    ed->pane_root = t->root;
    ed->focus = t->focus;
    if (ed->focus != NULL && ed->focus->win != NULL)
        ed->win = ed->focus->win;
    ed->layout_dirty = true;
    ed->full_damage = true;
    sag_state_mark_dirty(ed);
}

int sag_tab_shifted_index(int i, int from, int to)
{
    if (i == from)
        return to;
    if (to > from)
        return (i > from && i <= to) ? i - 1 : i;
    return (i >= to && i < from) ? i + 1 : i;
}

void sag_tab_reorder(Ed *ed, int from, int to)
{
    Tab moved;
    int n;

    if (ed == NULL)
        return;
    n = (int)ed->tabs.v.len;
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    moved = ed->tabs.v.data[from];
    if (to > from) {
        (void)memmove(&ed->tabs.v.data[from], &ed->tabs.v.data[from + 1],
                      (size_t)(to - from) * sizeof(moved));
    } else {
        (void)memmove(&ed->tabs.v.data[to + 1], &ed->tabs.v.data[to],
                      (size_t)(from - to) * sizeof(moved));
    }
    ed->tabs.v.data[to] = moved;
    /* Active is a POSITION, so it moves with the shift rather than
     * staying on a number that now names someone else. */
    if (ed->tabs.active >= 0)
        ed->tabs.active = sag_tab_shifted_index(ed->tabs.active, from, to);
    sag_state_mark_dirty(ed);
}

bool sag_tab_modified(const Ed *ed, int idx)
{
    const Tab *t;
    const Win *w;

    if (ed == NULL || idx < 0 || (size_t)idx >= ed->tabs.v.len)
        return false;
    t = &ed->tabs.v.data[idx];
    if (t->focus == NULL)
        return false;
    w = t->focus->win;
    /*
     * Asked, never remembered.  DoD 4 greps this file for a stored
     * boolean and must find none: undo back to the clean state has to
     * clear the marker, and every historical bug here is a flag that
     * drifted from the thing it claimed to describe.
     */
    return w != NULL && sag_buf_dirty(w->buf);
}

/* ---------------------------------------------------------------- */
/* Sprint 24 §3: lazy hydration                                     */
/* ---------------------------------------------------------------- */

Buffer *sag_tab_buffer(Ed *ed, int idx)
{
    Tab *t = sag_tab_at(ed, idx);

    if (t == NULL || t->focus == NULL)
        return NULL;
    return t->focus->win == NULL ? NULL : t->focus->win->buf;
}

bool sag_tab_is_resident(const Ed *ed, int idx)
{
    /*
     * The question is asked of the ALLOCATION — does this tab's buffer
     * hold a TextBuf — never of a flag.  A flag drifts; a pointer
     * cannot disagree with itself, and every save path ends up asking
     * this same question anyway.
     */
    return sag_buf_resident(sag_tab_buffer((Ed *)ed, idx));
}

int sag_tab_hydrate(Ed *ed, int idx)
{
    Buffer *b = sag_tab_buffer(ed, idx);
    Tab *t = sag_tab_at(ed, idx);

    if (b == NULL || t == NULL)
        return -1;
    if (b->tb != NULL)
        return 0; /* resident: returns without touching the disk */
    if (sag_buf_hydrate(ed, b) != 0) {
        sag_msg(ed, SAG_MSG_ERROR, "could not read %s",
                t->path != NULL ? t->path : "untitled");
        return -1;
    }
    /*
     * The view was built against no text; give it a fresh one now that
     * there is some — but the SCROLL POSITION is not part of "fresh".
     *
     * Sprint 25 §6 restores top/left into a deferred tab's window, and
     * this runs afterwards, on the switch that hydrates it.  Letting
     * vp_init zero them put every resumed tab back at line 0, which
     * looks exactly like a working restore until you notice you are
     * never where you left off.
     */
    if (t->focus != NULL && t->focus->win != NULL) {
        Win *w = t->focus->win;
        LineNo top = w->vp.top;
        u32 top_sub = w->vp.top_sub;
        CCol left = w->vp.left;
        bool wrap = w->vp.wrap;

        sag_vp_init(w);
        w->vp.top = top;
        w->vp.top_sub = top_sub;
        w->vp.left = left;
        w->vp.wrap = wrap;
        /* Restore step 9, deferred to here: clamp, never follow.  The
         * file may have shrunk since the state was written. */
        sag_vp_clamp(w);
    }
    /*
     * Sprint 25 §6: cursors restored into a deferred tab were never
     * checked against a buffer, because there was none.  This is the
     * first moment there is one — and the offsets came out of a file
     * that may have been rewritten since, so an unchecked cursor can
     * sit past the end, which every motion and every edit derives from.
     */
    {
        Pane *leaves[SAG_PANE_MAX_LEAVES * 2];
        u32 n = 0U;
        u32 i;

        if (t->root != NULL) {
            sag_pane_collect_leaves(t->root, leaves, SAG_ARRAY_LEN(leaves),
                                    &n);
        }
        for (i = 0U; i < n; i++) {
            if (leaves[i]->win != NULL && leaves[i]->win->buf == b)
                sag_cset_normalize(b->tb, &leaves[i]->win->cs);
        }
    }
    return 0;
}

void sag_tab_defer(Ed *ed, int idx)
{
    Buffer *b = sag_tab_buffer(ed, idx);

    if (ed == NULL || b == NULL)
        return;
    /* Never the tab being looked at: the window would be left pointing
     * at no text with a cursor in it. */
    if (idx == ed->tabs.active)
        return;
    sag_buf_defer(ed, b);
}

/* ---------------------------------------------------------------- */
/* Sprint 23 §3: the tab strip                                      */
/* ---------------------------------------------------------------- */

static const char *tab_basename(const Tab *t)
{
    const char *slash;

    if (t->path == NULL)
        return "untitled";
    slash = strrchr(t->path, '/');
    return slash != NULL && slash[1] != '\0' ? slash + 1 : t->path;
}

/*
 * `[N: name*]` — N is the 1-based index the user types for goto, and
 * the `*` is asked for, never remembered (§4).
 */
static void tab_label(const Ed *ed, int idx, char *out, size_t cap)
{
    (void)snprintf(out, cap, "[%d: %s%s]", idx + 1,
                   tab_basename(&ed->tabs.v.data[idx]),
                   sag_tab_modified(ed, idx)
                       ? sag_glyph(SAG_GLYPH_MODIFIED) : "");
}

/*
 * Orphans — files outside the workspace root — render dim.  Both sides
 * are already canonical (the tab's path from sag_tab_open, the root
 * from the workspace), so this is a prefix test rather than a
 * filesystem call on a draw path.  Sprint 25 refines "outside".
 */
static bool tab_is_orphan(const Ed *ed, int idx)
{
    const char *root = sag_ws_root(ed);
    const char *p = ed->tabs.v.data[idx].path;

    /* Sprint 25 §6 refines "outside" as promised: a file that was gone
     * when we restored is dim for the same reason, and for a reason the
     * user cares about more. */
    if (ed->tabs.v.data[idx].missing_at_restore)
        return true;
    return p != NULL && root != NULL &&
           strncmp(p, root, strlen(root)) != 0;
}

int sag_tab_row1_entries(const Ed *ed, StripEntry *out, int cap)
{
    u32 seen[SAG_TAB_MAX];
    int nseen = 0;
    int n = 0;
    size_t i;

    if (ed == NULL || out == NULL || cap <= 0)
        return 0;
    for (i = 0U; i < ed->tabs.v.len && n < cap; i++) {
        const Tab *t = &ed->tabs.v.data[i];

        (void)memset(&out[n], 0, sizeof(out[n]));
        if (t->group_id != 0U) {
            /* Two cells shorter than the entry it goes into, so the
             * brackets always fit. */
            char label[SAG_TAB_LABEL_MAX - 4];
            bool dup = false;
            int k;

            for (k = 0; k < nseen; k++) {
                if (seen[k] == t->group_id) {
                    dup = true;
                    break;
                }
            }
            /* Members are row 2's business; the group has had its one
             * entry already. */
            if (dup)
                continue;
            if (nseen < (int)SAG_ARRAY_LEN(seen))
                seen[nseen++] = t->group_id;
            sag_group_label(ed, t->group_id, label, sizeof(label));
            (void)snprintf(out[n].label, sizeof(out[n].label), "[%s]",
                           label);
            /*
             * NEGATIVE payload.  The sign is how the click router tells
             * a group from a tab without a second region kind — the
             * convention was written into region.h in Sprint 22 so the
             * two ends could not invent it separately.
             */
            out[n].payload = -(i32)t->group_id;
            n++;
            continue;
        }
        tab_label(ed, (int)i, out[n].label, sizeof(out[n].label));
        out[n].payload = (i32)i;
        out[n].dim = tab_is_orphan(ed, (int)i);
        n++;
    }
    return n;
}

int sag_tab_row1_active(const Ed *ed, const StripEntry *entries, int n)
{
    u32 gid;
    int i;

    if (ed == NULL || entries == NULL || ed->tabs.active < 0)
        return -1;
    gid = sag_active_group_id(ed);
    for (i = 0; i < n; i++) {
        if (gid != 0U) {
            if (entries[i].payload == -(i32)gid)
                return i;
        } else if (entries[i].payload == ed->tabs.active) {
            return i;
        }
    }
    return -1;
}

u32 sag_tab_strip_rows(const Ed *ed)
{
    StripEntry entries[SAG_TAB_MAX];
    int n;
    u32 rows = 0U;

    /*
     * A parameter, not a renderer: Sprint 22's layout reserves whatever
     * this returns before handing the rest to the pane tree, exactly as
     * it reserves the footer row.
     */
    if (ed == NULL)
        return 0U;
    n = sag_tab_row1_entries(ed, entries, (int)SAG_ARRAY_LEN(entries));
    /* One lone entry needs no strip to choose between. */
    if (n > 1)
        rows = 1U;
    /*
     * Inside a group the bar is two rows: row 1 keeps the group's own
     * entry visible so leaving is one press away, and row 2 lists the
     * members.  A member strip floating with no group above it reads as
     * a different widget every time you enter.
     */
    if (sag_active_group_id(ed) != 0U)
        rows = 2U;
    /* Sprint 27 §4: a dwell-opened preview needs the row too, or the
     * drop target it is offering has nowhere to be drawn. */
    if (sag_mouse_preview_group(ed) != 0U)
        rows = 2U;
    return rows;
}

/*
 * THE strip renderer.  Row 1 and row 2 both go through here.
 *
 * That is a law, not tidiness: facsimile drew the member row with the
 * same layout engine specifically so the two rows could not disagree
 * about where a click landed.  Placement split out of drawing is the
 * whole point of Sprint 22 — a second copy of this arithmetic would
 * drift the moment one row got a multibyte label the other did not.
 *
 * `scroll_mag` names the row in the scroll regions' payload (±1 row 1,
 * ±2 row 2) so a click on `>N` scrolls the row it belongs to.
 */
/* ---------------------------------------------------------------- */
/* Sprint 27 §4: the pre-drag slot table                            */
/* ---------------------------------------------------------------- */

typedef struct StripPreSlot {
    u16 col0, col1; /* half-open, in SCREEN cells */
    i32 pre_payload;
} StripPreSlot;

static StripPreSlot strip_pre[SAG_TAB_MAX];
static int strip_pre_n;
static u16 strip_pre_y;
static u16 strip_tail_x;

int sag_strip_slot_at(u16 x, u16 y)
{
    int i;

    /* The table only ever holds row 1, so the row is checked once
     * rather than stored per slot. */
    if (strip_pre_n == 0 || y != strip_pre_y)
        return -1;
    for (i = 0; i < strip_pre_n; i++) {
        if (x >= strip_pre[i].col0 && x < strip_pre[i].col1)
            return i;
    }
    return -1;
}

bool sag_strip_pre_payload(int slot, i32 *payload)
{
    if (slot < 0 || slot >= strip_pre_n || payload == NULL)
        return false;
    *payload = strip_pre[slot].pre_payload;
    return true;
}

int sag_strip_slot_count(void)
{
    return strip_pre_n;
}

u16 sag_strip_tail_x(void)
{
    return strip_tail_x;
}

/*
 * The drag's preview: the held entry is drawn where it would land.
 *
 * NOTHING IN Tabs.v CHANGES UNTIL THE DROP.  Swapping live would look
 * identical and be far worse underneath — cancelling would mean undoing
 * an arbitrary number of moves, a drag that wandered off the bar would
 * leave the array half-shuffled, and a future drop-into-a-pane must not
 * have quietly reordered the strip on the way.
 *
 * Insertion, not swap, so the entries the held one passes keep their
 * relative order — the same rule sag_tab_reorder commits with, applied
 * to the picture so the drop holds no surprise.
 */
static void apply_drag_preview(const Ed *ed, StripEntry *entries, int n,
                               int *active_entry)
{
    i32 held;
    int to;
    int from = -1;
    int i;
    StripEntry moved;

    if (!sag_mouse_drag_preview(ed, &held, &to) || n <= 1)
        return;
    for (i = 0; i < n; i++) {
        if (entries[i].payload == held) {
            from = i;
            break;
        }
    }
    if (from < 0)
        return;
    if (to < 0)
        to = 0;
    if (to >= n)
        to = n - 1;
    moved = entries[from];
    /* The ghost: dim marks the entry as travelling rather than
     * settled, so a drop that lands where it started is visibly a
     * no-op instead of looking like nothing happened. */
    moved.dim = true;
    if (from < to) {
        (void)memmove(&entries[from], &entries[from + 1],
                      sizeof(entries[0]) * (size_t)(to - from));
    } else if (to < from) {
        (void)memmove(&entries[to + 1], &entries[to],
                      sizeof(entries[0]) * (size_t)(from - to));
    }
    entries[to] = moved;
    if (active_entry != NULL && *active_entry >= 0)
        *active_entry = sag_tab_shifted_index(*active_entry, from, to);
}

static void strip_render(Ed *ed, Rect rect, StripEntry *entries, int n,
                         int active_entry, int *scroll, i32 scroll_mag,
                         bool record_slots);

/*
 * Row 1, with the drag preview applied and the pre-drag list recorded.
 *
 * `pre` is the list BEFORE the permutation; `entries` is what gets
 * drawn.  The two are the same array position by position when no drag
 * is in flight, which is exactly why the dwell's fixture has to force
 * them apart to prove it reads the right one.
 */
static void strip_render_row1(Ed *ed, Rect rect, StripEntry *entries,
                              int n, int active_entry, int *scroll)
{
    StripEntry pre[SAG_TAB_MAX];
    int i;

    if (n > (int)SAG_ARRAY_LEN(pre))
        n = (int)SAG_ARRAY_LEN(pre);
    if (n > 0)
        (void)memcpy(pre, entries, sizeof(pre[0]) * (size_t)n);
    apply_drag_preview(ed, entries, n, &active_entry);
    /* Cleared before the render fills the cell ranges in: a slot the
     * layout scrolled out of view must not keep last frame's cells and
     * answer for a position nobody can point at. */
    strip_pre_n = n;
    strip_pre_y = rect.y;
    strip_tail_x = rect.x;
    for (i = 0; i < n; i++) {
        strip_pre[i].col0 = 0U;
        strip_pre[i].col1 = 0U;
        strip_pre[i].pre_payload = pre[i].payload;
    }
    strip_render(ed, rect, entries, n, active_entry, scroll, 1, true);
}

static void strip_render(Ed *ed, Rect rect, StripEntry *entries, int n,
                         int active_entry, int *scroll, i32 scroll_mag,
                         bool record_slots)
{
    StripSpan spans[SAG_TAB_MAX];
    int n_spans = 0;
    bool more_left = false;
    bool more_right = false;
    int i;
    u16 avail;
    SagColor dim = {SAG_COLOR_RGB, 120U, 120U, 120U};
    SagColor fg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    SagColor bg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    Cell blank;

    if (rect.w == 0U || rect.h == 0U)
        return;
    /* Blank the row first: a shorter strip than last frame must not
     * leave the tail of the old one behind. */
    (void)memset(&blank, 0, sizeof(blank));
    sag_grid_fill(&ed->grid, rect.y, rect.x, (u16)(rect.x + rect.w), blank);
    if (n <= 0)
        return;

    /* Reserve a cell for `<` when scrolled, so the indicator never
     * overlaps the first entry it is pointing away from. */
    avail = rect.w;
    if (*scroll > 0 && avail > 1U)
        avail = (u16)(avail - 1U);
    sag_strip_layout(entries, n, avail, active_entry, scroll, spans,
                     &n_spans, &more_left, &more_right);

    for (i = 0; i < n_spans; i++) {
        int idx = spans[i].idx;
        u16 x = (u16)(rect.x + spans[i].col0 + (more_left ? 1U : 0U));
        u16 attrs = 0U;
        Rect span_rect;

        if (idx == active_entry)
            attrs = SAG_ATTR_REVERSE;
        else if (entries[idx].dim)
            attrs = SAG_ATTR_DIM;
        /* Draw only the bytes that fit the span the layout gave us.
         * Drawing the whole label writes its tail over the next
         * entry — which is exactly what the golden caught. */
        (void)sag_grid_puts(&ed->grid, rect.y, x,
                            (const u8 *)entries[idx].label,
                            sag_strip_label_bytes(entries[idx].label),
                            entries[idx].dim ? dim : fg, bg, attrs);
        /*
         * Registered with the SAME cells the layout produced and the
         * draw used.  Recomputing this from strlen while hit-testing is
         * the multibyte click-shift the Sprint 22 law forbids.
         */
        span_rect = (Rect){x, rect.y,
                           (u16)(spans[i].col1 - spans[i].col0), 1U};
        sag_region_add(SAG_REGION_TAB, span_rect, entries[idx].payload);
        /*
         * Sprint 27 §4.  The SAME cells, against the pre-drag list —
         * `idx` is a position in the visible strip, and the pre-drag
         * table is indexed by position for exactly that reason.  A
         * second derivation of where a slot sits is the multibyte
         * click-shift the Sprint 22 law forbids.
         */
        if (record_slots && idx >= 0 && idx < strip_pre_n) {
            strip_pre[idx].col0 = x;
            strip_pre[idx].col1 = (u16)(x + span_rect.w);
            if (strip_pre[idx].col1 > strip_tail_x)
                strip_tail_x = strip_pre[idx].col1;
        }
    }
    if (more_left) {
        Rect r = {rect.x, rect.y, 1U, 1U};

        (void)sag_grid_puts(&ed->grid, rect.y, rect.x,
                            (const u8 *)sag_glyph(SAG_GLYPH_MORE_LEFT),
                            sag_glyph_len(SAG_GLYPH_MORE_LEFT), dim, bg,
                            SAG_ATTR_DIM);
        sag_region_add(SAG_REGION_TAB_SCROLL, r, -scroll_mag);
    }
    if (more_right) {
        char more[16];
        int past = n - (n_spans > 0 ? spans[n_spans - 1].idx + 1 : 0);
        u16 w;
        u16 x;
        Rect r;

        (void)snprintf(more, sizeof(more), "%s%d",
                       sag_glyph(SAG_GLYPH_MORE_RIGHT), past);
        w = (u16)strlen(more);
        if (w < rect.w) {
            x = (u16)(rect.x + rect.w - w);
            (void)sag_grid_puts(&ed->grid, rect.y, x, (const u8 *)more,
                                strlen(more), dim, bg, SAG_ATTR_DIM);
            r = (Rect){x, rect.y, w, 1U};
            sag_region_add(SAG_REGION_TAB_SCROLL, r, scroll_mag);
        }
    }
}

/*
 * Row 2: the members of `gid`, one entry each, payload = GLOBAL tab
 * index — so the existing SAG_REGION_TAB click case handles them
 * unchanged.  The active member is reversed; the others render dim,
 * which is what makes row 2 read as secondary to row 1.
 *
 * This is also the hover-preview renderer (Sprint 27 calls it with a
 * group the user is only pointing at).  Same function, so the pinned
 * row and the preview cannot disagree.
 */
void sag_tab_member_strip_draw(Ed *ed, Rect rect, u32 gid)
{
    StripEntry entries[SAG_TAB_MAX];
    int members[SAG_TAB_MAX];
    int n;
    int i;
    int active_entry = -1;

    if (ed == NULL || gid == 0U)
        return;
    n = sag_group_members(ed, gid, members, (int)SAG_ARRAY_LEN(members));
    for (i = 0; i < n; i++) {
        (void)memset(&entries[i], 0, sizeof(entries[i]));
        (void)snprintf(entries[i].label, sizeof(entries[i].label), " %s%s",
                       tab_basename(&ed->tabs.v.data[members[i]]),
                       sag_tab_modified(ed, members[i])
                           ? sag_glyph(SAG_GLYPH_MODIFIED) : "");
        entries[i].payload = members[i];
        entries[i].dim = true;
        if (members[i] == ed->tabs.active)
            active_entry = i;
    }
    strip_render(ed, rect, entries, n, active_entry,
                 &ed->tabs.member_scroll, 2, false);
}

void sag_tab_strip_draw(Ed *ed, Rect rect)
{
    StripEntry entries[SAG_TAB_MAX];
    int n;
    u32 gid;

    if (ed == NULL || rect.w == 0U || rect.h == 0U)
        return;
    n = sag_tab_row1_entries(ed, entries, (int)SAG_ARRAY_LEN(entries));
    strip_render_row1(ed, (Rect){rect.x, rect.y, rect.w, 1U}, entries, n,
                      sag_tab_row1_active(ed, entries, n), &ed->tabs.scroll);
    gid = sag_active_group_id(ed);
    /*
     * Sprint 27 §4: a dwell opens a group's member strip as a drop
     * target, so row 2 shows the PREVIEWED group when there is one.
     * Same renderer as the pinned row, so the two cannot disagree about
     * placement — which is the whole reason s24 wrote it as one
     * function.
     */
    if (sag_mouse_preview_group(ed) != 0U)
        gid = sag_mouse_preview_group(ed);
    if (rect.h >= 2U && gid != 0U)
        sag_tab_member_strip_draw(ed,
                                  (Rect){rect.x, (u16)(rect.y + 1U),
                                         rect.w, 1U},
                                  gid);
}

/*
 * Click routing for the strip.  Everything that is not a tab span or a
 * scroll indicator is IGNORED — Sprint 27 owns wheel, drag-reorder and
 * the context menu, and a click half-handled here would move a tab the
 * user meant to scroll past.
 */
bool sag_tab_strip_click(Ed *ed, u16 x, u16 y)
{
    Region hit;

    if (ed == NULL)
        return false;
    hit = sag_region_hit(x, y);
    if (hit.kind == SAG_REGION_TAB_SCROLL) {
        /* The payload's MAGNITUDE names the row (1 or 2); its sign is
         * the direction.  Without the row, a click on row 2's `>N`
         * would scroll row 1 under the user's pointer. */
        bool row2 = hit.payload == 2 || hit.payload == -2;
        int *scroll = row2 ? &ed->tabs.member_scroll : &ed->tabs.scroll;
        int limit = (int)ed->tabs.v.len;
        int to = *scroll + (hit.payload < 0 ? -1 : 1);

        if (row2)
            limit = sag_group_member_count(ed, sag_active_group_id(ed));
        if (to < 0)
            to = 0;
        if (to >= limit)
            to = limit > 0 ? limit - 1 : 0;
        *scroll = to;
        ed->full_damage = true;
        return true;
    }
    if (hit.kind != SAG_REGION_TAB)
        return false;
    /*
     * The sign convention region.h wrote down in Sprint 22, now live:
     * a negative payload is a GROUP id, negated.  One region kind, and
     * the renderer and the router read the same rule.
     */
    if (hit.payload < 0) {
        sag_group_note_position(ed);
        /* Clicking a group's entry is an EXPLICIT entry, so it resumes
         * where the user left off — unlike a mid-walk arrival, which
         * enters from the side it came from. */
        sag_group_enter(ed, (u32)(-hit.payload));
        return true;
    }
    sag_tab_switch(ed, hit.payload);
    return true;
}

/* ---------------------------------------------------------------- */
/* Sprint 23 §5/§6: commands and the dirty-close prompt             */
/* ---------------------------------------------------------------- */

CmdStatus sag_tab_cmd_new(CmdCtx *cx)
{
    int idx;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    idx = sag_tab_open(cx->ed, NULL);
    /* The return value is checked at EVERY call site (DoD 6): a silent
     * cap failure is how facsimile loaded a new file into the
     * still-active tab and then wrote it over the old path. */
    if (idx < 0)
        return SAG_CMD_ERR_STATE;
    sag_tab_switch(cx->ed, idx);
    return SAG_CMD_OK;
}

CmdStatus sag_tab_cmd_open(CmdCtx *cx)
{
    int idx;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL)
        return SAG_CMD_ERR_ARG;
    idx = sag_tab_open(cx->ed, cx->sarg);
    if (idx < 0)
        return SAG_CMD_ERR_STATE;
    sag_tab_switch(cx->ed, idx);
    return SAG_CMD_OK;
}

static CmdStatus tab_step(CmdCtx *cx, int delta)
{
    int n;
    int at;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    n = (int)sag_tab_count(cx->ed);
    if (n <= 1)
        return SAG_CMD_OK;
    at = cx->ed->tabs.active + delta;
    /* Cyclic, because next-from-the-last meaning "nothing" is a worse
     * answer than wrapping when there is a strip showing the ring. */
    while (at < 0)
        at += n;
    sag_tab_switch(cx->ed, at % n);
    return SAG_CMD_OK;
}

CmdStatus sag_tab_cmd_next(CmdCtx *cx)
{
    return tab_step(cx, 1);
}

CmdStatus sag_tab_cmd_prev(CmdCtx *cx)
{
    return tab_step(cx, -1);
}

/* ---------------------------------------------------------------- */
/* Sprint 24 §7: the 500 ms digit-extension window                  */
/* ---------------------------------------------------------------- */

/*
 * Module-local, because nothing outside this file has any business
 * knowing a jump is half-finished.
 */
static i64 jump_value;
static i64 jump_deadline_ms;
static u32 jump_group;
static bool jump_on;
static TimerId jump_timer;

bool sag_tab_jump_armed(void)
{
    return jump_on;
}

void sag_tab_jump_clear(Ed *ed)
{
    if (ed != NULL && jump_timer != SAG_TIMER_NONE) {
        (void)sag_timer_cancel(&ed->timers, jump_timer);
        jump_timer = SAG_TIMER_NONE;
    }
    jump_on = false;
    jump_value = 0;
    jump_deadline_ms = 0;
    jump_group = 0U;
}

/*
 * Fired by the event-loop timer heap rather than by the next keystroke.
 *
 * The hint on the status line promises that a further digit will do
 * something; if it only cleared when a key arrived, the promise would
 * sit there indefinitely on an idle editor and then be broken.
 */
static void jump_expire(Ed *ed, void *ctx)
{
    (void)ctx;
    if (ed == NULL || !jump_on)
        return;
    jump_timer = SAG_TIMER_NONE;
    sag_tab_jump_clear(ed);
    sag_msg_clear(ed);
    ed->footer_dirty = true;
}

/* Announces what a further digit would do.  The window must never feel
 * like a lost keystroke. */
static void jump_arm(Ed *ed, int idx)
{
    const Tab *t = sag_tab_at(ed, idx);

    if (t == NULL)
        return;
    sag_tab_jump_clear(ed);
    jump_on = true;
    jump_value = idx + 1;
    jump_deadline_ms = ed->now_ms + SAG_JUMP_WINDOW_MS;
    jump_group = t->group_id;
    jump_timer = sag_timer_add(&ed->timers, jump_deadline_ms, jump_expire,
                               NULL);
    if (jump_group != 0U) {
        int n = sag_group_member_count(ed, jump_group);

        sag_msg(ed, SAG_MSG_INFO, "tab %lld — a digit picks a member (1-%d)",
                (long long)jump_value, n);
    } else {
        sag_msg(ed, SAG_MSG_INFO, "tab %lld — a digit extends to %lld_",
                (long long)jump_value, (long long)jump_value);
    }
}

/* 0 is the TENTH key on the digit row, not the zeroth thing. */
static int digit_ordinal(u32 code)
{
    int d = (int)(code - (u32)'0');

    return d == 0 ? 10 : d;
}

bool sag_tab_jump_key(Ed *ed, Key key)
{
    int digit;

    if (ed == NULL || !jump_on)
        return false;
    if (key.ev == SAG_KEY_RELEASE)
        return false;
    /*
     * Clear FIRST, then let the key dispatch normally.  Returning
     * without clearing would let a digit typed much later read as a
     * continuation of a jump the user has long forgotten.
     */
    if (ed->now_ms >= jump_deadline_ms) {
        sag_tab_jump_clear(ed);
        return false;
    }
    /*
     * Bare `5`, and also `alt+5` / `ctrl+5`: holding the modifier down
     * is the natural way to type `alt+1` `5`, and accepting only the
     * bare form sent the second digit to the main dispatch as its own
     * jump — tab 1 then tab 5, never tab 15.
     */
    if (key.code < (u32)'0' || key.code > (u32)'9') {
        sag_tab_jump_clear(ed);
        return false;
    }
    digit = digit_ordinal(key.code);

    if (jump_group != 0U) {
        int members[SAG_TAB_MAX];
        int n = sag_group_members(ed, jump_group, members,
                                  (int)SAG_ARRAY_LEN(members));

        /* The digit counts what ROW 2 shows, which is what the user is
         * looking at while counting. */
        if (digit >= 1 && digit <= n) {
            sag_tab_switch(ed, members[digit - 1]);
            sag_tab_jump_clear(ed);
            sag_msg_clear(ed);
        } else {
            sag_msg(ed, SAG_MSG_ERROR, "this group has %d members", n);
            sag_tab_jump_clear(ed);
        }
        return true;
    }

    {
        /* Here the digit is a DIGIT, not the tenth key: `1` then `0` is
         * tab 10, and `1` then `5` is tab 15. */
        i64 target = jump_value * 10 + (i64)(key.code - (u32)'0');
        int idx = (int)target - 1;

        if (idx >= 0 && idx < (int)sag_tab_count(ed)) {
            sag_tab_switch(ed, idx);
            /* Re-armed, so three digits work. */
            jump_arm(ed, idx);
        } else {
            sag_msg(ed, SAG_MSG_ERROR, "no tab %lld", (long long)target);
            sag_tab_jump_clear(ed);
        }
    }
    /*
     * Consumed either way.  The digit was part of a chord, so it must
     * not fall through and be inserted into the document.
     */
    return true;
}

/*
 * Jumps NOW and arms the window (§7).  The switch is the whole command;
 * arming is what lets a second digit supersede it without the first
 * jump having waited for one.
 */
CmdStatus sag_tab_cmd_goto(CmdCtx *cx)
{
    i64 want;
    int idx;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    want = cx->iarg;
    if (cx->count_given && cx->count != 0U)
        want = (i64)cx->count;
    /* 0 means tab 10: the digit row reads 1..9 then 0, so `0` is the
     * tenth key, not the zeroth tab. */
    if (want == 0)
        want = 10;
    idx = (int)want - 1;
    if (idx < 0 || idx >= (int)sag_tab_count(cx->ed)) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "no tab %lld",
                (long long)want);
        return SAG_CMD_ERR_ARG;
    }
    sag_tab_switch(cx->ed, idx);
    jump_arm(cx->ed, idx);
    return SAG_CMD_OK;
}

CmdStatus sag_tab_cmd_move(CmdCtx *cx)
{
    i64 want;
    int to;

    if (cx == NULL || cx->ed == NULL || cx->ed->tabs.active < 0)
        return SAG_CMD_ERR_STATE;
    want = cx->iarg;
    if (cx->count_given && cx->count != 0U)
        want = (i64)cx->count;
    to = (int)want - 1;
    if (to < 0 || to >= (int)sag_tab_count(cx->ed)) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "no position %lld",
                (long long)want);
        return SAG_CMD_ERR_ARG;
    }
    sag_tab_reorder(cx->ed, cx->ed->tabs.active, to);
    cx->ed->full_damage = true;
    return SAG_CMD_OK;
}

/*
 * The dirty-close prompt.
 *
 * It captures the tab_ID, not the index: another event — an async job
 * closing a tab (Sprint 19) — can compact the array while the prompt is
 * up, and an index captured a moment ago would then answer for a
 * different file.  The id is resolved when the answer arrives; if it is
 * gone the prompt dissolves silently.
 */
static void tab_prompt_show(Ed *ed)
{
    int idx = sag_tab_index_of_id(ed, ed->tab_prompt.tab_id);
    const Tab *t;

    if (idx < 0) {
        ed->tab_prompt.active = false;
        return;
    }
    t = sag_tab_at(ed, idx);
    sag_msg(ed, SAG_MSG_INFO,
            "save changes to %s?  [w]rite  [d]iscard  [esc] cancel",
            t->path != NULL ? t->path : "untitled");
}

CmdStatus sag_tab_cmd_close(CmdCtx *cx)
{
    Ed *ed;
    int idx;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    ed = cx->ed;
    idx = ed->tabs.active;
    if (idx < 0)
        return SAG_CMD_ERR_STATE;
    if (sag_tab_count(ed) <= 1U) {
        sag_msg(ed, SAG_MSG_ERROR, "cannot close the last tab");
        return SAG_CMD_ERR_STATE;
    }
    if (!sag_tab_modified(ed, idx)) {
        (void)sag_tab_close(ed, idx);
        return SAG_CMD_OK;
    }
    ed->tab_prompt.tab_id = sag_tab_at(ed, idx)->tab_id;
    ed->tab_prompt.active = true;
    tab_prompt_show(ed);
    return SAG_CMD_OK;
}

/*
 * Sprint 27 §5: the tab context menu's rows, as registry commands.
 *
 * They exist as commands rather than as menu-only handlers because of
 * invariant 9: every mouse action has a keyboard path, and the menu row
 * and the key must reach the same code rather than two implementations
 * that drift.
 */
CmdStatus sag_tab_cmd_close_others(CmdCtx *cx)
{
    Ed *ed;
    u32 keep;
    u32 doomed[SAG_TAB_MAX];
    u32 n = 0U;
    u32 i;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    ed = cx->ed;
    if (ed->tabs.active < 0)
        return SAG_CMD_ERR_STATE;
    if (sag_tab_count(ed) <= 1U) {
        sag_msg(ed, SAG_MSG_ERROR, "no other tabs");
        return SAG_CMD_ERR_STATE;
    }
    keep = sag_tab_at(ed, ed->tabs.active)->tab_id;
    /*
     * IDS FIRST, then close.  Every close compacts the array and
     * renumbers the indices above it, so a loop over indices would skip
     * a tab per close and eventually close the wrong file — the exact
     * hazard tabs.h exists for.
     */
    for (i = 0U; i < ed->tabs.v.len; i++) {
        if (ed->tabs.v.data[i].tab_id != keep)
            doomed[n++] = ed->tabs.v.data[i].tab_id;
    }
    for (i = 0U; i < n; i++) {
        int idx = sag_tab_index_of_id(ed, doomed[i]);

        /* A modified tab needs an answer, and the answer prompt is
         * per tab; refuse the whole gesture rather than closing half of
         * them and leaving a dialog holding the rest. */
        if (idx < 0)
            continue;
        if (sag_tab_modified(ed, idx)) {
            sag_msg(ed, SAG_MSG_ERROR,
                    "unsaved changes in another tab; save or force first");
            return SAG_CMD_ERR_STATE;
        }
    }
    for (i = 0U; i < n; i++) {
        int idx = sag_tab_index_of_id(ed, doomed[i]);

        if (idx >= 0)
            (void)sag_tab_close(ed, idx);
    }
    ed->layout_dirty = true;
    ed->full_damage = true;
    return SAG_CMD_OK;
}

CmdStatus sag_tab_cmd_copy_path(CmdCtx *cx)
{
    Ed *ed;
    const Tab *t;
    RegVal v;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_STATE;
    ed = cx->ed;
    t = sag_tab_at(ed, ed->tabs.active);
    if (t == NULL || t->path == NULL) {
        sag_msg(ed, SAG_MSG_ERROR, "this tab has no path");
        return SAG_CMD_ERR_STATE;
    }
    /* The CANONICAL path — which is what Tab.path already holds, set at
     * the one site that establishes a tab's name. */
    sag_regval_init(&v);
    bytebuf_append(&v.bytes, (const u8 *)t->path, strlen(t->path));
    v.type = (u8)SAG_REG_CHARWISE;
    /* Register `+` is the system clipboard, so this also travels out
     * through Sprint 12's OSC 52 path. */
    sag_reg_yank(&ed->regs, (u8)'+', &v);
    sag_regval_free(&v);
    sag_msg(ed, SAG_MSG_INFO, "copied %s", t->path);
    return SAG_CMD_OK;
}

bool sag_tab_prompt_key(Ed *ed, u8 answer)
{
    int idx;

    if (ed == NULL || !ed->tab_prompt.active)
        return false;
    idx = sag_tab_index_of_id(ed, ed->tab_prompt.tab_id);
    if (idx < 0) {
        /* The tab went away while the question was up; the question
         * goes away with it rather than answering for its successor. */
        ed->tab_prompt.active = false;
        sag_msg_clear(ed);
        return true;
    }
    switch (answer) {
    case 'w': {
        Tab *t = sag_tab_at(ed, idx);
        CmdStatus st;

        /*
         * Write through the ONE save path, and close only on success.
         * There is no route where "close" discards bytes the user asked
         * to keep (invariant 1), so an I/O error aborts the close and
         * leaves the tab and its text exactly as they were.
         */
        sag_tab_switch(ed, idx);
        st = sag_ed_file_save(ed, false);
        if (st != SAG_CMD_OK) {
            ed->tab_prompt.active = false;
            return true; /* the save path already reported why */
        }
        idx = sag_tab_index_of_id(ed, t->tab_id);
        if (idx >= 0)
            (void)sag_tab_close(ed, idx);
        break;
    }
    case 'd':
        (void)sag_tab_close(ed, idx);
        break;
    case 0x1BU: /* Esc cancels the close entirely. */
        break;
    default:
        /* Every other key is swallowed with the question restated,
         * rather than falling through to whatever it is normally
         * bound to. */
        tab_prompt_show(ed);
        return true;
    }
    ed->tab_prompt.active = false;
    sag_msg_clear(ed);
    return true;
}
