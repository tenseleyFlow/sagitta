#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include "term/grid.h"
#include "term/render.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct RenderFixture {
    Arena arena;
    Interner interner;
    Grid grid;
    Render render;
    Bytebuf out;
} RenderFixture;

static const char *render_truecolor_env(const char *name)
{
    return strcmp(name, "YEW_COLORS") == 0 ? "truecolor" : NULL;
}

static YewColor render_default_color(void)
{
    YewColor color = {YEW_COLOR_DEFAULT, 0u, 0u, 0u};

    return color;
}

static void render_fixture_init(RenderFixture *f, u16 rows, u16 cols,
                                bool sync)
{
    TtyCaps caps = {0};

    arena_init(&f->arena);
    interner_init(&f->interner, &f->arena);
    YEW_ASSERT(yew_grid_init(&f->grid, &f->interner, rows, cols));
    caps.truecolor = true;
    caps.sync_output = sync;
    yew_render_init(&f->render, &caps, render_truecolor_env);
    bytebuf_init(&f->out);
}

static void render_fixture_free(RenderFixture *f)
{
    bytebuf_free(&f->out);
    yew_grid_free(&f->grid);
    interner_free(&f->interner);
    arena_free_all(&f->arena);
}

static bool render_contains(const Bytebuf *out, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    if (n > out->len)
        return false;
    for (i = 0u; i + n <= out->len; i++) {
        if (memcmp(out->data + i, needle, n) == 0)
            return true;
    }
    return false;
}

void test_render_empty_frame_emits_zero_bytes(void)
{
    RenderFixture f;

    render_fixture_init(&f, 2u, 4u, true);
    yew_grid_flip(&f.grid);
    YEW_ASSERT_EQ_U64(yew_render_frame(&f.render, &f.grid, &f.out), 0u);
    YEW_ASSERT_EQ_U64(f.out.len, 0u);
    render_fixture_free(&f);
}

void test_render_oob_flushes_after_frame(void)
{
    static const u8 oob_a[] = {0x1bu, (u8)']', (u8)'5', (u8)'2'};
    static const u8 oob_b[] = {(u8)';', (u8)'c'};
    static const char esu[] = "\033[?2026l";
    RenderFixture f;
    YewColor color = render_default_color();
    size_t first_len;

    yew_term_oob_clear();
    render_fixture_init(&f, 1u, 1u, true);
    yew_grid_flip(&f.grid);
    yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u,
                 color, color, 0u);
    yew_term_oob_queue(oob_a, sizeof(oob_a));
    yew_term_oob_queue(oob_b, sizeof(oob_b));
    YEW_ASSERT_EQ_U64(yew_term_oob_pending(),
                      sizeof(oob_a) + sizeof(oob_b));
    (void)yew_render_frame(&f.render, &f.grid, &f.out);
    YEW_ASSERT_EQ_U64(yew_term_oob_pending(), 0u);
    YEW_ASSERT(f.out.len >= sizeof(esu) - 1u + sizeof(oob_a) +
                            sizeof(oob_b));
    YEW_ASSERT_EQ_MEM(f.out.data + f.out.len - sizeof(oob_a) - sizeof(oob_b),
                      oob_a, sizeof(oob_a));
    YEW_ASSERT_EQ_MEM(f.out.data + f.out.len - sizeof(oob_b),
                      oob_b, sizeof(oob_b));
    YEW_ASSERT_EQ_MEM(f.out.data + f.out.len - sizeof(oob_a) - sizeof(oob_b)
                                    - (sizeof(esu) - 1u),
                      esu, sizeof(esu) - 1u);

    first_len = f.out.len;
    yew_grid_flip(&f.grid);
    yew_term_oob_queue((const u8 *)"z", 1u);
    YEW_ASSERT_EQ_U64(yew_render_frame(&f.render, &f.grid, &f.out), 1u);
    YEW_ASSERT_EQ_U64(f.out.len, first_len + 1u);
    YEW_ASSERT_EQ_U64(f.out.data[f.out.len - 1u], (u8)'z');
    YEW_ASSERT_EQ_U64(yew_term_oob_pending(), 0u);
    yew_term_oob_clear();
    render_fixture_free(&f);
}

