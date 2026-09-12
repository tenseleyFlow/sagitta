/*
 * Sprint 23 §1/§2.  See tabs.h for the stable-id discipline.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ui/tabs.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/shadow.h"
#include "fl/flruntime.h"
#include "mod/git/fussmode.h"
#include "text/file.h"
#include "ui/groupnav.h"
#include "ui/glyphs.h"
#include "ui/groups.h"
#include "ui/mouse.h"
#include "ui/message.h"
#include "ui/region.h"
#include "ui/strip.h"
#include "syn/theme.h"
#include "util/log.h"

void yew_tabs_init(Tabs *t)
{
    if (t == NULL)
        return;
    (void)memset(t, 0, sizeof(*t));
    t->next_tab_id = 1U; /* 0 is the invalid id */
    t->active = -1;
}

/*
 * Sprint 57.15 §1.  See tabs.h for why the flag exists and why there
 * are two of them.
 */
void yew_tabs_scroll_owned(Tabs *t, bool row2)
{
    if (t == NULL)
        return;
    if (row2)
        t->member_scroll_user = true;
    else
        t->scroll_user = true;
}

void yew_tabs_follow_active(Tabs *t)
{
    if (t == NULL)
        return;
    t->scroll_user = false;
    t->member_scroll_user = false;
}

bool yew_tabs_scroll_is_owned(const Tabs *t, bool row2)
{
    if (t == NULL)
        return false;
    return row2 ? t->member_scroll_user : t->scroll_user;
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
        Pane *leaves[YEW_PANE_MAX_LEAVES * 2];
        u32 n = 0U;
        u32 i;

        yew_pane_collect_leaves(t->root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        for (i = 0U; i < n; i++)
            yew_ed_win_release(ed, leaves[i]->win);
        yew_pane_free(ed, t->root);
    }
    yew_xfree(t->path);
    yew_xfree(t->display_path);
    (void)memset(t, 0, sizeof(*t));
}

static bool tabs_has_window(const Ed *ed, const Win *want)
{
    size_t tab;

    if (ed == NULL || want == NULL)
        return false;
    for (tab = 0U; tab < ed->tabs.v.len; tab++) {
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 i;

        yew_pane_collect_leaves(ed->tabs.v.data[tab].root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        for (i = 0U; i < n; i++)
            if (leaves[i]->win == want)
                return true;
    }
    return false;
}

void yew_tabs_free(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < ed->tabs.v.len; i++)
        tab_destroy(ed, &ed->tabs.v.data[i]);
    TabVec_free(&ed->tabs.v);
    yew_tabs_init(&ed->tabs);
}

u32 yew_tab_count(const Ed *ed)
{
    return ed == NULL ? 0U : (u32)ed->tabs.v.len;
}

Tab *yew_tab_at(Ed *ed, int idx)
{
    if (ed == NULL || idx < 0 || (size_t)idx >= ed->tabs.v.len)
        return NULL;
    return &ed->tabs.v.data[idx];
}

const Tab *yew_tab_at_const(const Ed *ed, int idx)
{
    if (ed == NULL || idx < 0 || (size_t)idx >= ed->tabs.v.len)
        return NULL;
    return &ed->tabs.v.data[idx];
}

const char *yew_tab_display_path(const Tab *tab)
{
    if (tab == NULL)
        return NULL;
    return tab->display_path != NULL ? tab->display_path : tab->path;
}

int yew_tab_index_of_id(const Ed *ed, u32 id)
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
    resolved = yew_xrealpath(path);
    if (resolved != NULL)
        return resolved; /* realpath allocates; the Tab owns it */
    /* A path that does not exist yet is still a legitimate tab name;
     * keep what the user typed rather than refusing. */
    n = strlen(path) + 1U;
    out = yew_xmalloc(n);
    (void)memcpy(out, path, n);
    return out;
}

static char *logical_path(const Ed *ed, const char *path,
                          const char *canonical)
{
    const char *root;
    size_t root_len;

    if (canonical == NULL)
        return NULL;
    root = yew_ws_root(ed);
    if (root != NULL && root[0] != '\0') {
        root_len = strlen(root);
        if (root_len == 1U && root[0] == '/' && canonical[0] == '/' &&
            canonical[1] != '\0')
            return yew_xstrdup(canonical + 1U);
        if (strncmp(canonical, root, root_len) == 0 &&
            canonical[root_len] == '/' && canonical[root_len + 1U] != '\0')
            return yew_xstrdup(canonical + root_len + 1U);
    }
    /* Canonical identity must follow aliases; UI text must not silently
     * rewrite the absolute spelling the user supplied (notably Darwin's
     * /tmp -> /private/tmp alias). */
    if (path != NULL && path[0] == '/')
        return yew_xstrdup(path);
    if (path != NULL && path[0] != '\0' && path[0] != '/') {
        while (path[0] == '.' && path[1] == '/')
            path += 2U;
        if (path[0] != '\0')
            return yew_xstrdup(path);
    }
    return yew_xstrdup(canonical);
}

int yew_tab_find_by_path(const Ed *ed, const char *path)
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

        if (t->path != NULL && yew_file_same_identity(t->path, want)) {
            found = (int)i;
            break;
        }
    }
    yew_xfree(want);
    return found;
}

void yew_tab_set_path(Ed *ed, int idx, const char *path)
{
    Tab *t = yew_tab_at(ed, idx);
    char *canonical;
    char *display;

    if (t == NULL)
        return;
    canonical = canonical_path(path);
    display = logical_path(ed, path, canonical);
    yew_xfree(t->path);
    yew_xfree(t->display_path);
    t->path = canonical;
    t->display_path = display;
    yew_fuss_windows_changed(ed);
    yew_state_mark_dirty(ed);
}

int yew_tab_open(Ed *ed, const char *path)
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
        existing = yew_tab_find_by_path(ed, path);
        if (existing >= 0) {
            Tab *tab = yew_tab_at(ed, existing);
            Buffer *doc = tab == NULL ? NULL :
                          yew_ws_buf_by_id(ed, tab->buffer_id);
            bool was_loaded = doc != NULL && doc->tb != NULL;

            yew_tab_switch(ed, existing);
            /*
             * A tab's focused pane may temporarily show a scratch buffer
             * (macro editor, job output, diagnostics).  Reopening the
             * tab's path asks for the file, not merely its tab metadata.
             * Restore the stable buffer handle before returning so the
             * next edit or save cannot target the scratch view instead.
             */
            if (doc == NULL || yew_buf_hydrate(ed, doc) != 0 ||
                !yew_ed_show_buffer(ed, doc)) {
                yew_msg(ed, YEW_MSG_ERROR, "could not open %s", path);
                return -1;
            }
            if (!was_loaded)
                yew_fl_hook_buffer(ed, FL_EV_BUF_OPEN, doc);
            if (ed->prompt == YEW_PROMPT_NONE && doc->recovery_pending)
                yew_ed_prompt(ed, YEW_PROMPT_RECOVER);
            return existing;
        }
    }
    if (ed->tabs.v.len >= (size_t)YEW_TAB_MAX) {
        yew_msg(ed, YEW_MSG_ERROR, "too many tabs (max %d)", YEW_TAB_MAX);
        return -1;
    }
    win = yew_ed_win_clone(ed, ed->win);
    if (win == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "no room for another view");
        return -1;
    }
    (void)memset(&t, 0, sizeof(t));
    t.tab_id = ed->tabs.next_tab_id++;
    t.path = canonical_path(path);
    t.display_path = logical_path(ed, path, t.path);
    /*
     * The new tab gets its OWN buffer, and a file one is born
     * NON-RESIDENT: opening it costs no read at all.  The read happens
     * at the first switch, through yew_tab_hydrate.
     *
     * Sprint 23 pointed every tab at the window it cloned from, so all
     * tabs showed one buffer; that is why the save path could say
     * &ed->buffer and be right.  Both halves changed together.
     */
    buf = yew_ws_file_buf(ed, t.path);
    if (buf == NULL) {
        yew_ed_win_release(ed, win);
        yew_xfree(t.path);
        yew_xfree(t.display_path);
        yew_msg(ed, YEW_MSG_ERROR, "no room for another buffer");
        return -1;
    }
    yew_ed_win_set_buffer(ed, win, buf);
    t.root = yew_pane_new_leaf(win);
    t.focus = t.root;
    t.buffer_id = buf->id;
    TabVec_push(&ed->tabs.v, t);
    /* Sprint 57.15 §1: an OPEN changes the entry list under the offset.
     * yew_tab_switch is not on this path — a tab opened for a group is
     * never switched to — so the follow is resumed here. */
    yew_tabs_follow_active(&ed->tabs);
    yew_fuss_windows_changed(ed);
    yew_state_mark_dirty(ed);
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

