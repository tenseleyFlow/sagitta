/*
 * Sprint 25 §3/§4: emitting the v1 document.
 *
 * The emitter never stats the filesystem and never allocates per field
 * — one Bytebuf with geometric growth — so emitting a 512-tab workspace
 * is string formatting and nothing else.  A save that stat'ed 500 paths
 * would turn every tab switch into a disk round trip.
 */
#define _POSIX_C_SOURCE 200809L

#include "ws/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "ui/groups.h"
#include "ui/tabs.h"
#include "util/log.h"
#include "ws/workspace.h"

void sag_idmap_init(IdMapVec *m)
{
    if (m != NULL)
        (void)memset(m, 0, sizeof(*m));
}

void sag_idmap_free(IdMapVec *m)
{
    if (m == NULL)
        return;
    free(m->data);
    (void)memset(m, 0, sizeof(*m));
}

void sag_idmap_put(IdMapVec *m, u32 file_id, u32 live_id)
{
    if (m == NULL || file_id == 0U)
        return;
    if (m->len == m->cap) {
        u32 cap = m->cap == 0U ? 8U : m->cap * 2U;

        m->data = sag_xreallocarray(m->data, cap, sizeof(*m->data));
        m->cap = cap;
    }
    m->data[m->len].file_id = file_id;
    m->data[m->len].live_id = live_id;
    m->len++;
}

u32 sag_idmap_get(const IdMapVec *m, u32 file_id)
{
    u32 i;

    /* Ungrouped stays ungrouped without a lookup. */
    if (m == NULL || file_id == 0U)
        return 0U;
    for (i = 0U; i < m->len; i++) {
        if (m->data[i].file_id == file_id)
            return m->data[i].live_id;
    }
    /*
     * The record was missing or dropped.  Resolving to 0 makes the tab
     * UNGROUPED; resolving to "whatever id exists now" would silently
     * file someone's document into an unrelated group.
     */
    return 0U;
}

float sag_permille_to_ratio(i64 permille)
{
    if (permille < SAG_STATE_RATIO_MIN)
        permille = SAG_STATE_RATIO_MIN;
    if (permille > SAG_STATE_RATIO_MAX)
        permille = SAG_STATE_RATIO_MAX;
    return (float)permille / 1000.0f;
}

i64 sag_ratio_to_permille(float ratio)
{
    /* Rounded once, here.  s22 keeps exactly one rounding site for
     * cell splits; this is the format's equivalent, and the fixpoint
     * test walks all 999 values through both directions. */
    i64 v = (i64)((double)ratio * 1000.0 + 0.5);

    if (v < SAG_STATE_RATIO_MIN)
        v = SAG_STATE_RATIO_MIN;
    if (v > SAG_STATE_RATIO_MAX)
        v = SAG_STATE_RATIO_MAX;
    return v;
}

i64 sag_goal_to_i64(u64 goal)
{
    /* SAG_GCOL_EOL is UINT64_MAX; -1 is its representable spelling. */
    if (goal == SAG_GCOL_EOL)
        return -1;
    if (goal > (u64)9223372036854775807ULL)
        return -1;
    return (i64)goal;
}

u64 sag_goal_from_i64(i64 v)
{
    return v < 0 ? SAG_GCOL_EOL : (u64)v;
}

/* ---------------------------------------------------------------- */
/* The pre-order window numbering                                   */
/* ---------------------------------------------------------------- */

/*
 * THE PITFALL THIS EXISTS FOR (§4).
 *
 * `panes` names windows by index and `wins` lists them; the two must
 * agree.  Two independent traversals that disagree by one produce a
 * state file which restores every pane holding its NEIGHBOUR's cursor —
 * silent, entirely plausible-looking, and impossible to notice without
 * comparing against what you had before you quit.
 *
 * So there is ONE pre-order collection, and both sections read it.
 */
typedef struct WinOrder {
    Win *wins[SAG_PANE_MAX_LEAVES];
    u32 n;
} WinOrder;

static void collect_pre_order(Pane *p, WinOrder *o)
{
    if (p == NULL || o->n >= (u32)SAG_PANE_MAX_LEAVES)
        return;
    if (p->is_leaf) {
        o->wins[o->n++] = p->win;
        return;
    }
    collect_pre_order(p->a, o);
    collect_pre_order(p->b, o);
}

/* Index of `w` in the same pre-order the `wins` list will use. */
static i64 win_index(const WinOrder *o, const Win *w)
{
    u32 i;

    for (i = 0U; i < o->n; i++) {
        if (o->wins[i] == w)
            return (i64)i;
    }
    return 0;
}

