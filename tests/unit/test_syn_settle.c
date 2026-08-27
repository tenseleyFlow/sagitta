#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "syn/engine.h"
#include "text/piece.h"

#include "syn_toy.h"

typedef struct FakeSynClock {
    i64 now;
    i64 step;
} FakeSynClock;

static i64 fake_syn_clock(void *ctx)
{
    FakeSynClock *clock = ctx;

    clock->now += clock->step;
    return clock->now;
}

static TextBuf *repeat_lines(const char *line, u32 count)
{
    size_t line_len = strlen(line);
    size_t total = line_len * count;
    u8 *bytes = malloc(total == 0U ? 1U : total);
    TextBuf *tb;
    u32 i;

    YEW_ASSERT_NOT_NULL(bytes);
    for (i = 0U; i < count; i++)
        (void)memcpy(bytes + (size_t)i * line_len, line, line_len);
    tb = yew_textbuf_from_bytes(bytes, total);
    free(bytes);
    return tb;
}

static void settle_all(SynBuf *syn, const TextBuf *tb, i64 budget)
{
    SynSettleReport report;
    u32 calls = 0U;

    do {
        yew_syn_settle(syn, tb, LINENO(0U), LINENO(1U), budget, &report);
        YEW_ASSERT(++calls < 100000U);
    } while (!report.fixpoint);
}

void test_syn_settle_plain_edit_reaches_one_line_fixpoint(void)
{
    SynToy toy;
    SynBuf syn;
    TextBuf *tb = repeat_lines("plain text\n", 100000U);
    SynSettleReport report;

    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_attach(&syn, 1U, tb);
    settle_all(&syn, tb, INT64_MAX);
    yew_textbuf_insert(tb, BYTEOFF(3U), (const u8 *)"x", 1U);
    yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
    yew_syn_engine_reset_counters(toy.engine);
    yew_syn_settle(&syn, tb, LINENO(0U), LINENO(24U),
                   YEW_SYN_FRAME_BUDGET_US, &report);
    YEW_ASSERT(report.fixpoint);
    YEW_ASSERT(report.lines <= 2U);
    YEW_ASSERT(yew_syn_engine_line_calls(toy.engine) <= 2U);
    YEW_ASSERT_EQ_U64(syn.settled_to.v, yew_textbuf_line_count(tb));
    YEW_ASSERT(!syn.settling);
    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
}

void test_syn_settle_comment_wave_obeys_each_budget(void)
{
    SynToy toy;
    SynBuf syn;
    TextBuf *tb = repeat_lines("plain\n", 1000000U);
    FakeSynClock clock = {0, 1};
    SynSettleReport report;
    u32 calls = 0U;

    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_buf_set_clock(&syn, fake_syn_clock, &clock);
    yew_syn_attach(&syn, 1U, tb);
    settle_all(&syn, tb, INT64_MAX);
    yew_textbuf_insert(tb, BYTEOFF(0U), (const u8 *)"/*", 2U);
    yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
    do {
        yew_syn_settle(&syn, tb, LINENO(100U), LINENO(124U), 8, &report);
        YEW_ASSERT(report.us <= 8U);
        YEW_ASSERT(++calls < 10000U);
        if (calls == 1U) {
            SynSpan spans[8];
            SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
            yew_syn_spans(&syn, tb, LINENO(100U), &out);
            YEW_ASSERT(out.n > 0U);
            YEW_ASSERT_EQ_U64(spans[0].attr, YEW_ATTR_COMMENT);
        }
    } while (!report.fixpoint);
    YEW_ASSERT_EQ_U64(syn.settled_to.v, yew_textbuf_line_count(tb));
    YEW_ASSERT(!syn.settling);
    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
}

void test_syn_settle_speculative_view_is_corrected_once(void)
{
    SynToy toy;
    SynBuf syn;
    TextBuf *tb = repeat_lines("plain\n", 20000U);
    FakeSynClock clock = {0, 2};
    SynSettleReport report;
    u64 before;
    u32 calls = 0U;

    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_buf_set_clock(&syn, fake_syn_clock, &clock);
    yew_syn_attach(&syn, 1U, tb);
    settle_all(&syn, tb, INT64_MAX);
    yew_textbuf_insert(tb, BYTEOFF(0U), (const u8 *)"/*", 2U);
    yew_syn_edit(&syn, LINENO(0U), 0U, 0U);
    before = syn.provisional_corrections;
    do {
        yew_syn_settle(&syn, tb, LINENO(10000U), LINENO(10024U), 1,
                       &report);
        YEW_ASSERT(++calls < 10000U);
    } while (!report.fixpoint);
    YEW_ASSERT_EQ_U64(syn.provisional_corrections, before + 1U);
    YEW_ASSERT(!syn.spec_valid);
    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
}

