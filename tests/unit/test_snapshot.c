#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include "../pty/snapshot.h"

#include <stdlib.h>
#include <string.h>

static SagColor snap_default(void)
{
    SagColor color = {SAG_COLOR_DEFAULT, 0u, 0u, 0u};

    return color;
}

static bool buf_contains(const Bytebuf *buf, const char *needle)
{
    size_t len = strlen(needle);
    size_t i;

    for (i = 0u; i + len <= buf->len; i++) {
        if (memcmp(buf->data + i, needle, len) == 0)
            return true;
    }
    return false;
}

void test_snapshot_golden_v1_format(void)
{
    static const u8 cjk[] = {0xe6u, 0xbcu, 0xa2u};
    static const char expected[] =
        "# sagitta pty golden v1\n"
        "size 3x2 alt=1 cursor=1,2 vis=1\n"
        "modes 2004,1006 kitty=21 sync_pairs=3\n"
        "--- text\n"
        "A\346\274\242\n"
        "\n"
        "--- style\n"
        "ABB\n"
        "\n"
        "--- legend\n"
        "A fg=#c0caf5 bg=#1a1b26 attrs=b---------\n"
        "B fg=default bg=default attrs=----------\n";
    VtScreen screen;
    Bytebuf out;
    SagColor fg = {SAG_COLOR_RGB, 0xc0u, 0xcau, 0xf5u};
    SagColor bg = {SAG_COLOR_RGB, 0x1au, 0x1bu, 0x26u};
    SagColor def = snap_default();

    vt_init(&screen, 2, 3);
    screen.alt = true;
    screen.cur_r = 1;
    screen.cur_c = 2;
    screen.cur_vis = true;
    screen.modes = VT_MODE_BRACKETED_PASTE | VT_MODE_SGR_MOUSE;
    screen.kitty[0] = 21u;
    screen.ksp = 1;
    screen.nsync_pairs = 3u;
    SAG_ASSERT(vt_set_cell(&screen, 0, 0, (const u8 *)"A", 1u,
                           fg, bg, SAG_ATTR_BOLD, 1u));
    SAG_ASSERT(vt_set_cell(&screen, 0, 1, cjk, sizeof(cjk),
                           def, def, 0u, 2u));
    SAG_ASSERT(vt_set_cell(&screen, 0, 2, NULL, 0u,
                           def, def, 0u, 0u));
    bytebuf_init(&out);
    snapshot_write(&screen, &out);
    SAG_ASSERT_EQ_U64(out.len, sizeof(expected) - 1u);
    SAG_ASSERT_EQ_MEM(out.data, expected, sizeof(expected) - 1u);
    bytebuf_free(&out);
    vt_free(&screen);
}

