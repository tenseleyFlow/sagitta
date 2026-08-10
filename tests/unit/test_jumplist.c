/*
 * Sprint 21 §5 / DoD 10: the jumplist and changelist.
 *
 * Every rule here exists because the obvious implementation gets it
 * wrong in a way users feel rather than see: a list that records every
 * step floods, a list that forgets to record where you were standing
 * cannot bring you back, and a list that stores offsets instead of
 * marks points at the wrong line the moment anything above it changes.
 * So each rule gets a row, and the mark-vs-offset property gets an
 * explicit edit-then-read test rather than being taken on faith.
 */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/jumplist.h"
#include "text/piece.h"

/* Ten lines of five bytes each ("aaaa\n"), so line N starts at 5*N and
 * every offset in the tests is readable at a glance. */
static void jl_fixture(Ed *ed)
{
    EditCtx ec;
    u32 i;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_scratch(ed));
    ec = yew_ed_edit_ctx(ed);
    for (i = 0U; i < 10U; i++)
        YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF((u64)i * 5U),
                                   (const u8 *)"aaaa\n", 5U));
    /* The fixture's own inserts are changes; start each test from a
     * clean changelist so coalescing rows are not reading fixture
     * noise. */
    yew_changelist_init(&ed->buffer.changes);
}

static ByteOff jl_pos(const Ed *ed, const JumpList *jl, u32 index)
{
    const JumpEntry *je = yew_jumplist_at(jl, index);

    YEW_ASSERT_NOT_NULL(je);
    if (je == NULL)
        return BYTEOFF(0U);
    return yew_mark_pos(ed->buffer.marks, je->mark);
}

void test_jumplist_push_appends_in_order(void)
{
    Ed ed;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(0U), 100);
    yew_jump_push(ed.win, BYTEOFF(10U), 200);
    yew_jump_push(ed.win, BYTEOFF(20U), 300);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 3U);
    YEW_ASSERT_EQ_U64(jl_pos(&ed, &ed.win->jumps, 0U).v, 0U);
    YEW_ASSERT_EQ_U64(jl_pos(&ed, &ed.win->jumps, 1U).v, 10U);
    YEW_ASSERT_EQ_U64(jl_pos(&ed, &ed.win->jumps, 2U).v, 20U);
    /* Not walking: cur sits one past the newest. */
    YEW_ASSERT_EQ_U64(ed.win->jumps.cur, 3U);
    yew_ed_free(&ed);
}

/* Rule 2. */
void test_jumplist_push_same_position_is_noop(void)
{
    Ed ed;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(10U), 100);
    yew_jump_push(ed.win, BYTEOFF(10U), 200);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 1U);
    yew_ed_free(&ed);
}

/* Rule 1: same line replaces, and the newer column wins. */
void test_jumplist_push_same_line_replaces_keeping_new_column(void)
{
    Ed ed;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(0U), 100);
    /* Line 2 is bytes 10..14. */
    yew_jump_push(ed.win, BYTEOFF(10U), 200);
    yew_jump_push(ed.win, BYTEOFF(13U), 300);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 2U);
    YEW_ASSERT_EQ_U64(jl_pos(&ed, &ed.win->jumps, 1U).v, 13U);
    yew_ed_free(&ed);
}

/* Rule 3: the first step back records where we stood, so forward has
 * somewhere to return to.  Without it, Ctrl-I never comes home. */
void test_jumplist_first_back_pushes_current_position(void)
{
    Ed ed;
    Cursor *c;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(0U), 100);
    yew_jump_push(ed.win, BYTEOFF(10U), 200);
    c = yew_ed_cursor(&ed);
    c->pos = BYTEOFF(40U);

    YEW_ASSERT(yew_jump_back(&ed, ed.win, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 10U);
    /* 40 was recorded on the way out. */
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 3U);

    YEW_ASSERT(yew_jump_fwd(&ed, ed.win, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 40U);
    yew_ed_free(&ed);
}