void test_render_frame_envelope_goldens(void)
{
    static const char bare[] =
        "\033[?25l\033[2 q\033[H\033[0mx\033[H\033[?25h";
    static const char synced[] =
        "\033[?2026h\033[?25l\033[2 q\033[H\033[0mx\033[H\033[?25h"
        "\033[?2026l";
    RenderFixture f;
    YewColor color = render_default_color();

    render_fixture_init(&f, 1u, 1u, false);
    yew_grid_flip(&f.grid);
    yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u,
                 color, color, 0u);
    yew_grid_cursor(&f.grid, 0u, 0u, true);
    YEW_ASSERT_EQ_U64(yew_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(bare) - 1u);
    YEW_ASSERT_EQ_MEM(f.out.data, bare, sizeof(bare) - 1u);
    YEW_ASSERT(f.out.len <= 32u);
    render_fixture_free(&f);

    render_fixture_init(&f, 1u, 1u, true);
    yew_grid_flip(&f.grid);
    yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u,
                 color, color, 0u);
    yew_grid_cursor(&f.grid, 0u, 0u, true);
    YEW_ASSERT_EQ_U64(yew_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(synced) - 1u);
    YEW_ASSERT_EQ_MEM(f.out.data, synced, sizeof(synced) - 1u);
    render_fixture_free(&f);
}

void test_render_gap_motion_goldens(void)
{
    static const char *const middles[] = {
        "XbY", "XbcY", "XbcdY",
        "X\033[4CY", "X\033[5CY", "X\033[6CY"
    };
    YewColor color = render_default_color();
    u16 gap;

    for (gap = 1u; gap <= 6u; gap++) {
        RenderFixture f;

        render_fixture_init(&f, 1u, 8u, false);
        yew_grid_puts(&f.grid, 0u, 0u, (const u8 *)"abcdefgh", 8u,
                      color, color, 0u);
        yew_grid_flip(&f.grid);
        yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"X", 1u,
                     color, color, 0u);
        yew_grid_put(&f.grid, 0u, (u16)(gap + 1u), (const u8 *)"Y", 1u,
                     color, color, 0u);
        yew_render_frame(&f.render, &f.grid, &f.out);
        YEW_ASSERT(render_contains(&f.out, middles[gap - 1u]));
        render_fixture_free(&f);
    }
    {
        RenderFixture f;

        render_fixture_init(&f, 2u, 8u, false);
        yew_grid_flip(&f.grid);
        yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"X", 1u,
                     color, color, 0u);
        yew_grid_put(&f.grid, 1u, 3u, (const u8 *)"Y", 1u,
                     color, color, 0u);
        yew_render_frame(&f.render, &f.grid, &f.out);
        YEW_ASSERT(render_contains(&f.out, "X\033[2;4HY"));
        render_fixture_free(&f);
    }
    {
        static const u8 combining[] = {
            (u8)'e', 0xccu, 0x81u, 0xccu, 0x82u
        };
        RenderFixture f;

        render_fixture_init(&f, 1u, 3u, false);
        yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u,
                     color, color, 0u);
        yew_grid_put(&f.grid, 0u, 1u, combining, sizeof(combining),
                     color, color, 0u);
        yew_grid_put(&f.grid, 0u, 2u, (const u8 *)"c", 1u,
                     color, color, 0u);
        yew_grid_flip(&f.grid);
        yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"X", 1u,
                     color, color, 0u);
        yew_grid_put(&f.grid, 0u, 2u, (const u8 *)"Y", 1u,
                     color, color, 0u);
        yew_render_frame(&f.render, &f.grid, &f.out);
        YEW_ASSERT(render_contains(&f.out, "X\033[1CY"));
        render_fixture_free(&f);
    }
}

