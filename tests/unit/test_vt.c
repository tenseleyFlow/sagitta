#include "harness.h"

#include "../pty/vt.h"

#include <stdio.h>
#include <string.h>

static void feed_lit(VtScreen *v, const char *s)
{
    vt_feed(v, (const u8 *)s, strlen(s));
}

static bool buf_contains(const Bytebuf *buf, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    for (i = 0u; i + n <= buf->len; i++)
        if (memcmp(buf->data + i, needle, n) == 0)
            return true;
    return false;
}

static void enter_alt(VtScreen *v)
{
    feed_lit(v, "\033[?1049h");
    SAG_ASSERT(v->alt);
}

static void assert_color(SagColor got, u8 tag, u8 r, u8 g, u8 b)
{
    SAG_ASSERT_EQ_U64(got.tag, tag);
    SAG_ASSERT_EQ_U64(got.r, r);
    SAG_ASSERT_EQ_U64(got.g, g);
    SAG_ASSERT_EQ_U64(got.b, b);
}

static void assert_vt_equal(const VtScreen *a, const VtScreen *b)
{
    size_t count = (size_t)a->rows * (size_t)a->cols;
    size_t i;

    SAG_ASSERT_EQ_I64(a->rows, b->rows);
    SAG_ASSERT_EQ_I64(a->cols, b->cols);
    SAG_ASSERT_EQ_I64(a->cur_r, b->cur_r);
    SAG_ASSERT_EQ_I64(a->cur_c, b->cur_c);
    SAG_ASSERT_EQ_U64(a->alt, b->alt);
    SAG_ASSERT_EQ_U64(a->cur_vis, b->cur_vis);
    SAG_ASSERT_EQ_U64(a->in_sync, b->in_sync);
    SAG_ASSERT_EQ_U64(a->modes, b->modes);
    SAG_ASSERT_EQ_I64(a->ksp, b->ksp);
    SAG_ASSERT_EQ_U64(a->nerrors, b->nerrors);
    SAG_ASSERT_EQ_U64(a->nsync_pairs, b->nsync_pairs);
    SAG_ASSERT_EQ_MEM(a->kitty, b->kitty, sizeof(a->kitty));
    for (i = 0u; i < count; i++) {
        const u8 *ab;
        const u8 *bb;
        size_t an;
        size_t bn;

        SAG_ASSERT_EQ_MEM(&a->cells[i], &b->cells[i], sizeof(a->cells[i]));
        ab = vt_cell_bytes(a, &a->cells[i], &an);
        bb = vt_cell_bytes(b, &b->cells[i], &bn);
        SAG_ASSERT_EQ_U64(an, bn);
        SAG_ASSERT_EQ_MEM(ab, bb, an);
    }
}

void test_vt_closed_set_and_modes(void)
{
    VtScreen v;
    u32 all_modes = VT_MODE_BRACKETED_PASTE | VT_MODE_BUTTON_MOUSE |
                    VT_MODE_SGR_MOUSE | VT_MODE_FOCUS;

    vt_init(&v, 3, 8);
    feed_lit(&v, "\0337\033[?1049h\033[?2004h\033[?1002h\033[?1006h"
                 "\033[?1004h\033[>21u\033[?2026h\033[2;3H\033[4C"
                 "\033[?25l\033[6 q\033[?2026l");
    SAG_ASSERT_EQ_U64(v.nerrors, 0u);
    SAG_ASSERT_EQ_U64(v.modes, all_modes);
    SAG_ASSERT_EQ_I64(v.ksp, 1);
    SAG_ASSERT_EQ_U64(v.kitty[0], 21u);
    SAG_ASSERT_EQ_I64(v.cur_r, 1);
    SAG_ASSERT_EQ_I64(v.cur_c, 6);
    SAG_ASSERT(!v.cur_vis);
    SAG_ASSERT_EQ_U64(v.cursor_shape, 6u);
    SAG_ASSERT_EQ_U64(v.nsync_pairs, 1u);
    feed_lit(&v, "\033[0 q\033[<u\033[?2004l\033[?1002l\033[?1006l\033[?1004l"
                 "\033[?1049l\0338");
    SAG_ASSERT_EQ_U64(v.modes, 0u);
    SAG_ASSERT_EQ_I64(v.ksp, 0);
    SAG_ASSERT(!v.alt);
    SAG_ASSERT_EQ_U64(v.cursor_shape, 0u);
    SAG_ASSERT_EQ_I64(v.cur_r, 0);
    SAG_ASSERT_EQ_I64(v.cur_c, 0);
    vt_free(&v);
}

