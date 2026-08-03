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
    return strcmp(name, "SAG_COLORS") == 0 ? "truecolor" : NULL;
}

static SagColor render_default_color(void)
{
    SagColor color = {SAG_COLOR_DEFAULT, 0u, 0u, 0u};

    return color;
}

static void render_fixture_init(RenderFixture *f, u16 rows, u16 cols,
                                bool sync)
{
    TtyCaps caps = {0};

    arena_init(&f->arena);
    interner_init(&f->interner, &f->arena);
    SAG_ASSERT(sag_grid_init(&f->grid, &f->interner, rows, cols));
    caps.truecolor = true;
    caps.sync_output = sync;
    sag_render_init(&f->render, &caps, render_truecolor_env);
    bytebuf_init(&f->out);
}

static void render_fixture_free(RenderFixture *f)
{
    bytebuf_free(&f->out);
    sag_grid_free(&f->grid);
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
    sag_grid_flip(&f.grid);
    SAG_ASSERT_EQ_U64(sag_render_frame(&f.render, &f.grid, &f.out), 0u);
    SAG_ASSERT_EQ_U64(f.out.len, 0u);
    render_fixture_free(&f);
}

void test_render_oob_flushes_after_frame(void)
{
    static const u8 oob_a[] = {0x1bu, (u8)']', (u8)'5', (u8)'2'};
    static const u8 oob_b[] = {(u8)';', (u8)'c'};
    static const char esu[] = "\033[?2026l";
    RenderFixture f;
    SagColor color = render_default_color();
    size_t first_len;

    sag_term_oob_clear();
    render_fixture_init(&f, 1u, 1u, true);
    sag_grid_flip(&f.grid);
    sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u,
                 color, color, 0u);
    sag_term_oob_queue(oob_a, sizeof(oob_a));
    sag_term_oob_queue(oob_b, sizeof(oob_b));
    SAG_ASSERT_EQ_U64(sag_term_oob_pending(),
                      sizeof(oob_a) + sizeof(oob_b));
    (void)sag_render_frame(&f.render, &f.grid, &f.out);
    SAG_ASSERT_EQ_U64(sag_term_oob_pending(), 0u);
    SAG_ASSERT(f.out.len >= sizeof(esu) - 1u + sizeof(oob_a) +
                            sizeof(oob_b));
    SAG_ASSERT_EQ_MEM(f.out.data + f.out.len - sizeof(oob_a) - sizeof(oob_b),
                      oob_a, sizeof(oob_a));
    SAG_ASSERT_EQ_MEM(f.out.data + f.out.len - sizeof(oob_b),
                      oob_b, sizeof(oob_b));
    SAG_ASSERT_EQ_MEM(f.out.data + f.out.len - sizeof(oob_a) - sizeof(oob_b)
                                    - (sizeof(esu) - 1u),
                      esu, sizeof(esu) - 1u);

    first_len = f.out.len;
    sag_grid_flip(&f.grid);
    sag_term_oob_queue((const u8 *)"z", 1u);
    SAG_ASSERT_EQ_U64(sag_render_frame(&f.render, &f.grid, &f.out), 1u);
    SAG_ASSERT_EQ_U64(f.out.len, first_len + 1u);
    SAG_ASSERT_EQ_U64(f.out.data[f.out.len - 1u], (u8)'z');
    SAG_ASSERT_EQ_U64(sag_term_oob_pending(), 0u);
    sag_term_oob_clear();
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
    SagColor color = render_default_color();

    render_fixture_init(&f, 1u, 1u, false);
    sag_grid_flip(&f.grid);
    sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u,
                 color, color, 0u);
    sag_grid_cursor(&f.grid, 0u, 0u, true);
    SAG_ASSERT_EQ_U64(sag_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(bare) - 1u);
    SAG_ASSERT_EQ_MEM(f.out.data, bare, sizeof(bare) - 1u);
    SAG_ASSERT(f.out.len <= 32u);
    render_fixture_free(&f);

    render_fixture_init(&f, 1u, 1u, true);
    sag_grid_flip(&f.grid);
    sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u,
                 color, color, 0u);
    sag_grid_cursor(&f.grid, 0u, 0u, true);
    SAG_ASSERT_EQ_U64(sag_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(synced) - 1u);
    SAG_ASSERT_EQ_MEM(f.out.data, synced, sizeof(synced) - 1u);
    render_fixture_free(&f);
}

