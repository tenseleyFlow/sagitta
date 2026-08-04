/* Sprint 19 §5 + DoD 5, 6: the synchronous region filter.
 *
 * Every failure row must leave the buffer byte-identical, success must be
 * exactly one undo node, and the interleaved write/read loop must survive
 * a command that floods stdout before reading stdin. */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/shell.h"
#include "text/piece.h"

static void filter_fixture(Ed *ed, const char *text)
{
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
    sag_undo_free(ed->buffer.undo);
    sag_textbuf_free(ed->buffer.tb);
    ed->buffer.tb = sag_textbuf_from_bytes((const u8 *)text,
                                           (u64)strlen(text));
    ed->buffer.undo = sag_undo_new(ed->buffer.tb);
    sag_undo_mark_saved(ed->buffer.undo);
    ed->win->vp.rows = 10U;
    ed->win->vp.cols = 80U;
}

static Span whole(const Ed *ed)
{
    Span s;

    s.lo = 0U;
    s.hi = sag_textbuf_len(ed->buffer.tb);
    return s;
}

/* Materializes the buffer so tests can byte-compare it. */
static void buffer_text(const Ed *ed, Bytebuf *out)
{
    TextIter it;
    u64 at = 0U;
    u64 end = sag_textbuf_len(ed->buffer.tb);

    while (at < end) {
        const u8 *chunk = NULL;
        size_t len = 0U;

        if (!sag_textiter_begin(&it, ed->buffer.tb, BYTEOFF(at)) ||
            !sag_textiter_chunk(&it, ed->buffer.tb, &chunk, &len) ||
            len == 0U)
            break;
        if ((u64)len > end - at)
            len = (size_t)(end - at);
        bytebuf_append(out, chunk, len);
        at += (u64)len;
    }
}

static void assert_text(const Ed *ed, const char *expect)
{
    Bytebuf got;

    bytebuf_init(&got);
    buffer_text(ed, &got);
    SAG_ASSERT_EQ_U64((u64)got.len, (u64)strlen(expect));
    if (got.len != 0U)
        SAG_ASSERT_EQ_MEM(got.data, expect, got.len);
    bytebuf_free(&got);
}

/* Undo-node count, so "exactly one transaction" is measurable. */
static u32 undo_nodes(const Ed *ed)
{
    UndoNodeInfo info[64];

    return sag_undo_list(ed->buffer.undo, info, SAG_ARRAY_LEN(info));
}

void test_filter_ok_replaces_region_in_one_transaction(void)
{
    Ed ed;
    SagFilterResult r;
    u32 before;

    filter_fixture(&ed, "beta\nalpha\ngamma\n");
    before = undo_nodes(&ed);
    r = sag_shell_filter(&ed, ed.win, whole(&ed), "sort", NULL);
    SAG_ASSERT_EQ_I64((i64)r, (i64)SAG_FILT_OK);
    assert_text(&ed, "alpha\nbeta\ngamma\n");
    /* DoD 5: exactly one node, so one undo restores the original. */
    SAG_ASSERT_EQ_U64(undo_nodes(&ed) - before, 1U);
    sag_ed_free(&ed);
}

void test_filter_undo_restores_original_exactly(void)
{
    Ed ed;
    EditCtx ec;

    filter_fixture(&ed, "beta\nalpha\ngamma\n");
    SAG_ASSERT(sag_shell_filter(&ed, ed.win, whole(&ed), "sort", NULL) ==
               SAG_FILT_OK);
    ec = sag_ed_edit_ctx(&ed);
    SAG_ASSERT(sag_undo(&ec));
    assert_text(&ed, "beta\nalpha\ngamma\n");
    sag_ed_free(&ed);
}

void test_filter_nonzero_exit_leaves_buffer_untouched(void)
{
    Ed ed;
    SagFilterResult r;
    u32 before;

    filter_fixture(&ed, "keep me\n");
    before = undo_nodes(&ed);
    /* Rollback is deliberate: replacing the region with a diagnostic
     * destroys the text AND hides the error. */
    r = sag_shell_filter(&ed, ed.win, whole(&ed),
                         "echo boom >&2; exit 2", NULL);
    SAG_ASSERT(r == SAG_FILT_NONZERO);
    assert_text(&ed, "keep me\n");
    SAG_ASSERT_EQ_U64(undo_nodes(&ed), before);
    sag_ed_free(&ed);
}

