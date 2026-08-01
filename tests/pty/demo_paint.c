#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "term/grid.h"
#include "term/input.h"
#include "term/render.h"
#include "term/tty.h"
#include "text/clipboard.h"
#include "text/register.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"

typedef struct Demo {
    Tty tty;
    In input;
    Arena arena;
    Interner interner;
    Grid grid;
    Render render;
    Bytebuf frame;
    const char *scene;
    bool input_ready;
    bool grid_ready;
    bool damage_flip;
    bool clipboard_after_render;
    bool echo_ready;
    Key echo_key;
} Demo;

static i64 now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * 1000 + (i64)ts.tv_nsec / 1000000;
}

static bool write_all(int fd, const u8 *data, size_t len)
{
    while (len != 0U) {
        ssize_t n = write(fd, data, len);

        if (n > 0) {
            data += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static SagColor rgb(u8 r, u8 g, u8 b)
{
    SagColor color = {SAG_COLOR_RGB, r, g, b};

    return color;
}

static SagColor indexed(u8 index)
{
    SagColor color = {SAG_COLOR_INDEXED, index, 0U, 0U};

    return color;
}

static const char *demo_getenv(const char *name)
{
    return getenv(name);
}

static void put_text(Grid *g, u16 row, u16 col, const char *text,
                     SagColor fg, SagColor bg, u16 attrs)
{
    if (row < g->rows && col < g->cols)
        (void)sag_grid_puts(g, row, col, (const u8 *)text, strlen(text),
                            fg, bg, attrs);
}

static void put_bytes(Grid *g, u16 row, u16 col, const u8 *text, size_t n,
                      SagColor fg, SagColor bg, u16 attrs)
{
    if (row < g->rows && col < g->cols)
        (void)sag_grid_puts(g, row, col, text, n, fg, bg, attrs);
}

static void paint_box(Grid *g)
{
    static const u8 tl[] = {0xe2U, 0x94U, 0x8cU};
    static const u8 tr[] = {0xe2U, 0x94U, 0x90U};
    static const u8 bl[] = {0xe2U, 0x94U, 0x94U};
    static const u8 br[] = {0xe2U, 0x94U, 0x98U};
    static const u8 hz[] = {0xe2U, 0x94U, 0x80U};
    static const u8 vt[] = {0xe2U, 0x94U, 0x82U};
    SagColor edge = rgb(0x7aU, 0xa2U, 0xf7U);
    SagColor bg = rgb(0x1aU, 0x1bU, 0x26U);
    u16 row;
    u16 col;

    if (g->rows < 2U || g->cols < 2U)
        return;
    (void)sag_grid_put(g, 0U, 0U, tl, sizeof(tl), edge, bg, 0U);
    (void)sag_grid_put(g, 0U, (u16)(g->cols - 1U), tr, sizeof(tr), edge,
                       bg, 0U);
    (void)sag_grid_put(g, (u16)(g->rows - 1U), 0U, bl, sizeof(bl), edge,
                       bg, 0U);
    (void)sag_grid_put(g, (u16)(g->rows - 1U), (u16)(g->cols - 1U), br,
                       sizeof(br), edge, bg, 0U);
    for (col = 1U; col + 1U < g->cols; col++) {
        (void)sag_grid_put(g, 0U, col, hz, sizeof(hz), edge, bg, 0U);
        (void)sag_grid_put(g, (u16)(g->rows - 1U), col, hz, sizeof(hz),
                           edge, bg, 0U);
    }
    for (row = 1U; row + 1U < g->rows; row++) {
        (void)sag_grid_put(g, row, 0U, vt, sizeof(vt), edge, bg, 0U);
        (void)sag_grid_put(g, row, (u16)(g->cols - 1U), vt, sizeof(vt),
                           edge, bg, 0U);
    }
}

static void paint_basic(Grid *g)
{
    static const char title[] = "sagitta";
    Cell strip = g->blank;
    u16 title_col;
    u16 title_row;

    sag_grid_clear(g);
    paint_box(g);
    title_col = g->cols > sizeof(title) - 1U
                    ? (u16)((g->cols - (sizeof(title) - 1U)) / 2U) : 0U;
    title_row = g->rows / 2U;
    put_text(g, title_row, title_col, title,
             rgb(0xc0U, 0xcaU, 0xf5U), rgb(0x1aU, 0x1bU, 0x26U),
             SAG_ATTR_BOLD);
    if (g->rows > 2U) {
        strip.fg = indexed(0U);
        strip.bg = rgb(0x9eU, 0xceU, 0x6aU);
        sag_grid_fill(g, (u16)(g->rows - 2U), 1U,
                      g->cols > 1U ? (u16)(g->cols - 1U) : g->cols, strip);
        put_text(g, (u16)(g->rows - 2U), 3U, "first paint",
                 indexed(0U), strip.bg, SAG_ATTR_BOLD);
    }
    sag_grid_cursor(g, title_row, title_col, true);
}

static void paint_wide(Grid *g)
{
    static const u8 cjk[] = {
        'C', 'J', 'K', ':', ' ', 0xe6U, 0xbcU, 0xa2U,
        0xe5U, 0xadU, 0x97U
    };
    static const u8 emoji[] = {
        'e', 'm', 'o', 'j', 'i', ':', ' ',
        0xf0U, 0x9fU, 0x98U, 0x80U, ' ',
        0xe2U, 0x98U, 0x80U, 0xefU, 0xb8U, 0x8fU
    };
    static const u8 family[] = {
        'f', 'a', 'm', 'i', 'l', 'y', ':', ' ',
        0xf0U, 0x9fU, 0x91U, 0xa8U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa9U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa7U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa6U
    };
    static const u8 combining[] = {
        's', 't', 'a', 'c', 'k', ':', ' ', 'e',
        0xccU, 0x81U, 0xccU, 0xa7U, 0xccU, 0x82U,
        0xccU, 0x88U, 0xccU, 0x84U
    };
    static const u8 wide[] = {0xe6U, 0xbcU, 0xa2U};
    SagColor fg = rgb(0xbbU, 0x9aU, 0xf7U);
    SagColor bg = {0};

    sag_grid_clear(g);
    put_bytes(g, 1U, 2U, cjk, sizeof(cjk), fg, bg, 0U);
    put_bytes(g, 3U, 2U, emoji, sizeof(emoji),
              rgb(0xe0U, 0xafU, 0x68U), bg, 0U);
    put_bytes(g, 5U, 2U, family, sizeof(family),
              rgb(0x7dU, 0xcfU, 0xffU), bg, 0U);
    put_bytes(g, 7U, 2U, combining, sizeof(combining),
              rgb(0x9eU, 0xceU, 0x6aU), bg, SAG_ATTR_UNDERCURL);
    if (g->rows > 9U && g->cols != 0U)
        (void)sag_grid_put(g, 9U, (u16)(g->cols - 1U), wide,
                           sizeof(wide), fg, bg, 0U);
    sag_grid_cursor(g, 5U < g->rows ? 5U : 0U, 2U < g->cols ? 2U : 0U,
                    true);
}

static void paint_colors(Grid *g)
{
    Cell cell = g->blank;
    u16 col;
    u16 limit;

    sag_grid_clear(g);
    limit = g->cols < 64U ? g->cols : 64U;
    for (col = 0U; col < limit; col++) {
        cell.bg = rgb((u8)(col * 4U), (u8)(255U - col * 4U),
                      (u8)(col * 3U));
        sag_grid_fill(g, 1U, col, (u16)(col + 1U), cell);
    }
    for (col = 0U; col < 16U && col < g->cols; col++) {
        cell.bg = indexed((u8)col);
        sag_grid_fill(g, 3U, col, (u16)(col + 1U), cell);
    }
    cell.bg = (SagColor){0};
    if (g->rows > 5U)
        sag_grid_fill(g, 5U, 0U, g->cols, cell);
    put_text(g, 5U, 1U, "default", (SagColor){0}, (SagColor){0}, 0U);
    sag_grid_cursor(g, 5U < g->rows ? 5U : 0U, 1U, false);
}

static void paint_echo(Grid *g, const Key *key)
{
    static const u8 hex[] = "0123456789abcdef";
    char line[192];
    char text[16U * 2U + 1U];
    size_t i;

    for (i = 0U; i < key->ntext; i++) {
        text[i * 2U] = (char)hex[key->text[i] >> 4U];
        text[i * 2U + 1U] = (char)hex[key->text[i] & 0x0fU];
    }
    text[key->ntext * 2U] = '\0';
    (void)snprintf(line, sizeof(line),
                   "kind=%u code=%u mods=%u ev=%u text=%s",
                   (unsigned)key->kind, (unsigned)key->code,
                   (unsigned)key->mods, (unsigned)key->ev, text);
    sag_grid_clear(g);
    put_text(g, 2U, 2U, line, rgb(0xc0U, 0xcaU, 0xf5U), (SagColor){0},
             SAG_ATTR_BOLD);
    sag_grid_cursor(g, 2U, 2U, true);
}

static void paint_echo_waiting(Grid *g)
{
    sag_grid_clear(g);
    put_text(g, 2U, 2U, "waiting for key", rgb(0xc0U, 0xcaU, 0xf5U),
             (SagColor){0}, SAG_ATTR_BOLD);
    sag_grid_cursor(g, 2U, 2U, true);
}

static void paint_damage(Demo *d)
{
    const u8 glyph = d->damage_flip ? (u8)'X' : (u8)'x';

    paint_basic(&d->grid);
    (void)sag_grid_put(&d->grid, 1U, 1U, &glyph, 1U,
                       (SagColor){0}, (SagColor){0}, 0U);
}

static void paint_scene(Demo *d)
{
    if (strcmp(d->scene, "wide") == 0)
        paint_wide(&d->grid);
    else if (strcmp(d->scene, "colors") == 0)
        paint_colors(&d->grid);
    else if (strcmp(d->scene, "damage") == 0)
        paint_damage(d);
    else if (strcmp(d->scene, "echo") == 0) {
        if (d->echo_ready)
            paint_echo(&d->grid, &d->echo_key);
        else
            paint_echo_waiting(&d->grid);
    } else {
        paint_basic(&d->grid);
    }
}

static void invalidate_front(Grid *g)
{
    size_t i;
    size_t count = (size_t)g->rows * g->cols;

    for (i = 0U; i < count; i++)
        g->front[i].w = 0xffU;
    sag_grid_mark_all(g);
}

static bool emit_frame(Demo *d, size_t *emitted)
{
    size_t n;

    d->frame.len = 0U;
    n = sag_render_frame(&d->render, &d->grid, &d->frame);
    if (emitted != NULL)
        *emitted = n;
    if (n != d->frame.len)
        return false;
    if (d->clipboard_after_render)
        sag_clip_after_render(&d->frame, now_ms());
    if (!write_all(d->tty.wfd, d->frame.data, d->frame.len))
        return false;
    sag_grid_flip(&d->grid);
    return true;
}

static bool probe(Demo *d)
{
    u8 data[1024];

    sag_tty_probe_start(&d->tty, now_ms());
    while (!sag_tty_probe_done(&d->tty)) {
        struct pollfd fds[2];
        i64 now = now_ms();
        i64 left = sag_tty_probe_deadline(&d->tty, now);
        int timeout = left < 0 ? 0 : left > 1000 ? 1000 : (int)left;
        int result;

        fds[0].fd = d->tty.rfd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = sag_tty_signal_fd(&d->tty);
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        result = poll(fds, 2U, timeout);
        if (result > 0 && (fds[1].revents & POLLIN) != 0) {
            bool winch;
            bool cont;

            sag_tty_drain_signals(&d->tty, &winch, &cont, NULL);
            if (!d->tty.raw || (winch && !sag_tty_winsize(&d->tty)))
                return false;
        }
        if (result > 0 && (fds[0].revents & (POLLIN | POLLHUP)) != 0) {
            ssize_t n = read(fds[0].fd, data, sizeof(data));

            if (n > 0)
                (void)sag_tty_probe_feed(&d->tty, data, (size_t)n);
            else if (n < 0 && errno != EINTR && errno != EAGAIN)
                return false;
        } else if (result < 0 && errno != EINTR) {
            return false;
        }
        sag_tty_probe_tick(&d->tty, now_ms());
    }
    return true;
}

static void demo_free(Demo *d)
{
    sag_clip_shutdown();
    if (d->grid_ready)
        sag_grid_free(&d->grid);
    if (d->input_ready)
        sag_input_free(&d->input);
    bytebuf_free(&d->frame);
    sag_term_oob_clear();
    interner_free(&d->interner);
    arena_free_all(&d->arena);
    /* Keep close last: its restore blob must be the final terminal output. */
    sag_tty_close(&d->tty);
}

static bool handle_key(Demo *d, const Key *key, bool *running)
{
    size_t emitted;

    if (key->kind != SAG_EV_KEY)
        return true;
    if (key->code == (u32)'q' && key->mods == 0U) {
        *running = false;
        return true;
    }
    if (strcmp(d->scene, "echo") == 0) {
        d->echo_key = *key;
        d->echo_ready = true;
        paint_echo(&d->grid, &d->echo_key);
        return emit_frame(d, NULL);
    }
    if (strcmp(d->scene, "damage") == 0) {
        const u8 glyph = d->damage_flip ? (u8)'x' : (u8)'X';

        d->damage_flip = !d->damage_flip;
        (void)sag_grid_put(&d->grid, 1U, 1U, &glyph, 1U,
                           (SagColor){0}, (SagColor){0}, 0U);
        if (!emit_frame(d, &emitted))
            return false;
        if (emitted > 32U)
            return false;
    }
    return true;
}

static bool handle_signals(Demo *d)
{
    bool winch;
    bool cont;
    bool repaint = false;

    sag_tty_drain_signals(&d->tty, &winch, &cont, NULL);
    if (cont) {
        if (!d->tty.raw)
            return false;
        sag_tty_altscreen(&d->tty, true);
        if (!d->tty.alt)
            return false;
        sag_input_enable(d->tty.wfd, &d->tty.caps);
        invalidate_front(&d->grid);
        repaint = true;
    }
    if (winch) {
        if (!sag_tty_winsize(&d->tty))
            return false;
        if (d->grid.rows != (u16)d->tty.rows ||
            d->grid.cols != (u16)d->tty.cols) {
            if (!sag_grid_resize(&d->grid, (u16)d->tty.rows,
                                 (u16)d->tty.cols))
                return false;
            paint_scene(d);
            repaint = true;
        }
    }
    return !repaint || emit_frame(d, NULL);
}

static int event_loop(Demo *d)
{
    bool running = true;
    bool eof = false;

    while (running) {
        struct pollfd fds[2];
        i64 deadline = sag_input_deadline(&d->input, now_ms());
        int timeout = deadline < 0 ? -1 : deadline > 1000 ? 1000
                                                       : (int)deadline;
        int result;
        Key key;

        fds[0].fd = d->tty.rfd;
        fds[0].events = eof ? 0 : POLLIN;
        fds[0].revents = 0;
        fds[1].fd = sag_tty_signal_fd(&d->tty);
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        result = poll(fds, 2U, timeout);
        if (result < 0 && errno != EINTR)
            return 1;
        if ((fds[1].revents & POLLIN) != 0 && !handle_signals(d))
            return 1;
        if (!eof && (fds[0].revents & (POLLIN | POLLHUP)) != 0) {
            u8 bytes[4096];
            ssize_t n = read(fds[0].fd, bytes, sizeof(bytes));

            if (n > 0)
                sag_input_feed(&d->input, bytes, (size_t)n);
            else if (n == 0) {
                eof = true;
                sag_input_eof(&d->input);
            } else if (errno != EINTR && errno != EAGAIN) {
                return 1;
            }
        }
        while (sag_input_next(&d->input, now_ms(), &key)) {
            if (!handle_key(d, &key, &running))
                return 1;
        }
        if (eof && running)
            return 1;
    }
    return 0;
}

static bool parse_args(int argc, char **argv, const char **scene, bool *crash)
{
    int i;

    *scene = "basic";
    *crash = false;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            *scene = argv[++i];
        } else if (strcmp(argv[i], "--crash") == 0) {
            *crash = true;
        } else {
            return false;
        }
    }
    return strcmp(*scene, "basic") == 0 || strcmp(*scene, "wide") == 0 ||
           strcmp(*scene, "colors") == 0 ||
           strcmp(*scene, "damage") == 0 ||
           strcmp(*scene, "resize") == 0 || strcmp(*scene, "echo") == 0 ||
           strcmp(*scene, "osc52") == 0;
}

int main(int argc, char **argv)
{
    Demo demo;
    TtyCaps render_caps;
    const char *scene;
    bool crash;
    int result;

    if (!parse_args(argc, argv, &scene, &crash)) {
        (void)fprintf(stderr,
                      "usage: demo_paint [--scene basic|wide|colors|damage|resize|echo|osc52] [--crash]\n");
        return 2;
    }
    memset(&demo, 0, sizeof(demo));
    demo.scene = scene;
    arena_init(&demo.arena);
    interner_init(&demo.interner, &demo.arena);
    bytebuf_init(&demo.frame);
    if (!sag_tty_open(&demo.tty) || !sag_tty_raw(&demo.tty) ||
        !probe(&demo)) {
        demo_free(&demo);
        return 1;
    }
    sag_tty_altscreen(&demo.tty, true);
    if (!demo.tty.alt) {
        demo_free(&demo);
        return 1;
    }
    sag_input_enable(demo.tty.wfd, &demo.tty.caps);
    sag_input_init(&demo.input, &demo.tty.caps);
    demo.input_ready = true;
    sag_input_seed(&demo.input, &demo.tty.pending);
    if (!sag_grid_init(&demo.grid, &demo.interner, (u16)demo.tty.rows,
                       (u16)demo.tty.cols)) {
        demo_free(&demo);
        return 1;
    }
    demo.grid_ready = true;
    render_caps = demo.tty.caps;
    /* The damage scene owns the renderer's raw <=32-byte gate. Capability
     * framing is already exercised by every modern-profile paint scene. */
    if (strcmp(demo.scene, "damage") == 0)
        render_caps.sync_output = false;
    sag_render_init(&demo.render, &render_caps, demo_getenv);
    paint_scene(&demo);
    if (strcmp(demo.scene, "osc52") == 0) {
        Registers registers;
        RegVal value;

        if (setenv("SAG_CLIPBOARD", "osc52", 1) != 0 ||
            setenv("SAG_OSC52", "plain", 1) != 0 ||
            setenv("SAG_CLIPBOARD_TARGET", "c", 1) != 0 ||
            setenv("SAG_OSC52_MAX", "100000", 1) != 0) {
            demo_free(&demo);
            return 1;
        }
        sag_reg_init(&registers);
        sag_regval_init(&value);
        bytebuf_append(&value.bytes, "sagitta", 7U);
        sag_reg_yank(&registers, 0U, &value);
        demo.clipboard_after_render = true;
        sag_regval_free(&value);
        sag_reg_free(&registers);
    }
    if (!emit_frame(&demo, NULL)) {
        demo_free(&demo);
        return 1;
    }
    if (crash)
        (void)raise(SIGSEGV);
    result = event_loop(&demo);
    demo_free(&demo);
    return result;
}