void test_render_gap_motion_goldens(void)
{
    static const char *const middles[] = {
        "XbY", "XbcY", "XbcdY",
        "X\033[4CY", "X\033[5CY", "X\033[6CY"
    };
    SagColor color = render_default_color();
    u16 gap;

    for (gap = 1u; gap <= 6u; gap++) {
        RenderFixture f;

        render_fixture_init(&f, 1u, 8u, false);
        sag_grid_puts(&f.grid, 0u, 0u, (const u8 *)"abcdefgh", 8u,
                      color, color, 0u);
        sag_grid_flip(&f.grid);
        sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"X", 1u,
                     color, color, 0u);
        sag_grid_put(&f.grid, 0u, (u16)(gap + 1u), (const u8 *)"Y", 1u,
                     color, color, 0u);
        sag_render_frame(&f.render, &f.grid, &f.out);
        SAG_ASSERT(render_contains(&f.out, middles[gap - 1u]));
        render_fixture_free(&f);
    }
    {
        RenderFixture f;

        render_fixture_init(&f, 2u, 8u, false);
        sag_grid_flip(&f.grid);
        sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"X", 1u,
                     color, color, 0u);
        sag_grid_put(&f.grid, 1u, 3u, (const u8 *)"Y", 1u,
                     color, color, 0u);
        sag_render_frame(&f.render, &f.grid, &f.out);
        SAG_ASSERT(render_contains(&f.out, "X\033[2;4HY"));
        render_fixture_free(&f);
    }
    {
        static const u8 combining[] = {
            (u8)'e', 0xccu, 0x81u, 0xccu, 0x82u
        };
        RenderFixture f;

        render_fixture_init(&f, 1u, 3u, false);
        sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u,
                     color, color, 0u);
        sag_grid_put(&f.grid, 0u, 1u, combining, sizeof(combining),
                     color, color, 0u);
        sag_grid_put(&f.grid, 0u, 2u, (const u8 *)"c", 1u,
                     color, color, 0u);
        sag_grid_flip(&f.grid);
        sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"X", 1u,
                     color, color, 0u);
        sag_grid_put(&f.grid, 0u, 2u, (const u8 *)"Y", 1u,
                     color, color, 0u);
        sag_render_frame(&f.render, &f.grid, &f.out);
        SAG_ASSERT(render_contains(&f.out, "X\033[1CY"));
        render_fixture_free(&f);
    }
}

void test_render_erase_to_eol_heuristic(void)
{
    RenderFixture f;
    SagColor color = render_default_color();
    Cell styled_blank;

    render_fixture_init(&f, 1u, 8u, false);
    sag_grid_puts(&f.grid, 0u, 0u, (const u8 *)"abcdefgh", 8u,
                  color, color, 0u);
    sag_grid_flip(&f.grid);
    sag_grid_clear(&f.grid);
    sag_render_frame(&f.render, &f.grid, &f.out);
    SAG_ASSERT(render_contains(&f.out, "\033[0m\033[K"));
    render_fixture_free(&f);

    render_fixture_init(&f, 1u, 8u, false);
    sag_grid_puts(&f.grid, 0u, 0u, (const u8 *)"abcdefgh", 8u,
                  color, color, 0u);
    sag_grid_flip(&f.grid);
    styled_blank = f.grid.blank;
    styled_blank.bg = (SagColor){SAG_COLOR_INDEXED, 1u, 0u, 0u};
    sag_grid_fill(&f.grid, 0u, 0u, 8u, styled_blank);
    sag_render_frame(&f.render, &f.grid, &f.out);
    SAG_ASSERT(!render_contains(&f.out, "\033[K"));
    SAG_ASSERT(render_contains(&f.out, "\033[0;41m        "));
    render_fixture_free(&f);
}

