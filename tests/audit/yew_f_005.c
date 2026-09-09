/*
 * YEW-F-005 — replacement with multiple cursors exits as an internal error.
 *
 * Correct behavior: a replace-all crossing a live multi-cursor set applies
 * every match back-to-front in one undo transaction, adjusts both cursors,
 * and returns normally. One undo then restores the text and cursor set.
 *
 * Baseline failure: an outer Fletch edit block owns a MACRO transaction.
 * The replacement plan preserves that open transaction, so its first edit
 * reaches the multi-cursor choke point with the wrong reason and exits with
 * YEW_EXIT_BUG. The child process contains that expected baseline exit so
 * the remaining audit cases still run.
 */
#include "audit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/search_cmds.h"

static bool buffer_is(const TextBuf *tb, const char *expected, size_t len)
{
    TextIter it;
    u64 at = 0U;

    if (yew_textbuf_len(tb) != (u64)len)
        return false;
    if (len == 0U)
        return true;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(0U)))
        return false;
    for (;;) {
        const u8 *chunk = NULL;
        u64 n = 0U;

        if (!yew_textiter_chunk(&it, tb, &chunk, &n) || n == 0U)
            return false;
        if (n > (u64)len - at ||
            memcmp(chunk, expected + (size_t)at, (size_t)n) != 0)
            return false;
        at += n;
        if (at == (u64)len)
            return true;
        if (!yew_textiter_advance(&it, tb))
            return false;
    }
}

static int replace_with_two_cursors(void)
{
    static const char original[] = "aa bb cc\n";
    static const char replaced[] = "LONGLONG bb cc\n";
    Ed ed;
    Cursor second = {BYTEOFF(6U), {0U}, BYTEOFF(6U)};
    CmdCtx cx = {0};
    EditCtx ec;
    CmdStatus command_status;
    bool correct;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed))
        return 2;
    ec = yew_ed_edit_ctx(&ed);
    if (!yew_edit_insert(&ec, BYTEOFF(0U), (const u8 *)original,
                         sizeof(original) - 1U))
        return 2;
    ed.win->cs.curs.data[0].pos = BYTEOFF(3U);
    ed.win->cs.curs.data[0].anchor = BYTEOFF(3U);
    if (!yew_cset_add(&ed.win->cs, second))
        return 2;

    /* This is the Fletch entry path exactly: fl_api's enlistment opens the
     * outer MACRO transaction with cset withheld, because yew_undo_begin
     * otherwise rightly refuses a non-MULTI transaction with two cursors.
     * The command itself receives the live context and must still succeed. */
    ec = yew_ed_edit_ctx(&ed);
    ec.cset = NULL;
    yew_undo_begin(&ec, YEW_TXN_MACRO);

    cx.ed = &ed;
    cx.win = ed.win;
    cx.range.kind = YEW_RANGE_BUFFER;
    cx.range.given = true;
    cx.sarg = "/a/LONG/g";
    cx.sarg_len = 9U;
    cx.source = YEW_SRC_TEST;
    command_status = yew_search_cmd_replace(&cx);
    ec = yew_ed_edit_ctx(&ed);
    yew_undo_end(&ec);
    yew_ed_finish_edit(&ed, &ec);
    correct = command_status == YEW_CMD_OK &&
              buffer_is(ed.buffer.tb, replaced, sizeof(replaced) - 1U) &&
              ed.win->cs.curs.len == 2U &&
              ed.win->cs.curs.data[0].pos.v == 9U &&
              ed.win->cs.curs.data[1].pos.v == 12U;
    if (!correct)
        return 3;
    ec = yew_ed_edit_ctx(&ed);
    if (!yew_undo(&ec))
        return 3;
    yew_ed_finish_edit(&ed, &ec);
    correct = buffer_is(ed.buffer.tb, original, sizeof(original) - 1U) &&
              ed.win->cs.curs.len == 2U &&
              ed.win->cs.curs.data[0].pos.v == 3U &&
              ed.win->cs.curs.data[1].pos.v == 6U;
    yew_ed_free(&ed);
    return correct ? 0 : 3;
}

bool test_yew_f_005(char *why, size_t why_cap)
{
    char captured[512];
    size_t captured_len = 0U;
    int errpipe[2];
    pid_t child;
    pid_t waited;
    int status = 0;

    if (pipe(errpipe) != 0) {
        (void)snprintf(why, why_cap, "pipe failed: errno %d", errno);
        return true; /* Infrastructure failure must be a hard XPASS. */
    }
    (void)fflush(NULL);
    child = fork();
    if (child < 0) {
        (void)close(errpipe[0]);
        (void)close(errpipe[1]);
        (void)snprintf(why, why_cap, "fork failed: errno %d", errno);
        return true; /* Infrastructure failure must be a hard XPASS. */
    }
    if (child == 0) {
        (void)close(errpipe[0]);
        if (dup2(errpipe[1], STDERR_FILENO) < 0)
            _Exit(2);
        (void)close(errpipe[1]);
        _Exit(replace_with_two_cursors());
    }
    (void)close(errpipe[1]);
    while (captured_len + 1U < sizeof(captured)) {
        ssize_t got = read(errpipe[0], captured + captured_len,
                           sizeof(captured) - captured_len - 1U);

        if (got > 0) {
            captured_len += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        break;
    }
    captured[captured_len] = '\0';
    (void)close(errpipe[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        (void)snprintf(why, why_cap, "waitpid failed: errno %d", errno);
        return true;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
    if (WIFEXITED(status) && WEXITSTATUS(status) == YEW_EXIT_BUG &&
        strstr(captured,
               "multi-cursor edits require a MULTI transaction") != NULL) {
        (void)snprintf(why, why_cap,
                       "two-cursor replace exited %d: %s",
                       YEW_EXIT_BUG,
                       "multi-cursor edits require a MULTI transaction");
        return false;
    }
    /* A different child failure is not the recorded finding. Treat it like
     * an XPASS so the audit runner fails instead of masking new breakage. */
    return true;
}
