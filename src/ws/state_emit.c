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
#include "fl/data.h"
#include "fl/gc.h"
#include "ui/groups.h"
#include "ui/tabs.h"
#include "util/intern.h"
#include "util/log.h"
#include "ws/workspace.h"

typedef struct StateEmit {
    Bytebuf *out;
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    FlValue root;
    FlValue stack[64];
    u32 depth;
} StateEmit;

static void state_add(StateEmit *e, const char *key, FlValue value)
{
    FlValue parent;

    if (e->depth == 0U) {
        e->root = value;
        return;
    }
    parent = e->stack[e->depth - 1U];
    if (parent.t == (u8)FL_LIST) {
        (void)fl_list_push(&e->vm, (FlList *)parent.as.o, value);
        return;
    }
    if (parent.t == (u8)FL_MAP) {
        FlStr *k;

        if (key == NULL)
            YEW_BUG("workspace state: map value without a key");
        k = fl_str_new(&e->vm, key, (u32)strlen(key));
        (void)fl_map_set(&e->vm, (FlMap *)parent.as.o,
                         FL_OBJ_V(FL_STR, k), value);
        return;
    }
    YEW_BUG("workspace state: value added outside a container");
}

static void state_emit_init(StateEmit *e, Bytebuf *out)
{
    (void)memset(e, 0, sizeof(*e));
    e->out = out;
    arena_init(&e->arena);
    interner_init(&e->in, &e->arena);
    fl_diag_init(&e->dc, &e->arena);
    (void)fl_vm_init(&e->vm, &e->arena, &e->in, &e->dc);
    e->root = FL_NIL_V;
}

static void state_emit_done(StateEmit *e)
{
    if (e->depth != 0U)
        YEW_BUG("workspace state: %u data container(s) left open",
                (unsigned)e->depth);
    fl_data_write(e->out, e->root, 0U);
    fl_vm_free(&e->vm);
    interner_free(&e->in);
    arena_free_all(&e->arena);
}

static void state_open(StateEmit *e, const char *key, FlValue value)
{
    if (e->depth >= YEW_ARRAY_LEN(e->stack))
        YEW_BUG("workspace state: emitter nesting overflow");
    state_add(e, key, value);
    e->stack[e->depth++] = value;
}

static void state_map_open(StateEmit *e, const char *key)
{
    state_open(e, key, FL_OBJ_V(FL_MAP, fl_map_new(&e->vm)));
}

static void state_map_close(StateEmit *e)
{
    if (e->depth == 0U || e->stack[e->depth - 1U].t != (u8)FL_MAP)
        YEW_BUG("workspace state: unmatched map close");
    e->depth--;
}

static void state_list_open(StateEmit *e, const char *key)
{
    state_open(e, key, FL_OBJ_V(FL_LIST, fl_list_new(&e->vm)));
}

static void state_list_close(StateEmit *e)
{
    if (e->depth == 0U || e->stack[e->depth - 1U].t != (u8)FL_LIST)
        YEW_BUG("workspace state: unmatched list close");
    e->depth--;
}

static void state_str(StateEmit *e, const char *key, const char *s, u64 n)
{
    if (s == NULL) {
        state_add(e, key, FL_NIL_V);
        return;
    }
    if (n > (u64)UINT32_MAX)
        YEW_BUG("workspace state: string too large");
    state_add(e, key,
              FL_OBJ_V(FL_STR, fl_str_new(&e->vm, s, (u32)n)));
}

static void state_int(StateEmit *e, const char *key, i64 value)
{
    state_add(e, key, FL_INT_V(value));
}

static void state_bool(StateEmit *e, const char *key, bool value)
{
    state_add(e, key, FL_BOOL_V(value));
}

static void state_nil(StateEmit *e, const char *key)
{
    state_add(e, key, FL_NIL_V);
}

static void state_lit(StateEmit *e, const char *key, const FlLit *lit)
{
    u32 i;

    if (lit == NULL) {
        state_nil(e, key);
        return;
    }
    switch (lit->kind) {
    case FL_LIT_NIL:
        state_nil(e, key);
        break;
    case FL_LIT_BOOL:
        state_bool(e, key, lit->i != 0);
        break;
    case FL_LIT_INT:
        state_int(e, key, lit->i);
        break;
    case FL_LIT_STR:
        state_str(e, key, lit->s, lit->slen);
        break;
    case FL_LIT_LIST:
        state_list_open(e, key);
        for (i = 0U; i < lit->len; i++)
            state_lit(e, NULL, lit->items[i]);
        state_list_close(e);
        break;
    case FL_LIT_MAP:
        state_map_open(e, key);
        for (i = 0U; i < lit->len; i++)
            state_lit(e, lit->keys[i], lit->items[i]);
        state_map_close(e);
        break;
    default:
        YEW_BUG("workspace state: unknown retained literal kind");
    }
}

