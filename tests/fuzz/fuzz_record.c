/* Sprint 35: arbitrary recorder events must always emit parseable Fletch. */
#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/parse.h"
#include "fl/record.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct DiagCount {
    u32 errors;
} DiagCount;

static void count_diag(void *ctx, FlDiagLevel level, FlSpan span,
                       const char *message, const char *rendered)
{
    DiagCount *count = ctx;

    (void)span;
    (void)message;
    (void)rendered;
    if (level == FL_DIAG_ERROR)
        count->errors++;
}

static u32 read_u32(const u8 *data, size_t len, size_t at)
{
    u32 value = 0U;
    size_t i;

    for (i = 0U; i < 4U && at + i < len; i++)
        value |= (u32)data[at + i] << (i * 8U);
    return value;
}

static bool check_record(const u8 *data, size_t len, char *why,
                         size_t why_cap)
{
    enum { EVENT_BYTES = 24U, EVENT_MAX = 256U };
    Arena arena;
    Interner in;
    DiagCtx dc;
    DiagCount count = {0};
    FlProgram program;
    Bytebuf source;
    Rec rec;
    Ed ed;
    size_t nevents;
    size_t i;

    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed)) {
        sag_ed_free(&ed);
        (void)snprintf(why, why_cap, "scratch editor initialization failed");
        return false;
    }
    sag_record_init(&rec);
    rec.reg = (u8)'a';
    rec.mode_at_start = len == 0U ? (u8)SAG_MODE_L :
        (u8)(data[0] % (u8)SAG_MODE__N);
    bytebuf_append(&rec.blob, data, len);
    nevents = (len + EVENT_BYTES - 1U) / EVENT_BYTES;
    if (nevents > EVENT_MAX)
        nevents = EVENT_MAX;
    for (i = 0U; i < nevents; i++) {
        size_t at = i * EVENT_BYTES;
        u32 raw_cmd = read_u32(data, len, at);
        u32 raw_slice = read_u32(data, len, at + 8U);
        RecEvent event = {0};

        event.cmd.v = sag_cmd_count() == 0U ? 0U :
            1U + raw_cmd % sag_cmd_count();
        event.count = 1U + read_u32(data, len, at + 4U) % 100U;
        event.count_given = at + 1U < len && (data[at + 1U] & 1U) != 0U;
        event.iarg = (i64)(i32)read_u32(data, len, at + 12U);
        event.bang = at + 4U < len && (data[at + 4U] & 1U) != 0U;
        event.range_kind = at + 5U < len
                               ? data[at + 5U] %
                                     ((u8)SAG_REC_RANGE_SPAN + 1U)
                               : (u8)SAG_REC_RANGE_NONE;
        event.range_given = at + 6U < len &&
                            (data[at + 6U] & 1U) != 0U;
        event.range_lo = read_u32(data, len, at + 16U);
        event.range_hi = event.range_lo +
                         read_u32(data, len, at + 20U);
        event.mode = at + 2U < len ? data[at + 2U] % (u8)SAG_MODE__N : 0U;
        event.src = at + 3U < len ? data[at + 3U] %
            ((u8)SAG_SRC_TEST + 1U) : (u8)SAG_SRC_KEY;
        if (len != 0U) {
            event.sarg_at = raw_slice % (u32)(len + 1U);
            event.sarg_len = read_u32(data, len, at + 12U) %
                (u32)(len - event.sarg_at + 1U);
        }
        RecEventVec_push(&rec.ev, event);
    }

    bytebuf_init(&source);
    sag_record_emit(&rec, &ed, &source);
    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, count_diag, &count);
    (void)fl_diag_add_file(&dc, "fuzz-record.fl",
                           (const char *)source.data, source.len);
    program = fl_parse(&arena, &dc, &in, (const char *)source.data,
                       source.len, 0U);
    if (count.errors != 0U || program.had_error || program.incomplete) {
        (void)snprintf(why, why_cap,
                       "emitter produced unparseable source (%u diagnostics)",
                       (unsigned)count.errors);
        bytebuf_free(&source);
        sag_record_free(&rec);
        sag_ed_free(&ed);
        interner_free(&in);
        arena_free_all(&arena);
        return false;
    }
    bytebuf_free(&source);
    sag_record_free(&rec);
    sag_ed_free(&ed);
    interner_free(&in);
    arena_free_all(&arena);
    return true;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_record",
                         "tests/fuzz/corpus/record", check_record);
}
