#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/multicursor.h"
#include "term/render.h"
#include "text/edit.h"
#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/viewport.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

enum {
    FIXTURE_LINES = 10000,
    PERF_ROUNDS = 5,
    PERF_ROWS = 24,
    PERF_COLS = 80
};

static volatile u64 perf_multicursor_sink;

static void timeout_handler(int signo)
{
    static const char message[] =
        "perf-multicursor: edit exceeded 2-second safety timeout\n";
    ssize_t written;

    (void)signo;
    written = write(STDERR_FILENO, message, sizeof(message) - 1U);
    (void)written;
    _Exit(1);
}

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static bool model_init(Ed *ed, Win *win, size_t cursor_count)
{
    u8 *bytes = malloc(FIXTURE_LINES * 2U);
    Cursor *cursors = malloc(cursor_count * sizeof(*cursors));
    Cursor primary = {BYTEOFF(0U), {0U}, BYTEOFF(0U)};
    TtyCaps caps;
    size_t i;

    if (bytes == NULL || cursors == NULL) {
        free(cursors);
        free(bytes);
        return false;
    }
    for (i = 0U; i < FIXTURE_LINES; i++) {
        bytes[i * 2U] = 'a';
        bytes[i * 2U + 1U] = '\n';
    }
    (void)memset(ed, 0, sizeof(*ed));
    (void)memset(win, 0, sizeof(*win));
    (void)memset(&caps, 0, sizeof(caps));
    arena_init(&ed->arena);
    interner_init(&ed->interner, &ed->arena);
    bytebuf_init(&ed->frame);
    if (!yew_grid_init(&ed->grid, &ed->interner, PERF_ROWS, PERF_COLS)) {
        interner_free(&ed->interner);
        arena_free_all(&ed->arena);
        bytebuf_free(&ed->frame);
        free(cursors);
        free(bytes);
        return false;
    }
    ed->buffer.tb = yew_textbuf_from_owned_bytes(bytes, FIXTURE_LINES * 2U);
    ed->buffer.undo = yew_undo_new(ed->buffer.tb);
    ed->buffer.marks = yew_marks_new();
    win->buf = &ed->buffer;
    yew_cset_init(&win->cs, primary);
    yew_vp_init(win);
    for (i = 1U; i < cursor_count; i++) {
        size_t line = (i * FIXTURE_LINES) / cursor_count;

        cursors[i - 1U].pos = BYTEOFF(line * 2U);
        cursors[i - 1U].anchor = cursors[i - 1U].pos;
        cursors[i - 1U].goal_col = (GCol){0U};
    }
    if (!yew_cset_add_many(&win->cs, cursors, (u32)cursor_count - 1U)) {
        free(cursors);
        yew_vp_free(win);
        yew_cset_free(&win->cs);
        yew_marks_free(ed->buffer.marks);
        yew_undo_free(ed->buffer.undo);
        yew_textbuf_free(ed->buffer.tb);
        yew_grid_free(&ed->grid);
        interner_free(&ed->interner);
        arena_free_all(&ed->arena);
        bytebuf_free(&ed->frame);
        return false;
    }
    free(cursors);
    ed->win = win;
    ed->mode = YEW_MODE_L;
    ed->prev_unit = YEW_MODE_L;
    ed->model_ready = true;
    yew_render_init(&ed->render, &caps, NULL);
    yew_layout(ed);
    yew_draw_win(ed, win);
    yew_grid_mark_all(&ed->grid);
    (void)yew_render_frame(&ed->render, &ed->grid, &ed->frame);
    yew_grid_flip(&ed->grid);
    return win->cs.curs.len == cursor_count;
}

static void model_free(Ed *ed, Win *win)
{
    yew_vp_free(win);
    yew_cset_free(&win->cs);
    yew_marks_free(ed->buffer.marks);
    yew_undo_free(ed->buffer.undo);
    yew_textbuf_free(ed->buffer.tb);
    yew_grid_free(&ed->grid);
    bytebuf_free(&ed->frame);
    interner_free(&ed->interner);
    arena_free_all(&ed->arena);
}

static size_t prepare_paint(Ed *ed, Win *win)
{
    size_t emitted;

    yew_draw_win(ed, win);
    ed->frame.len = 0U;
    emitted = yew_render_frame(&ed->render, &ed->grid, &ed->frame);
    yew_grid_flip(&ed->grid);
    return emitted;
}

static bool measure(size_t cursor_count, i64 budget_ns)
{
    i64 samples[PERF_ROUNDS];
    size_t round;

    for (round = 0U; round < PERF_ROUNDS; round++) {
        Ed ed;
        Win win;
        CmdCtx cx = {0};
        EditCtx ec;
        CmdStatus status;
        i64 start;
        i64 elapsed;

        if (!model_init(&ed, &win, cursor_count))
            return false;
        cx.ed = &ed;
        cx.win = &win;
        cx.count = 1U;
        cx.sarg = "x";
        cx.sarg_len = 1U;
        cx.source = YEW_SRC_TEST;
        ec = yew_ed_edit_ctx(&ed);
        start = now_ns();
        (void)alarm(2U);
        yew_undo_begin(&ec, YEW_TXN_MULTI);
        status = yew_mc_run(&win,
                            yew_cmd_lookup("ed.edit.insert.text", 19U),
                            &cx);
        if (status == YEW_CMD_OK)
            yew_undo_end(&ec);
        else
            yew_undo_abort(&ec);
        yew_ed_finish_edit(&ed, &ec);
        perf_multicursor_sink ^= (u64)prepare_paint(&ed, &win);
        (void)alarm(0U);
        if (start < 0 || status != YEW_CMD_OK) {
            model_free(&ed, &win);
            return false;
        }
        elapsed = now_ns() - start;
        if (elapsed < 0 ||
            yew_textbuf_len(ed.buffer.tb) != FIXTURE_LINES * 2U +
                                              cursor_count) {
            model_free(&ed, &win);
            return false;
        }
        yew_cset_check_text(ed.buffer.tb, &win.cs);
        samples[round] = elapsed;
        perf_multicursor_sink ^= yew_textbuf_len(ed.buffer.tb) +
                                 win.cs.curs.len;
        model_free(&ed, &win);
    }
    for (round = 1U; round < PERF_ROUNDS; round++) {
        i64 value = samples[round];
        size_t at = round;

        while (at != 0U && samples[at - 1U] > value) {
            samples[at] = samples[at - 1U];
            at--;
        }
        samples[at] = value;
    }
    (void)printf("perf-multicursor: cursors=%zu p99_ms=%.3f budget_ms=%.3f\n",
                 cursor_count,
                 (double)samples[PERF_ROUNDS - 1U] / 1000000.0,
                 (double)budget_ns / 1000000.0);
    return samples[PERF_ROUNDS - 1U] <= budget_ns;
}

int main(void)
{
    struct sigaction action;
    bool ok;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = timeout_handler;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0)
        return 2;
    ok = measure(1000U, INT64_C(5000000));
    ok = measure(10000U, INT64_C(50000000)) && ok;
    if (!ok)
        (void)fprintf(stderr, "perf-multicursor: latency budget exceeded\n");
    return ok ? 0 : 1;
}