void test_syn_settle_indicator_waits_250ms_and_degraded_wins(void)
{
    SynBuf syn;
    FakeSynClock clock = {1000000, 0};
    char status[128];

    yew_syn_buf_init(&syn);
    yew_syn_buf_set_clock(&syn, fake_syn_clock, &clock);
    syn.settling = true;
    syn.edit_us = clock.now;
    YEW_ASSERT(!yew_syn_status_visible(&syn));
    clock.now += (i64)YEW_SYN_SETTLING_MS * 1000 - 1;
    YEW_ASSERT(!yew_syn_status_visible(&syn));
    clock.now++;
    YEW_ASSERT(yew_syn_status_visible(&syn));
    yew_syn_status(&syn, 999U, status, sizeof(status));
    YEW_ASSERT(strstr(status, "settled") != NULL);
    YEW_ASSERT(strstr(status, "degraded=no") != NULL);
    syn.degraded = true;
    syn.settling = false;
    YEW_ASSERT(yew_syn_status_visible(&syn));
    yew_syn_status(&syn, 999U, status, sizeof(status));
    YEW_ASSERT(strstr(status, "degraded=yes") != NULL);
    yew_syn_detach(&syn);
}

void test_syn_spans_cache_avoids_unchanged_rehighlight(void)
{
    char row[256];
    SynToy toy;
    SynBuf syn;
    TextBuf *tb;
    SynSpan spans[128];
    SynLineOut out;
    u32 line;
    u32 at = 0U;

    /* More than 8,192 spans across 200 rows: a small shared slab would
     * evict the first rows before the unchanged redraw reached them. */
    for (line = 0U; line < 48U; line++) {
        row[at++] = 'i';
        row[at++] = 'f';
        row[at++] = '+';
    }
    row[at++] = '\n';
    row[at] = '\0';
    tb = repeat_lines(row, 300U);

    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_attach(&syn, 1U, tb);
    settle_all(&syn, tb, INT64_MAX);
    out = (SynLineOut){spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};
    yew_syn_engine_reset_counters(toy.engine);
    yew_syn_spans(&syn, tb, LINENO(100U), &out);
    YEW_ASSERT(out.n > 40U);
    YEW_ASSERT_EQ_U64(yew_syn_engine_line_calls(toy.engine), 0U);
    for (line = 0U; line < 200U; line++) {
        out.n = 0U;
        yew_syn_spans(&syn, tb, LINENO(line), &out);
        YEW_ASSERT(out.n > 40U);
    }
    yew_syn_engine_reset_counters(toy.engine);
    for (line = 0U; line < 200U; line++) {
        out.n = 0U;
        yew_syn_spans(&syn, tb, LINENO(line), &out);
    }
    YEW_ASSERT_EQ_U64(yew_syn_engine_line_calls(toy.engine), 0U);
    out.n = 0U;
    yew_syn_spans(&syn, tb, LINENO(200U), &out);
    YEW_ASSERT_EQ_U64(yew_syn_engine_line_calls(toy.engine), 1U);
    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
}

