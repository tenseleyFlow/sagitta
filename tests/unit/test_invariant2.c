#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "unicode/coords.h"

enum {
    INV2_RI_COUNT = 65,
    INV2_MARK_COUNT = 300
};

typedef struct Inv2Line {
    u64 lo;
    u64 content_hi;
    u64 chars;
    u64 graphemes;
    u64 cells;
} Inv2Line;

/*
 * Sprint 58 invariant 2's hand-computed coordinate oracle.  These offsets
 * describe the final buffer, after the CR byte is inserted immediately
 * before an LF that remains in the original store.  Do not derive this table
 * from the implementation under test.
 */
static const Inv2Line inv2_lines[] = {
    {0U, 25U, 7U, 1U, 2U},
    {26U, 286U, 65U, 33U, 66U},
    {287U, 888U, 301U, 1U, 1U},
    {889U, 892U, 3U, 3U, 12U},
    {893U, 902U, 9U, 9U, 36U},
    {903U, 907U, 4U, 4U, 4U},
    {909U, 915U, 2U, 2U, 0U},
};

static const u8 inv2_family[] = {
    0xF0U, 0x9FU, 0x91U, 0xA8U, 0xE2U, 0x80U, 0x8DU,
    0xF0U, 0x9FU, 0x91U, 0xA9U, 0xE2U, 0x80U, 0x8DU,
    0xF0U, 0x9FU, 0x91U, 0xA7U, 0xE2U, 0x80U, 0x8DU,
    0xF0U, 0x9FU, 0x91U, 0xA6U,
};

static const u8 inv2_ri[] = {0xF0U, 0x9FU, 0x87U, 0xA6U};
static const u8 inv2_mark[] = {0xCCU, 0x81U};
static const u8 inv2_surrogate[] = {0xEDU, 0xA0U, 0x80U};
static const u8 inv2_overlong[] = {
    0xC0U, 0x80U,
    0xE0U, 0x80U, 0x80U,
    0xF0U, 0x80U, 0x80U, 0x80U,
};
static const u8 inv2_bom_pair[] = {
    0xEFU, 0xBBU, 0xBFU, 0xEFU, 0xBBU, 0xBFU,
};

static TextBuf *inv2_textbuf(Bytebuf *want)
{
    Bytebuf source;
    TextBuf *tb;
    u32 i;

    bytebuf_init(&source);
    bytebuf_append(&source, inv2_family, sizeof(inv2_family));
    bytebuf_push_u8(&source, (u8)'\n');
    for (i = 0U; i < INV2_RI_COUNT; i++)
        bytebuf_append(&source, inv2_ri, sizeof(inv2_ri));
    bytebuf_push_u8(&source, (u8)'\n');
    bytebuf_push_u8(&source, (u8)'e');
    for (i = 0U; i < INV2_MARK_COUNT; i++)
        bytebuf_append(&source, inv2_mark, sizeof(inv2_mark));
    bytebuf_push_u8(&source, (u8)'\n');
    bytebuf_append(&source, inv2_surrogate, sizeof(inv2_surrogate));
    bytebuf_push_u8(&source, (u8)'\n');
    bytebuf_append(&source, inv2_overlong, sizeof(inv2_overlong));
    bytebuf_push_u8(&source, (u8)'\n');
    bytebuf_append(&source, "crlf\n", 5U);
    bytebuf_append(&source, inv2_bom_pair, sizeof(inv2_bom_pair));

    tb = yew_textbuf_from_bytes(source.data, source.len);
    YEW_ASSERT_NOT_NULL(tb);
    /* The LF was copied into the original store; CR comes from add storage. */
    yew_textbuf_insert(tb, BYTEOFF(907U), (const u8 *)"\r", 1U);
    YEW_ASSERT(yew_textbuf_piece_count(tb) >= 3U);

    bytebuf_init(want);
    bytebuf_append(want, source.data, 907U);
    bytebuf_push_u8(want, (u8)'\r');
    bytebuf_append(want, source.data + 907U, source.len - 907U);
    bytebuf_free(&source);
    return tb;
}

static void inv2_assert_bytes(const TextBuf *tb, const Bytebuf *want)
{
    TextIter it;
    size_t done = 0U;

    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), want->len);
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    while (done < want->len) {
        const u8 *bytes;
        u64 len;
        size_t take;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        take = len < (u64)(want->len - done) ? (size_t)len
                                             : want->len - done;
        YEW_ASSERT_EQ_MEM(bytes, want->data + done, take);
        done += take;
        if (done < want->len)
            YEW_ASSERT(yew_textiter_advance(&it, tb));
    }
}

