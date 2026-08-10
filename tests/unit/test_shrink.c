#include "harness.h"

#include "../fuzz/shrink.h"

void test_trace_roundtrip(void)
{
    static const char source[] =
        "# yew textbuf trace v1\n"
        "# seed=0x9e3779b97f4a7c15 mix=typing base=empty\n"
        "ins 0 5 68656c6c6f\nins 5 1 0a\ndel 2 4\nline_of 3\n"
        "undo\nsnap 0\nundo_to 7\nrelease 0\nredo\ncheck\n";
    Trace first;
    Trace second;
    Bytebuf written;
    Bytebuf rewritten;
    Bytebuf snippet;
    char why[128];

    trace_init(&first);
    trace_init(&second);
    bytebuf_init(&written);
    bytebuf_init(&rewritten);
    bytebuf_init(&snippet);
    YEW_ASSERT(trace_parse(&first, (const u8 *)source, strlen(source),
                           why, sizeof(why)));
    YEW_ASSERT_EQ_U64(first.len, 10U);
    YEW_ASSERT_EQ_U64(first.seed, UINT64_C(0x9e3779b97f4a7c15));
    YEW_ASSERT_EQ_STR(first.mix, "typing");
    YEW_ASSERT_EQ_STR(first.base, "empty");
    YEW_ASSERT_EQ_U64(first.ops[0].payload.len, 5U);
    YEW_ASSERT_EQ_MEM(first.ops[0].payload.data, "hello", 5U);
    trace_write(&first, &written);
    YEW_ASSERT(trace_parse(&second, written.data, written.len,
                           why, sizeof(why)));
    trace_write(&second, &rewritten);
    YEW_ASSERT_EQ_U64(written.len, rewritten.len);
    YEW_ASSERT_EQ_MEM(written.data, rewritten.data, written.len);
    YEW_ASSERT_EQ_U64(second.ops[2].kind, TRACE_DEL);
    YEW_ASSERT_EQ_U64(second.ops[2].a, 2U);
    YEW_ASSERT_EQ_U64(second.ops[2].b, 4U);
    trace_write_c_snippet(&second, &snippet);
    bytebuf_push_u8(&snippet, 0U);
    YEW_ASSERT(strstr((const char *)snippet.data, "yew_textbuf_insert") != NULL);
    YEW_ASSERT(strstr((const char *)snippet.data, "yew_textbuf_delete") != NULL);
    YEW_ASSERT(strstr((const char *)snippet.data, "yew_textbuf_check") != NULL);
    bytebuf_free(&snippet);
    bytebuf_free(&rewritten);
    bytebuf_free(&written);
    trace_free(&second);
    trace_free(&first);
}

typedef struct {
    u64 target_ordinal;
    u32 check_id;
} SyntheticFailure;

static bool synthetic_probe(const Trace *trace, TraceFailure *failure,
                            void *context)
{
    const SyntheticFailure *synthetic = context;
    size_t inserts = 0U;
    size_t i;

    for (i = 0U; i < trace->len; i++) {
        const TraceOp *op = &trace->ops[i];
        if (op->kind == TRACE_INS)
            inserts++;
        if (op->ordinal == synthetic->target_ordinal &&
            op->kind == TRACE_DEL && op->b >= op->a &&
            op->b - op->a >= 7U && inserts >= 3U) {
            failure->kind = TRACE_FAILURE_CHECK;
            failure->first_op = op->ordinal;
            failure->check_id = synthetic->check_id;
            return true;
        }
    }
    return false;
}

void test_shrink_synthetic_5000(void)
{
    Trace trace;
    SyntheticFailure synthetic = {4000U, 77U};
    FailurePred pred;
    TraceFailure replay;
    u32 replays = 0U;
    size_t i;

    trace_init(&trace);
    for (i = 0U; i < 5000U; i++) {
        if (i == 100U || i == 2000U || i == 3999U)
            YEW_ASSERT(trace_push(&trace, TRACE_INS, (u64)i, 1U,
                                  (const u8 *)"z", 1U));
        else if (i == 4000U)
            YEW_ASSERT(trace_push(&trace, TRACE_DEL, 91U, 123U, NULL, 0U));
        else
            YEW_ASSERT(trace_push(&trace, TRACE_LINE_OF, (u64)i, 0U,
                                  NULL, 0U));
    }
    memset(&pred, 0, sizeof(pred));
    pred.probe = synthetic_probe;
    pred.context = &synthetic;
    pred.target.kind = TRACE_FAILURE_CHECK;
    pred.target.first_op = synthetic.target_ordinal;
    pred.target.check_id = synthetic.check_id;
    pred.replays_out = &replays;
    YEW_ASSERT(synthetic_probe(&trace, &replay, &synthetic));
    YEW_ASSERT(trace_failure_equal(&replay, &pred.target));
    YEW_ASSERT(shrink(&trace, pred));
    YEW_ASSERT(trace.len <= 5U);
    YEW_ASSERT(trace.len >= 4U);
    memset(&replay, 0, sizeof(replay));
    YEW_ASSERT(synthetic_probe(&trace, &replay, &synthetic));
    YEW_ASSERT(trace_failure_equal(&replay, &pred.target));
    YEW_ASSERT(replays <= 5000U);
    trace_free(&trace);
}
