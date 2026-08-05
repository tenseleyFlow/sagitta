/*
 * Sprint 21 §5.  See jumplist.h for the ownership split and why
 * positions are marks.
 *
 * The ring is stored oldest-to-newest with `head` one past the newest,
 * and every rule below is expressed in LOGICAL indices (0 = oldest) so
 * the wraparound arithmetic lives in exactly two helpers.
 */
#include "edit/jumplist.h"

#include <string.h>

#include "edit/ed.h"
#include "text/piece.h"
#include "ui/win.h"
#include "ui/message.h"
#include "util/log.h"

void sag_jumplist_init(JumpList *jl)
{
    if (jl != NULL)
        (void)memset(jl, 0, sizeof(*jl));
}

void sag_changelist_init(ChangeList *cl)
{
    if (cl != NULL)
        (void)memset(cl, 0, sizeof(*cl));
}

/* Logical index (0 = oldest) to slot. */
static u32 ring_slot(u32 head, u32 len, u32 cap, u32 index)
{
    return (head + cap - len + index) % cap;
}

static JumpEntry *ring_at(JumpEntry *e, u32 head, u32 len, u32 cap,
                          u32 index)
{
    return &e[ring_slot(head, len, cap, index)];
}

u32 sag_jumplist_len(const JumpList *jl)
{
    return jl == NULL ? 0U : jl->len;
}

const JumpEntry *sag_jumplist_at(const JumpList *jl, u32 index)
{
    if (jl == NULL || index >= jl->len)
        return NULL;
    return &jl->e[ring_slot(jl->head, jl->len, SAG_JUMPLIST_MAX, index)];
}

static void entry_release(Buffer *b, JumpEntry *je)
{
    if (b != NULL && b->marks != NULL && sag_mark_alive(b->marks, je->mark))
        sag_mark_del(b->marks, je->mark);
    (void)memset(je, 0, sizeof(*je));
}

static JumpEntry entry_make(Buffer *b, ByteOff at, i64 now_ms)
{
    JumpEntry je;

    (void)memset(&je, 0, sizeof(je));
    je.buf_id = b->id;
    je.mark = sag_mark_add(b->marks, at, SAG_BIAS_LEFT);
    je.line_hint = sag_textbuf_line_of(b->tb, at);
    je.stamp_ms = (u64)(now_ms < 0 ? 0 : now_ms);
    return je;
}

/*
 * The generic push, shared by both lists.  `cap` differs and nothing
 * else does — the dedupe rules are identical, and having written them
 * once there is no second copy to drift.
 */
static void ring_push(JumpEntry *e, u32 *head, u32 *len, u32 *cur, u32 cap,
                      Buffer *b, ByteOff at, i64 now_ms)
{
    LineNo line = sag_textbuf_line_of(b->tb, at);

    /*
     * Rule 4: a push while walking truncates the forward tail.  Vim
     * rotates instead; truncating is browser-history semantics, chosen
     * because rotation makes list order depend on where you were
     * standing and nobody can predict it.
     */
    if (*cur < *len) {
        u32 keep = *cur + 1U;
        u32 i;

        /*
         * The tail starts one PAST the walk position: `cur` names the
         * entry we are standing on, and standing on it does not make it
         * forward history.  Dropping from `cur` instead loses the place
         * the user walked back to the moment they jump somewhere else.
         */
        for (i = keep; i < *len; i++)
            entry_release(b, ring_at(e, *head, *len, cap, i));
        *head = ring_slot(*head, *len, cap, keep % cap);
        *len = keep;
    }
    if (*len > 0U) {
        JumpEntry *newest = ring_at(e, *head, *len, cap, *len - 1U);

        if (sag_mark_alive(b->marks, newest->mark) &&
            newest->buf_id == b->id) {
            ByteOff pos = sag_mark_pos(b->marks, newest->mark);

            /* Rule 2: pushing the position already on top is a no-op. */
            if (pos.v == at.v) {
                *cur = *len;
                return;
            }
            /* Rule 1: same line replaces, keeping the newer column, so
             * editing around one line does not fill the ring with it. */
            if (sag_textbuf_line_of(b->tb, pos).v == line.v) {
                entry_release(b, newest);
                *newest = entry_make(b, at, now_ms);
                *cur = *len;
                return;
            }
        }
    }
    if (*len == cap) {
        /* Full: the oldest falls off the back. */
        entry_release(b, ring_at(e, *head, *len, cap, 0U));
        *len -= 1U;
    }
    e[*head] = entry_make(b, at, now_ms);
    *head = (*head + 1U) % cap;
    *len += 1U;
    *cur = *len;
}

void sag_jump_push(Win *w, ByteOff from, i64 now_ms)
{
    if (w == NULL || w->buf == NULL || w->buf->marks == NULL)
        return;
    ring_push(w->jumps.e, &w->jumps.head, &w->jumps.len, &w->jumps.cur,
              SAG_JUMPLIST_MAX, w->buf, from, now_ms);
}