void test_filter_spawn_failure_leaves_buffer_untouched(void)
{
    Ed ed;
    SagFilterResult r;

    filter_fixture(&ed, "keep me\n");
    /* 127 from the shell is a command-not-found, i.e. a nonzero exit;
     * either way the buffer must survive intact. */
    r = sag_shell_filter(&ed, ed.win, whole(&ed),
                         "definitely-not-a-real-command-xyz", NULL);
    SAG_ASSERT(r == SAG_FILT_NONZERO || r == SAG_FILT_SPAWN);
    assert_text(&ed, "keep me\n");
    sag_ed_free(&ed);
}

void test_filter_timeout_leaves_buffer_untouched(void)
{
    Ed ed;
    SagFilterResult r;

    filter_fixture(&ed, "keep me\n");
    /* Reads stdin then hangs, so it outlives the filter timeout. */
    r = sag_shell_filter(&ed, ed.win, whole(&ed),
                         "cat >/dev/null; trap '' TERM; sleep 30", NULL);
    SAG_ASSERT(r == SAG_FILT_TIMEOUT);
    assert_text(&ed, "keep me\n");
    sag_ed_free(&ed);
}

void test_filter_empty_output_deletes_region(void)
{
    Ed ed;
    SagFilterResult r;
    u32 before;

    filter_fixture(&ed, "alpha\nbeta\n");
    before = undo_nodes(&ed);
    /* Exit 0 with no output: NOT rolled back, because a filter can
     * legitimately produce nothing.  It is announced instead, since a
     * silently vanished selection reads as data loss.
     *
     * Note `grep` with no match does NOT land here — it exits 1, which is
     * the NONZERO row and keeps the region.  That difference is the whole
     * reason the two rows are specified separately. */
    r = sag_shell_filter(&ed, ed.win, whole(&ed), "cat >/dev/null", NULL);
    SAG_ASSERT(r == SAG_FILT_OK);
    assert_text(&ed, "");
    /* The deletion is still one transaction, so undo brings it back. */
    SAG_ASSERT_EQ_U64(undo_nodes(&ed) - before, 1U);
    sag_ed_free(&ed);
}

void test_filter_partial_region_leaves_the_rest(void)
{
    Ed ed;
    Span region;

    filter_fixture(&ed, "head\nbeta\nalpha\ntail\n");
    /* Only the middle two lines go through sort. */
    region.lo = 5U;
    region.hi = 16U;
    SAG_ASSERT(sag_shell_filter(&ed, ed.win, region, "sort", NULL) ==
               SAG_FILT_OK);
    assert_text(&ed, "head\nalpha\nbeta\ntail\n");
    sag_ed_free(&ed);
}

void test_filter_survives_output_before_stdin_is_read(void)
{
    Ed ed;
    Bytebuf big;
    u64 i;
    SagFilterResult r;
    i64 start;

    /* THE DEADLOCK REGRESSION (DoD 6).  This command writes 1 MiB to
     * stdout BEFORE reading a byte of stdin.  If the filter wrote the
     * whole region before draining stdout, both sides would block once
     * the 64 KiB pipe filled and neither would ever move.  Removing the
     * POLLOUT/POLLIN interleave in sag_shell_filter hangs this test. */
    bytebuf_init(&big);
    for (i = 0U; i < 40000U; i++)
        bytebuf_append(&big, "0123456789abcdefghijklmnopqrstuvwxyz\n", 37U);
    bytebuf_push_u8(&big, 0U);
    filter_fixture(&ed, (const char *)big.data);
    start = sag_now_ms();
    r = sag_shell_filter(&ed, ed.win, whole(&ed),
                         "yes 0123456789abcdefghijklmnopqrstuvwxyz "
                         "| head -c 1048576; cat >/dev/null",
                         NULL);
    SAG_ASSERT(r == SAG_FILT_OK);
    /* Well inside the 5 s filter timeout, i.e. it did not merely time out. */
    SAG_ASSERT(sag_now_ms() - start < 4000);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), 1048576U);
    bytebuf_free(&big);
    sag_ed_free(&ed);
}