static void emit_panes(FlEmit *e, const char *key, const Pane *p,
                       const WinOrder *o)
{
    if (p == NULL) {
        /* A tab with no tree still restores as a single window. */
        sag_fl_map_open(e, key);
        sag_fl_int(e, "win", 0);
        sag_fl_map_close(e);
        return;
    }
    sag_fl_map_open(e, key);
    if (p->is_leaf) {
        sag_fl_int(e, "win", win_index(o, p->win));
    } else {
        sag_fl_str(e, "split", p->dir == SAG_SPLIT_H ? "h" : "v", 1U);
        sag_fl_int(e, "ratio_permille", sag_ratio_to_permille(p->ratio));
        emit_panes(e, "a", p->a, o);
        emit_panes(e, "b", p->b, o);
    }
    sag_fl_map_close(e);
}

/* ---------------------------------------------------------------- */

/*
 * A ring entry, resolved to path + line + col.
 *
 * The stored form is a MARK, which means nothing to another process, so
 * it is resolved here — but only when the buffer is actually loaded.
 * For a deferred buffer the mark is dead and `line_hint` is what the
 * entry is for: s21 keeps it precisely so a closed buffer's history
 * survives as somewhere to reopen, and resolving it would mean reading
 * every file the user has ever jumped through (§3.3's whole point).
 */
static void emit_ring_entry(FlEmit *e, const Ed *ed, const JumpEntry *je)
{
    Buffer *b = sag_ws_buf_by_id((Ed *)ed, je->buf_id);
    LineNo line = je->line_hint;
    u64 col = 0U;

    if (b != NULL && b->marks != NULL && b->tb != NULL &&
        sag_mark_alive(b->marks, je->mark)) {
        ByteOff pos = sag_mark_pos(b->marks, je->mark);

        line = sag_textbuf_line_of(b->tb, pos);
        col = pos.v - sag_textbuf_line_start(b->tb, line).v;
    }
    sag_fl_map_open(e, NULL);
    if (b == NULL || b->path == NULL)
        sag_fl_nil(e, "path");
    else
        sag_fl_str(e, "path", b->path, (u64)strlen(b->path));
    sag_fl_int(e, "line", (i64)line.v);
    sag_fl_int(e, "col", (i64)col);
    sag_fl_int(e, "stamp", (i64)je->stamp_ms);
    sag_fl_map_close(e);
}

static void emit_jumps(FlEmit *e, const Ed *ed, const Win *w)
{
    u32 n = sag_jumplist_len(&w->jumps);
    u32 i;

    if (n > (u32)SAG_STATE_MAX_JUMPS)
        n = (u32)SAG_STATE_MAX_JUMPS;
    sag_fl_map_open(e, "jumps");
    /* `cur` is a LOGICAL index and `cur == len` means "standing at now"
     * (s21).  It is written as-is so a session that quit mid-walk
     * resumes mid-walk. */
    sag_fl_int(e, "cur", (i64)w->jumps.cur);
    sag_fl_list_open(e, "entries");
    for (i = 0U; i < n; i++)
        emit_ring_entry(e, ed, sag_jumplist_at(&w->jumps, i));
    sag_fl_list_close(e);
    sag_fl_map_close(e);
}

static void emit_win(FlEmit *e, const Ed *ed, const Win *w)
{
    u32 i;

    sag_fl_map_open(e, NULL);
    sag_fl_list_open(e, "cursors");
    for (i = 0U; i < w->cs.curs.len && i < (u32)SAG_STATE_MAX_CURSORS;
         i++) {
        const Cursor *c = &w->cs.curs.data[i];

        sag_fl_map_open(e, NULL);
        sag_fl_int(e, "pos", (i64)c->pos.v);
        sag_fl_int(e, "anchor", (i64)c->anchor.v);
        sag_fl_int(e, "goal", sag_goal_to_i64(c->goal_col.v));
        sag_fl_map_close(e);
    }
    sag_fl_list_close(e);
    sag_fl_int(e, "primary", (i64)w->cs.primary);
    sag_fl_map_open(e, "view");
    sag_fl_int(e, "top", (i64)w->vp.top.v);
    sag_fl_int(e, "top_sub", (i64)w->vp.top_sub);
    sag_fl_int(e, "left", (i64)w->vp.left.v);
    sag_fl_bool(e, "wrap", w->vp.wrap);
    sag_fl_map_close(e);
    emit_jumps(e, ed, w);
    sag_fl_map_close(e);
}