void test_render_erase_to_eol_heuristic(void)
{
    RenderFixture f;
    YewColor color = render_default_color();
    Cell styled_blank;

    render_fixture_init(&f, 1u, 8u, false);
    yew_grid_puts(&f.grid, 0u, 0u, (const u8 *)"abcdefgh", 8u,
                  color, color, 0u);
    yew_grid_flip(&f.grid);
    yew_grid_clear(&f.grid);
    yew_render_frame(&f.render, &f.grid, &f.out);
    YEW_ASSERT(render_contains(&f.out, "\033[0m\033[K"));
    render_fixture_free(&f);

    render_fixture_init(&f, 1u, 8u, false);
    yew_grid_puts(&f.grid, 0u, 0u, (const u8 *)"abcdefgh", 8u,
                  color, color, 0u);
    yew_grid_flip(&f.grid);
    styled_blank = f.grid.blank;
    styled_blank.bg = (YewColor){YEW_COLOR_INDEXED, 1u, 0u, 0u};
    yew_grid_fill(&f.grid, 0u, 0u, 8u, styled_blank);
    yew_render_frame(&f.render, &f.grid, &f.out);
    YEW_ASSERT(!render_contains(&f.out, "\033[K"));
    YEW_ASSERT(render_contains(&f.out, "\033[0;41m        "));
    render_fixture_free(&f);
}

void test_render_bold_dim_shared_reset_transitions(void)
{
    static const u16 states[] = {
        0u, YEW_ATTR_BOLD, YEW_ATTR_DIM, YEW_ATTR_BOLD | YEW_ATTR_DIM
    };
    static const char *const delta[4][4] = {
        {NULL, "\033[1m", "\033[2m", "\033[1;2m"},
        {"\033[22m", NULL, "\033[22;2m", "\033[2m"},
        {"\033[22m", "\033[22;1m", NULL, "\033[1m"},
        {"\033[22m", "\033[22;1m", "\033[22;2m", NULL}
    };
    YewColor color = render_default_color();
    size_t from;
    size_t to;

    for (from = 0u; from < 4u; from++) {
        for (to = 0u; to < 4u; to++) {
            RenderFixture f;

            render_fixture_init(&f, 1u, 2u, false);
            yew_grid_flip(&f.grid);
            yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u,
                         color, color, states[from]);
            yew_grid_put(&f.grid, 0u, 1u, (const u8 *)"b", 1u,
                         color, color, states[to]);
            yew_render_frame(&f.render, &f.grid, &f.out);
            if (delta[from][to] == NULL) {
                YEW_ASSERT(render_contains(&f.out, "ab"));
            } else {
                char expected[32];
                size_t n = strlen(delta[from][to]);

                YEW_ASSERT(n + 2u < sizeof(expected));
                expected[0] = 'a';
                memcpy(expected + 1u, delta[from][to], n);
                expected[n + 1u] = 'b';
                expected[n + 2u] = '\0';
                YEW_ASSERT(render_contains(&f.out, expected));
            }
            render_fixture_free(&f);
        }
    }
}

void test_render_three_resets_use_full_reset(void)
{
    RenderFixture f;
    YewColor color = render_default_color();

    render_fixture_init(&f, 1u, 2u, false);
    yew_grid_flip(&f.grid);
    yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u, color, color,
                 YEW_ATTR_ITALIC | YEW_ATTR_BLINK | YEW_ATTR_REVERSE);
    yew_grid_put(&f.grid, 0u, 1u, (const u8 *)"b", 1u, color, color, 0u);
    yew_render_frame(&f.render, &f.grid, &f.out);
    YEW_ASSERT(render_contains(&f.out, "a\033[0mb"));
    render_fixture_free(&f);
}