void test_jumplist_back_and_forward_walk_by_count(void)
{
    Ed ed;
    Cursor *c;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(0U), 100);
    yew_jump_push(ed.win, BYTEOFF(10U), 200);
    yew_jump_push(ed.win, BYTEOFF(20U), 300);
    c = yew_ed_cursor(&ed);
    c->pos = BYTEOFF(45U);

    YEW_ASSERT(yew_jump_back(&ed, ed.win, 2U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 10U);
    YEW_ASSERT(yew_jump_fwd(&ed, ed.win, 2U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 45U);
    yew_ed_free(&ed);
}

/* Rule 4: browser history, not rotation. */
void test_jumplist_push_after_walk_truncates_forward_tail(void)
{
    Ed ed;
    Cursor *c;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(0U), 100);
    yew_jump_push(ed.win, BYTEOFF(10U), 200);
    yew_jump_push(ed.win, BYTEOFF(20U), 300);
    c = yew_ed_cursor(&ed);
    c->pos = BYTEOFF(45U);
    YEW_ASSERT(yew_jump_back(&ed, ed.win, 2U));

    yew_jump_push(ed.win, BYTEOFF(30U), 400);
    /* 0, 10 survive; 20 and 45 were ahead of the walk position. */
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 3U);
    YEW_ASSERT_EQ_U64(jl_pos(&ed, &ed.win->jumps, 2U).v, 30U);
    YEW_ASSERT(!yew_jump_fwd(&ed, ed.win, 1U));
    yew_ed_free(&ed);
}

void test_jumplist_walk_past_the_ends_reports_and_does_not_move(void)
{
    Ed ed;

    jl_fixture(&ed);
    /* Empty list: nothing to go back to. */
    YEW_ASSERT(!yew_jump_back(&ed, ed.win, 1U));
    YEW_ASSERT(!yew_jump_fwd(&ed, ed.win, 1U));

    yew_jump_push(ed.win, BYTEOFF(10U), 100);
    yew_ed_cursor(&ed)->pos = BYTEOFF(40U);
    YEW_ASSERT(yew_jump_back(&ed, ed.win, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 10U);
    /* Already at the oldest. */
    YEW_ASSERT(!yew_jump_back(&ed, ed.win, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 10U);
    yew_ed_free(&ed);
}

void test_jumplist_ring_wraps_at_exactly_100(void)
{
    Ed ed;
    u32 i;

    jl_fixture(&ed);
    /* 120 pushes, each on its own line so nothing coalesces.  The
     * fixture is 10 lines, so reuse offsets cyclically but never
     * repeat the previous line. */
    for (i = 0U; i < 120U; i++) {
        u64 line = (u64)(i % 10U);

        yew_jump_push(ed.win, BYTEOFF(line * 5U), (i64)i);
    }
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), YEW_JUMPLIST_MAX);
    /* The oldest 20 fell off the back: entry 0 is push #20, whose line
     * is 20 % 10 == 0. */
    YEW_ASSERT_EQ_U64(jl_pos(&ed, &ed.win->jumps, 0U).v, 0U);
    YEW_ASSERT_EQ_U64(
        jl_pos(&ed, &ed.win->jumps, YEW_JUMPLIST_MAX - 1U).v,
        (u64)((119U % 10U) * 5U));
    yew_ed_free(&ed);
}

/*
 * The property the whole mark-not-offset decision exists for: an edit
 * ABOVE a recorded position must carry that position along.  Stored as
 * an offset, this test reads back the wrong line.
 */
void test_jumplist_entries_follow_edits_above_them(void)
{
    Ed ed;
    EditCtx ec;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(25U), 100);
    ec = yew_ed_edit_ctx(&ed);
    /* Insert a whole line at the top; everything below shifts by 5. */
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"zzzz\n", 5U));
    YEW_ASSERT_EQ_U64(jl_pos(&ed, &ed.win->jumps, 0U).v, 30U);
    yew_ed_free(&ed);
}