void test_vt_probe_profiles(void)
{
    static const char queries[] = "\033[?u\033[?2026$p\033[c";
    static const char *const replies[] = {
        "\033[?0u\033[?2026;2$y\033[?62;22c",
        "\033[?2026;2$y\033[?62;22c",
        "\033[?0u\033[?2026;0$y\033[?62;22c",
        ""
    };
    VtProfile profile;

    SAG_ASSERT(vt_profile_from_name("modern", &profile));
    SAG_ASSERT_EQ_U64(profile, VT_PROFILE_MODERN);
    SAG_ASSERT(!vt_profile_from_name("unknown", &profile));
    for (profile = VT_PROFILE_MODERN; profile <= VT_PROFILE_DUMB;
         profile = (VtProfile)(profile + 1)) {
        VtScreen v;
        Bytebuf got;

        vt_init(&v, 2, 4);
        vt_set_profile(&v, profile);
        feed_lit(&v, queries);
        SAG_ASSERT_EQ_U64(v.probes,
                          VT_PROBE_KITTY | VT_PROBE_SYNC | VT_PROBE_DA);
        SAG_ASSERT_EQ_U64(v.nprobes, 3u);
        SAG_ASSERT_EQ_U64(v.probe_order[0], VT_PROBE_KITTY);
        SAG_ASSERT_EQ_U64(v.probe_order[1], VT_PROBE_SYNC);
        SAG_ASSERT_EQ_U64(v.probe_order[2], VT_PROBE_DA);
        bytebuf_init(&got);
        vt_take_replies(&v, &got);
        SAG_ASSERT_EQ_U64(got.len, strlen(replies[profile]));
        SAG_ASSERT_EQ_MEM(got.data, replies[profile], got.len);
        SAG_ASSERT_EQ_U64(v.replies.len, 0u);
        bytebuf_free(&got);
        vt_free(&v);
    }
}

void test_vt_sgr_closed_rows(void)
{
    VtScreen v;
    unsigned i;
    u16 all_attrs = SAG_ATTR_BOLD | SAG_ATTR_DIM | SAG_ATTR_ITALIC |
                    SAG_ATTR_UNDERCURL | SAG_ATTR_BLINK | SAG_ATTR_REVERSE |
                    SAG_ATTR_CONCEAL | SAG_ATTR_STRIKE | SAG_ATTR_OVERLINE;

    vt_init(&v, 2, 8);
    enter_alt(&v);
    feed_lit(&v, "\033[1;2;3;4;4:3;5;7;8;9;53m");
    SAG_ASSERT_EQ_U64(v.attrs, all_attrs);
    feed_lit(&v, "\033[38;5;255;48;2;1;2;3m");
    assert_color(v.fg, SAG_COLOR_INDEXED, 255u, 0u, 0u);
    assert_color(v.bg, SAG_COLOR_RGB, 1u, 2u, 3u);
    feed_lit(&v, "X");
    SAG_ASSERT_EQ_U64(v.cells[0].attrs, all_attrs);
    feed_lit(&v, "\033[22;23;24;25;27;28;29;55;39;49m");
    SAG_ASSERT_EQ_U64(v.attrs, 0u);
    assert_color(v.fg, SAG_COLOR_DEFAULT, 0u, 0u, 0u);
    assert_color(v.bg, SAG_COLOR_DEFAULT, 0u, 0u, 0u);
    feed_lit(&v, "\033[31;104m");
    assert_color(v.fg, SAG_COLOR_INDEXED, 1u, 0u, 0u);
    assert_color(v.bg, SAG_COLOR_INDEXED, 12u, 0u, 0u);
    for (i = 0u; i < 8u; i++) {
        char seq[24];

        (void)snprintf(seq, sizeof(seq), "\033[%u;%u;%u;%um",
                       30u + i, 90u + i, 40u + i, 100u + i);
        feed_lit(&v, seq);
        assert_color(v.fg, SAG_COLOR_INDEXED, (u8)(8u + i), 0u, 0u);
        assert_color(v.bg, SAG_COLOR_INDEXED, (u8)(8u + i), 0u, 0u);
    }
    feed_lit(&v, "abcd\033[1;2H\033[K");
    for (i = 1u; i < 8u; i++)
        assert_color(v.cells[i].bg, SAG_COLOR_DEFAULT, 0u, 0u, 0u);
    feed_lit(&v, "\033[m");
    SAG_ASSERT_EQ_U64(v.nerrors, 0u);
    feed_lit(&v, "\033[38;5;256m");
    SAG_ASSERT_EQ_U64(v.nerrors, 1u);
    vt_free(&v);
}