bool yew_tab_close(Ed *ed, int idx)
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
    yew_group_remove_member(ed, idx);
    tab_destroy(ed, &ed->tabs.v.data[idx]);
    (void)memmove(&ed->tabs.v.data[idx], &ed->tabs.v.data[idx + 1],
                  (ed->tabs.v.len - (size_t)idx - 1U) *
                      sizeof(*ed->tabs.v.data));
    ed->tabs.v.len--;
    /* Resolved AFTER compaction, from the id chosen before it. */
    ed->tabs.active = yew_tab_index_of_id(ed, survivor);
    if (ed->tabs.active < 0 && ed->tabs.v.len > 0U)
        ed->tabs.active = 0;
    yew_tab_switch(ed, ed->tabs.active);
    yew_fuss_windows_changed(ed);
    yew_state_mark_dirty(ed);
    return true;
}

void yew_tab_switch(Ed *ed, int idx)
{
    Tab *t;
    Win *before;

    if (ed == NULL)
        return;
    /*
     * Sprint 57.15 §1: THE ACTIVE ENTRY IS MOVING, so the strip resumes
     * following it.  Unconditional, and before the `t == NULL` bail:
     * closing the last tab leaves no active entry at all, which is as
     * much a change as any other, and every route to a different tab —
     * switch, close, open, group enter/leave, a jump, a picker — comes
     * through here.  Clearing at the one funnel is why no gesture has
     * to remember to.
     */
    yew_tabs_follow_active(&ed->tabs);
    before = ed->win;
    t = yew_tab_at(ed, idx);
    if (t == NULL) {
        ed->tabs.active = ed->tabs.v.len == 0U ? -1 : ed->tabs.active;
        return;
    }
    if (tabs_has_window(ed, before) &&
        (t->focus == NULL || t->focus->win != before))
        yew_shadow_dismiss(ed, before);
    ed->tabs.active = idx;
    /*
     * Hydrate FIRST.  Every route to a different tab comes through
     * here, so this is the one place the read can be guaranteed to
     * happen before anything tries to draw the text (§3).
     */
    (void)yew_tab_hydrate(ed, idx);
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
    if (ed->prompt == YEW_PROMPT_NONE && ed->win != NULL &&
        ed->win->buf != NULL && ed->win->buf->recovery_pending)
        yew_ed_prompt(ed, YEW_PROMPT_RECOVER);
    ed->layout_dirty = true;
    ed->full_damage = true;
    yew_state_mark_dirty(ed);
    if (ed->win != before)
        yew_fl_hook_window(ed, FL_EV_WIN_FOCUS, ed->win);
}

int yew_tab_shifted_index(int i, int from, int to)
{
    if (i == from)
        return to;
    if (to > from)
        return (i > from && i <= to) ? i - 1 : i;
    return (i >= to && i < from) ? i + 1 : i;
}

void yew_tab_reorder(Ed *ed, int from, int to)
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
        ed->tabs.active = yew_tab_shifted_index(ed->tabs.active, from, to);
    yew_state_mark_dirty(ed);
}

bool yew_tab_modified(const Ed *ed, int idx)
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
    return w != NULL && yew_buf_dirty(w->buf);
}

/* ---------------------------------------------------------------- */
/* Sprint 24 §3: lazy hydration                                     */
/* ---------------------------------------------------------------- */

Buffer *yew_tab_buffer(Ed *ed, int idx)
{
    Tab *t = yew_tab_at(ed, idx);

    if (t == NULL || t->focus == NULL)
        return NULL;
    return t->focus->win == NULL ? NULL : t->focus->win->buf;
}

bool yew_tab_is_resident(const Ed *ed, int idx)
{
    /*
     * The question is asked of the ALLOCATION — does this tab's buffer
     * hold a TextBuf — never of a flag.  A flag drifts; a pointer
     * cannot disagree with itself, and every save path ends up asking
     * this same question anyway.
     */
    return yew_buf_resident(yew_tab_buffer((Ed *)ed, idx));
}