void test_render_bold_dim_shared_reset_transitions(void)
{
    static const u16 states[] = {
        0u, SAG_ATTR_BOLD, SAG_ATTR_DIM, SAG_ATTR_BOLD | SAG_ATTR_DIM
    };
    static const char *const delta[4][4] = {
        {NULL, "\033[1m", "\033[2m", "\033[1;2m"},
        {"\033[22m", NULL, "\033[22;2m", "\033[2m"},
        {"\033[22m", "\033[22;1m", NULL, "\033[1m"},
        {"\033[22m", "\033[22;1m", "\033[22;2m", NULL}
    };
    SagColor color = render_default_color();
    size_t from;
    size_t to;

    for (from = 0u; from < 4u; from++) {
        for (to = 0u; to < 4u; to++) {
            RenderFixture f;

            render_fixture_init(&f, 1u, 2u, false);
            sag_grid_flip(&f.grid);
            sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u,
                         color, color, states[from]);
            sag_grid_put(&f.grid, 0u, 1u, (const u8 *)"b", 1u,
                         color, color, states[to]);
            sag_render_frame(&f.render, &f.grid, &f.out);
            if (delta[from][to] == NULL) {
                SAG_ASSERT(render_contains(&f.out, "ab"));
            } else {
                char expected[32];
                size_t n = strlen(delta[from][to]);

                SAG_ASSERT(n + 2u < sizeof(expected));
                expected[0] = 'a';
                memcpy(expected + 1u, delta[from][to], n);
                expected[n + 1u] = 'b';
                expected[n + 2u] = '\0';
                SAG_ASSERT(render_contains(&f.out, expected));
            }
            render_fixture_free(&f);
        }
    }
}

void test_render_three_resets_use_full_reset(void)
{
    RenderFixture f;
    SagColor color = render_default_color();

    render_fixture_init(&f, 1u, 2u, false);
    sag_grid_flip(&f.grid);
    sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u, color, color,
                 SAG_ATTR_ITALIC | SAG_ATTR_BLINK | SAG_ATTR_REVERSE);
    sag_grid_put(&f.grid, 0u, 1u, (const u8 *)"b", 1u, color, color, 0u);
    sag_render_frame(&f.render, &f.grid, &f.out);
    SAG_ASSERT(render_contains(&f.out, "a\033[0mb"));
    render_fixture_free(&f);
}

void test_render_same_state_is_byte_identical(void)
{
    RenderFixture a;
    RenderFixture b;
    SagColor color = {SAG_COLOR_RGB, 12u, 34u, 56u};

    render_fixture_init(&a, 2u, 5u, true);
    render_fixture_init(&b, 2u, 5u, true);
    sag_grid_puts(&a.grid, 1u, 1u, (const u8 *)"xyz", 3u,
                  color, render_default_color(), SAG_ATTR_BOLD);
    sag_grid_puts(&b.grid, 1u, 1u, (const u8 *)"xyz", 3u,
                  color, render_default_color(), SAG_ATTR_BOLD);
    sag_render_frame(&a.render, &a.grid, &a.out);
    sag_render_frame(&b.render, &b.grid, &b.out);
    SAG_ASSERT_EQ_U64(a.out.len, b.out.len);
    SAG_ASSERT_EQ_MEM(a.out.data, b.out.data, a.out.len);
    render_fixture_free(&a);
    render_fixture_free(&b);
}