/*
 * `deferred` is an INSTRUCTION, not a fact (§3.3).
 *
 * It says "create this tab with a path and no buffer".  Residency
 * itself is never persisted — it is asked of the allocation (s24 D3),
 * and a persisted flag would be a second, lying answer to a question
 * that already has a structural one.
 */
static bool tab_should_defer(const Ed *ed, int idx)
{
    if (idx == ed->tabs.active)
        return false;
    return !sag_tab_is_resident(ed, idx);
}

static void emit_tab(FlEmit *e, const Ed *ed, int idx)
{
    const Tab *t = sag_tab_at((Ed *)ed, idx);
    WinOrder order;
    u32 i;

    (void)memset(&order, 0, sizeof(order));
    collect_pre_order(t->root, &order);

    sag_fl_map_open(e, NULL);
    sag_fl_int(e, "id", (i64)t->tab_id);
    /* nil, not "", for an untitled tab: the restore drops it rather
     * than recreating a scratch buffer nobody asked for. */
    if (t->path == NULL)
        sag_fl_nil(e, "path");
    else
        sag_fl_str(e, "path", t->path, (u64)strlen(t->path));
    sag_fl_int(e, "group", (i64)t->group_id);
    sag_fl_int(e, "group_ordinal", (i64)t->group_ordinal);
    sag_fl_bool(e, "deferred", tab_should_defer(ed, idx));
    /*
     * WHICH PANE HAS FOCUS, as an index into the same pre-order `wins`
     * list — not an id, for the reason §3.1 gives about `panes.win`.
     *
     * Without it a restored split always focuses its first leaf, and
     * the drawn cursor follows focus: a session that quit with the
     * right-hand pane active came back with the cursor in the left one.
     * The resume_exact gate found this, because it is invisible to any
     * test that checks tabs and paths rather than the grid.
     */
    sag_fl_int(e, "focus", t->focus == NULL ? 0
                                            : win_index(&order,
                                                        t->focus->win));
    emit_panes(e, "panes", t->root, &order);
    sag_fl_list_open(e, "wins");
    for (i = 0U; i < order.n; i++)
        emit_win(e, ed, order.wins[i]);
    sag_fl_list_close(e);
    sag_fl_map_close(e);
}

static void emit_file_records(FlEmit *e, const Ed *ed)
{
    u32 i;
    u32 written = 0U;

    sag_fl_list_open(e, "files");
    for (i = 0U; i < ed->ws.nbufs && written < (u32)SAG_STATE_MAX_FILES;
         i++) {
        const Buffer *b = ed->ws.bufs[i];
        u32 m;
        bool any_mark = false;

        /* Scratch buffers have no file behind them, so there is nothing
         * a file record could be about. */
        if (b->path == NULL || (b->flags & SAG_BUF_SCRATCH) != 0U)
            continue;
        sag_fl_map_open(e, NULL);
        sag_fl_str(e, "path", b->path, (u64)strlen(b->path));
        sag_fl_list_open(e, "marks");
        for (m = 0U; m < 26U; m++) {
            char name[2];

            if (!b->named_set[m])
                continue;
            any_mark = true;
            name[0] = (char)('a' + m);
            name[1] = '\0';
            sag_fl_map_open(e, NULL);
            sag_fl_str(e, "name", name, 1U);
            /*
             * Resolved to an OFFSET here.  A mark handle means nothing
             * to another session, and resolving at read time would need
             * the buffer loaded — which is exactly what deferral
             * avoids.
             */
            {
                ByteOff at = BYTEOFF(0U);

                (void)sag_ed_mark_get((Ed *)ed, b, (u8)('a' + m), &at);
                sag_fl_int(e, "pos", (i64)at.v);
            }
            sag_fl_map_close(e);
        }
        (void)any_mark;
        sag_fl_list_close(e);
        /*
         * The changelist is per BUFFER because a change is a property
         * of the TEXT, so it rides the file record; the jumplist is per
         * window and rides the win record.  Splitting them the other
         * way round would give two panes on one file one shared history
         * of where each view had been, which is the opposite of what a
         * split is for (s21 §5, serialized).
         */
        {
            u32 c;
            u32 nc = b->changes.len;

            if (nc > (u32)SAG_STATE_MAX_JUMPS)
                nc = (u32)SAG_STATE_MAX_JUMPS;
            sag_fl_map_open(e, "changes");
            sag_fl_int(e, "cur", (i64)b->changes.cur);
            sag_fl_list_open(e, "entries");
            for (c = 0U; c < nc; c++) {
                u32 at = (b->changes.head + SAG_CHANGELIST_MAX -
                          b->changes.len + c) % SAG_CHANGELIST_MAX;

                emit_ring_entry(e, ed, &b->changes.e[at]);
            }
            sag_fl_list_close(e);
            sag_fl_map_close(e);
        }
        /*
         * Undo is a SIBLING FILE, never inline (§3.4): payloads are
         * arbitrary binary that would need \xNN encoding at 4x size,
         * the state file is rewritten in full on every change, and the
         * two have different lifetimes — the undo tree can be dropped
         * alone when it fails validation.
         */
        sag_fl_map_open(e, "undo");
        {
            char sidecar[32];
            u64 h = sag_fnv1a64((const u8 *)b->path, strlen(b->path));

            (void)snprintf(sidecar, sizeof(sidecar), "%016lx.sagu",
                           (unsigned long)h);
            sag_fl_str(e, "file", sidecar, (u64)strlen(sidecar));
            sag_fl_int(e, "version", 1);
        }
        sag_fl_map_close(e);
        sag_fl_map_close(e);
        written++;
    }
    sag_fl_list_close(e);
}