int yew_tab_hydrate(Ed *ed, int idx)
{
    Buffer *b = yew_tab_buffer(ed, idx);
    Tab *t = yew_tab_at(ed, idx);

    if (b == NULL || t == NULL)
        return -1;
    if (b->tb != NULL)
        return 0; /* resident: returns without touching the disk */
    if (yew_buf_hydrate(ed, b) != 0) {
        yew_msg(ed, YEW_MSG_ERROR, "could not read %s",
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

        yew_vp_init(w);
        w->vp.top = top;
        w->vp.top_sub = top_sub;
        w->vp.left = left;
        w->vp.wrap = wrap;
        /* Restore step 9, deferred to here: clamp, never follow.  The
         * file may have shrunk since the state was written. */
        yew_vp_clamp(w);
    }
    /*
     * Sprint 25 §6: cursors restored into a deferred tab were never
     * checked against a buffer, because there was none.  This is the
     * first moment there is one — and the offsets came out of a file
     * that may have been rewritten since, so an unchecked cursor can
     * sit past the end, which every motion and every edit derives from.
     */
    {
        Pane *leaves[YEW_PANE_MAX_LEAVES * 2];
        u32 n = 0U;
        u32 i;

        if (t->root != NULL) {
            yew_pane_collect_leaves(t->root, leaves, YEW_ARRAY_LEN(leaves),
                                    &n);
        }
        for (i = 0U; i < n; i++) {
            if (leaves[i]->win != NULL && leaves[i]->win->buf == b)
                yew_cset_normalize(b->tb, &leaves[i]->win->cs);
        }
    }
    yew_fl_hook_buffer(ed, FL_EV_BUF_OPEN, b);
    return 0;
}

void yew_tab_defer(Ed *ed, int idx)
{
    Buffer *b = yew_tab_buffer(ed, idx);

    if (ed == NULL || b == NULL)
        return;
    /* Never the tab being looked at: the window would be left pointing
     * at no text with a cursor in it. */
    if (idx == ed->tabs.active)
        return;
    yew_buf_defer(ed, b);
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
 * Facsimile's padded, bracket-free tab pill.  `num` is the 1-based
 * number the user types for goto — the tab's POSITION ON ITS ROW, not
 * its index in the array (Sprint 57.10: a group in the middle of the
 * bar would otherwise leave a visible gap in the numbering and make
 * `alt+4` land on a tab labelled 5).  The `*` is asked for, never
 * remembered.  Padding belongs to the tab's hit span; an unowned gap
 * between tabs would make the modern strip look clickable where it is
 * not.
 */
static void tab_label(const Ed *ed, int idx, int num, char *out,
                      size_t cap)
{
    (void)snprintf(out, cap, " %d %s%s ", num,
                   tab_basename(&ed->tabs.v.data[idx]),
                   yew_tab_modified(ed, idx)
                       ? yew_glyph(YEW_GLYPH_MODIFIED) : "");
}

static void tab_group_label(int number, const char *label, char *out,
                            size_t cap)
{
    char prefix[16];
    int wrote;
    size_t at;
    size_t keep;

    if (out == NULL || cap == 0U)
        return;
    out[0] = '\0';
    wrote = snprintf(prefix, sizeof(prefix), " %d ", number);
    if (wrote < 0)
        return;
    at = (size_t)wrote;
    if (at >= sizeof(prefix))
        at = sizeof(prefix) - 1U;
    if (at >= cap)
        at = cap - 1U;
    (void)memcpy(out, prefix, at);
    if (at + 1U >= cap) {
        out[at] = '\0';
        return;
    }
    keep = label == NULL ? 0U : strlen(label);
    if (keep > cap - at - 2U)
        keep = cap - at - 2U;
    if (keep != 0U)
        (void)memcpy(out + at, label, keep);
    at += keep;
    out[at++] = ' ';
    out[at] = '\0';
}

/*
 * Orphans — files outside the workspace root — render dim.  Both sides
 * are already canonical (the tab's path from yew_tab_open, the root
 * from the workspace), so this is a prefix test rather than a
 * filesystem call on a draw path.  Sprint 25 refines "outside".
 */
static bool tab_is_orphan(const Ed *ed, int idx)
{
    const char *root = yew_ws_root(ed);
    const char *p = ed->tabs.v.data[idx].path;

    /* Sprint 25 §6 refines "outside" as promised: a file that was gone
     * when we restored is dim for the same reason, and for a reason the
     * user cares about more. */
    if (ed->tabs.v.data[idx].missing_at_restore)
        return true;
    return p != NULL && root != NULL &&
           strncmp(p, root, strlen(root)) != 0;
}

int yew_tab_row1_entries(const Ed *ed, StripEntry *out, int cap)
{
    u32 seen[YEW_TAB_MAX];
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
             * padding always fits. */
            char label[YEW_TAB_LABEL_MAX - 4];
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
            if (nseen < (int)YEW_ARRAY_LEN(seen))
                seen[nseen++] = t->group_id;
            yew_group_label(ed, t->group_id, label, sizeof(label));
            /* Numbered like its neighbours: the row-1 position is what
             * `ctrl+N` (and `alt+N` from outside a group) addresses, so
             * a group must show the number that reaches it. */
            tab_group_label(n + 1, label, out[n].label,
                            sizeof(out[n].label));
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
        tab_label(ed, (int)i, n + 1, out[n].label, sizeof(out[n].label));
        out[n].payload = (i32)i;
        out[n].dim = tab_is_orphan(ed, (int)i);
        out[n].modified = yew_tab_modified(ed, (int)i);
        n++;
    }
    return n;
}

int yew_tab_row1_active(const Ed *ed, const StripEntry *entries, int n)
{
    u32 gid;
    int i;

    if (ed == NULL || entries == NULL || ed->tabs.active < 0)
        return -1;
    gid = yew_active_group_id(ed);
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

u32 yew_tab_strip_rows(const Ed *ed)
{
    StripEntry entries[YEW_TAB_MAX];
    int n;
    u32 rows = 0U;

    /*
     * A parameter, not a renderer: Sprint 22's layout reserves whatever
     * this returns before handing the rest to the pane tree, exactly as
     * it reserves the footer row.
     */
    if (ed == NULL)
        return 0U;
    n = yew_tab_row1_entries(ed, entries, (int)YEW_ARRAY_LEN(entries));
    /* Sprint 57.8: even one tab owns a row because its tail carries the
     * always-reachable new-untitled action. */
    if (n > 0)
        rows = 1U;
    /*
     * Inside a group the bar is two rows: row 1 keeps the group's own
     * entry visible so leaving is one press away, and row 2 lists the
     * members.  A member strip floating with no group above it reads as
     * a different widget every time you enter.
     */
    if (yew_active_group_id(ed) != 0U)
        rows = 2U;
    /* Sprint 27 §4: a dwell-opened preview needs the row too, or the
     * drop target it is offering has nowhere to be drawn. */
    if (yew_mouse_preview_group(ed) != 0U)
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

/*
 * One table per ROW.  The two rows scroll and permute independently
 * (Sprint 24 made that a law), so a single table would answer for
 * whichever row rendered last — and row 2 renders second.
 *
 * Row 1's holds the PRE-DRAG payloads, because the dwell has to ask
 * what was under the pointer before the preview moved it.  Row 2's
 * records CELLS ONLY: the member strip's targeting never needs to
 * un-permute a payload, and a `pre_payload` that nothing reads is a
 * field that can rot.
 */
typedef struct StripSlots {
    StripPreSlot v[YEW_TAB_MAX];
    int n;
    u16 y;
    u16 tail_x;
} StripSlots;

static StripSlots strip_row1;
static StripSlots strip_row2;

int yew_strip_slot_at(u16 x, u16 y)
{
    int i;

    /* The table only ever holds row 1, so the row is checked once
     * rather than stored per slot. */
    if (strip_row1.n == 0 || y != strip_row1.y)
        return -1;
    for (i = 0; i < strip_row1.n; i++) {
        if (x >= strip_row1.v[i].col0 && x < strip_row1.v[i].col1)
            return i;
    }
    return -1;
}

bool yew_strip_pre_payload(int slot, i32 *payload)
{
    if (slot < 0 || slot >= strip_row1.n || payload == NULL)
        return false;
    *payload = strip_row1.v[slot].pre_payload;
    return true;
}

bool yew_strip_slot_cells(int slot, u16 *col0, u16 *col1)
{
    if (slot < 0 || slot >= strip_row1.n || col0 == NULL || col1 == NULL)
        return false;
    /* Zero width is "the layout scrolled this one off", not "an empty
     * entry": strip_render_row1 clears the range and only the spans it
     * actually drew fill one in. */
    if (strip_row1.v[slot].col1 <= strip_row1.v[slot].col0)
        return false;
    *col0 = strip_row1.v[slot].col0;
    *col1 = strip_row1.v[slot].col1;
    return true;
}

int yew_strip_slot_count(void)
{
    return strip_row1.n;
}

u16 yew_strip_tail_x(void)
{
    return strip_row1.tail_x;
}

/*
 * Sprint 57.14 field repair: ROW 2'S SLOT TABLE.
 *
 * The positions the member strip drew, in the order it drew them —
 * including the GAP the carried tab is holding open, which is a
 * position like any other because that is where it lands.  Same law as
 * row 1's: placement is established once, while drawing, and the drag
 * aims with it rather than re-deriving it.
 */
int yew_strip_member_slot_count(void)
{
    return strip_row2.n;
}

bool yew_strip_member_slot_cells(int slot, u16 *col0, u16 *col1)
{
    if (slot < 0 || slot >= strip_row2.n || col0 == NULL || col1 == NULL)
        return false;
    if (strip_row2.v[slot].col1 <= strip_row2.v[slot].col0)
        return false; /* scrolled off, exactly as row 1 means it */
    *col0 = strip_row2.v[slot].col0;
    *col1 = strip_row2.v[slot].col1;
    return true;
}

/* Where row 2's blank tail begins — "put it last", aimable. */
u16 yew_strip_member_tail_x(void)
{
    return strip_row2.tail_x;
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
 * relative order — the same rule yew_tab_reorder commits with, applied
 * to the picture so the drop holds no surprise.
 */
static int apply_drag_preview(const Ed *ed, StripEntry *entries, int n,
                              int *active_entry)
{
    i32 held = 0;
    i32 ignored = 0;
    u16 fx = 0U;
    u16 fy = 0U;
    u16 grab = 0U;
    int to;
    int from = -1;
    int i;
    StripEntry moved;

    /*
     * The FLOAT decides whether an entry is held, not the target: a drag
     * whose pointer has not yet named a slot is still carrying
     * something, and the strip must not show it in two places.
     */
    if (!yew_mouse_drag_float(ed, &held, &fx, &fy, &grab))
        return -1;
    for (i = 0; i < n; i++) {
        if (entries[i].payload == held) {
            from = i;
            break;
        }
    }
    /* A member dragged off row 2 has no row-1 entry to hold open. */
    if (from < 0)
        return -1;
    if (n <= 1 || !yew_mouse_drag_preview(ed, &ignored, &to))
        return from; /* no target yet: the gap stays where it was lifted */
    if (to < 0)
        to = 0;
    if (to >= n)
        to = n - 1;
    moved = entries[from];
    if (from < to) {
        (void)memmove(&entries[from], &entries[from + 1],
                      sizeof(entries[0]) * (size_t)(to - from));
    } else if (to < from) {
        (void)memmove(&entries[to + 1], &entries[to],
                      sizeof(entries[0]) * (size_t)(from - to));
    }
    entries[to] = moved;
    if (active_entry != NULL && *active_entry >= 0)
        *active_entry = yew_tab_shifted_index(*active_entry, from, to);
    /*
     * The GAP.  Sprint 27 drew this entry dim, in place, as a ghost;
     * 57.14 draws it at the pointer instead and leaves the permuted
     * position blank, so the strip shows exactly where the drop lands
     * and the held entry exists exactly once.
     */
    return to;
}

static void strip_render(Ed *ed, Rect rect, StripEntry *entries, int n,
                         int active_entry, int *scroll, bool user_scroll,
                         i32 scroll_mag, StripSlots *slots, bool draw_new,
                         int held_idx);

/*
 * Sprint 57.15 §2: did EITHER row put a chevron on screen this draw?
 *
 * A product of the render, like the region table and the pre-drag slot
 * table, and set at the same statement that registers the scroll
 * region — so "a chevron is drawn" and "there is something to hover"
 * cannot drift apart.  yew_tab_strip_draw resets it and hands the
 * answer to the router, which owns mode 1003.
 */
static bool strip_any_chevron;

/*
 * Row 1, with the drag preview applied and the pre-drag list recorded.
 *
 * `pre` is the list BEFORE the permutation; `entries` is what gets
 * drawn.  The two are the same array position by position when no drag
 * is in flight, which is exactly why the dwell's fixture has to force
 * them apart to prove it reads the right one.
 */
static void strip_render_row1(Ed *ed, Rect rect, StripEntry *entries,
                              int n, int active_entry, int *scroll,
                              bool user_scroll)
{
    StripEntry pre[YEW_TAB_MAX];
    int held;
    int i;

    if (n > (int)YEW_ARRAY_LEN(pre))
        n = (int)YEW_ARRAY_LEN(pre);
    if (n > 0)
        (void)memcpy(pre, entries, sizeof(pre[0]) * (size_t)n);
    held = apply_drag_preview(ed, entries, n, &active_entry);
    /* Cleared before the render fills the cell ranges in: a slot the
     * layout scrolled out of view must not keep last frame's cells and
     * answer for a position nobody can point at. */
    strip_row1.n = n;
    strip_row1.y = rect.y;
    strip_row1.tail_x = rect.x;
    for (i = 0; i < n; i++) {
        strip_row1.v[i].col0 = 0U;
        strip_row1.v[i].col1 = 0U;
        strip_row1.v[i].pre_payload = pre[i].payload;
    }
    strip_render(ed, rect, entries, n, active_entry, scroll, user_scroll,
                 1, &strip_row1, true, held);
}

static ThemeEnt tab_base_style(const Ed *ed)
{
    const ThemeEnt *fg = yew_theme_ui_tab(ed, "fg");
    const ThemeEnt *bg = yew_theme_ui_tab(ed, "bg");
    ThemeEnt style = {
        {YEW_COLOR_DEFAULT, 0U, 0U, 0U},
        {YEW_COLOR_DEFAULT, 0U, 0U, 0U},
        0U
    };

    if (fg != NULL) {
        if (fg->fg.tag != YEW_COLOR_DEFAULT)
            style.fg = fg->fg;
        style.attrs = fg->attrs;
    }
    if (bg != NULL) {
        if (bg->bg.tag != YEW_COLOR_DEFAULT)
            style.bg = bg->bg;
        style.attrs = (u16)(style.attrs | bg->attrs);
    }
    return style;
}

/*
 * A UI role is an overlay, not a replacement: foreground-only custom
 * roles must retain the strip surface, and background-only roles must
 * retain readable text.  Presence still owns attrs so `mono: "plain"`
 * can deliberately clear a colorful rendition's emphasis.
 */
static ThemeEnt tab_role_style(const Ed *ed, const char *role,
                               ThemeEnt fallback)
{
    const ThemeEnt *themed = yew_theme_ui_tab(ed, role);

    if (themed == NULL)
        return fallback;
    if (themed->fg.tag != YEW_COLOR_DEFAULT)
        fallback.fg = themed->fg;
    if (themed->bg.tag != YEW_COLOR_DEFAULT)
        fallback.bg = themed->bg;
    fallback.attrs = themed->attrs;
    return fallback;
}

static Cell tab_blank(ThemeEnt style)
{
    Cell blank;

    (void)memset(&blank, 0, sizeof(blank));
    blank.fg = style.fg;
    blank.bg = style.bg;
    blank.attrs = style.attrs;
    return blank;
}

/*
 * Sprint 57.15 §1: `user_scroll` says the offset belongs to the user,
 * and the layout is then told there is no entry to follow (−1).  The
 * highlight still uses `active_entry`, because which entry is active
 * and which entry the placement chases are two different questions —
 * conflating them is what made an explicit scroll last exactly one
 * frame.  See ui/strip.h.
 */
static void strip_render(Ed *ed, Rect rect, StripEntry *entries, int n,
                         int active_entry, int *scroll, bool user_scroll,
                         i32 scroll_mag, StripSlots *slots, bool draw_new,
                         int held_idx)
{
    StripSpan spans[YEW_TAB_MAX];
    int n_spans = 0;
    bool more_left = false;
    bool more_right = false;
    int i;
    u16 avail;
    u16 tail_x;
    ThemeEnt base;
    ThemeEnt surface;
    ThemeEnt inactive;
    ThemeEnt modified;
    ThemeEnt orphan;
    ThemeEnt active;
    ThemeEnt add;
    u32 flash_gid;

    if (rect.w == 0U || rect.h == 0U)
        return;
    /*
     * Sprint 57.14 §3: the dwell's cue.  Asked once per render and
     * computed from ed->now_ms, never from a frame counter — the same
     * state and the same clock must paint the same cells (invariant 5).
     */
    flash_gid = yew_mouse_dwell_flash(ed);
    base = tab_base_style(ed);
    surface = tab_role_style(ed, "tab.bar", base);
    inactive = tab_role_style(ed, "tab.inactive", surface);
    modified = surface;
    modified.attrs = (u16)(modified.attrs | YEW_ATTR_BOLD);
    modified = tab_role_style(ed, "tab.modified", modified);
    orphan = surface;
    orphan.attrs = (u16)(orphan.attrs | YEW_ATTR_DIM);
    orphan = tab_role_style(ed, "tab.orphan", orphan);
    active = surface;
    active.attrs = (u16)(active.attrs | YEW_ATTR_REVERSE);
    active = tab_role_style(ed, "tab.active", active);
    add = surface;
    add.attrs = (u16)(add.attrs | YEW_ATTR_BOLD);
    add = tab_role_style(ed, "tab.add", add);
    /* Blank the row first: a shorter strip than last frame must not
     * leave the tail of the old one behind. */
    yew_grid_fill(&ed->grid, rect.y, rect.x, (u16)(rect.x + rect.w),
                  tab_blank(surface));
    if (n <= 0)
        return;

    /* Reserve a cell for `<` when scrolled, so the indicator never
     * overlaps the first entry it is pointing away from. */
    avail = rect.w;
    if (*scroll > 0 && avail > 1U)
        avail = (u16)(avail - 1U);
    yew_strip_layout(entries, n, avail, user_scroll ? -1 : active_entry,
                     scroll, spans, &n_spans, &more_left, &more_right);
    tail_x = (u16)(rect.x + (more_left ? 1U : 0U));

    for (i = 0; i < n_spans; i++) {
        int idx = spans[i].idx;
        u16 x = (u16)(rect.x + spans[i].col0 + (more_left ? 1U : 0U));
        ThemeEnt style = inactive;
        Rect span_rect;

        if (idx == active_entry)
            style = active;
        else if (entries[idx].dim)
            style = orphan;
        else if (entries[idx].modified)
            style = modified;
        /*
         * The cue TOGGLES reverse rather than substituting a style, so
         * it is visible whether or not the group is the active entry —
         * a flash that painted "active" over the active entry would
         * announce nothing at all.
         */
        if (flash_gid != 0U && entries[idx].payload == -(i32)flash_gid)
            style.attrs = (u16)(style.attrs ^ YEW_ATTR_REVERSE);
        span_rect = (Rect){x, rect.y,
                           (u16)(spans[i].col1 - spans[i].col0), 1U};
        if (idx != held_idx) {
            /* Draw only the bytes that fit the span the layout gave us.
             * Drawing the whole label writes its tail over the next
             * entry — which is exactly what the golden caught. */
            (void)yew_grid_puts(&ed->grid, rect.y, x,
                                (const u8 *)entries[idx].label,
                                yew_strip_label_bytes(entries[idx].label),
                                style.fg, style.bg, style.attrs);
            /*
             * Registered with the SAME cells the layout produced and the
             * draw used.  Recomputing this from strlen while hit-testing
             * is the multibyte click-shift the Sprint 22 law forbids.
             */
            yew_region_add(YEW_REGION_TAB, span_rect, entries[idx].payload);
        }
        /*
         * Sprint 57.14 §2: the held entry keeps its CELLS and loses its
         * ink and its region.  The row was blanked above, so what is
         * left is a gap exactly its width, sitting where the drop lands;
         * registering it would hand the pointer a target for the thing
         * it is already holding.  The pre-drag slot below is still
         * recorded, because that is what the drag aims with.
         */
        tail_x = (u16)(x + span_rect.w);
        /*
         * Sprint 27 §4.  The SAME cells, against the pre-drag list —
         * `idx` is a position in the visible strip, and the pre-drag
         * table is indexed by position for exactly that reason.  A
         * second derivation of where a slot sits is the multibyte
         * click-shift the Sprint 22 law forbids.
         */
        if (slots != NULL && idx >= 0 && idx < slots->n) {
            slots->v[idx].col0 = x;
            slots->v[idx].col1 = (u16)(x + span_rect.w);
            if (slots->v[idx].col1 > slots->tail_x)
                slots->tail_x = slots->v[idx].col1;
        }
    }
    if (more_left) {
        Rect r = {rect.x, rect.y, 1U, 1U};

        (void)yew_grid_puts(&ed->grid, rect.y, rect.x,
                            (const u8 *)yew_glyph(YEW_GLYPH_MORE_LEFT),
                            yew_glyph_len(YEW_GLYPH_MORE_LEFT), orphan.fg,
                            surface.bg, YEW_ATTR_DIM);
        yew_region_add(YEW_REGION_TAB_SCROLL, r, -scroll_mag);
        strip_any_chevron = true;
    }
    if (more_right) {
        char more[16];
        int past = n - (n_spans > 0 ? spans[n_spans - 1].idx + 1 : 0);
        u16 w;
        u16 x;
        Rect r;

        (void)snprintf(more, sizeof(more), "%s%d",
                       yew_glyph(YEW_GLYPH_MORE_RIGHT), past);
        w = (u16)strlen(more);
        if (w < rect.w) {
            x = (u16)(rect.x + rect.w - w);
            (void)yew_grid_puts(&ed->grid, rect.y, x, (const u8 *)more,
                                strlen(more), orphan.fg, surface.bg,
                                YEW_ATTR_DIM);
            r = (Rect){x, rect.y, w, 1U};
            yew_region_add(YEW_REGION_TAB_SCROLL, r, scroll_mag);
            strip_any_chevron = true;
        }
    } else if (draw_new &&
               (u32)tail_x + 3U <= (u32)rect.x + rect.w) {
        Rect r = {tail_x, rect.y, 3U, 1U};

        (void)yew_grid_puts(&ed->grid, rect.y, tail_x,
                            (const u8 *)" + ", 3U,
                            add.fg, add.bg, add.attrs);
        yew_region_add(YEW_REGION_TAB_NEW, r, 0);
    }
}

/*
 * Row 2: the members of `gid`, one entry each, payload = GLOBAL tab
 * index — so the existing YEW_REGION_TAB click case handles them
 * unchanged.  The active member is reversed; the others render dim,
 * which is what makes row 2 read as secondary to row 1.
 *
 * This is also the hover-preview renderer (Sprint 27 calls it with a
 * group the user is only pointing at).  Same function, so the pinned
 * row and the preview cannot disagree.
 */
void yew_tab_member_strip_draw(Ed *ed, Rect rect, u32 gid)
{
    StripEntry entries[YEW_TAB_MAX];
    int members[YEW_TAB_MAX];
    int order[YEW_TAB_MAX];
    int n;
    int n_draw;
    int i;
    int active_entry = -1;
    int held = -1;
    i32 held_payload = 0;
    u16 fx = 0U;
    u16 fy = 0U;
    u16 grab = 0U;
    bool floating;
    u32 want_gid = 0U;
    int to = -1;

    if (ed == NULL || gid == 0U)
        return;
    /*
     * Row 2 hides the held member for the same reason row 1 does: a tab
     * dragged off the member strip is being carried at the pointer, and
     * it may not also be sitting in the row it was lifted from.
     */
    floating = yew_mouse_drag_float(ed, &held_payload, &fx, &fy, &grab);
    n = yew_group_members(ed, gid, members, (int)YEW_ARRAY_LEN(members));
    for (i = 0; i < n; i++)
        order[i] = members[i];
    n_draw = n;
    /*
     * Sprint 57.14 field repair: THE GAP, on row 2 as well.
     *
     * Row 2 already accepted a drop and showed nothing about it, so the
     * picture and the outcome disagreed.  The members now open a space
     * exactly where the release lands — permuted when the carried tab is
     * already a member of this group, INSERTED when it is joining from
     * outside, because in that case the list it lands in is one longer
     * than the one on screen.
     *
     * Only while ROW 2 OWNS THE PREVIEW: `yew_mouse_drag_member_preview`
     * is false the moment the pointer goes back to row 1, and then this
     * row closes up again.
     */
    if (floating && held_payload >= 0 && n_draw < (int)YEW_ARRAY_LEN(order) &&
        yew_mouse_drag_member_preview(ed, &want_gid, &to) &&
        want_gid == gid) {
        int from = -1;

        for (i = 0; i < n_draw; i++) {
            if (order[i] == held_payload) {
                from = i;
                break;
            }
        }
        if (from >= 0) {
            /* A reorder inside the group: the list keeps its length. */
            if (to < 0)
                to = 0;
            if (to > n_draw - 1)
                to = n_draw - 1;
            if (from < to) {
                (void)memmove(&order[from], &order[from + 1],
                              sizeof(order[0]) * (size_t)(to - from));
            } else if (to < from) {
                (void)memmove(&order[to + 1], &order[to],
                              sizeof(order[0]) * (size_t)(from - to));
            }
        } else {
            /* A JOIN: the list grows by the entry being carried in. */
            if (to < 0)
                to = 0;
            if (to > n_draw)
                to = n_draw;
            (void)memmove(&order[to + 1], &order[to],
                          sizeof(order[0]) * (size_t)(n_draw - to));
            n_draw++;
        }
        order[to] = held_payload;
        held = to;
    }
    for (i = 0; i < n_draw; i++) {
        (void)memset(&entries[i], 0, sizeof(entries[i]));
        /*
         * Sprint 57.10: numbered 1..n so `alt+N` inside the group has a
         * visible target — the digit counts what row 2 SHOWS, which is
         * why the labels are built after the permutation and not before
         * it.  A gap that kept its old number would name a position it
         * is no longer in.
         */
        (void)snprintf(entries[i].label, sizeof(entries[i].label),
                       " %d %s%s ", i + 1,
                       tab_basename(&ed->tabs.v.data[order[i]]),
                       yew_tab_modified(ed, order[i])
                           ? yew_glyph(YEW_GLYPH_MODIFIED) : "");
        entries[i].payload = order[i];
        entries[i].dim = tab_is_orphan(ed, order[i]);
        entries[i].modified = yew_tab_modified(ed, order[i]);
        if (order[i] == ed->tabs.active)
            active_entry = i;
        if (held < 0 && floating && held_payload >= 0 &&
            entries[i].payload == held_payload)
            held = i;
    }
    /* Cleared before the render fills the cell ranges in, exactly as
     * row 1 does it: a position the layout scrolled out of view must not
     * keep last frame's cells. */
    strip_row2.n = n_draw;
    strip_row2.y = rect.y;
    strip_row2.tail_x = rect.x;
    for (i = 0; i < n_draw; i++) {
        strip_row2.v[i].col0 = 0U;
        strip_row2.v[i].col1 = 0U;
        strip_row2.v[i].pre_payload = order[i];
    }
    strip_render(ed, rect, entries, n_draw, active_entry,
                 &ed->tabs.member_scroll,
                 yew_tabs_scroll_is_owned(&ed->tabs, true),
                 2, &strip_row2, false, held);
}


/* ---------------------------------------------------------------- */
/* Sprint 57.14 §2: the float                                        */
/* ---------------------------------------------------------------- */

static Rect strip_float;

Rect yew_strip_float_rect(void)
{
    return strip_float;
}

/*
 * The float's label, built from the payload rather than lifted from the
 * strip's entry list — a member dragged off row 2 has no row-1 entry to
 * copy, and the float must look the same whichever row it came from.
 *
 * No leading NUMBER, unlike every label on a row: the numbers address
 * positions, and a float is between positions.  One that carried a
 * number would be naming a slot it is not in.
 */
static void strip_float_label(const Ed *ed, i32 payload, char *out,
                              size_t cap)
{
    if (out == NULL || cap == 0U)
        return;
    out[0] = '\0';
    if (payload < 0) {
        char label[YEW_TAB_LABEL_MAX - 4];

        yew_group_label(ed, (u32)(-payload), label, sizeof(label));
        (void)snprintf(out, cap, " %s ", label);
        return;
    }
    {
        const Tab *t = yew_tab_at_const(ed, (int)payload);

        if (t == NULL)
            return;
        (void)snprintf(out, cap, " %s%s ", tab_basename(t),
                       yew_tab_modified(ed, (int)payload)
                           ? yew_glyph(YEW_GLYPH_MODIFIED) : "");
    }
}

/*
 * Drawn LAST, after both rows, so the row the pointer happens to be over
 * cannot paint over the thing the pointer is carrying.
 *
 * IT REGISTERS NO REGION, and that is not an oversight in the Sprint 22
 * law: a region maps a cell to something the user can aim at, and these
 * cells are already in the user's hand.  Registering them would make the
 * pointer hover the held entry wherever it went, which is exactly the
 * "you are hovering the thing you are holding" failure the pre-drag slot
 * table exists to avoid.  `yew_strip_float_rect` exists so a test can
 * hit-test every cell it covers and prove the registry stayed quiet.
 */
static void strip_draw_float(Ed *ed)
{
    char label[YEW_TAB_LABEL_MAX];
    i32 payload = 0;
    u16 px = 0U;
    u16 py = 0U;
    u16 grab = 0U;
    u16 w;
    u16 x0;
    u16 end;
    ThemeEnt style;

    strip_float = (Rect){0U, 0U, 0U, 0U};
    if (ed == NULL || !ed->grid_ready || ed->grid.cols == 0U ||
        ed->grid.rows == 0U)
        return;
    if (!yew_mouse_drag_float(ed, &payload, &px, &py, &grab))
        return;
    strip_float_label(ed, payload, label, sizeof(label));
    if (label[0] == '\0')
        return;
    w = yew_strip_label_cells(label);
    if (w == 0U)
        return;
    if (w > ed->grid.cols)
        w = ed->grid.cols;
    /* The grip the press took, clamped into the grid on both axes: a
     * float half off the screen reads as a rendering fault rather than
     * as a tab held near the edge. */
    x0 = px > grab ? (u16)(px - grab) : 0U;
    if ((u32)x0 + (u32)w > (u32)ed->grid.cols)
        x0 = (u16)(ed->grid.cols - w);
    if (py >= ed->grid.rows)
        py = (u16)(ed->grid.rows - 1U);
    /* The same chain the strip's own active entry is built from — base,
     * then the bar's surface, then the active role — so a theme that
     * colours only the foreground of `tab.active` keeps the bar under
     * the float instead of falling back to the editor's default. */
    style = tab_role_style(
        ed, "tab.active",
        tab_role_style(ed, "tab.bar", tab_base_style(ed)));
    style.attrs = (u16)(style.attrs | YEW_ATTR_REVERSE | YEW_ATTR_BOLD);
    end = yew_grid_puts(&ed->grid, py, x0, (const u8 *)label,
                        yew_strip_label_bytes(label), style.fg, style.bg,
                        style.attrs);
    strip_float = (Rect){x0, py, (u16)(end > x0 ? end - x0 : 0), 1U};
}

static void strip_draw_rows(Ed *ed, Rect rect)
{
    StripEntry entries[YEW_TAB_MAX];
    int n;
    u32 gid;

    if (rect.w == 0U || rect.h == 0U)
        return;
    n = yew_tab_row1_entries(ed, entries, (int)YEW_ARRAY_LEN(entries));
    strip_render_row1(ed, (Rect){rect.x, rect.y, rect.w, 1U}, entries, n,
                      yew_tab_row1_active(ed, entries, n), &ed->tabs.scroll,
                      yew_tabs_scroll_is_owned(&ed->tabs, false));
    gid = yew_active_group_id(ed);
    /*
     * Sprint 27 §4: a dwell opens a group's member strip as a drop
     * target, so row 2 shows the PREVIEWED group when there is one.
     * Same renderer as the pinned row, so the two cannot disagree about
     * placement — which is the whole reason s24 wrote it as one
     * function.
     */
    if (yew_mouse_preview_group(ed) != 0U)
        gid = yew_mouse_preview_group(ed);
    if (rect.h >= 2U && gid != 0U)
        yew_tab_member_strip_draw(ed,
                                  (Rect){rect.x, (u16)(rect.y + 1U),
                                         rect.w, 1U},
                                  gid);
    /* Last, and over everything the strip just drew. */
    strip_draw_float(ed);
}

void yew_tab_strip_draw(Ed *ed, Rect rect)
{
    if (ed == NULL)
        return;
    /*
     * Reset BEFORE the guard inside strip_draw_rows, and report after
     * it whatever happened: a strip with no rows reserved draws no
     * chevron, and mode 1003 must come down for that as surely as for a
     * chevron that scrolled away.  A `return` in the middle of the draw
     * would otherwise leave the router armed against last frame.
     */
    strip_any_chevron = false;
    strip_draw_rows(ed, rect);
    yew_mouse_note_chevrons(strip_any_chevron);
}

/*
 * Click routing for the strip.  Everything that is not a tab span or a
 * scroll indicator is IGNORED — Sprint 27 owns wheel, drag-reorder and
 * the context menu, and a click half-handled here would move a tab the
 * user meant to scroll past.
 */
bool yew_tab_strip_click(Ed *ed, u16 x, u16 y)
{
    Region hit;

    if (ed == NULL)
        return false;
    hit = yew_region_hit(x, y);
    if (hit.kind == YEW_REGION_TAB_SCROLL) {
        /* The payload's MAGNITUDE names the row (1 or 2); its sign is
         * the direction.  Without the row, a click on row 2's `>N`
         * would scroll row 1 under the user's pointer. */
        bool row2 = hit.payload == 2 || hit.payload == -2;
        int *scroll = row2 ? &ed->tabs.member_scroll : &ed->tabs.scroll;
        int limit = (int)ed->tabs.v.len;
        int to = *scroll + (hit.payload < 0 ? -1 : 1);

        if (row2)
            limit = yew_group_member_count(ed, yew_active_group_id(ed));
        if (to < 0)
            to = 0;
        if (to >= limit)
            to = limit > 0 ? limit - 1 : 0;
        *scroll = to;
        /* Sprint 57.15 §1: the user aimed at the chevron, so this
         * offset is theirs until the active entry moves. */
        yew_tabs_scroll_owned(&ed->tabs, row2);
        ed->full_damage = true;
        return true;
    }
    if (hit.kind != YEW_REGION_TAB)
        return false;
    /*
     * The sign convention region.h wrote down in Sprint 22, now live:
     * a negative payload is a GROUP id, negated.  One region kind, and
     * the renderer and the router read the same rule.
     */
    if (hit.payload < 0) {
        yew_group_note_position(ed);
        /* Clicking a group's entry is an EXPLICIT entry, so it resumes
         * where the user left off — unlike a mid-walk arrival, which
         * enters from the side it came from. */
        yew_group_enter(ed, (u32)(-hit.payload));
        return true;
    }
    yew_tab_switch(ed, hit.payload);
    return true;
}

/* ---------------------------------------------------------------- */
/* Sprint 23 §5/§6: commands and the dirty-close prompt             */
/* ---------------------------------------------------------------- */

CmdStatus yew_tab_cmd_new(CmdCtx *cx)
{
    int idx;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    idx = yew_tab_open(cx->ed, NULL);
    /* The return value is checked at EVERY call site (DoD 6): a silent
     * cap failure is how facsimile loaded a new file into the
     * still-active tab and then wrote it over the old path. */
    if (idx < 0)
        return YEW_CMD_ERR_STATE;
    yew_tab_switch(cx->ed, idx);
    return YEW_CMD_OK;
}

CmdStatus yew_tab_cmd_open(CmdCtx *cx)
{
    int idx;

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL)
        return YEW_CMD_ERR_ARG;
    idx = yew_tab_open(cx->ed, cx->sarg);
    if (idx < 0)
        return YEW_CMD_ERR_STATE;
    yew_tab_switch(cx->ed, idx);
    return YEW_CMD_OK;
}

static CmdStatus tab_step(CmdCtx *cx, int delta)
{
    int n;
    int at;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    n = (int)yew_tab_count(cx->ed);
    if (n <= 1)
        return YEW_CMD_OK;
    at = cx->ed->tabs.active + delta;
    /* Cyclic, because next-from-the-last meaning "nothing" is a worse
     * answer than wrapping when there is a strip showing the ring. */
    while (at < 0)
        at += n;
    yew_tab_switch(cx->ed, at % n);
    return YEW_CMD_OK;
}

CmdStatus yew_tab_cmd_next(CmdCtx *cx)
{
    return tab_step(cx, 1);
}

CmdStatus yew_tab_cmd_prev(CmdCtx *cx)
{
    return tab_step(cx, -1);
}

/* ---------------------------------------------------------------- */
/* Sprint 24 §7 / 57.10: numbered jumps and the 500 ms window       */
/* ---------------------------------------------------------------- */

/*
 * A number addresses a ROW.  Row 1 counts its entries left to right —
 * a group is one entry — and row 2 counts the active group's members.
 * `alt+N` addresses the row the active tab lives on; `ctrl+N` always
 * addresses row 1, which is how it walks between groups from anywhere.
 *
 * Both rows number what the user is looking at, which is the whole
 * reason the numbering is positional rather than the array index: with
 * a group in the middle of the bar the flat indices skip, and `alt+4`
 * would land on a tab labelled 5.
 */
typedef enum JumpMode {
    JUMP_ROW1,
    JUMP_MEMBER
} JumpMode;

/*
 * Module-local, because nothing outside this file has any business
 * knowing a jump is half-finished.
 */
static i64 jump_value;
static i64 jump_deadline_ms;
static JumpMode jump_mode;
static bool jump_on;
static TimerId jump_timer;

bool yew_tab_jump_armed(void)
{
    return jump_on;
}

void yew_tab_jump_clear(Ed *ed)
{
    if (ed != NULL && jump_timer != YEW_TIMER_NONE) {
        (void)yew_timer_cancel(&ed->timers, jump_timer);
        jump_timer = YEW_TIMER_NONE;
    }
    jump_on = false;
    jump_value = 0;
    jump_deadline_ms = 0;
    jump_mode = JUMP_ROW1;
}

/*
 * Row-1 entry `pos` (1-based).  An ungrouped entry is switched to; a
 * group entry is ENTERED — resume at its last-active member, exactly
 * as `t <down>` and a row-1 click do, so three routes into a group
 * cannot land on three different members.  False when there is no such
 * entry, and nothing has moved.
 *
 * The position is noted BEFORE the switch: leaving a group by number is
 * still leaving it, and coming back must resume here.
 */
static bool jump_to_row1(Ed *ed, i64 pos)
{
    StripEntry entries[YEW_TAB_MAX];
    int n = yew_tab_row1_entries(ed, entries, (int)YEW_ARRAY_LEN(entries));
    i32 payload;

    if (pos < 1 || pos > (i64)n)
        return false;
    payload = entries[pos - 1].payload;
    yew_group_note_position(ed);
    if (payload < 0)
        yew_group_enter(ed, (u32)-payload);
    else
        yew_tab_switch(ed, payload);
    return true;
}

/* Member `pos` (1-based, by ordinal) of `gid` — what row 2 shows. */
static bool jump_to_member(Ed *ed, u32 gid, i64 pos)
{
    int members[YEW_TAB_MAX];
    int n = yew_group_members(ed, gid, members, (int)YEW_ARRAY_LEN(members));

    if (pos < 1 || pos > (i64)n)
        return false;
    yew_tab_switch(ed, members[pos - 1]);
    return true;
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
    jump_timer = YEW_TIMER_NONE;
    yew_tab_jump_clear(ed);
    yew_msg_clear(ed);
    ed->footer_dirty = true;
}

/*
 * Announces what a further digit would do.  The window must never feel
 * like a lost keystroke — and the hint names the ROW, so the status
 * line never promises a tab when a member is meant.
 */
static void jump_arm(Ed *ed, JumpMode mode, i64 value)
{
    yew_tab_jump_clear(ed);
    jump_on = true;
    jump_mode = mode;
    jump_value = value;
    jump_deadline_ms = ed->now_ms + YEW_JUMP_WINDOW_MS;
    jump_timer = yew_timer_add(&ed->timers, jump_deadline_ms, jump_expire,
                               NULL);
    yew_msg(ed, YEW_MSG_INFO, "%s %lld — a digit extends to %lld_",
            mode == JUMP_MEMBER ? "member" : "tab", (long long)value,
            (long long)value);
}

/* The mode `alt+N` would pick right now: the row the active tab is on. */
static JumpMode jump_mode_for_alt(const Ed *ed)
{
    return yew_active_group_id(ed) != 0U ? JUMP_MEMBER : JUMP_ROW1;
}

bool yew_tab_jump_key(Ed *ed, Key key)
{
    i64 target;
    bool ok;

    if (ed == NULL || !jump_on)
        return false;
    if (key.ev == YEW_KEY_RELEASE)
        return false;
    /*
     * Clear FIRST, then let the key dispatch normally.  Returning
     * without clearing would let a digit typed much later read as a
     * continuation of a jump the user has long forgotten.
     */
    if (ed->now_ms >= jump_deadline_ms) {
        yew_tab_jump_clear(ed);
        return false;
    }
    if (key.code < (u32)'0' || key.code > (u32)'9') {
        yew_tab_jump_clear(ed);
        return false;
    }
    /*
     * Bare `5` continues whatever window is open: `alt+1` `5` is tab
     * 15.  A HELD modifier continues only a window of its own kind —
     * `ctrl` digits are row-1 numbers, `alt` digits count the row the
     * active tab is on — and otherwise the window clears and the key
     * dispatches as the fresh jump it reads as.  That is what makes
     * `alt+3` (entering a group from row 1) then `alt+2` pick the
     * group's second member, which is what the bar is now showing.
     */
    if ((key.mods & YEW_MOD_CTRL) != 0U && jump_mode != JUMP_ROW1) {
        yew_tab_jump_clear(ed);
        return false;
    }
    if ((key.mods & YEW_MOD_ALT) != 0U && jump_mode != jump_mode_for_alt(ed)) {
        yew_tab_jump_clear(ed);
        return false;
    }
    /* Here the digit is a DIGIT, not the tenth key: `1` then `0` is
     * entry 10, and `1` then `5` is entry 15. */
    target = jump_value * 10 + (i64)(key.code - (u32)'0');
    if (jump_mode == JUMP_MEMBER) {
        u32 gid = yew_active_group_id(ed);

        ok = gid != 0U && jump_to_member(ed, gid, target);
        if (!ok)
            yew_msg(ed, YEW_MSG_ERROR, "no member %lld", (long long)target);
    } else {
        ok = jump_to_row1(ed, target);
        if (!ok)
            yew_msg(ed, YEW_MSG_ERROR, "no tab %lld", (long long)target);
    }
    if (ok) {
        /* Re-armed, so three digits work. */
        jump_arm(ed, jump_mode, target);
    } else {
        yew_tab_jump_clear(ed);
    }
    /*
     * Consumed either way.  The digit was part of a chord, so it must
     * not fall through and be inserted into the document.
     */
    return true;
}

/* The number a goto command was given: iarg, a count overriding it, and
 * 0 meaning 10 — the digit row reads 1..9 then 0, so `0` is the tenth
 * key, not the zeroth thing. */
static i64 goto_want(const CmdCtx *cx)
{
    i64 want = cx->iarg;

    if (cx->count_given && cx->count != 0U)
        want = (i64)cx->count;
    return want == 0 ? 10 : want;
}

/*
 * Jumps NOW and arms the window (§7).  The switch is the whole command;
 * arming is what lets a second digit supersede it without the first
 * jump having waited for one.
 */
static CmdStatus goto_row1(Ed *ed, i64 want)
{
    if (!jump_to_row1(ed, want)) {
        yew_msg(ed, YEW_MSG_ERROR, "no tab %lld", (long long)want);
        return YEW_CMD_ERR_ARG;
    }
    jump_arm(ed, JUMP_ROW1, want);
    return YEW_CMD_OK;
}

CmdStatus yew_tab_cmd_goto(CmdCtx *cx)
{
    i64 want;
    u32 gid;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    want = goto_want(cx);
    gid = yew_active_group_id(cx->ed);
    if (gid == 0U)
        return goto_row1(cx->ed, want);
    /* Inside a group the number counts row 2 — the members. */
    if (!jump_to_member(cx->ed, gid, want)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "no member %lld", (long long)want);
        return YEW_CMD_ERR_ARG;
    }
    jump_arm(cx->ed, JUMP_MEMBER, want);
    return YEW_CMD_OK;
}

CmdStatus yew_tab_cmd_goto_bar(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    return goto_row1(cx->ed, goto_want(cx));
}

CmdStatus yew_tab_cmd_move(CmdCtx *cx)
{
    i64 want;
    int to;

    if (cx == NULL || cx->ed == NULL || cx->ed->tabs.active < 0)
        return YEW_CMD_ERR_STATE;
    want = cx->iarg;
    if (cx->count_given && cx->count != 0U)
        want = (i64)cx->count;
    to = (int)want - 1;
    if (to < 0 || to >= (int)yew_tab_count(cx->ed)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "no position %lld",
                (long long)want);
        return YEW_CMD_ERR_ARG;
    }
    yew_tab_reorder(cx->ed, cx->ed->tabs.active, to);
    cx->ed->full_damage = true;
    return YEW_CMD_OK;
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
    int idx = yew_tab_index_of_id(ed, ed->tab_prompt.tab_id);
    const Tab *t;

    if (idx < 0) {
        ed->tab_prompt.active = false;
        return;
    }
    t = yew_tab_at(ed, idx);
    yew_msg(ed, YEW_MSG_INFO,
            "save changes to %s?  [w]rite  [d]iscard  [esc] cancel",
            t->path != NULL ? t->path : "untitled");
}

CmdStatus yew_tab_cmd_close(CmdCtx *cx)
{
    Ed *ed;
    int idx;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    idx = ed->tabs.active;
    if (idx < 0)
        return YEW_CMD_ERR_STATE;
    if (yew_tab_count(ed) <= 1U) {
        yew_msg(ed, YEW_MSG_ERROR, "cannot close the last tab");
        return YEW_CMD_ERR_STATE;
    }
    if (!yew_tab_modified(ed, idx)) {
        (void)yew_tab_close(ed, idx);
        return YEW_CMD_OK;
    }
    ed->tab_prompt.tab_id = yew_tab_at(ed, idx)->tab_id;
    ed->tab_prompt.active = true;
    tab_prompt_show(ed);
    return YEW_CMD_OK;
}

/*
 * Sprint 27 §5: the tab context menu's rows, as registry commands.
 *
 * They exist as commands rather than as menu-only handlers because of
 * invariant 9: every mouse action has a keyboard path, and the menu row
 * and the key must reach the same code rather than two implementations
 * that drift.
 */
CmdStatus yew_tab_cmd_close_others(CmdCtx *cx)
{
    Ed *ed;
    u32 keep;
    u32 doomed[YEW_TAB_MAX];
    u32 n = 0U;
    u32 i;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    if (ed->tabs.active < 0)
        return YEW_CMD_ERR_STATE;
    if (yew_tab_count(ed) <= 1U) {
        yew_msg(ed, YEW_MSG_ERROR, "no other tabs");
        return YEW_CMD_ERR_STATE;
    }
    keep = yew_tab_at(ed, ed->tabs.active)->tab_id;
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
        int idx = yew_tab_index_of_id(ed, doomed[i]);

        /* A modified tab needs an answer, and the answer prompt is
         * per tab; refuse the whole gesture rather than closing half of
         * them and leaving a dialog holding the rest. */
        if (idx < 0)
            continue;
        if (yew_tab_modified(ed, idx)) {
            yew_msg(ed, YEW_MSG_ERROR,
                    "unsaved changes in another tab; save or force first");
            return YEW_CMD_ERR_STATE;
        }
    }
    for (i = 0U; i < n; i++) {
        int idx = yew_tab_index_of_id(ed, doomed[i]);

        if (idx >= 0)
            (void)yew_tab_close(ed, idx);
    }
    ed->layout_dirty = true;
    ed->full_damage = true;
    return YEW_CMD_OK;
}

