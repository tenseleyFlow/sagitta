/*
 * Sprint 23 §1/§2.  See tabs.h for the stable-id discipline.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ui/tabs.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/message.h"
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
        sag_pane_free(t->root);
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

int sag_tab_open(Ed *ed, const char *path)
{
    Tab t;
    int existing;
    Win *win;

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
    t.root = sag_pane_new_leaf(win);
    t.focus = t.root;
    t.buffer_id = win->buf != NULL ? win->buf->id : 0U;
    t.deferred = false; /* real hydration is Sprint 24 */
    TabVec_push(&ed->tabs.v, t);
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