void test_render_put_order_is_byte_identical(void)
{
    RenderFixture a;
    RenderFixture b;
    SagColor color = render_default_color();

    render_fixture_init(&a, 1u, 5u, false);
    render_fixture_init(&b, 1u, 5u, false);
    sag_grid_put(&a.grid, 0u, 4u, (const u8 *)"z", 1u, color, color, 0u);
    sag_grid_put(&a.grid, 0u, 0u, (const u8 *)"a", 1u, color, color, 0u);
    sag_grid_put(&b.grid, 0u, 0u, (const u8 *)"a", 1u, color, color, 0u);
    sag_grid_put(&b.grid, 0u, 4u, (const u8 *)"z", 1u, color, color, 0u);
    sag_render_frame(&a.render, &a.grid, &a.out);
    sag_render_frame(&b.render, &b.grid, &b.out);
    SAG_ASSERT_EQ_U64(a.out.len, b.out.len);
    SAG_ASSERT_EQ_MEM(a.out.data, b.out.data, a.out.len);
    render_fixture_free(&a);
    render_fixture_free(&b);
}

void test_render_resize_repaint_is_deterministic(void)
{
    RenderFixture f;
    Cell *first;
    SagColor color = render_default_color();

    render_fixture_init(&f, 24u, 80u, false);
    sag_grid_puts(&f.grid, 0u, 0u, (const u8 *)"sagitta", 7u,
                  color, color, 0u);
    sag_render_frame(&f.render, &f.grid, &f.out);
    first = malloc((size_t)f.grid.rows * f.grid.cols * sizeof(*first));
    SAG_ASSERT_NOT_NULL(first);
    memcpy(first, f.grid.back,
           (size_t)f.grid.rows * f.grid.cols * sizeof(*first));
    SAG_ASSERT(sag_grid_resize(&f.grid, 12u, 40u));
    sag_grid_puts(&f.grid, 0u, 0u, (const u8 *)"sagitta", 7u,
                  color, color, 0u);
    f.out.len = 0u;
    SAG_ASSERT(sag_render_frame(&f.render, &f.grid, &f.out) != 0u);
    SAG_ASSERT(sag_grid_resize(&f.grid, 24u, 80u));
    sag_grid_puts(&f.grid, 0u, 0u, (const u8 *)"sagitta", 7u,
                  color, color, 0u);
    f.out.len = 0u;
    sag_render_frame(&f.render, &f.grid, &f.out);
    SAG_ASSERT_EQ_MEM(f.grid.back, first,
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
    SagColor color = render_default_color();

    render_fixture_init(&f, 1u, 4u, false);
    sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u, color, color, 0u);
    sag_render_frame(&f.render, &f.grid, &f.out);
    sag_grid_flip(&f.grid);
    f.out.len = 0u;
    sag_grid_cursor(&f.grid, 0u, 2u, true);
    SAG_ASSERT_EQ_U64(sag_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(move_show) - 1u);
    SAG_ASSERT_EQ_MEM(f.out.data, move_show, sizeof(move_show) - 1u);
    f.out.len = 0u;
    SAG_ASSERT_EQ_U64(sag_render_frame(&f.render, &f.grid, &f.out), 0u);
    SAG_ASSERT_EQ_U64(f.out.len, 0u);
    sag_grid_cursor(&f.grid, 0u, 2u, false);
    SAG_ASSERT_EQ_U64(sag_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(hide) - 1u);
    SAG_ASSERT_EQ_MEM(f.out.data, hide, sizeof(hide) - 1u);
    f.out.len = 0u;
    sag_grid_cursor_shape(&f.grid, SAG_CURSOR_BAR);
    SAG_ASSERT_EQ_U64(sag_render_frame(&f.render, &f.grid, &f.out),
                      sizeof(bar) - 1u);
    SAG_ASSERT_EQ_MEM(f.out.data, bar, sizeof(bar) - 1u);
    render_fixture_free(&f);
}