CmdStatus yew_tab_cmd_copy_path(CmdCtx *cx)
{
    Ed *ed;
    const Tab *t;
    RegVal v;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    t = yew_tab_at(ed, ed->tabs.active);
    if (t == NULL || t->path == NULL) {
        yew_msg(ed, YEW_MSG_ERROR, "this tab has no path");
        return YEW_CMD_ERR_STATE;
    }
    /* The CANONICAL path — which is what Tab.path already holds, set at
     * the one site that establishes a tab's name. */
    yew_regval_init(&v);
    bytebuf_append(&v.bytes, (const u8 *)t->path, strlen(t->path));
    v.type = (u8)YEW_REG_CHARWISE;
    /* Register `+` is the system clipboard, so this also travels out
     * through Sprint 12's OSC 52 path. */
    yew_reg_yank(&ed->regs, (u8)'+', &v);
    yew_regval_free(&v);
    yew_msg(ed, YEW_MSG_INFO, "copied %s", t->path);
    return YEW_CMD_OK;
}

/*
 * Sprint 57.13 §4: the tab menu's `Open in Split Right` / `Open in
 * Split Below` rows.
 *
 * The buffer comes from the ACTIVE TAB, not from the focused pane.
 * They are usually the same, but a pane can be showing a scratch view
 * (job output, a diff, the macro editor) while the tab still owns the
 * file — and "open this tab in a split" then has to mean the file, or
 * the new pane is a second copy of the scratch the user was trying to
 * get away from.
 *
 * yew_pane_split clones the focused Win, so the split starts on the
 * focused pane's buffer; yew_ed_win_set_buffer then points it at the
 * tab's.  Two panes on one buffer is the supported case (the tab model
 * forbids two TABS on one path, not two views).
 */