void test_invariant2_adversarial_columns_and_all_units(void)
{
    static const UnitOps *const units[] = {
        &yew_unit_line,
        &yew_unit_word,
        &yew_unit_block,
        &yew_unit_char,
    };
    Bytebuf want;
    TextBuf *tb = inv2_textbuf(&want);
    Buffer buffer = {0};
    Win win = {0};
    UnitCtx unit = {0};
    size_t i;

    buffer.tb = tb;
    buffer.tabwidth = 4U;
    win.buf = &buffer;
    win.vp.rows = 24U;
    win.vp.cols = 80U;
    unit.tb = tb;
    unit.buf = &buffer;
    unit.win = &win;

    YEW_ASSERT_EQ_U64(want.len, 915U);
    YEW_ASSERT_EQ_U64(yew_textbuf_line_count(tb),
                      YEW_ARRAY_LEN(inv2_lines));
    inv2_assert_bytes(tb, &want);

    for (i = 0U; i < YEW_ARRAY_LEN(inv2_lines); i++) {
        const Inv2Line *oracle = &inv2_lines[i];
        Span line = yew_textbuf_line_span(tb, LINENO(i));
        ByteOff at = BYTEOFF(line.lo);
        u64 span_hi = i + 1U < YEW_ARRAY_LEN(inv2_lines)
                          ? inv2_lines[i + 1U].lo
                          : want.len;
        u64 graphemes = 0U;
        u64 cells = 0U;

        YEW_ASSERT_EQ_U64(line.lo, oracle->lo);
        YEW_ASSERT_EQ_U64(line.hi, span_hi);
        while (at.v < oracle->content_hi) {
            YewTextCluster cluster;

            YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, at).v,
                              graphemes);
            YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, at, 4U).v,
                              cells);
            YEW_ASSERT(yew_text_cluster_next(tb, line, at, &cluster));
            YEW_ASSERT_EQ_U64(cluster.bytes.lo, at.v);
            YEW_ASSERT(cluster.bytes.hi > cluster.bytes.lo);
            cells += cluster.cells;
            graphemes++;
            at = BYTEOFF(cluster.bytes.hi);
        }
        YEW_ASSERT_EQ_U64(at.v, oracle->content_hi);
        YEW_ASSERT_EQ_U64(yew_off_to_charcol(tb, line, at).v,
                          oracle->chars);
        YEW_ASSERT_EQ_U64(yew_off_to_gcol(tb, line, at).v,
                          oracle->graphemes);
        YEW_ASSERT_EQ_U64(yew_off_to_ccol(tb, line, at, 4U).v,
                          oracle->cells);
        YEW_ASSERT_EQ_U64(graphemes, oracle->graphemes);
        YEW_ASSERT_EQ_U64(cells, oracle->cells);
        YEW_ASSERT_EQ_U64(yew_gcol_to_off(tb, line,
                          (GCol){oracle->graphemes}).v,
                          oracle->content_hi);
    }

    for (i = 0U; i < YEW_ARRAY_LEN(units); i++) {
        const UnitOps *ops = units[i];
        u8 alt;

        for (alt = 0U; alt < 2U; alt++) {
            ByteOff at = BYTEOFF(0U);
            u64 steps = 0U;

            while (at.v < want.len) {
                ByteOff next = ops->next(&unit, at, alt != 0U);

                YEW_ASSERT(next.v > at.v);
                YEW_ASSERT(next.v <= want.len);
                YEW_ASSERT(yew_is_grapheme_boundary(tb, next));
                at = next;
                YEW_ASSERT(++steps <= want.len + 1U);
            }
            while (at.v != 0U) {
                ByteOff prev = ops->prev(&unit, at, alt != 0U);

                YEW_ASSERT(prev.v < at.v);
                YEW_ASSERT(yew_is_grapheme_boundary(tb, prev));
                at = prev;
                YEW_ASSERT(++steps <= 2U * want.len + 2U);
            }
        }
    }

    inv2_assert_bytes(tb, &want);
    bytebuf_free(&want);
    yew_textbuf_free(tb);
}
