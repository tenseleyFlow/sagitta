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
                   sag_tab_modified(ed, idx) ? "*" : "");
}

u32 sag_tab_strip_rows(const Ed *ed)
{
    /*
     * A parameter, not a renderer: Sprint 22's layout reserves whatever
     * this returns before handing the rest to the pane tree, exactly as
     * it reserves the footer row.  One tab needs no strip.
     */
    return ed != NULL && ed->tabs.v.len > 1U ? 1U : 0U;
}

void sag_tab_strip_draw(Ed *ed, Rect rect)
{
    StripEntry entries[SAG_TAB_MAX];
    StripSpan spans[SAG_TAB_MAX];
    int n_spans = 0;
    bool more_left = false;
    bool more_right = false;
    int n;
    int i;
    u16 avail;
    SagColor dim = {SAG_COLOR_RGB, 120U, 120U, 120U};
    SagColor fg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    SagColor bg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};

    if (ed == NULL || rect.w == 0U || rect.h == 0U)
        return;
    n = (int)ed->tabs.v.len;
    if (n <= 0)
        return;
    for (i = 0; i < n; i++) {
        (void)memset(&entries[i], 0, sizeof(entries[i]));
        tab_label(ed, i, entries[i].label, sizeof(entries[i].label));
        entries[i].payload = i;
        /* Orphans — files outside the workspace — render dim.  Sprint
         * 25 refines what "outside" means. */
        {
            /*
             * Orphans are files outside the workspace root.  Both sides
             * are already canonical — the tab's path from sag_tab_open
             * and the root from the workspace — so this is a prefix
             * test, not a filesystem call on a draw path.  Sprint 25
             * refines what "outside" means.
             */
            const char *root = sag_ws_root(ed);
            const char *p = ed->tabs.v.data[i].path;

            entries[i].dim = p != NULL && root != NULL &&
                             strncmp(p, root, strlen(root)) != 0;
        }
    }
    /* Reserve a cell for `<` when scrolled, so the indicator never
     * overlaps the first entry it is pointing away from. */
    avail = rect.w;
    if (ed->tabs.scroll > 0 && avail > 1U)
        avail = (u16)(avail - 1U);
    sag_strip_layout(entries, n, avail, ed->tabs.active, &ed->tabs.scroll,
                     spans, &n_spans, &more_left, &more_right);

    {
        /* Blank the row first: a shorter strip than last frame must not
         * leave the tail of the old one behind. */
        Cell blank;

        (void)memset(&blank, 0, sizeof(blank));
        sag_grid_fill(&ed->grid, rect.y, rect.x,
                      (u16)(rect.x + rect.w), blank);
    }
    for (i = 0; i < n_spans; i++) {
        int idx = spans[i].idx;
        u16 x = (u16)(rect.x + spans[i].col0 + (more_left ? 1U : 0U));
        u16 attrs = 0U;
        Rect span_rect;

        if (idx == ed->tabs.active)
            attrs = SAG_ATTR_REVERSE;
        else if (entries[idx].dim)
            attrs = SAG_ATTR_DIM;
        (void)sag_grid_puts(&ed->grid, rect.y, x,
                            (const u8 *)entries[idx].label,
                            strlen(entries[idx].label),
                            entries[idx].dim ? dim : fg, bg, attrs);
        /*
         * Registered with the SAME cells the layout produced and the
         * draw used.  Recomputing this from strlen while hit-testing is
         * the multibyte click-shift the Sprint 22 law forbids.
         */
        span_rect = (Rect){x, rect.y,
                           (u16)(spans[i].col1 - spans[i].col0), 1U};
        sag_region_add(SAG_REGION_TAB, span_rect, idx);
    }
    if (more_left) {
        Rect r = {rect.x, rect.y, 1U, 1U};

        (void)sag_grid_puts(&ed->grid, rect.y, rect.x, (const u8 *)"<",
                            1U, dim, bg, SAG_ATTR_DIM);
        sag_region_add(SAG_REGION_TAB_SCROLL, r, -1);
    }
    if (more_right) {
        char more[16];
        int past = n - (n_spans > 0 ? spans[n_spans - 1].idx + 1 : 0);
        u16 w;
        u16 x;
        Rect r;

        (void)snprintf(more, sizeof(more), ">%d", past);
        w = (u16)strlen(more);
        if (w < rect.w) {
            x = (u16)(rect.x + rect.w - w);
            (void)sag_grid_puts(&ed->grid, rect.y, x, (const u8 *)more,
                                strlen(more), dim, bg, SAG_ATTR_DIM);
            r = (Rect){x, rect.y, w, 1U};
            sag_region_add(SAG_REGION_TAB_SCROLL, r, 1);
        }
    }
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
        int to = ed->tabs.scroll + (hit.payload < 0 ? -1 : 1);

        if (to < 0)
            to = 0;
        if (to >= (int)ed->tabs.v.len)
            to = (int)ed->tabs.v.len - 1;
        ed->tabs.scroll = to;
        ed->full_damage = true;
        return true;
    }
    if (hit.kind != SAG_REGION_TAB)
        return false;
    /*
     * Sprint 24 introduces negative payloads for groups.  Until then
     * one arriving means the renderer and the router disagree about the
     * convention, which is the bug the sign rule was written down early
     * to prevent — so say so loudly rather than switching to tab -3.
     */
    if (hit.payload < 0)
        SAG_BUG("negative SAG_REGION_TAB payload: groups land in "
                "Sprint 24");
    sag_tab_switch(ed, hit.payload);
    return true;
}