static CmdStatus tab_open_split(CmdCtx *cx, SplitDir dir)
{
    Ed *ed;
    const Tab *tab;
    Buffer *buffer;
    Pane *split;

    if (cx == NULL || cx->ed == NULL || cx->ed->focus == NULL)
        return YEW_CMD_ERR_STATE;
    ed = cx->ed;
    tab = yew_tab_at(ed, ed->tabs.active);
    if (tab == NULL)
        return YEW_CMD_ERR_STATE;
    /*
     * `buffer_id`, not yew_tab_buffer(): the latter answers "what is
     * this tab's focused pane showing", which is the very thing a
     * parked scratch view makes wrong.  buffer_id is the stable handle
     * the tab was opened on, and the same one yew_tab_open restores a
     * parked pane from.
     */
    buffer = yew_ws_buf_by_id(ed, tab->buffer_id);
    if (buffer == NULL)
        return YEW_CMD_ERR_STATE;
    /* Deferred tabs carry no text until something asks; a split that
     * showed an empty buffer would look like a truncated file. */
    if (yew_buf_hydrate(ed, buffer) != 0) {
        yew_msg(ed, YEW_MSG_ERROR, "could not read %s",
                tab->path != NULL ? tab->path : "untitled");
        return YEW_CMD_ERR_IO;
    }
    split = yew_pane_split(ed, ed->focus, dir);
    if (split == NULL) {
        if (yew_pane_leaf_count(ed->pane_root) >=
            (u32)YEW_PANE_MAX_LEAVES)
            yew_msg(ed, YEW_MSG_ERROR, "too many panes (max %d)",
                    YEW_PANE_MAX_LEAVES);
        else
            yew_msg(ed, YEW_MSG_ERROR, "no room to split");
        return YEW_CMD_ERR_STATE;
    }
    yew_ed_win_set_buffer(ed, split->win, buffer);
    yew_pane_refocus(ed, split);
    return YEW_CMD_OK;
}