/* Resolves an entry, reporting whether it still names a live position. */
static bool entry_live(Ed *ed, const JumpEntry *je, Buffer **b_out,
                       ByteOff *pos_out)
{
    Buffer *b = sag_ws_buf_by_id(ed, je->buf_id);

    if (b == NULL || b->marks == NULL || !sag_mark_alive(b->marks, je->mark))
        return false;
    if (b_out != NULL)
        *b_out = b;
    if (pos_out != NULL)
        *pos_out = sag_mark_pos(b->marks, je->mark);
    return true;
}

/*
 * Rule 5: entries whose mark has died are dropped, not merely skipped —
 * otherwise a closed buffer's entries are re-examined on every walk and
 * the list never shrinks.  Rebuilding contiguously is O(100) and keeps
 * `cur` meaningful: it moves down by however many dead entries were
 * behind it.
 */
static void ring_compact(Ed *ed, JumpEntry *e, u32 *head, u32 *len,
                         u32 *cur, u32 cap)
{
    JumpEntry kept[SAG_JUMPLIST_MAX];
    u32 nkept = 0U;
    u32 new_cur = 0U;
    u32 i;
    bool at_now = *cur >= *len;

    for (i = 0U; i < *len; i++) {
        JumpEntry *je = ring_at(e, *head, *len, cap, i);

        if (entry_live(ed, je, NULL, NULL)) {
            kept[nkept++] = *je;
            continue;
        }
        if (i < *cur)
            new_cur++;
    }
    if (nkept == *len)
        return;
    (void)memset(e, 0, sizeof(*e) * cap);
    (void)memcpy(e, kept, sizeof(*kept) * nkept);
    *head = nkept % cap;
    *len = nkept;
    *cur = at_now ? nkept : (*cur >= new_cur ? *cur - new_cur : 0U);
    if (*cur > nkept)
        *cur = nkept;
}

static bool walk(Ed *ed, Win *w, JumpEntry *e, u32 *head, u32 *len,
                 u32 *cur, u32 cap, bool older, u32 count,
                 const char *empty_msg)
{
    u32 moved = 0U;
    Buffer *b = NULL;
    ByteOff pos = BYTEOFF(0U);

    if (ed == NULL || w == NULL || w->buf == NULL)
        return false;
    ring_compact(ed, e, head, len, cur, cap);
    if (*len == 0U) {
        sag_msg(ed, SAG_MSG_INFO, "%s", empty_msg);
        return false;
    }
    if (older && *cur >= *len) {
        /*
         * Rule 3: the first step back records where we are standing, so
         * forward can bring us home.  Missing this is the classic
         * "Ctrl-I doesn't come back" bug.  The push leaves that entry
         * newest, and we are standing ON it — hence cur = len-1 with no
         * jump, before the first real step.
         */
        Cursor *c = sag_ed_cursor(ed);

        if (c != NULL) {
            ring_push(e, head, len, cur, cap, w->buf, c->pos, 0);
            *cur = *len - 1U;
        }
    }
    while (moved < count) {
        u32 next;

        if (older) {
            if (*cur == 0U)
                break;
            next = *cur - 1U;
        } else {
            if (*cur + 1U >= *len)
                break;
            next = *cur + 1U;
        }
        *cur = next;
        moved++;
    }
    if (moved == 0U) {
        sag_msg(ed, SAG_MSG_INFO, "%s", empty_msg);
        return false;
    }
    if (!entry_live(ed, ring_at(e, *head, *len, cap, *cur), &b, &pos)) {
        sag_msg(ed, SAG_MSG_INFO, "%s", empty_msg);
        return false;
    }
    if (b != w->buf && !sag_ed_show_buffer(ed, b))
        return false;
    {
        Cursor *c = sag_ed_cursor(ed);

        if (c != NULL) {
            c->pos = pos;
            /* A jump is an absolute move, so the column the user was
             * aiming at on some earlier vertical motion is stale. */
            c->goal_col = (GCol){0U};
        }
    }
    sag_win_follow_cursor(w);
    sag_ed_damage_document(ed);
    return true;
}

bool sag_jump_back(Ed *ed, Win *w, u32 count)
{
    if (w == NULL)
        return false;
    return walk(ed, w, w->jumps.e, &w->jumps.head, &w->jumps.len,
                &w->jumps.cur, SAG_JUMPLIST_MAX, true,
                count == 0U ? 1U : count, "no older jump position");
}

bool sag_jump_fwd(Ed *ed, Win *w, u32 count)
{
    if (w == NULL)
        return false;
    return walk(ed, w, w->jumps.e, &w->jumps.head, &w->jumps.len,
                &w->jumps.cur, SAG_JUMPLIST_MAX, false,
                count == 0U ? 1U : count, "no newer jump position");
}