/* Rule 5: a dead entry is dropped, not stepped onto. */
void test_jumplist_dead_marks_are_skipped_and_dropped(void)
{
    Ed ed;
    Buffer *scratch;
    const JumpEntry *je;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(0U), 100);

    scratch = yew_ws_scratch_new(&ed, "*other*", 0U);
    YEW_ASSERT_NOT_NULL(scratch);
    YEW_ASSERT(yew_ed_show_buffer(&ed, scratch));
    yew_jump_push(ed.win, BYTEOFF(0U), 200);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 2U);
    je = yew_jumplist_at(&ed.win->jumps, 1U);
    YEW_ASSERT_NOT_NULL(je);
    YEW_ASSERT_EQ_U64(je->buf_id, scratch->id);

    /* Closing the buffer kills that entry; the walk must drop it and
     * land on the surviving one rather than abort on a dead handle. */
    YEW_ASSERT(yew_ed_show_buffer(&ed, &ed.buffer));
    yew_ws_scratch_drop(&ed, scratch);
    /* Stand somewhere other than the surviving entry, so the walk has
     * an actual step to take once the dead one is gone. */
    yew_ed_cursor(&ed)->pos = BYTEOFF(40U);
    YEW_ASSERT(yew_jump_back(&ed, ed.win, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 0U);
    /* The dead entry is gone; 40 was recorded on the way out. */
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 2U);
    yew_ed_free(&ed);
}

/* Changelist coalescing: DoD 10 pins the 499/501 boundary explicitly. */
void test_changelist_coalesces_within_the_time_window(void)
{
    Ed ed;
    ChangeList *cl;

    jl_fixture(&ed);
    cl = &ed.buffer.changes;
    /* Different lines throughout, so only the clock can coalesce. */
    yew_change_record(&ed.buffer, BYTEOFF(0U), 1000);
    yew_change_record(&ed.buffer, BYTEOFF(10U), 1499);
    YEW_ASSERT_EQ_U64(cl->len, 1U);

    yew_change_record(&ed.buffer, BYTEOFF(20U), 2000);
    YEW_ASSERT_EQ_U64(cl->len, 2U);
    yew_ed_free(&ed);
}

void test_changelist_does_not_coalesce_past_the_window(void)
{
    Ed ed;
    ChangeList *cl;

    jl_fixture(&ed);
    cl = &ed.buffer.changes;
    yew_change_record(&ed.buffer, BYTEOFF(0U), 1000);
    yew_change_record(&ed.buffer, BYTEOFF(10U), 1501);
    YEW_ASSERT_EQ_U64(cl->len, 2U);
    yew_ed_free(&ed);
}

void test_changelist_coalesces_on_the_same_line_regardless_of_time(void)
{
    Ed ed;
    ChangeList *cl;

    jl_fixture(&ed);
    cl = &ed.buffer.changes;
    /* Line 3 is bytes 15..19.  Hours apart, still one entry. */
    yew_change_record(&ed.buffer, BYTEOFF(15U), 1000);
    yew_change_record(&ed.buffer, BYTEOFF(18U), 9999999);
    YEW_ASSERT_EQ_U64(cl->len, 1U);
    /* Different line, far apart in time: a second entry. */
    yew_change_record(&ed.buffer, BYTEOFF(30U), 19999999);
    YEW_ASSERT_EQ_U64(cl->len, 2U);
    yew_ed_free(&ed);
}