CmdStatus yew_tab_cmd_open_split_h(CmdCtx *cx)
{
    return tab_open_split(cx, YEW_SPLIT_H);
}

CmdStatus yew_tab_cmd_open_split_v(CmdCtx *cx)
{
    return tab_open_split(cx, YEW_SPLIT_V);
}

bool yew_tab_prompt_key(Ed *ed, u8 answer)
{
    int idx;

    if (ed == NULL || !ed->tab_prompt.active)
        return false;
    idx = yew_tab_index_of_id(ed, ed->tab_prompt.tab_id);
    if (idx < 0) {
        /* The tab went away while the question was up; the question
         * goes away with it rather than answering for its successor. */
        ed->tab_prompt.active = false;
        yew_msg_clear(ed);
        return true;
    }
    switch (answer) {
    case 'w': {
        Tab *t = yew_tab_at(ed, idx);
        CmdStatus st;

        /*
         * Write through the ONE save path, and close only on success.
         * There is no route where "close" discards bytes the user asked
         * to keep (invariant 1), so an I/O error aborts the close and
         * leaves the tab and its text exactly as they were.
         */
        yew_tab_switch(ed, idx);
        st = yew_ed_file_save(ed, false);
        if (st != YEW_CMD_OK) {
            ed->tab_prompt.active = false;
            return true; /* the save path already reported why */
        }
        idx = yew_tab_index_of_id(ed, t->tab_id);
        if (idx >= 0)
            (void)yew_tab_close(ed, idx);
        break;
    }
    case 'd':
        (void)yew_tab_close(ed, idx);
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
    yew_msg_clear(ed);
    return true;
}