void test_syn_settle_repeated_edit_preserves_unfinished_unknown_wave(void)
{
    static const u8 opening[] = "/*\n\n\n\n\n\n\n\n\n\n";
    SynToy toy;
    SynBuf syn;
    TextBuf *tb = repeat_lines("plain\n", 2000U);
    FakeSynClock clock = {0, 2};
    SynSettleReport report;
    u32 state = YEW_SYN_STATE_ROOT;
    u64 line;

    syn_toy_init(&toy);
    yew_syn_buf_init(&syn);
    yew_syn_buf_bind(&syn, toy.engine);
    yew_syn_buf_set_clock(&syn, fake_syn_clock, &clock);
    yew_syn_attach(&syn, 1U, tb);
    settle_all(&syn, tb, INT64_MAX);

    yew_textbuf_insert(tb, BYTEOFF(0U), opening, sizeof(opening) - 1U);
    yew_syn_edit(&syn, LINENO(0U), 0U, 10U);
    yew_syn_settle(&syn, tb, LINENO(1000U), LINENO(1024U), 1, &report);
    YEW_ASSERT(!report.fixpoint);
    YEW_ASSERT(syn.wave.v > 0U);
    YEW_ASSERT(syn.wave.v < 11U);
    YEW_ASSERT_EQ_U64(syn.entry.data[syn.wave.v + 1U],
                      YEW_SYN_STATE_UNKNOWN);

    {
        ByteOff at = yew_textbuf_line_start(tb, LINENO(500U));
        yew_textbuf_insert(tb, at, (const u8 *)"x", 1U);
        yew_syn_edit(&syn, LINENO(500U), 0U, 0U);
    }
    YEW_ASSERT(syn.wave.v < 11U);
    settle_all(&syn, tb, INT64_MAX);

    for (line = 0U; line < yew_textbuf_line_count(tb); line++) {
        Span span = yew_textbuf_line_span(tb, LINENO(line));
        u64 len = span.hi - span.lo;
        u8 *bytes = malloc(len == 0U ? 1U : (size_t)len);
        TextIter it;
        u64 copied = 0U;
        SynSpan spans[32];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};

        YEW_ASSERT_NOT_NULL(bytes);
        if (len != 0U && yew_textiter_begin(&it, tb, BYTEOFF(span.lo))) {
            do {
                const u8 *chunk;
                u64 n;
                u64 take;
                YEW_ASSERT(yew_textiter_chunk(&it, tb, &chunk, &n));
                take = n < len - copied ? n : len - copied;
                (void)memcpy(bytes + copied, chunk, (size_t)take);
                copied += take;
            } while (copied < len && yew_textiter_advance(&it, tb));
        }
        YEW_ASSERT_EQ_U64(copied, len);
        if (len != 0U && bytes[len - 1U] == (u8)'\n')
            len--;
        YEW_ASSERT_EQ_U64(syn.entry.data[line], state);
        yew_syn_line(toy.engine, state, bytes, (u32)len, &out);
        state = out.exit_state;
        free(bytes);
    }
    YEW_ASSERT_EQ_U64(syn.settled_to.v, yew_textbuf_line_count(tb));
    yew_syn_detach(&syn);
    syn_toy_free(&toy);
    yew_textbuf_free(tb);
}

void test_syn_spans_exact_byte_cap_ignores_line_terminator(void)
{
    static const char *suffixes[] = {"\n", "\r\n"};
    SynToy toy;
    u32 variant;

    syn_toy_init(&toy);
    for (variant = 0U; variant < YEW_ARRAY_LEN(suffixes); variant++) {
        size_t suffix_len = strlen(suffixes[variant]);
        size_t total = YEW_SYN_LINE_BYTE_CAP + suffix_len;
        u8 *bytes = malloc(total);
        TextBuf *tb;
        SynBuf syn;
        SynSpan spans[16];
        SynLineOut out = {spans, 0U, YEW_ARRAY_LEN(spans), 0U, 0U};

        YEW_ASSERT_NOT_NULL(bytes);
        (void)memset(bytes, 'x', YEW_SYN_LINE_BYTE_CAP);
        (void)memcpy(bytes + YEW_SYN_LINE_BYTE_CAP, suffixes[variant],
                     suffix_len);
        tb = yew_textbuf_from_bytes(bytes, total);
        free(bytes);
        yew_syn_buf_init(&syn);
        yew_syn_buf_bind(&syn, toy.engine);
        yew_syn_attach(&syn, 1U, tb);
        settle_all(&syn, tb, INT64_MAX);
        yew_syn_spans(&syn, tb, LINENO(0U), &out);
        YEW_ASSERT_EQ_U64(out.stop, YEW_SYN_STOP_OK);
        YEW_ASSERT_EQ_U64(out.exit_state, YEW_SYN_STATE_ROOT);
        yew_syn_detach(&syn);
        yew_textbuf_free(tb);
    }
    syn_toy_free(&toy);
}