static const WsBoolOption *state_bool_override(const WsState *s,
                                                const char *key, u64 len)
{
    u32 i;

    for (i = 0U; i < s->bool_options_len; i++) {
        const WsBoolOption *o = &s->bool_options[i];

        if ((u64)o->key_len == len && memcmp(o->key, key, (size_t)len) == 0)
            return o;
    }
    return NULL;
}

static bool retained_has_option(const FlLit *options,
                                const WsBoolOption *o)
{
    u32 i;

    if (options == NULL || options->kind != FL_LIT_MAP)
        return false;
    for (i = 0U; i < options->len; i++)
        if (options->keylens[i] == (u64)o->key_len &&
            memcmp(options->keys[i], o->key, o->key_len) == 0)
            return true;
    return false;
}

static void state_options(StateEmit *e, const WsState *s)
{
    const FlLit *options = s->options;
    u32 i;

    state_map_open(e, "options");
    if (options != NULL && options->kind == FL_LIT_MAP) {
        for (i = 0U; i < options->len; i++) {
            const WsBoolOption *override = state_bool_override(
                s, options->keys[i], options->keylens[i]);

            if (override != NULL)
                state_bool(e, override->key, override->value);
            else
                state_lit(e, options->keys[i], options->items[i]);
        }
    }
    for (i = 0U; i < s->bool_options_len; i++)
        if (!retained_has_option(options, &s->bool_options[i]))
            state_bool(e, s->bool_options[i].key,
                       s->bool_options[i].value);
    state_map_close(e);
}

void yew_idmap_init(IdMapVec *m)
{
    if (m != NULL)
        (void)memset(m, 0, sizeof(*m));
}

void yew_idmap_free(IdMapVec *m)
{
    if (m == NULL)
        return;
    yew_xfree(m->data);
    (void)memset(m, 0, sizeof(*m));
}

void yew_idmap_put(IdMapVec *m, u32 file_id, u32 live_id)
{
    if (m == NULL || file_id == 0U)
        return;
    if (m->len == m->cap) {
        u32 cap = m->cap == 0U ? 8U : m->cap * 2U;

        m->data = yew_xreallocarray(m->data, cap, sizeof(*m->data));
        m->cap = cap;
    }
    m->data[m->len].file_id = file_id;
    m->data[m->len].live_id = live_id;
    m->len++;
}

u32 yew_idmap_get(const IdMapVec *m, u32 file_id)
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

float yew_permille_to_ratio(i64 permille)
{
    if (permille < YEW_STATE_RATIO_MIN)
        permille = YEW_STATE_RATIO_MIN;
    if (permille > YEW_STATE_RATIO_MAX)
        permille = YEW_STATE_RATIO_MAX;
    return (float)permille / 1000.0f;
}

i64 yew_ratio_to_permille(float ratio)
{
    /* Rounded once, here.  s22 keeps exactly one rounding site for
     * cell splits; this is the format's equivalent, and the fixpoint
     * test walks all 999 values through both directions. */
    i64 v = (i64)((double)ratio * 1000.0 + 0.5);

    if (v < YEW_STATE_RATIO_MIN)
        v = YEW_STATE_RATIO_MIN;
    if (v > YEW_STATE_RATIO_MAX)
        v = YEW_STATE_RATIO_MAX;
    return v;
}

i64 yew_goal_to_i64(u64 goal)
{
    /* YEW_GCOL_EOL is UINT64_MAX; -1 is its representable spelling. */
    if (goal == YEW_GCOL_EOL)
        return -1;
    if (goal > (u64)9223372036854775807ULL)
        return -1;
    return (i64)goal;
}

u64 yew_goal_from_i64(i64 v)
{
    return v < 0 ? YEW_GCOL_EOL : (u64)v;
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
    Win *wins[YEW_PANE_MAX_LEAVES];
    u32 n;
} WinOrder;

