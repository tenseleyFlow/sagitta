#include "harness.h"

#include "text/cursor.h"

static Cursor cursor_at(const TextBuf *tb, u64 off)
{
    Cursor c;
    LineNo line = sag_textbuf_line_of(tb, BYTEOFF(off));

    c.pos = BYTEOFF(off);
    c.goal_col = sag_off_to_gcol(tb, sag_textbuf_line_span(tb, line), c.pos);
    c.anchor = c.pos;
    return c;
}

static void assert_cursor_boundary(const TextBuf *tb, const Cursor *c)
{
    SAG_ASSERT(c->pos.v <= sag_textbuf_len(tb));
    SAG_ASSERT(sag_is_grapheme_boundary(tb, c->pos));
    SAG_ASSERT(c->anchor.v <= sag_textbuf_len(tb));
    SAG_ASSERT(sag_is_grapheme_boundary(tb, c->anchor));
}

void test_cursor_horizontal_graphemes(void)
{
    static const u8 text[] =
        "A"
        "e\xcc\x81"
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
        "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
        "\xf0\x9f\x91\xa6"
        "\r\nZ";
    static const u64 boundaries[] = {0U, 1U, 4U, 29U, 31U, 32U};
    TextBuf *tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
    Cursor c = cursor_at(tb, 0U);
    size_t i;

    for (i = 1U; i < SAG_ARRAY_LEN(boundaries); i++) {
        sag_cursor_right(tb, &c);
        SAG_ASSERT_EQ_U64(c.pos.v, boundaries[i]);
        SAG_ASSERT_EQ_U64(c.anchor.v, c.pos.v);
        assert_cursor_boundary(tb, &c);
    }
    sag_cursor_right(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, boundaries[SAG_ARRAY_LEN(boundaries) - 1U]);
    for (i = SAG_ARRAY_LEN(boundaries) - 1U; i > 0U; i--) {
        sag_cursor_left(tb, &c);
        SAG_ASSERT_EQ_U64(c.pos.v, boundaries[i - 1U]);
        assert_cursor_boundary(tb, &c);
    }
    sag_cursor_left(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 0U);
    sag_textbuf_free(tb);
}

void test_cursor_vertical_sticky_goal(void)
{
    static const u8 text[] = "abcdef\nx\nabcdef";
    TextBuf *tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
    Cursor c = cursor_at(tb, 5U);

    SAG_ASSERT_EQ_U64(c.goal_col.v, 5U);
    sag_cursor_down(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 8U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 5U);
    sag_cursor_down(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 14U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 5U);
    sag_cursor_up(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 8U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 5U);
    sag_cursor_up(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 5U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 5U);
    sag_cursor_up(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 5U);
    assert_cursor_boundary(tb, &c);
    sag_textbuf_free(tb);
}

void test_cursor_horizontal_resolves_vertical_clamp(void)
{
    static const u8 text[] = "abcdef\nxy";
    TextBuf *tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
    Cursor c = cursor_at(tb, 5U);

    sag_cursor_down(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 8U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 5U);
    sag_cursor_right(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 9U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 2U);
    sag_cursor_left(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 8U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 1U);
    assert_cursor_boundary(tb, &c);
    sag_textbuf_free(tb);
}

void test_cursor_horizontal_recomputes_cross_line_and_edges(void)
{
    static const u8 text[] = "abcdef\nabcdefghij";
    TextBuf *tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
    Cursor c = cursor_at(tb, 7U);

    sag_cursor_left(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 6U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 6U);
    sag_cursor_down(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 13U);

    c.pos = BYTEOFF(0U);
    c.anchor = c.pos;
    c.goal_col = (GCol){99U};
    sag_cursor_left(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 0U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 0U);

    c.pos = BYTEOFF(sizeof(text) - 1U);
    c.anchor = c.pos;
    c.goal_col = (GCol){99U};
    sag_cursor_right(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, sizeof(text) - 1U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 10U);
    sag_textbuf_free(tb);
}

void test_cursor_home_end_and_crlf(void)
{
    static const u8 text[] = "abc\r\nq\r\nlast";
    TextBuf *tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
    Cursor c = cursor_at(tb, 1U);

    sag_cursor_line_end(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 3U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, UINT64_MAX);
    sag_cursor_down(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 6U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, UINT64_MAX);
    sag_cursor_down(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 11U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, UINT64_MAX);
    sag_cursor_line_home(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 8U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 0U);
    sag_cursor_buf_end(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, sizeof(text) - 1U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, UINT64_MAX);
    sag_cursor_left(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, sizeof(text) - 2U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 3U);
    sag_cursor_buf_home(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 0U);
    SAG_ASSERT_EQ_U64(c.goal_col.v, 0U);
    assert_cursor_boundary(tb, &c);
    sag_textbuf_free(tb);
}