void test_changelist_walks_older_and_newer(void)
{
    Ed ed;

    jl_fixture(&ed);
    yew_change_record(&ed.buffer, BYTEOFF(0U), 1000);
    yew_change_record(&ed.buffer, BYTEOFF(20U), 5000);
    yew_change_record(&ed.buffer, BYTEOFF(40U), 9000);
    yew_ed_cursor(&ed)->pos = BYTEOFF(45U);

    YEW_ASSERT(yew_change_older(&ed, ed.win, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 40U);
    YEW_ASSERT(yew_change_older(&ed, ed.win, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 20U);
    YEW_ASSERT(yew_change_newer(&ed, ed.win, 1U));
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v, 40U);
    yew_ed_free(&ed);
}

/* The edit chokepoint feeds the changelist — one op stream, N
 * consumers.  Nothing calls yew_change_record here. */
void test_changelist_is_fed_by_the_edit_chokepoint(void)
{
    Ed ed;
    EditCtx ec;

    jl_fixture(&ed);
    ec = yew_ed_edit_ctx(&ed);
    ec.now_ms = 1000;
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)"x", 1U));
    YEW_ASSERT_EQ_U64(ed.buffer.changes.len, 1U);
    /* A far-away edit, long after: its own entry. */
    ec.now_ms = 90000;
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(30U), (const u8 *)"y", 1U));
    YEW_ASSERT_EQ_U64(ed.buffer.changes.len, 2U);
    yew_ed_free(&ed);
}

/*
 * The ownership split, stated as a test: two views of one buffer keep
 * separate jumplists and share one changelist.
 */
void test_jumplist_is_per_window_changelist_is_per_buffer(void)
{
    Ed ed;
    Win other;

    jl_fixture(&ed);
    (void)memset(&other, 0, sizeof(other));
    other.buf = &ed.buffer;

    yew_jump_push(ed.win, BYTEOFF(0U), 100);
    yew_jump_push(ed.win, BYTEOFF(10U), 200);
    yew_jump_push(&other, BYTEOFF(20U), 300);

    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 2U);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&other.jumps), 1U);

    /* One changelist, reached through either window. */
    yew_change_record(other.buf, BYTEOFF(35U), 1000);
    YEW_ASSERT_EQ_U64(ed.win->buf->changes.len, 1U);
    YEW_ASSERT(ed.win->buf == other.buf);
    yew_ed_free(&ed);
}

/*
 * DoD 11's other half: the serializer exists and round-trips in memory.
 * Nothing calls it until Sprint 25, and nothing here touches a file.
 */
void test_jumplist_serializer_round_trips_in_memory(void)
{
    Ed ed;
    Bytebuf out;
    JumpList back;

    jl_fixture(&ed);
    yew_jump_push(ed.win, BYTEOFF(0U), 100);
    yew_jump_push(ed.win, BYTEOFF(12U), 200);
    yew_jump_push(ed.win, BYTEOFF(27U), 300);

    bytebuf_init(&out);
    yew_jumplist_serialize(&ed.win->jumps, &ed, &out);
    YEW_ASSERT(out.len > 0U);

    YEW_ASSERT(yew_jumplist_deserialize(&back, out.data, out.len));
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&back), 3U);
    /* Marks do not survive a process, so line/col is what comes back. */
    YEW_ASSERT_EQ_U64(yew_jumplist_at(&back, 0U)->line_hint.v, 0U);
    YEW_ASSERT_EQ_U64(yew_jumplist_at(&back, 1U)->line_hint.v, 2U);
    YEW_ASSERT_EQ_U64(yew_jumplist_at(&back, 2U)->line_hint.v, 5U);
    YEW_ASSERT_EQ_U64(yew_jumplist_at(&back, 2U)->stamp_ms, 300U);
    /* A walk position is never "mid-walk" after a load. */
    YEW_ASSERT_EQ_U64(back.cur, back.len);
    bytebuf_free(&out);
    yew_ed_free(&ed);
}

void test_jumplist_deserialize_rejects_a_foreign_blob(void)
{
    JumpList back;

    YEW_ASSERT(!yew_jumplist_deserialize(&back, (const u8 *)"nope\n", 5U));
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&back), 0U);
}