static void collect_pre_order(Pane *p, WinOrder *o)
{
    if (p == NULL || o->n >= (u32)YEW_PANE_MAX_LEAVES)
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

static void emit_panes(StateEmit *e, const char *key, const Pane *p,
                       const WinOrder *o)
{
    if (p == NULL) {
        /* A tab with no tree still restores as a single window. */
        state_map_open(e, key);
        state_int(e, "win", 0);
        state_map_close(e);
        return;
    }
    state_map_open(e, key);
    if (p->is_leaf) {
        state_int(e, "win", win_index(o, p->win));
    } else {
        state_str(e, "split", p->dir == YEW_SPLIT_H ? "h" : "v", 1U);
        state_int(e, "ratio_permille", yew_ratio_to_permille(p->ratio));
        emit_panes(e, "a", p->a, o);
        emit_panes(e, "b", p->b, o);
    }
    state_map_close(e);
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
static void emit_ring_entry(StateEmit *e, const Ed *ed, const JumpEntry *je)
{
    Buffer *b = yew_ws_buf_by_id((Ed *)ed, je->buf_id);
    LineNo line = je->line_hint;
    u64 col = 0U;

    if (b != NULL && b->marks != NULL && b->tb != NULL &&
        yew_mark_alive(b->marks, je->mark)) {
        ByteOff pos = yew_mark_pos(b->marks, je->mark);

        line = yew_textbuf_line_of(b->tb, pos);
        col = pos.v - yew_textbuf_line_start(b->tb, line).v;
    }
    state_map_open(e, NULL);
    if (b == NULL || b->path == NULL)
        state_nil(e, "path");
    else
        state_str(e, "path", b->path, (u64)strlen(b->path));
    state_int(e, "line", (i64)line.v);
    state_int(e, "col", (i64)col);
    state_int(e, "stamp", (i64)je->stamp_ms);
    state_map_close(e);
}

static void emit_jumps(StateEmit *e, const Ed *ed, const Win *w)
{
    u32 n = yew_jumplist_len(&w->jumps);
    u32 i;

    if (n > (u32)YEW_STATE_MAX_JUMPS)
        n = (u32)YEW_STATE_MAX_JUMPS;
    state_map_open(e, "jumps");
    /* `cur` is a LOGICAL index and `cur == len` means "standing at now"
     * (s21).  It is written as-is so a session that quit mid-walk
     * resumes mid-walk. */
    state_int(e, "cur", (i64)w->jumps.cur);
    state_list_open(e, "entries");
    for (i = 0U; i < n; i++)
        emit_ring_entry(e, ed, yew_jumplist_at(&w->jumps, i));
    state_list_close(e);
    state_map_close(e);
}

static void emit_win(StateEmit *e, const Ed *ed, const Win *w)
{
    u32 i;

    state_map_open(e, NULL);
    state_list_open(e, "cursors");
    for (i = 0U; i < w->cs.curs.len && i < (u32)YEW_STATE_MAX_CURSORS;
         i++) {
        const Cursor *c = &w->cs.curs.data[i];

        state_map_open(e, NULL);
        state_int(e, "pos", (i64)c->pos.v);
        state_int(e, "anchor", (i64)c->anchor.v);
        state_int(e, "goal", yew_goal_to_i64(c->goal_col.v));
        state_map_close(e);
    }
    state_list_close(e);
    state_int(e, "primary", (i64)w->cs.primary);
    state_map_open(e, "view");
    state_int(e, "top", (i64)w->vp.top.v);
    state_int(e, "top_sub", (i64)w->vp.top_sub);
    state_int(e, "left", (i64)w->vp.left.v);
    state_bool(e, "wrap", w->vp.wrap);
    state_map_close(e);
    emit_jumps(e, ed, w);
    state_map_close(e);
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
    return !yew_tab_is_resident(ed, idx);
}

static void emit_tab(StateEmit *e, const Ed *ed, int idx)
{
    const Tab *t = yew_tab_at((Ed *)ed, idx);
    WinOrder order;
    u32 i;

    (void)memset(&order, 0, sizeof(order));
    collect_pre_order(t->root, &order);

    state_map_open(e, NULL);
    state_int(e, "id", (i64)t->tab_id);
    /* nil, not "", for an untitled tab: the restore drops it rather
     * than recreating a scratch buffer nobody asked for. */
    if (t->path == NULL)
        state_nil(e, "path");
    else
        state_str(e, "path", t->path, (u64)strlen(t->path));
    state_int(e, "group", (i64)t->group_id);
    state_int(e, "group_ordinal", (i64)t->group_ordinal);
    state_bool(e, "deferred", tab_should_defer(ed, idx));
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
    state_int(e, "focus", t->focus == NULL ? 0
                                            : win_index(&order,
                                                        t->focus->win));
    emit_panes(e, "panes", t->root, &order);
    state_list_open(e, "wins");
    for (i = 0U; i < order.n; i++)
        emit_win(e, ed, order.wins[i]);
    state_list_close(e);
    state_map_close(e);
}

static void emit_file_records(StateEmit *e, const Ed *ed)
{
    u32 i;
    u32 written = 0U;

    state_list_open(e, "files");
    for (i = 0U; i < ed->ws.nbufs && written < (u32)YEW_STATE_MAX_FILES;
         i++) {
        const Buffer *b = ed->ws.bufs[i];
        u32 m;
        bool any_mark = false;

        /* Scratch buffers have no file behind them, so there is nothing
         * a file record could be about. */
        if (b->path == NULL || (b->flags & YEW_BUF_SCRATCH) != 0U)
            continue;
        state_map_open(e, NULL);
        state_str(e, "path", b->path, (u64)strlen(b->path));
        state_list_open(e, "marks");
        for (m = 0U; m < 26U; m++) {
            char name[2];

            if (!b->named_set[m])
                continue;
            any_mark = true;
            name[0] = (char)('a' + m);
            name[1] = '\0';
            state_map_open(e, NULL);
            state_str(e, "name", name, 1U);
            /*
             * Resolved to an OFFSET here.  A mark handle means nothing
             * to another session, and resolving at read time would need
             * the buffer loaded — which is exactly what deferral
             * avoids.
             */
            {
                ByteOff at = BYTEOFF(0U);

                (void)yew_ed_mark_get((Ed *)ed, b, (u8)('a' + m), &at);
                state_int(e, "pos", (i64)at.v);
            }
            state_map_close(e);
        }
        (void)any_mark;
        state_list_close(e);
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

            if (nc > (u32)YEW_STATE_MAX_JUMPS)
                nc = (u32)YEW_STATE_MAX_JUMPS;
            state_map_open(e, "changes");
            state_int(e, "cur", (i64)b->changes.cur);
            state_list_open(e, "entries");
            for (c = 0U; c < nc; c++) {
                u32 at = (b->changes.head + YEW_CHANGELIST_MAX -
                          b->changes.len + c) % YEW_CHANGELIST_MAX;

                emit_ring_entry(e, ed, &b->changes.e[at]);
            }
            state_list_close(e);
            state_map_close(e);
        }
        /*
         * Undo is a SIBLING FILE, never inline (§3.4): payloads are
         * arbitrary binary that would need \xNN encoding at 4x size,
         * the state file is rewritten in full on every change, and the
         * two have different lifetimes — the undo tree can be dropped
         * alone when it fails validation.
         */
        state_map_open(e, "undo");
        {
            char sidecar[32];
            u64 h = yew_fnv1a64((const u8 *)b->path, strlen(b->path));

            (void)snprintf(sidecar, sizeof(sidecar), "%016lx.yewu",
                           (unsigned long)h);
            state_str(e, "file", sidecar, (u64)strlen(sidecar));
            state_int(e, "version", 1);
        }
        state_map_close(e);
        state_map_close(e);
        written++;
    }
    state_list_close(e);
}

void yew_state_emit(const Ed *ed, Bytebuf *out)
{
    StateEmit e;
    u32 i;
    int n;

    if (ed == NULL || out == NULL)
        return;
    state_emit_init(&e, out);
    state_map_open(&e, NULL);
    state_int(&e, "version", YEW_STATE_VERSION);
    state_str(&e, "writer", "yew", 3U);

    state_map_open(&e, "workspace");
    {
        const char *root = yew_ws_root(ed);

        state_str(&e, "path", root == NULL ? "" : root,
                   root == NULL ? 0U : (u64)strlen(root));
        /* Display only; nothing reads it back to make a decision. */
        state_int(&e, "saved_at", (i64)time(NULL));
    }
    state_map_close(&e);

    /*
     * Options land in Sprint 36.  Until then whatever was READ is
     * written back unchanged: an older yew opening a newer
     * session's workspace must not silently delete every key it has
     * never heard of, and since the document is rewritten in full on
     * every change, dropping them once makes it permanent.
     */
    state_options(&e, &ed->state);

    /*
     * GROUPS FIRST.  See state.h — the restore attaches each tab as it
     * finishes that tab's record, so the group has to already exist.
     */
    state_list_open(&e, "groups");
    for (i = 0U; i < ed->groups.v.len; i++) {
        const TabGroup *g = &ed->groups.v.data[i];

        state_map_open(&e, NULL);
        state_int(&e, "id", (i64)g->id);
        state_str(&e, "label", g->label == NULL ? "" : g->label,
                   g->label == NULL ? 0U : (u64)strlen(g->label));
        state_str(&e, "dir_path", g->dir_path == NULL ? "" : g->dir_path,
                   g->dir_path == NULL ? 0U : (u64)strlen(g->dir_path));
        if (g->last_active_member == NULL)
            state_nil(&e, "last_active_member");
        else
            state_str(&e, "last_active_member", g->last_active_member,
                       (u64)strlen(g->last_active_member));
        state_map_close(&e);
    }
    state_list_close(&e);

    state_list_open(&e, "tabs");
    n = (int)yew_tab_count(ed);
    if (n > YEW_STATE_MAX_TABS)
        n = YEW_STATE_MAX_TABS;
    for (i = 0U; i < (u32)n; i++)
        emit_tab(&e, ed, (int)i);
    state_list_close(&e);

    {
        const Tab *active = yew_tab_at((Ed *)ed, ed->tabs.active);

        state_int(&e, "active_tab", active == NULL ? 0
                                                    : (i64)active->tab_id);
    }
    emit_file_records(&e, ed);
    state_map_close(&e);
    state_emit_done(&e);
}