void test_vt_utf8_graphemes_and_wide_cells(void)
{
    static const u8 text[] = {
        'e', 0xccu, 0x81u,
        0xe6u, 0xbcu, 0xa2u,
        0xf0u, 0x9fu, 0x91u, 0xa8u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa9u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa7u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa6u
    };
    static const u8 family[] = {
        0xf0u, 0x9fu, 0x91u, 0xa8u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa9u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa7u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa6u
    };
    VtScreen v;
    const u8 *stored;
    size_t n;

    vt_init(&v, 2, 10);
    enter_alt(&v);
    vt_feed(&v, text, sizeof(text));
    SAG_ASSERT_EQ_U64(v.nerrors, 0u);
    SAG_ASSERT_EQ_U64(v.cells[0].w, 1u);
    SAG_ASSERT_EQ_U64(v.cells[1].w, 2u);
    SAG_ASSERT_EQ_U64(v.cells[2].w, 0u);
    SAG_ASSERT_EQ_U64(v.cells[3].w, 2u);
    SAG_ASSERT_EQ_U64(v.cells[4].w, 0u);
    stored = vt_cell_bytes(&v, &v.cells[3], &n);
    SAG_ASSERT_EQ_U64(n, sizeof(family));
    SAG_ASSERT_EQ_MEM(stored, family, sizeof(family));
    vt_free(&v);
}

void test_vt_chunk_safe_every_boundary(void)
{
    static const u8 stream[] = {
        0x1bu, '[', '?', '1', '0', '4', '9', 'h',
        0x1bu, '[', '?', '2', '0', '2', '6', 'h',
        0x1bu, '[', '2', ';', '2', 'H',
        0x1bu, '[', '1', ';', '3', '8', ';', '2', ';', '4', ';', '5', ';', '6', 'm',
        0xe6u, 0xbcu, 0xa2u, 'x',
        0x1bu, '[', '?', '2', '0', '2', '6', 'l'
    };
    VtScreen whole;
    size_t split;

    vt_init(&whole, 4, 12);
    vt_feed(&whole, stream, sizeof(stream));
    for (split = 0u; split <= sizeof(stream); split++) {
        VtScreen chunked;

        vt_init(&chunked, 4, 12);
        vt_feed(&chunked, stream, split);
        vt_feed(&chunked, stream + split, sizeof(stream) - split);
        assert_vt_equal(&whole, &chunked);
        vt_free(&chunked);
    }
    vt_free(&whole);
}

