#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "text/file.h"
#include "text/undo.h"
#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/viewport.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

enum {
    SCROLL_LINES = 10000,
    SCROLL_FRAMES = 240
};

typedef struct {
    const char *name;
    u16 cols;
    u16 rows;
    bool wrap;
    double minimum_fps;
} ScrollCase;

static volatile u64 scroll_sink;

static bool now_ns(i64 *out)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return false;
    *out = (i64)ts.tv_sec * INT64_C(1000000000) + (i64)ts.tv_nsec;
    return true;
}

static bool load_baselines(ScrollCase *cases, size_t count)
{
    FILE *file = fopen("tests/perf/baselines/scroll.txt", "r");
    char line[160];
    bool found[4] = {false, false, false, false};

    if (file == NULL) {
        (void)fprintf(stderr, "perf_scroll: cannot read baseline\n");
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[64];
        double minimum;
        size_t i;

        if (line[0] == '#' || sscanf(line, "%63s %lf", name, &minimum) != 2)
            continue;
        for (i = 0U; i < count; i++) {
            if (strcmp(name, cases[i].name) == 0) {
                cases[i].minimum_fps = minimum;
                found[i] = true;
                break;
            }
        }
    }
    if (fclose(file) != 0)
        return false;
    for (size_t i = 0U; i < count; i++) {
        if (!found[i] || cases[i].minimum_fps < 120.0) {
            (void)fprintf(stderr, "perf_scroll: missing hard gate for %s\n",
                          cases[i].name);
            return false;
        }
    }
    return true;
}

static void make_fixture(Bytebuf *fixture)
{
    u32 line;

    bytebuf_init(fixture);
    for (line = 0U; line < SCROLL_LINES; line++) {
        u32 cluster;

        if (line == SCROLL_LINES / 2U) {
            bytebuf_append(fixture, "pathological_long_line ", 23U);
            for (cluster = 0U; cluster < 32768U; cluster++) {
                if (cluster % 17U == 0U)
                    bytebuf_append(fixture, "\xE6\xBC\xA2", 3U);
                else if (cluster % 13U == 0U)
                    bytebuf_push_u8(fixture, (u8)'\t');
                else
                    bytebuf_push_u8(fixture, (u8)'x');
            }
            bytebuf_push_u8(fixture, (u8)'\n');
            continue;
        }
        bytebuf_printf(fixture,
                       "static int sagitta_scroll_%05u(void) { return %u; }"
                       "\t/* \xE6\xBC\xA2\xE5\xAD\x97 /viewport/statusline/benchmark */\n",
                       line, line);
    }
}

static bool measure(const ScrollCase *pc, const Bytebuf *fixture,
                    double *fps)
{
    Ed ed;
    Buffer buffer;
    Buffer *bufptrs[1];
    Win win;
    Cursor cursor;
    i64 start;
    i64 end;
    unsigned int frame;
    TtyCaps caps;
    bool ok = false;

    (void)memset(&ed, 0, sizeof(ed));
    (void)memset(&buffer, 0, sizeof(buffer));
    (void)memset(&win, 0, sizeof(win));
    (void)memset(&caps, 0, sizeof(caps));
    arena_init(&ed.arena);
    interner_init(&ed.interner, &ed.arena);
    bytebuf_init(&ed.frame);
    if (!sag_grid_init(&ed.grid, &ed.interner, pc->rows, pc->cols))
        goto done_arena;

    buffer.tb = sag_textbuf_from_bytes(fixture->data, fixture->len);
    buffer.undo = sag_undo_new(buffer.tb);
    sag_undo_mark_saved(buffer.undo);
    sag_filemeta_init(&buffer.meta);
    buffer.path = (char *)"tests/perf/scroll-fixture.c";

    cursor.pos = sag_textbuf_line_start(buffer.tb,
                                        LINENO(SCROLL_LINES / 2U));
    cursor.anchor = cursor.pos;
    cursor.goal_col = (GCol){0U};
    win.buf = &buffer;
    sag_cset_init(&win.cs, cursor);
    sag_vp_init(&win);
    win.number_style = SAG_NUM_HYBRID;
    win.vp.wrap = pc->wrap;

    ed.mode = SAG_MODE_L;
    ed.prev_unit = SAG_MODE_L;
    ed.win = &win;
    bufptrs[0] = &buffer;
    ed.ws.bufs = bufptrs;
    ed.ws.nbufs = 1U;
    sag_render_init(&ed.render, &caps, NULL);
    sag_layout(&ed);
    sag_draw_win(&ed, &win);
    sag_grid_mark_all(&ed.grid);
    (void)sag_render_frame(&ed.render, &ed.grid, &ed.frame);
    sag_grid_flip(&ed.grid);

    if (!now_ns(&start))
        goto done_model;
    for (frame = 0U; frame < SCROLL_FRAMES; frame++) {
        sag_vp_scroll(&win, 1);
        sag_vp_push_cursor(&win);
        sag_draw_win(&ed, &win);
        sag_grid_mark_all(&ed.grid);
        ed.frame.len = 0U;
        scroll_sink ^= (u64)sag_render_frame(&ed.render, &ed.grid,
                                             &ed.frame);
        scroll_sink ^= (u64)ed.frame.len;
        sag_grid_flip(&ed.grid);
    }
    if (!now_ns(&end) || end <= start)
        goto done_model;
    *fps = (double)SCROLL_FRAMES * 1000000000.0 / (double)(end - start);
    ok = true;

done_model:
    sag_vp_free(&win);
    sag_cset_free(&win.cs);
    sag_undo_free(buffer.undo);
    sag_textbuf_free(buffer.tb);
    sag_filemeta_dispose(&buffer.meta);
    sag_grid_free(&ed.grid);
done_arena:
    bytebuf_free(&ed.frame);
    interner_free(&ed.interner);
    arena_free_all(&ed.arena);
    return ok;
}

int main(void)
{
    ScrollCase cases[] = {
        {"scroll_nowrap_80x24", 80U, 24U, false, 0.0},
        {"scroll_wrap_80x24", 80U, 24U, true, 0.0},
        {"scroll_nowrap_200x60", 200U, 60U, false, 0.0},
        {"scroll_wrap_200x60", 200U, 60U, true, 0.0}
    };
    Bytebuf fixture;
    size_t i;
    int status = 0;

    if (!load_baselines(cases, SAG_ARRAY_LEN(cases)))
        return 2;
    make_fixture(&fixture);
    for (i = 0U; i < SAG_ARRAY_LEN(cases); i++) {
        double fps;

        if (!measure(&cases[i], &fixture, &fps)) {
            (void)fprintf(stderr, "perf_scroll: %s measurement failed\n",
                          cases[i].name);
            bytebuf_free(&fixture);
            return 2;
        }
        (void)printf("%s %.2f fps (minimum %.2f)%s\n", cases[i].name,
                     fps, cases[i].minimum_fps,
                     fps < cases[i].minimum_fps ? " FAIL" : "");
        if (fps < cases[i].minimum_fps)
            status = 1;
    }
    bytebuf_free(&fixture);
    return status;
}