void test_filter_tolerates_epipe_from_early_exit(void)
{
    Ed ed;
    Bytebuf big;
    u64 i;
    SagFilterResult r;

    /* `head -1` exits after one line, so our writes get EPIPE with most
     * of the region unsent.  That is not an error: stop writing, close
     * stdin, keep draining output. */
    bytebuf_init(&big);
    for (i = 0U; i < 30000U; i++)
        bytebuf_append(&big, "line of text goes here\n", 23U);
    bytebuf_push_u8(&big, 0U);
    filter_fixture(&ed, (const char *)big.data);
    r = sag_shell_filter(&ed, ed.win, whole(&ed), "head -1", NULL);
    SAG_ASSERT(r == SAG_FILT_OK);
    assert_text(&ed, "line of text goes here\n");
    bytebuf_free(&big);
    sag_ed_free(&ed);
}

void test_filter_preserves_invalid_utf8_bytes(void)
{
    Ed ed;
    Bytebuf got;
    static const u8 raw[] = {'a', 0xFFU, 'b', 0xC3U, '\n'};

    /* Invariant 2: `cat` must return the bytes we gave it, byte-exact.
     * Storage is verbatim; the escape policy governs rendering only. */
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    sag_undo_free(ed.buffer.undo);
    sag_textbuf_free(ed.buffer.tb);
    ed.buffer.tb = sag_textbuf_from_bytes(raw, (u64)sizeof(raw));
    ed.buffer.undo = sag_undo_new(ed.buffer.tb);
    ed.win->vp.rows = 10U;
    ed.win->vp.cols = 80U;

    SAG_ASSERT(sag_shell_filter(&ed, ed.win, whole(&ed), "cat", NULL) ==
               SAG_FILT_OK);
    bytebuf_init(&got);
    buffer_text(&ed, &got);
    SAG_ASSERT_EQ_U64((u64)got.len, (u64)sizeof(raw));
    SAG_ASSERT_EQ_MEM(got.data, raw, sizeof(raw));
    bytebuf_free(&got);
    sag_ed_free(&ed);
}

void test_filter_large_output_round_trips(void)
{
    Ed ed;
    Bytebuf big;
    u64 i;

    /* 4 MiB, ~64x the pipe capacity: proves the drain keeps up across
     * many read budgets rather than only fitting in one. */
    bytebuf_init(&big);
    for (i = 0U; i < 100000U; i++)
        bytebuf_append(&big, "0123456789abcdefghijklmnopqrstuvwxyz\n", 37U);
    bytebuf_push_u8(&big, 0U);
    filter_fixture(&ed, (const char *)big.data);
    SAG_ASSERT(sag_shell_filter(&ed, ed.win, whole(&ed), "cat", NULL) ==
               SAG_FILT_OK);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(ed.buffer.tb), 3700000U);
    bytebuf_free(&big);
    sag_ed_free(&ed);
}

void test_filter_ten_thousand_lines_is_one_node(void)
{
    Ed ed;
    Bytebuf big;
    u64 i;
    u32 before;

    bytebuf_init(&big);
    for (i = 0U; i < 10000U; i++)
        bytebuf_printf(&big, "%05llu line\n", (unsigned long long)(9999U - i));
    bytebuf_push_u8(&big, 0U);
    filter_fixture(&ed, (const char *)big.data);
    before = undo_nodes(&ed);
    SAG_ASSERT(sag_shell_filter(&ed, ed.win, whole(&ed), "sort", NULL) ==
               SAG_FILT_OK);
    /* DoD 5: still exactly one node at 10k lines. */
    SAG_ASSERT_EQ_U64(undo_nodes(&ed) - before, 1U);
    bytebuf_free(&big);
    sag_ed_free(&ed);
}