void test_render_same_state_is_byte_identical(void)
{
    RenderFixture a;
    RenderFixture b;
    YewColor color = {YEW_COLOR_RGB, 12u, 34u, 56u};

    render_fixture_init(&a, 2u, 5u, true);
    render_fixture_init(&b, 2u, 5u, true);
    yew_grid_puts(&a.grid, 1u, 1u, (const u8 *)"xyz", 3u,
                  color, render_default_color(), YEW_ATTR_BOLD);
    yew_grid_puts(&b.grid, 1u, 1u, (const u8 *)"xyz", 3u,
                  color, render_default_color(), YEW_ATTR_BOLD);
    yew_render_frame(&a.render, &a.grid, &a.out);
    yew_render_frame(&b.render, &b.grid, &b.out);
    YEW_ASSERT_EQ_U64(a.out.len, b.out.len);
    YEW_ASSERT_EQ_MEM(a.out.data, b.out.data, a.out.len);
    render_fixture_free(&a);
    render_fixture_free(&b);
}

void test_render_put_order_is_byte_identical(void)
{
    RenderFixture a;
    RenderFixture b;
    YewColor color = render_default_color();

    render_fixture_init(&a, 1u, 5u, false);
    render_fixture_init(&b, 1u, 5u, false);
    yew_grid_put(&a.grid, 0u, 4u, (const u8 *)"z", 1u, color, color, 0u);
    yew_grid_put(&a.grid, 0u, 0u, (const u8 *)"a", 1u, color, color, 0u);
    yew_grid_put(&b.grid, 0u, 0u, (const u8 *)"a", 1u, color, color, 0u);
    yew_grid_put(&b.grid, 0u, 4u, (const u8 *)"z", 1u, color, color, 0u);
    yew_render_frame(&a.render, &a.grid, &a.out);
    yew_render_frame(&b.render, &b.grid, &b.out);
    YEW_ASSERT_EQ_U64(a.out.len, b.out.len);
    YEW_ASSERT_EQ_MEM(a.out.data, b.out.data, a.out.len);
    render_fixture_free(&a);
    render_fixture_free(&b);
}

void test_render_resize_repaint_is_deterministic(void)
{
    RenderFixture f;
    Cell *first;
    YewColor color = render_default_color();

    render_fixture_init(&f, 24u, 80u, false);
    yew_grid_puts(&f.grid, 0u, 0u, (const u8 *)"yew",
                  sizeof("yew") - 1u,
                  color, color, 0u);
    yew_render_frame(&f.render, &f.grid, &f.out);
    first = malloc((size_t)f.grid.rows * f.grid.cols * sizeof(*first));
    YEW_ASSERT_NOT_NULL(first);
    memcpy(first, f.grid.back,
           (size_t)f.grid.rows * f.grid.cols * sizeof(*first));
    YEW_ASSERT(yew_grid_resize(&f.grid, 12u, 40u));
    yew_grid_puts(&f.grid, 0u, 0u, (const u8 *)"yew",
                  sizeof("yew") - 1u,
                  color, color, 0u);
    f.out.len = 0u;
    YEW_ASSERT(yew_render_frame(&f.render, &f.grid, &f.out) != 0u);
    YEW_ASSERT(yew_grid_resize(&f.grid, 24u, 80u));
    yew_grid_puts(&f.grid, 0u, 0u, (const u8 *)"yew",
                  sizeof("yew") - 1u,
                  color, color, 0u);
    f.out.len = 0u;
    yew_render_frame(&f.render, &f.grid, &f.out);
    YEW_ASSERT_EQ_MEM(f.grid.back, first,
                      (size_t)f.grid.rows * f.grid.cols * sizeof(*first));
    free(first);
    render_fixture_free(&f);
}