void sag_state_emit(const Ed *ed, Bytebuf *out)
{
    FlEmit e;
    u32 i;
    int n;

    if (ed == NULL || out == NULL)
        return;
    sag_fl_emit_init(&e, out);
    sag_fl_map_open(&e, NULL);
    sag_fl_int(&e, "version", SAG_STATE_VERSION);
    sag_fl_str(&e, "writer", "sagitta", 7U);

    sag_fl_map_open(&e, "workspace");
    {
        const char *root = sag_ws_root(ed);

        sag_fl_str(&e, "path", root == NULL ? "" : root,
                   root == NULL ? 0U : (u64)strlen(root));
        /* Display only; nothing reads it back to make a decision. */
        sag_fl_int(&e, "saved_at", (i64)time(NULL));
    }
    sag_fl_map_close(&e);

    /*
     * Options land in Sprint 36.  Until then whatever was READ is
     * written back unchanged: an older sagitta opening a newer
     * session's workspace must not silently delete every key it has
     * never heard of, and since the document is rewritten in full on
     * every change, dropping them once makes it permanent.
     */
    if (ed->state.options != NULL &&
        ed->state.options->kind == FL_LIT_MAP) {
        sag_fl_emit_lit(&e, "options", ed->state.options);
    } else {
        sag_fl_map_open(&e, "options");
        sag_fl_map_close(&e);
    }

    /*
     * GROUPS FIRST.  See state.h — the restore attaches each tab as it
     * finishes that tab's record, so the group has to already exist.
     */
    sag_fl_list_open(&e, "groups");
    for (i = 0U; i < ed->groups.v.len; i++) {
        const TabGroup *g = &ed->groups.v.data[i];

        sag_fl_map_open(&e, NULL);
        sag_fl_int(&e, "id", (i64)g->id);
        sag_fl_str(&e, "label", g->label == NULL ? "" : g->label,
                   g->label == NULL ? 0U : (u64)strlen(g->label));
        sag_fl_str(&e, "dir_path", g->dir_path == NULL ? "" : g->dir_path,
                   g->dir_path == NULL ? 0U : (u64)strlen(g->dir_path));
        if (g->last_active_member == NULL)
            sag_fl_nil(&e, "last_active_member");
        else
            sag_fl_str(&e, "last_active_member", g->last_active_member,
                       (u64)strlen(g->last_active_member));
        sag_fl_map_close(&e);
    }
    sag_fl_list_close(&e);

    sag_fl_list_open(&e, "tabs");
    n = (int)sag_tab_count(ed);
    if (n > SAG_STATE_MAX_TABS)
        n = SAG_STATE_MAX_TABS;
    for (i = 0U; i < (u32)n; i++)
        emit_tab(&e, ed, (int)i);
    sag_fl_list_close(&e);

    {
        const Tab *active = sag_tab_at((Ed *)ed, ed->tabs.active);

        sag_fl_int(&e, "active_tab", active == NULL ? 0
                                                    : (i64)active->tab_id);
    }
    emit_file_records(&e, ed);
    sag_fl_map_close(&e);
    sag_fl_emit_done(&e);
}