void test_snapshot_legend_supports_sixty_styles(void)
{
    static const char token_row[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "#0#1#2#3#4#5#6#7\n";
    VtScreen screen;
    Bytebuf out;
    SagColor def = snap_default();
    int col;

    vt_init(&screen, 1, 60);
    for (col = 0; col < 60; col++) {
        SagColor fg = {SAG_COLOR_RGB, (u8)col, (u8)(col * 3),
                       (u8)(255 - col)};

        SAG_ASSERT(vt_set_cell(&screen, 0, col, (const u8 *)"x", 1u,
                               fg, def, 0u, 1u));
    }
    bytebuf_init(&out);
    snapshot_write(&screen, &out);
    SAG_ASSERT(buf_contains(&out, token_row));
    SAG_ASSERT(buf_contains(&out,
        "#7 fg=#3bb1c4 bg=default attrs=----------\n"));
    bytebuf_free(&out);
    vt_free(&screen);
}

void test_snapshot_trim_reconstructs_roundtrip(void)
{
    static const u8 family[] = {
        0xf0u, 0x9fu, 0x91u, 0xa8u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa9u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa7u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa6u
    };
    VtScreen before;
    VtScreen after;
    Bytebuf first;
    Bytebuf second;
    Bytebuf error;
    SagColor def = snap_default();
    SagColor styled = {SAG_COLOR_INDEXED, 7u, 0u, 0u};

    vt_init(&before, 2, 7);
    before.alt = true;
    SAG_ASSERT(vt_set_cell(&before, 0, 0, (const u8 *)"x", 1u,
                           def, def, 0u, 1u));
    SAG_ASSERT(vt_set_cell(&before, 0, 1, family, sizeof(family),
                           def, def, SAG_ATTR_ITALIC, 2u));
    SAG_ASSERT(vt_set_cell(&before, 0, 2, NULL, 0u,
                           def, def, SAG_ATTR_ITALIC, 0u));
    SAG_ASSERT(vt_set_cell(&before, 0, 4, NULL, 0u,
                           styled, def, 0u, 1u));
    bytebuf_init(&first);
    bytebuf_init(&second);
    bytebuf_init(&error);
    snapshot_write(&before, &first);
    SAG_ASSERT(snapshot_read(&first, &after, &error));
    SAG_ASSERT_EQ_U64(error.len, 0u);
    snapshot_write(&after, &second);
    SAG_ASSERT_EQ_U64(second.len, first.len);
    SAG_ASSERT_EQ_MEM(second.data, first.data, first.len);
    SAG_ASSERT(buf_contains(&first, "x"));
    SAG_ASSERT(!buf_contains(&first, "       \n"));
    bytebuf_free(&error);
    bytebuf_free(&second);
    bytebuf_free(&first);
    vt_free(&after);
    vt_free(&before);
}

void test_snapshot_diff_reports_one_cell(void)
{
    static const char want_text[] =
        "head\n" "\346\274\242" "abc\n--- legend\nA same\n";
    static const char got_text[] =
        "head\n" "\346\274\242" "axc\n--- legend\nA same\n";
    static const char expected[] =
        "snapshot differs at line 2, column 4\n"
        "want: \346\274\242" "abc\n"
        " got: \346\274\242" "axc\n"
        "         ^\n";
    Bytebuf want;
    Bytebuf got;
    Bytebuf msg;

    bytebuf_init(&want);
    bytebuf_init(&got);
    bytebuf_init(&msg);
    bytebuf_append(&want, want_text, sizeof(want_text) - 1u);
    bytebuf_append(&got, got_text, sizeof(got_text) - 1u);
    SAG_ASSERT(!snapshot_compare(&got, &want, &msg));
    SAG_ASSERT_EQ_U64(msg.len, sizeof(expected) - 1u);
    SAG_ASSERT_EQ_MEM(msg.data, expected, sizeof(expected) - 1u);
    bytebuf_free(&msg);
    bytebuf_free(&got);
    bytebuf_free(&want);
}

void test_snapshot_diff_reports_one_row(void)
{
    static const char want_text[] = "a\nb\nc\n";
    static const char got_text[] = "a\nchanged\nc\n";
    Bytebuf want;
    Bytebuf got;
    Bytebuf msg;

    bytebuf_init(&want);
    bytebuf_init(&got);
    bytebuf_init(&msg);
    bytebuf_append(&want, want_text, sizeof(want_text) - 1u);
    bytebuf_append(&got, got_text, sizeof(got_text) - 1u);
    SAG_ASSERT(!snapshot_compare(&got, &want, &msg));
    SAG_ASSERT(buf_contains(&msg, "snapshot differs at line 2, column 1\n"));
    SAG_ASSERT(buf_contains(&msg, "want: b\n got: changed\n      ^\n"));
    bytebuf_free(&msg);
    bytebuf_free(&got);
    bytebuf_free(&want);
}

void test_snapshot_diff_reports_changed_legend(void)
{
    static const char want_text[] =
        "same\n--- legend\nA fg=default bg=default attrs=----------\n";
    static const char got_text[] =
        "same\n--- legend\nA fg=idx:1 bg=default attrs=----------\n";
    Bytebuf want;
    Bytebuf got;
    Bytebuf msg;

    bytebuf_init(&want);
    bytebuf_init(&got);
    bytebuf_init(&msg);
    bytebuf_append(&want, want_text, sizeof(want_text) - 1u);
    bytebuf_append(&got, got_text, sizeof(got_text) - 1u);
    SAG_ASSERT(!snapshot_compare(&got, &want, &msg));
    SAG_ASSERT(buf_contains(&msg,
        "legend want: A fg=default bg=default attrs=----------\n"));
    SAG_ASSERT(buf_contains(&msg,
        "legend got:  A fg=idx:1 bg=default attrs=----------\n"));
    bytebuf_free(&msg);
    bytebuf_free(&got);
    bytebuf_free(&want);
}

void test_snapshot_diff_full_dumps_both_snapshots(void)
{
    static const char expected[] =
        "snapshot differs\n--- want\nwant\n--- got\ngot\n";
    Bytebuf want;
    Bytebuf got;
    Bytebuf msg;

    bytebuf_init(&want);
    bytebuf_init(&got);
    bytebuf_init(&msg);
    bytebuf_append(&want, "want\n", 5u);
    bytebuf_append(&got, "got\n", 4u);
    SAG_ASSERT_EQ_I64(setenv("SAG_PTY_DIFF", "full", 1), 0);
    SAG_ASSERT(!snapshot_compare(&got, &want, &msg));
    SAG_ASSERT_EQ_I64(unsetenv("SAG_PTY_DIFF"), 0);
    SAG_ASSERT_EQ_U64(msg.len, sizeof(expected) - 1u);
    SAG_ASSERT_EQ_MEM(msg.data, expected, sizeof(expected) - 1u);
    bytebuf_free(&msg);
    bytebuf_free(&got);
    bytebuf_free(&want);
}
