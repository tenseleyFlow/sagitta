/* Sprint 53: the live Git differ is debounced, sliced, and atomic. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "mod/git/editor.h"
#include "text/piece.h"
#include "text/undo.h"
#include "util/buf.h"

typedef struct GitEditorClock {
    i64 now;
    i64 step;
} GitEditorClock;

static i64 git_editor_fake_now(void *ctx)
{
    GitEditorClock *clock = ctx;
    i64 now = clock->now;

    clock->now += clock->step;
    return now;
}

static void git_editor_fixture(Ed *ed, const u8 *live, size_t live_len)
{
    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, live, live_len, "git-editor"));
    ed->buffer.path = arena_strdup(&ed->arena, "git-editor.txt");
}

static void git_editor_pump_until(Ed *ed, i64 now_ms, u64 published)
{
    YewGitEditorStats stats;
    u32 ticks = 0U;

    do {
        yew_git_editor_tick(ed, now_ms);
        yew_git_editor_stats(ed, &stats);
        ticks++;
    } while (stats.diff_published < published && ticks < 20000U);
    YEW_ASSERT(ticks < 20000U);
}

static void git_editor_lines(Bytebuf *base, Bytebuf *live, u32 lines,
                             u32 change_stride)
{
    u32 i;

    bytebuf_init(base);
    bytebuf_init(live);
    for (i = 0U; i < lines; i++) {
        bytebuf_printf(base, "line-%05u shared payload\n", i);
        if (change_stride != 0U && i % change_stride == 0U)
            bytebuf_printf(live, "line-%05u changed payload\n", i);
        else
            bytebuf_printf(live, "line-%05u shared payload\n", i);
    }
}

static void git_editor_assert_text(const TextBuf *tb, const u8 *want,
                                   size_t want_len)
{
    TextIter it;
    const u8 *bytes;
    u64 len;
    size_t at = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), want_len);
    if (want_len == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    do {
        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        YEW_ASSERT(len <= want_len - at);
        YEW_ASSERT(memcmp(bytes, want + at, (size_t)len) == 0);
        at += (size_t)len;
    } while (at < want_len && yew_textiter_advance(&it, tb));
    YEW_ASSERT_EQ_U64(at, want_len);
}

static Cursor git_editor_selection(const TextBuf *tb, u32 line)
{
    ByteOff start = yew_textbuf_line_start(tb, LINENO(line));
    Cursor cursor = {start, {0U}, BYTEOFF(start.v + 1U)};

    return cursor;
}

void test_git_editor_runtime_slices_and_publishes_atomically(void)
{
    Bytebuf base;
    Bytebuf live;
    GitEditorClock clock = {0, 50};
    YewGitEditorStats stats;
    const HunkList *hunks;
    Ed ed;

    git_editor_lines(&base, &live, 3000U, 100U);
    git_editor_fixture(&ed, live.data, live.len);
    yew_git_editor_test_clock(&ed, git_editor_fake_now, &clock);
    YEW_ASSERT(yew_git_editor_test_base(&ed, &ed.buffer,
                                        base.data, base.len, "index-1",
                                        false, 1000));
    hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
    YEW_ASSERT_NOT_NULL(hunks);
    YEW_ASSERT_EQ_U64(hunks->h.len, 0U);
    yew_git_editor_tick(&ed, 1000);
    yew_git_editor_stats(&ed, &stats);
    YEW_ASSERT_EQ_U64(stats.diff_published, 0U);
    YEW_ASSERT(yew_git_editor_deadline(&ed, 1000) == 0);
    git_editor_pump_until(&ed, 1000, 1U);
    yew_git_editor_stats(&ed, &stats);
    hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
    YEW_ASSERT_EQ_U64(stats.diff_started, 1U);
    YEW_ASSERT(stats.diff_slices > 1U);
    YEW_ASSERT_EQ_U64(stats.diff_published, 1U);
    YEW_ASSERT(stats.diff_max_slice_us <= YEW_DIFF_BUDGET_US);
    YEW_ASSERT_EQ_U64(hunks->h.len, 30U);
    YEW_ASSERT_EQ_U64(hunks->buf_gen, ed.buffer.tb->gen);
    YEW_ASSERT(yew_git_editor_deadline(&ed, 1000) == -1);
    yew_ed_free(&ed);
    bytebuf_free(&live);
    bytebuf_free(&base);
}

void test_git_editor_runtime_cancels_stale_work_before_publish(void)
{
    Bytebuf base;
    Bytebuf live;
    GitEditorClock clock = {0, 100};
    YewGitEditorStats stats;
    const HunkList *hunks;
    Ed ed;

    git_editor_lines(&base, &live, 4000U, 80U);
    git_editor_fixture(&ed, live.data, live.len);
    yew_git_editor_test_clock(&ed, git_editor_fake_now, &clock);
    YEW_ASSERT(yew_git_editor_test_base(&ed, &ed.buffer,
                                        base.data, base.len, "index-2",
                                        false, 2000));
    yew_git_editor_tick(&ed, 2000);
    yew_textbuf_insert(ed.buffer.tb,
                       BYTEOFF(yew_textbuf_len(ed.buffer.tb)),
                       (const u8 *)"new-tail\n", 9U);
    ed.now_ms = 2001;
    yew_git_editor_prepare(&ed, ed.win);
    yew_git_editor_stats(&ed, &stats);
    YEW_ASSERT_EQ_U64(stats.diff_cancelled, 1U);
    YEW_ASSERT_EQ_U64(stats.diff_published, 0U);
    hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
    YEW_ASSERT_EQ_U64(hunks->h.len, 0U);
    git_editor_pump_until(&ed, 2151, 1U);
    yew_git_editor_stats(&ed, &stats);
    hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
    YEW_ASSERT_EQ_U64(stats.diff_started, 2U);
    YEW_ASSERT_EQ_U64(stats.diff_published, 1U);
    YEW_ASSERT_EQ_U64(hunks->buf_gen, ed.buffer.tb->gen);
    YEW_ASSERT(hunks->h.len != 0U);
    yew_ed_free(&ed);
    bytebuf_free(&live);
    bytebuf_free(&base);
}

void test_git_editor_runtime_debounces_two_hundred_edits_to_one_diff(void)
{
    static const u8 initial[] = "alpha\n";
    GitEditorClock clock = {0, 25};
    YewGitEditorStats before;
    YewGitEditorStats after;
    Ed ed;
    u32 i;

    git_editor_fixture(&ed, initial, sizeof(initial) - 1U);
    yew_git_editor_test_clock(&ed, git_editor_fake_now, &clock);
    YEW_ASSERT(yew_git_editor_test_base(&ed, &ed.buffer,
                                        initial, sizeof(initial) - 1U,
                                        "index-3", false, 3000));
    git_editor_pump_until(&ed, 3000, 1U);
    yew_git_editor_stats(&ed, &before);
    for (i = 0U; i < 200U; i++) {
        yew_textbuf_insert(ed.buffer.tb,
                           BYTEOFF(yew_textbuf_len(ed.buffer.tb)),
                           (const u8 *)"x", 1U);
        ed.now_ms = 4000 + (i64)i;
        yew_git_editor_prepare(&ed, ed.win);
    }
    yew_git_editor_tick(&ed, 4348);
    yew_git_editor_stats(&ed, &after);
    YEW_ASSERT_EQ_U64(after.diff_started, before.diff_started);
    git_editor_pump_until(&ed, 4349, before.diff_published + 1U);
    yew_git_editor_stats(&ed, &after);
    YEW_ASSERT_EQ_U64(after.diff_started, before.diff_started + 1U);
    YEW_ASSERT_EQ_U64(after.diff_published, before.diff_published + 1U);
    YEW_ASSERT(after.diff_max_slice_us <= YEW_DIFF_BUDGET_US);
    yew_ed_free(&ed);
}

void test_git_editor_discard_all_selections_is_one_undo_step(void)
{
    static const u8 base[] =
        "zero\none\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n";
    static const u8 live[] =
        "zero\nONE\ntwo\nthree\nfour\nFIVE\nsix\nseven\neight\nNINE\nten\n";
    CmdCtx cx = {0};
    CmdId discard;
    CmdId undo;
    const CmdDesc *desc;
    Cursor cursor;
    char *fixture_path;
    const HunkList *hunks;
    Ed ed;

    git_editor_fixture(&ed, live, sizeof(live) - 1U);
    YEW_ASSERT(yew_git_editor_test_base(&ed, &ed.buffer,
                                        base, sizeof(base) - 1U,
                                        "index-selection", true, 5000));
    git_editor_pump_until(&ed, 5000, 1U);
    hunks = yew_git_editor_test_hunks(&ed, &ed.buffer);
    YEW_ASSERT_NOT_NULL(hunks);
    YEW_ASSERT_EQ_U64(hunks->h.len, 3U);
    YEW_ASSERT(hunks->base_is_head);

    ed.win->cs.curs.data[0] = git_editor_selection(ed.buffer.tb, 1U);
    cursor = git_editor_selection(ed.buffer.tb, 5U);
    YEW_ASSERT(yew_cset_add(&ed.win->cs, cursor));
    cursor = git_editor_selection(ed.buffer.tb, 9U);
    YEW_ASSERT(yew_cset_add(&ed.win->cs, cursor));

    discard = yew_cmd_lookup("ed.git.hunk.discard", 19U);
    YEW_ASSERT(discard.v != 0U);
    desc = yew_cmd_desc(discard);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT((desc->flags & YEW_CMD_CHANGES_BUFFER) != 0U);
    YEW_ASSERT((desc->flags & YEW_CMD_MULTI_AGGREGATE) != 0U);
    /* This is an in-memory fixture: keep command durability semantics while
     * avoiding a synthetic file identity with no crash-journal path. */
    fixture_path = ed.buffer.path;
    ed.buffer.path = NULL;
    cx.win = ed.win;
    cx.count = 1U;
    cx.source = YEW_SRC_TEST;
    YEW_ASSERT_EQ_U64(yew_ed_invoke(&ed, discard, &cx), YEW_CMD_OK);
    git_editor_assert_text(ed.buffer.tb, base, sizeof(base) - 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo),
                      ed.buffer.undo->root + 1U);
    YEW_ASSERT_EQ_U64(ed.buffer.undo->depth, 0U);

    undo = yew_cmd_lookup("ed.edit.undo", 12U);
    YEW_ASSERT(undo.v != 0U);
    YEW_ASSERT_EQ_U64(yew_ed_invoke(&ed, undo, &cx), YEW_CMD_OK);
    git_editor_assert_text(ed.buffer.tb, live, sizeof(live) - 1U);
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.buffer.undo), ed.buffer.undo->root);
    ed.buffer.path = fixture_path;
    yew_ed_free(&ed);
}