void sag_change_record(Buffer *b, ByteOff at, i64 now_ms)
{
    ChangeList *cl;
    LineNo line;

    if (b == NULL || b->marks == NULL || b->tb == NULL)
        return;
    cl = &b->changes;
    line = sag_textbuf_line_of(b->tb, at);
    /*
     * Coalesce a burst into one entry: same line, or close enough in
     * time that it is one act of typing.  Without this, `g;` walks
     * character by character through the sentence just typed, which is
     * not what "go to the last change" means.
     */
    if (cl->has_last && cl->len > 0U &&
        (cl->last_line.v == line.v ||
         (now_ms >= cl->last_ms &&
          now_ms - cl->last_ms <= SAG_CHANGE_COALESCE_MS))) {
        JumpEntry *newest = ring_at(cl->e, cl->head, cl->len,
                                    SAG_CHANGELIST_MAX, cl->len - 1U);

        entry_release(b, newest);
        *newest = entry_make(b, at, now_ms);
        cl->cur = cl->len;
        cl->last_ms = now_ms;
        cl->last_line = line;
        return;
    }
    ring_push(cl->e, &cl->head, &cl->len, &cl->cur, SAG_CHANGELIST_MAX, b,
              at, now_ms);
    cl->last_ms = now_ms;
    cl->last_line = line;
    cl->has_last = true;
}

bool sag_change_older(Ed *ed, Win *w, u32 count)
{
    if (w == NULL || w->buf == NULL)
        return false;
    return walk(ed, w, w->buf->changes.e, &w->buf->changes.head,
                &w->buf->changes.len, &w->buf->changes.cur,
                SAG_CHANGELIST_MAX, true, count == 0U ? 1U : count,
                "no older change position");
}

bool sag_change_newer(Ed *ed, Win *w, u32 count)
{
    if (w == NULL || w->buf == NULL)
        return false;
    return walk(ed, w, w->buf->changes.e, &w->buf->changes.head,
                &w->buf->changes.len, &w->buf->changes.cur,
                SAG_CHANGELIST_MAX, false, count == 0U ? 1U : count,
                "no newer change position");
}

/*
 * Sprint 25's schema, landed and uncalled.  DoD 11 greps this file for
 * file-opening and writing calls and requires none — the deferral is a
 * gate, not a promise.  Marks resolve to line/col here because a mark
 * handle is meaningless in another process.
 *
 * Text, not a packed struct: the file this feeds is workspace state a
 * user may have to read or repair by hand, and a version byte plus one
 * record per line survives a schema change that a binary blob does not.
 */
void sag_jumplist_serialize(const JumpList *jl, const Ed *ed, Bytebuf *out)
{
    u32 i;

    if (jl == NULL || out == NULL)
        return;
    bytebuf_printf(out, "jumplist 1\n");
    for (i = 0U; i < jl->len; i++) {
        const JumpEntry *je = sag_jumplist_at(jl, i);
        Buffer *b = sag_ws_buf_by_id((Ed *)ed, je->buf_id);
        ByteOff pos = BYTEOFF(0U);
        LineNo line = je->line_hint;
        u64 col = 0U;

        if (b != NULL && b->marks != NULL &&
            sag_mark_alive(b->marks, je->mark)) {
            pos = sag_mark_pos(b->marks, je->mark);
            line = sag_textbuf_line_of(b->tb, pos);
            col = pos.v - sag_textbuf_line_start(b->tb, line).v;
        }
        bytebuf_printf(out, "%s\t%llu\t%llu\t%llu\n",
                       b != NULL && b->path != NULL ? b->path : "",
                       (unsigned long long)line.v,
                       (unsigned long long)col,
                       (unsigned long long)je->stamp_ms);
    }
}

bool sag_jumplist_deserialize(JumpList *jl, const u8 *bytes, size_t len)
{
    size_t at = 0U;
    u32 count = 0U;

    if (jl == NULL || bytes == NULL)
        return false;
    sag_jumplist_init(jl);
    if (len < 11U || memcmp(bytes, "jumplist 1\n", 11U) != 0)
        return false;
    at = 11U;
    while (at < len && count < SAG_JUMPLIST_MAX) {
        size_t eol = at;
        size_t field = at;
        u64 nums[3];
        u32 n = 0U;
        JumpEntry je;

        while (eol < len && bytes[eol] != '\n')
            eol++;
        /* Path field: kept for Sprint 25, which reopens by it. */
        while (field < eol && bytes[field] != '\t')
            field++;
        (void)memset(&je, 0, sizeof(je));
        while (field < eol && n < 3U) {
            u64 v = 0U;

            field++; /* the tab */
            while (field < eol && bytes[field] >= '0' &&
                   bytes[field] <= '9') {
                v = v * 10U + (u64)(bytes[field] - '0');
                field++;
            }
            nums[n++] = v;
        }
        if (n == 3U) {
            je.line_hint = LINENO(nums[0]);
            je.stamp_ms = nums[2];
            jl->e[jl->head] = je;
            jl->head = (jl->head + 1U) % SAG_JUMPLIST_MAX;
            jl->len++;
            count++;
        }
        at = eol < len ? eol + 1U : len;
    }
    jl->cur = jl->len;
    return true;
}
