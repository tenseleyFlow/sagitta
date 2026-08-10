#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "syn/engine.h"
#include "text/piece.h"

#include "syn_toy.h"

static TextBuf *lines_buf(u32 lines)
{
    u8 *bytes;
    TextBuf *tb;
    u32 i;

    YEW_ASSERT(lines > 0U);
    if (lines == 1U)
        return yew_textbuf_new();
    bytes = malloc(lines - 1U);
    YEW_ASSERT_NOT_NULL(bytes);
    for (i = 0U; i + 1U < lines; i++)
        bytes[i] = (u8)'\n';
    tb = yew_textbuf_from_bytes(bytes, lines - 1U);
    free(bytes);
    return tb;
}

static void assert_syn_shape(const SynBuf *syn, u64 lines)
{
    YEW_ASSERT_EQ_U64(syn->entry.len, lines);
    YEW_ASSERT(syn->entry.len > 0U);
    YEW_ASSERT_EQ_U64(syn->entry.data[0], YEW_SYN_STATE_ROOT);
}

void test_syn_splice_table_preserves_root_and_length(void)
{
    typedef struct SpliceRow {
        u32 lines;
        u32 lo;
        u32 removed;
        u32 inserted;
    } SpliceRow;
    static const SpliceRow rows[] = {
        {1U, 0U, 0U, 0U},       /* empty text buffer                  */
        {2U, 0U, 0U, 1U},       /* edit line zero, add one line       */
        {5U, 4U, 0U, 1U},       /* edit the last line                 */
        {5U, 2U, 1U, 0U},       /* one line becomes no extra line     */
        {5U, 2U, 0U, 3U},       /* multiline insertion               */
        {8U, 1U, 3U, 1U},       /* multiline replacement             */
        {8U, 0U, 7U, 0U},       /* collapse to one line               */
        {3U, 1U, 0U, 10000U}    /* bulk paste                         */
    };
    SynToy toy;
    u32 r;

    syn_toy_init(&toy);
    YEW_ASSERT_EQ_U64(YEW_ARRAY_LEN(rows), 8U);
    for (r = 0U; r < YEW_ARRAY_LEN(rows); r++) {
        TextBuf *tb = lines_buf(rows[r].lines);
        SynBuf syn;
        u64 expected = rows[r].lines - rows[r].removed + rows[r].inserted;

        yew_syn_buf_init(&syn);
        yew_syn_buf_bind(&syn, toy.engine);
        yew_syn_attach(&syn, 1U, tb);
        syn.settled_to = LINENO(rows[r].lines);
        syn.wave = LINENO(rows[r].lines);
        assert_syn_shape(&syn, rows[r].lines);
        yew_syn_edit(&syn, LINENO(rows[r].lo), rows[r].removed,
                     rows[r].inserted);
        assert_syn_shape(&syn, expected);
        YEW_ASSERT_EQ_U64(syn.splice_count, 1U);
        YEW_ASSERT_EQ_U64(syn.settled_to.v,
                          rows[r].lo + 1U < rows[r].lines ?
                          rows[r].lo + 1U : rows[r].lines);
        YEW_ASSERT_EQ_U64(syn.wave.v, rows[r].lo);
        yew_syn_detach(&syn);
        yew_textbuf_free(tb);
    }
    syn_toy_free(&toy);
}

void test_syn_splice_inserts_unknown_slots_and_retains_tail(void)
{
    TextBuf *tb = lines_buf(6U);
    SynToy toy;
    SynBuf syn;
    u32 i;

    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_attach(&syn, 1U, tb);
    for (i = 1U; i < 6U; i++)
        syn.entry.data[i] = 100U + i;
    yew_syn_edit(&syn, LINENO(2U), 1U, 3U);
    assert_syn_shape(&syn, 8U);
    YEW_ASSERT_EQ_U64(syn.entry.data[1], 101U);
    YEW_ASSERT_EQ_U64(syn.entry.data[2], 102U);
    YEW_ASSERT_EQ_U64(syn.entry.data[3], YEW_SYN_STATE_UNKNOWN);
    YEW_ASSERT_EQ_U64(syn.entry.data[4], YEW_SYN_STATE_UNKNOWN);
    YEW_ASSERT_EQ_U64(syn.entry.data[5], YEW_SYN_STATE_UNKNOWN);
    YEW_ASSERT_EQ_U64(syn.entry.data[6], 104U);
    YEW_ASSERT_EQ_U64(syn.entry.data[7], 105U);
    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
}

void test_syn_splice_clamps_settled_and_wave_to_damage(void)
{
    TextBuf *tb = lines_buf(20U);
    SynToy toy;
    SynBuf syn;

    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_attach(&syn, 1U, tb);
    syn.settled_to = LINENO(18U);
    syn.wave = LINENO(16U);
    yew_syn_edit(&syn, LINENO(4U), 5U, 2U);
    YEW_ASSERT_EQ_U64(syn.settled_to.v, 5U);
    YEW_ASSERT_EQ_U64(syn.wave.v, 4U);
    YEW_ASSERT(syn.settling);
    assert_syn_shape(&syn, 17U);
    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
}

void test_syn_splice_bulk_insert_uses_one_vector_splice(void)
{
    TextBuf *tb = lines_buf(3U);
    SynToy toy;
    SynBuf syn;

    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_attach(&syn, 1U, tb);
    yew_syn_edit(&syn, LINENO(1U), 0U, 10000U);
    assert_syn_shape(&syn, 10003U);
    YEW_ASSERT_EQ_U64(syn.splice_count, 1U);
    YEW_ASSERT(syn.entry.cap >= syn.entry.len);
    YEW_ASSERT_EQ_U64(syn.entry.data[1], YEW_SYN_STATE_UNKNOWN);
    YEW_ASSERT_EQ_U64(syn.entry.data[10001U], YEW_SYN_STATE_UNKNOWN);
    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
}
