/*
 * Sprint 29 deliverable 6: the Fletch lexer fuzzer.
 *
 * The property that matters is FORWARD PROGRESS.  A lexer hangs when a
 * token consumes zero bytes and the scanner returns to the same offset
 * forever; the shape is easy to write by accident in an error path,
 * where the natural thing is to report and return without advancing.
 * Asserting it per iteration turns a hang -- which the watchdog can
 * only report as "expired", with no clue where -- into a message
 * naming the offset and the kind that stalled.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "fl/lex.h"
#include "util/arena.h"
#include "util/intern.h"

enum { FL_FUZZ_MAX_INPUT = 1024U * 1024U };

/* Swallow diagnostics: a fuzzer feeding random bytes produces an
 * enormous number of them and they are not what is under test here. */
static void quiet(void *ctx, FlDiagLevel level, FlSpan sp,
                  const char *msg, const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    (void)msg;
    (void)rendered;
}

static bool check_fl_lex(const u8 *data, size_t len, char *why,
                         size_t why_cap)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlLexer lx;
    size_t prev = 0U;
    u64 steps = 0U;
    bool ok = true;

    if (len > FL_FUZZ_MAX_INPUT)
        len = FL_FUZZ_MAX_INPUT;
    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, quiet, NULL);
    (void)fl_diag_add_file(&dc, "fuzz.fl", (const char *)data, len);
    fl_lex_init(&lx, &arena, &dc, &in, (const char *)data, len, 0U);

    for (;;) {
        FlTok t = fl_lex_next(&lx);

        if (t.kind == FL_T_EOF)
            break;
        if (lx.at <= prev) {
            (void)snprintf(why, why_cap,
                           "lexer made no progress at offset %zu "
                           "(kind %d)", prev, (int)t.kind);
            ok = false;
            break;
        }
        prev = lx.at;
        /*
         * One token per byte is the ceiling, so anything past it means
         * the loop is not bounded by the input at all -- a different
         * failure from a stalled offset, and worth its own message.
         */
        if (++steps > (u64)len + 2U) {
            (void)snprintf(why, why_cap,
                           "lexer produced %llu tokens for %zu bytes",
                           (unsigned long long)steps, len);
            ok = false;
            break;
        }
    }
    /* EOF must be sticky: a caller that reads one token too many has to
     * get EOF again rather than fall off the end of the buffer. */
    if (ok) {
        FlTok again = fl_lex_next(&lx);

        if (again.kind != FL_T_EOF) {
            (void)snprintf(why, why_cap,
                           "lexer yielded kind %d after EOF",
                           (int)again.kind);
            ok = false;
        }
    }
    interner_free(&in);
    arena_free_all(&arena);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_fl_lex",
                         "tests/fuzz/corpus/fl_parse", check_fl_lex);
}