void test_vt_rejects_unknown_and_protocol_errors(void)
{
    static const u8 cjk[] = {0xe6u, 0xbcu, 0xa2u};
    VtScreen v;

    vt_init(&v, 1, 3);
    enter_alt(&v);
    feed_lit(&v, "\033[5L");
    SAG_ASSERT_EQ_U64(v.nerrors, 1u);
    SAG_ASSERT(buf_contains(&v.errors, "unknown sequence: ESC [ 5 L"));
    feed_lit(&v, "\033[?2026h\033[?2026h");
    SAG_ASSERT_EQ_U64(v.nerrors, 2u);
    feed_lit(&v, "\033[1;3H");
    vt_feed(&v, cjk, sizeof(cjk));
    SAG_ASSERT_EQ_U64(v.nerrors, 3u);
    feed_lit(&v, "\033[1;1H");
    vt_feed(&v, cjk, sizeof(cjk));
    feed_lit(&v, "\033[1;2Hx");
    SAG_ASSERT_EQ_U64(v.nerrors, 4u);
    feed_lit(&v, "\033[?2026l\033[?2026l");
    SAG_ASSERT_EQ_U64(v.nerrors, 5u);
    vt_set_restore_policy(&v, true);
    feed_lit(&v, "\033[?2026l\033[<u");
    SAG_ASSERT_EQ_U64(v.nerrors, 5u);
    feed_lit(&v, "\033[>1u\033[>2u\033[>3u\033[>4u"
                 "\033[>5u\033[>6u\033[>7u\033[>8u");
    SAG_ASSERT_EQ_I64(v.ksp, 8);
    feed_lit(&v, "\033[>9u");
    SAG_ASSERT_EQ_U64(v.nerrors, 6u);
    feed_lit(&v, "\033[<u\033[<u\033[<u\033[<u\033[<u"
                 "\033[<u\033[<u\033[<u\033[<u");
    SAG_ASSERT_EQ_I64(v.ksp, 0);
    SAG_ASSERT_EQ_U64(v.nerrors, 6u);
    vt_free(&v);
}

void test_vt_resize_preserves_and_repairs_cells(void)
{
    static const u8 family[] = {
        0xf0u, 0x9fu, 0x91u, 0xa8u, 0xe2u, 0x80u, 0x8du,
        0xf0u, 0x9fu, 0x91u, 0xa9u
    };
    VtScreen v;
    const u8 *stored;
    size_t n;

    vt_init(&v, 2, 4);
    enter_alt(&v);
    vt_feed(&v, family, sizeof(family));
    vt_resize(&v, 3, 5);
    stored = vt_cell_bytes(&v, &v.cells[0], &n);
    SAG_ASSERT_EQ_U64(n, sizeof(family));
    SAG_ASSERT_EQ_MEM(stored, family, n);
    SAG_ASSERT_EQ_U64(v.cells[0].w, 2u);
    SAG_ASSERT_EQ_U64(v.cells[1].w, 0u);
    vt_resize(&v, 3, 1);
    SAG_ASSERT_EQ_U64(v.cells[0].w, 1u);
    vt_free(&v);
}

void test_vt_primary_screen_policy_and_transcript(void)
{
    VtScreen strict;
    VtScreen allowed;

    vt_init(&strict, 2, 4);
    feed_lit(&strict, "fatal\n");
    SAG_ASSERT(strict.primary_written);
    SAG_ASSERT(strict.nerrors != 0u);
    SAG_ASSERT_EQ_MEM(strict.primary.data, "fatal", 5u);
    vt_free(&strict);

    vt_init(&allowed, 2, 4);
    vt_set_primary_policy(&allowed, true);
    feed_lit(&allowed, "driver deferred");
    SAG_ASSERT(allowed.primary_written);
    SAG_ASSERT_EQ_U64(allowed.nerrors, 0u);
    SAG_ASSERT_EQ_U64(allowed.primary.len, strlen("driver deferred"));
    SAG_ASSERT_EQ_MEM(allowed.primary.data, "driver deferred",
                      allowed.primary.len);
    vt_free(&allowed);
}

void test_vt_combining_storage_is_bounded(void)
{
    static const u8 acute[] = {0xccu, 0x81u};
    VtScreen v;
    size_t i;
    size_t n;

    vt_init(&v, 1, 2);
    enter_alt(&v);
    feed_lit(&v, "x");
    for (i = 0u; i < 5000u; i++)
        vt_feed(&v, acute, sizeof(acute));
    (void)vt_cell_bytes(&v, &v.cells[0], &n);
    SAG_ASSERT(n <= VT_CLUSTER_BYTES_MAX);
    SAG_ASSERT(v.glyphs.len <= 2u * VT_CLUSTER_BYTES_MAX);
    SAG_ASSERT(v.errors.len <= VT_TRANSCRIPT_BYTES_MAX);
    SAG_ASSERT(v.nerrors != 0u);
    vt_free(&v);
}