void test_render_cursor_only_frames_track_position_and_visibility(void)
{
    static const char move_show[] = "\033[1;3H\033[?25h";
    static const char hide[] = "\033[1;3H\033[?25l";
    static const char bar[] = "\033[6 q\033[1;3H";
    RenderFixture f;
    YewColor color = render_default_color();

    render_fixture_init(&f, 1u, 4u, false);
    yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u, color, color, 0u);
    yew_render_frame(&f.render, &f.grid, &f.out);
    yew_grid_flip(&f.grid);
    f.out.len = 0u;
    yew_grid_cursor(&f.grid, 0u, 2u, true);
    YEW_ASSERT_EQ_U64(yew_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(move_show) - 1u);
    YEW_ASSERT_EQ_MEM(f.out.data, move_show, sizeof(move_show) - 1u);
    f.out.len = 0u;
    YEW_ASSERT_EQ_U64(yew_render_frame(&f.render, &f.grid, &f.out), 0u);
    YEW_ASSERT_EQ_U64(f.out.len, 0u);
    yew_grid_cursor(&f.grid, 0u, 2u, false);
    YEW_ASSERT_EQ_U64(yew_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(hide) - 1u);
    YEW_ASSERT_EQ_MEM(f.out.data, hide, sizeof(hide) - 1u);
    f.out.len = 0u;
    yew_grid_cursor_shape(&f.grid, YEW_CURSOR_BAR);
    YEW_ASSERT_EQ_U64(yew_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(bar) - 1u);
    YEW_ASSERT_EQ_MEM(f.out.data, bar, sizeof(bar) - 1u);
    render_fixture_free(&f);
}

void test_render_tier16_downconverts_high_index(void)
{
    RenderFixture f;
    YewColor fg = {YEW_COLOR_INDEXED, 196u, 0u, 0u};
    YewColor bg = render_default_color();

    render_fixture_init(&f, 1u, 1u, false);
    f.render.tier = YEW_RENDER_TIER_16;
    yew_grid_flip(&f.grid);
    yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u, fg, bg, 0u);
    yew_render_frame(&f.render, &f.grid, &f.out);
    YEW_ASSERT(render_contains(&f.out, "\033[0;91mx"));
    YEW_ASSERT(!render_contains(&f.out, "38;5"));
    render_fixture_free(&f);
}

void test_render_invalid_byte_style_is_reverse(void)
{
    RenderFixture f;
    YewColor color = render_default_color();

    render_fixture_init(&f, 1u, 1u, false);
    yew_grid_flip(&f.grid);
    yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u, color, color,
                 YEW_ATTR_INVALID_BYTE);
    yew_render_frame(&f.render, &f.grid, &f.out);
    YEW_ASSERT(render_contains(&f.out, "\033[0;7mx"));
    render_fixture_free(&f);
}

