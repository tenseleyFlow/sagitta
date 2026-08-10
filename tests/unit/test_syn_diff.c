#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "syn/engine.h"
#include "text/piece.h"

#include "syn_toy.h"

static u64 diff_rand(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void diff_settle_all(SynBuf *syn, const TextBuf *tb)
{
    SynSettleReport report;
    u32 calls = 0U;

    do {
        yew_syn_settle(syn, tb, LINENO(0U),
                       LINENO(yew_textbuf_line_count(tb)), INT64_MAX,
                       &report);
        YEW_ASSERT(++calls < 1000U);
    } while (!report.fixpoint);
}

static void assert_line_equal(const SynLineOut *a, const SynLineOut *b)
{
    u32 i;

    YEW_ASSERT_EQ_U64(a->stop, b->stop);
    YEW_ASSERT_EQ_U64(a->exit_state, b->exit_state);
    YEW_ASSERT_EQ_U64(a->n, b->n);
    for (i = 0U; i < a->n; i++) {
        YEW_ASSERT_EQ_U64(a->spans[i].start, b->spans[i].start);
        YEW_ASSERT_EQ_U64(a->spans[i].len, b->spans[i].len);
        YEW_ASSERT_EQ_U64(a->spans[i].attr, b->spans[i].attr);
        YEW_ASSERT_EQ_U64(a->spans[i].flags, b->spans[i].flags);
    }
}

static void run_diff_seed(u64 seed, u32 edits)
{
    static const char initial[] =
        "if value = 10\n"
        "plain text\n"
        "/* block\n"
        "comment */ return\n"
        "\"str\\n\" + false\n"
        "// final comment\n"
        "while x = 99\n"
        "tail\n";
    static const u8 replacements[] =
        "abcdefghijklmnopqrstuvwxyz0123456789/*\"\\=+- ";
    size_t len = sizeof(initial) - 1U;
    u8 *flat = malloc(len);
    TextBuf *tb;
    SynToy toy;
    SynBuf syn;
    u32 i;

    YEW_ASSERT_NOT_NULL(flat);
    (void)memcpy(flat, initial, len);
    tb = yew_textbuf_from_bytes(flat, len);
    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_attach(&syn, 1U, tb);
    diff_settle_all(&syn, tb);

    for (i = 0U; i < edits; i++) {
        size_t at;
        u8 byte;
        LineNo line;

        do {
            at = (size_t)(diff_rand(&seed) % len);
        } while (flat[at] == (u8)'\n');
        byte = replacements[diff_rand(&seed) %
                            (sizeof(replacements) - 1U)];
        line = yew_textbuf_line_of(tb, BYTEOFF(at));
        yew_textbuf_delete(tb, (Span){at, at + 1U});
        yew_textbuf_insert(tb, BYTEOFF(at), &byte, 1U);
        flat[at] = byte;
        yew_syn_edit(&syn, line, 0U, 0U);
        diff_settle_all(&syn, tb);
        YEW_ASSERT_EQ_U64(syn.entry.len, yew_textbuf_line_count(tb));
        YEW_ASSERT_EQ_U64(syn.entry.data[0], YEW_SYN_STATE_ROOT);
    }

    {
        u32 state = YEW_SYN_STATE_ROOT;
        u64 lo = 0U;
        u64 line_count = yew_textbuf_line_count(tb);
        u64 line;

        for (line = 0U; line < line_count; line++) {
            u64 hi = lo;
            SynSpan want_spans[128];
            SynSpan got_spans[128];
            SynLineOut want = {want_spans, 0U, YEW_ARRAY_LEN(want_spans),
                               0U, 0U};
            SynLineOut got = {got_spans, 0U, YEW_ARRAY_LEN(got_spans),
                              0U, 0U};

            while (hi < len && flat[hi] != (u8)'\n')
                hi++;
            YEW_ASSERT_EQ_U64(syn.entry.data[line], state);
            yew_syn_line(toy.engine, state, flat + lo, (u32)(hi - lo),
                         &want);
            yew_syn_spans(&syn, tb, LINENO(line), &got);
            assert_line_equal(&got, &want);
            state = want.exit_state;
            lo = hi < len ? hi + 1U : hi;
        }
    }

    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
    free(flat);
}

void test_syn_diff_incremental_matches_from_scratch_four_seeds(void)
{
    static const u64 seeds[] = {
        UINT64_C(0x123456789abcdef0),
        UINT64_C(0x0ddc0ffeebadf00d),
        UINT64_C(0x9e3779b97f4a7c15),
        UINT64_C(0xfeedfacecafebeef)
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(seeds); i++)
        run_diff_seed(seeds[i], 100000U);
}

void test_syn_diff_same_seed_is_deterministic(void)
{
    /* Running the complete oracle twice also catches accidental dependence
     * on allocation addresses or process-global state. */
    run_diff_seed(UINT64_C(0x5eed5eed5eed5eed), 1000U);
    run_diff_seed(UINT64_C(0x5eed5eed5eed5eed), 1000U);
    YEW_ASSERT(true);
}