void test_cursor_clamp_repairs_new_cluster(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"eX", 2U);
    Cursor c = cursor_at(tb, 1U);
    static const u8 combining[] = {0xccU, 0x81U};

    sag_textbuf_insert(tb, BYTEOFF(1U), combining, sizeof(combining));
    SAG_ASSERT(!sag_is_grapheme_boundary(tb, c.pos));
    sag_cursor_clamp(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 0U);
    SAG_ASSERT_EQ_U64(c.anchor.v, 0U);
    assert_cursor_boundary(tb, &c);

    c.pos = BYTEOFF(2U); /* inside the combining mark's UTF-8 encoding */
    c.anchor = c.pos;
    sag_cursor_clamp(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 0U);
    SAG_ASSERT_EQ_U64(c.anchor.v, 0U);

    c.pos = BYTEOFF(UINT64_MAX);
    c.anchor = BYTEOFF(UINT64_MAX);
    sag_cursor_clamp(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 4U);
    SAG_ASSERT_EQ_U64(c.anchor.v, 4U);
    assert_cursor_boundary(tb, &c);
    sag_textbuf_free(tb);
}

void test_cursor_motion_preserves_selection_anchor(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abc\ndef", 7U);
    Cursor c = cursor_at(tb, 2U);

    c.anchor = BYTEOFF(0U);
    sag_cursor_right(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 3U);
    SAG_ASSERT_EQ_U64(c.anchor.v, 0U);
    sag_cursor_down(tb, &c);
    SAG_ASSERT_EQ_U64(c.anchor.v, 0U);
    sag_cursor_line_home(tb, &c);
    SAG_ASSERT_EQ_U64(c.pos.v, 4U);
    SAG_ASSERT_EQ_U64(c.anchor.v, 0U);
    assert_cursor_boundary(tb, &c);
    sag_textbuf_free(tb);
}

static u64 cursor_rng(u64 *state)
{
    u64 x = *state;

    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    *state = x;
    return x;
}

void test_cursor_motion_fuzz_four_seeds(void)
{
    static const u8 text[] =
        "alpha e\xcc\x81 omega\r\n"
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
        "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
        "\xf0\x9f\x91\xa6\n"
        "\xe6\xbc\xa2\tend\n"
        "short";
    static const u64 seeds[] = {
        1U, UINT64_C(0x243f6a8885a308d3),
        UINT64_C(0x9e3779b97f4a7c15), UINT64_C(0xd1b54a32d192ed03)
    };
    static const u8 ins_ascii[] = "x";
    static const u8 ins_extend[] = {0xccU, 0x81U};
    static const u8 ins_crlf[] = "\r\n";
    static const u8 ins_wide[] = {0xe6U, 0xbcU, 0xa2U};
    size_t seed_i;

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(seeds); seed_i++) {
        TextBuf *tb = sag_textbuf_from_bytes(text, sizeof(text) - 1U);
        Cursor c = cursor_at(tb, 0U);
        u64 state = seeds[seed_i];
        u64 i;

        for (i = 0U; i < 10000U; i++) {
            switch (cursor_rng(&state) % 10U) {
            case 0U: sag_cursor_left(tb, &c); break;
            case 1U: sag_cursor_right(tb, &c); break;
            case 2U: sag_cursor_up(tb, &c); break;
            case 3U: sag_cursor_down(tb, &c); break;
            case 4U: sag_cursor_line_home(tb, &c); break;
            case 5U: sag_cursor_line_end(tb, &c); break;
            case 6U: sag_cursor_buf_home(tb, &c); break;
            case 7U: sag_cursor_buf_end(tb, &c); break;
            case 8U: {
                const u8 *payload;
                u64 payload_len;

                switch (cursor_rng(&state) % 4U) {
                case 0U:
                    payload = ins_ascii;
                    payload_len = sizeof(ins_ascii) - 1U;
                    break;
                case 1U:
                    payload = ins_extend;
                    payload_len = sizeof(ins_extend);
                    break;
                case 2U:
                    payload = ins_crlf;
                    payload_len = sizeof(ins_crlf) - 1U;
                    break;
                default:
                    payload = ins_wide;
                    payload_len = sizeof(ins_wide);
                    break;
                }
                sag_textbuf_insert(tb, c.pos, payload, payload_len);
                sag_cursor_clamp(tb, &c);
                break;
            }
            default:
                if (c.pos.v < sag_textbuf_len(tb)) {
                    ByteOff next = sag_grapheme_next(tb, c.pos);

                    SAG_ASSERT(next.v > c.pos.v);
                    sag_textbuf_delete(tb, (Span){c.pos.v, next.v});
                    sag_cursor_clamp(tb, &c);
                }
                break;
            }
            assert_cursor_boundary(tb, &c);
        }
        sag_textbuf_free(tb);
    }
}