void test_render_underline_undercurl_shared_reset(void)
{
    static const u16 states[] = {
        0u, YEW_ATTR_UNDERLINE, YEW_ATTR_UNDERCURL,
        YEW_ATTR_UNDERLINE | YEW_ATTR_UNDERCURL
    };
    static const char *const delta[4][4] = {
        {NULL, "\033[4m", "\033[4:3m", "\033[4:3m"},
        {"\033[24m", NULL, "\033[24;4:3m", "\033[24;4:3m"},
        {"\033[24m", "\033[24;4m", NULL, NULL},
        {"\033[24m", "\033[24;4m", NULL, NULL}
    };
    YewColor color = render_default_color();
    size_t from;
    size_t to;

    for (from = 0u; from < YEW_ARRAY_LEN(states); from++) {
        for (to = 0u; to < YEW_ARRAY_LEN(states); to++) {
            RenderFixture f;

            render_fixture_init(&f, 1u, 2u, false);
            yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u,
                         color, color, states[from]);
            yew_grid_put(&f.grid, 0u, 1u, (const u8 *)"b", 1u,
                         color, color, states[to]);
            yew_render_frame(&f.render, &f.grid, &f.out);
            if (delta[from][to] == NULL) {
                YEW_ASSERT(render_contains(&f.out, "ab"));
            } else {
                char expected[32];
                int n = snprintf(expected, sizeof(expected), "a%sb",
                                 delta[from][to]);

                YEW_ASSERT(n > 0 && (size_t)n < sizeof(expected));
                YEW_ASSERT(render_contains(&f.out, expected));
            }
            render_fixture_free(&f);
        }
    }

    {
        RenderFixture f;
        YewColor error = {YEW_COLOR_RGB, 255u, 95u, 95u};

        render_fixture_init(&f, 1u, 2u, false);
        yew_render_set_underline_colors(
            &f.render, error,
            (YewColor){YEW_COLOR_RGB, 229u, 192u, 123u},
            (YewColor){YEW_COLOR_RGB, 97u, 175u, 239u});
        yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u,
                     color, color,
                     (u16)(YEW_ATTR_UNDERLINE | YEW_CELL_UL_ERROR));
        yew_grid_put(&f.grid, 0u, 1u, (const u8 *)"b", 1u,
                     color, color, YEW_ATTR_UNDERLINE);
        yew_render_frame(&f.render, &f.grid, &f.out);
        YEW_ASSERT(render_contains(&f.out, "58;2;255;95;95"));
        YEW_ASSERT(render_contains(&f.out, "a\033[59mb"));
        YEW_ASSERT_EQ_U64(sizeof(Cell), 20u);
        YEW_ASSERT((YEW_CELL_UL_MASK & YEW_ATTR_INVALID_BYTE) == 0u);
        render_fixture_free(&f);
    }

    {
        RenderFixture f;

        render_fixture_init(&f, 1u, 1u, false);
        f.render.tier = YEW_RENDER_TIER_256;
        f.render.undercurl = false;
        yew_render_set_underline_colors(
            &f.render, (YewColor){YEW_COLOR_RGB, 255u, 95u, 95u},
            (YewColor){YEW_COLOR_RGB, 229u, 192u, 123u},
            (YewColor){YEW_COLOR_RGB, 97u, 175u, 239u});
        yew_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u,
                     color, color,
                     (u16)(YEW_ATTR_UNDERCURL | YEW_CELL_UL_WARN));
        yew_render_frame(&f.render, &f.grid, &f.out);
        YEW_ASSERT(!render_contains(&f.out, "58;"));
        YEW_ASSERT(!render_contains(&f.out, "59m"));
        YEW_ASSERT(render_contains(&f.out, "\033[0;4m"));
        render_fixture_free(&f);
    }
}

static int invalid_cell_child_exit(int scenario)
{
    pid_t child;
    pid_t waited;
    int status;

    YEW_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        RenderFixture f;
        Cell bad;

        (void)close(STDERR_FILENO);
        (void)setenv("YEW_LOG", "/dev/null", 1);
        render_fixture_init(&f, 1u, 1u, false);
        bad = f.grid.blank;
        if (scenario < 2) {
            bad.flags = CELL_INTERNED;
            bad.id = UINT32_MAX;
        } else if (scenario == 2) {
            bad.utf8[0] = 0x1bu;
        } else if (scenario < 5) {
            bad.utf8[0] = (u8)'a';
            bad.utf8[1] = (u8)'b';
        } else {
            bad.utf8[0] = 0xffu;
        }
        if (scenario == 0 || scenario == 3)
            yew_grid_fill(&f.grid, 0u, 0u, 1u, bad);
        else {
            f.grid.back[0] = bad;
            yew_grid_mark_all(&f.grid);
            (void)yew_render_frame(&f.render, &f.grid, &f.out);
        }
        _exit(0);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    YEW_ASSERT_EQ_I64(waited, child);
    YEW_ASSERT(WIFEXITED(status));
    return WEXITSTATUS(status);
}

void test_render_invalid_cells_are_bugs(void)
{
    YEW_ASSERT_EQ_I64(invalid_cell_child_exit(0), YEW_EXIT_BUG);
    YEW_ASSERT_EQ_I64(invalid_cell_child_exit(1), YEW_EXIT_BUG);
    YEW_ASSERT_EQ_I64(invalid_cell_child_exit(2), YEW_EXIT_BUG);
    YEW_ASSERT_EQ_I64(invalid_cell_child_exit(3), YEW_EXIT_BUG);
    YEW_ASSERT_EQ_I64(invalid_cell_child_exit(4), YEW_EXIT_BUG);
    YEW_ASSERT_EQ_I64(invalid_cell_child_exit(5), YEW_EXIT_BUG);
}