void test_render_tier16_downconverts_high_index(void)
{
    RenderFixture f;
    SagColor fg = {SAG_COLOR_INDEXED, 196u, 0u, 0u};
    SagColor bg = render_default_color();

    render_fixture_init(&f, 1u, 1u, false);
    f.render.tier = SAG_RENDER_TIER_16;
    sag_grid_flip(&f.grid);
    sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u, fg, bg, 0u);
    sag_render_frame(&f.render, &f.grid, &f.out);
    SAG_ASSERT(render_contains(&f.out, "\033[0;91mx"));
    SAG_ASSERT(!render_contains(&f.out, "38;5"));
    render_fixture_free(&f);
}

void test_render_invalid_byte_style_is_reverse(void)
{
    RenderFixture f;
    SagColor color = render_default_color();

    render_fixture_init(&f, 1u, 1u, false);
    sag_grid_flip(&f.grid);
    sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"x", 1u, color, color,
                 SAG_ATTR_INVALID_BYTE);
    sag_render_frame(&f.render, &f.grid, &f.out);
    SAG_ASSERT(render_contains(&f.out, "\033[0;7mx"));
    render_fixture_free(&f);
}

void test_render_underline_undercurl_shared_reset(void)
{
    static const u16 states[] = {
        0u, SAG_ATTR_UNDERLINE, SAG_ATTR_UNDERCURL,
        SAG_ATTR_UNDERLINE | SAG_ATTR_UNDERCURL
    };
    static const char *const delta[4][4] = {
        {NULL, "\033[4m", "\033[4:3m", "\033[4:3m"},
        {"\033[24m", NULL, "\033[24;4:3m", "\033[24;4:3m"},
        {"\033[24m", "\033[24;4m", NULL, NULL},
        {"\033[24m", "\033[24;4m", NULL, NULL}
    };
    SagColor color = render_default_color();
    size_t from;
    size_t to;

    for (from = 0u; from < SAG_ARRAY_LEN(states); from++) {
        for (to = 0u; to < SAG_ARRAY_LEN(states); to++) {
            RenderFixture f;

            render_fixture_init(&f, 1u, 2u, false);
            sag_grid_put(&f.grid, 0u, 0u, (const u8 *)"a", 1u,
                         color, color, states[from]);
            sag_grid_put(&f.grid, 0u, 1u, (const u8 *)"b", 1u,
                         color, color, states[to]);
            sag_render_frame(&f.render, &f.grid, &f.out);
            if (delta[from][to] == NULL) {
                SAG_ASSERT(render_contains(&f.out, "ab"));
            } else {
                char expected[32];
                int n = snprintf(expected, sizeof(expected), "a%sb",
                                 delta[from][to]);

                SAG_ASSERT(n > 0 && (size_t)n < sizeof(expected));
                SAG_ASSERT(render_contains(&f.out, expected));
            }
            render_fixture_free(&f);
        }
    }
}

static int invalid_cell_child_exit(int scenario)
{
    pid_t child;
    pid_t waited;
    int status;

    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        RenderFixture f;
        Cell bad;

        (void)close(STDERR_FILENO);
        (void)setenv("SAG_LOG", "/dev/null", 1);
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
            sag_grid_fill(&f.grid, 0u, 0u, 1u, bad);
        else {
            f.grid.back[0] = bad;
            sag_grid_mark_all(&f.grid);
            (void)sag_render_frame(&f.render, &f.grid, &f.out);
        }
        _exit(0);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    return WEXITSTATUS(status);
}

void test_render_invalid_cells_are_bugs(void)
{
    SAG_ASSERT_EQ_I64(invalid_cell_child_exit(0), SAG_EXIT_BUG);
    SAG_ASSERT_EQ_I64(invalid_cell_child_exit(1), SAG_EXIT_BUG);
    SAG_ASSERT_EQ_I64(invalid_cell_child_exit(2), SAG_EXIT_BUG);
    SAG_ASSERT_EQ_I64(invalid_cell_child_exit(3), SAG_EXIT_BUG);
    SAG_ASSERT_EQ_I64(invalid_cell_child_exit(4), SAG_EXIT_BUG);
    SAG_ASSERT_EQ_I64(invalid_cell_child_exit(5), SAG_EXIT_BUG);
}
