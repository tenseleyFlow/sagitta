/*
 * Sprint 29 deliverable 6: the Fletch parser fuzzer.
 *
 * Runs both entry points over the same bytes and checks the properties
 * that are cheap to state and expensive to discover in the field: the
 * error cap holds, the arena does not run away, and the REPL's two
 * flags are never both set.
 *
 * fl_parse_literal is fuzzed beside fl_parse because §12.2 states a
 * SECURITY property -- a pure-literal file grants nothing and runs
 * nothing -- and §12.1 makes it checkable.  A crash there is a
 * different class of bug from a crash in the full parser: state files
 * are loaded from repositories checked out from strangers.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl/parse.h"
#include "util/arena.h"
#include "util/intern.h"

enum {
    FL_FUZZ_MAX_INPUT = 1024U * 1024U,
    /*
     * Arena guard.  Arena exposes no total, but blocks double, so
     * `next_block_size` is within a factor of two of everything
     * allocated -- close enough to catch a parser that turns a
     * kilobyte of input into a gigabyte of nodes, which is the failure
     * this is for.
     */
    FL_FUZZ_ARENA_LIMIT = 64U * 1024U * 1024U
};

typedef struct Counter {
    u32 n;
} Counter;

static void count_diag(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    Counter *c = ctx;

    (void)sp;
    (void)msg;
    (void)rendered;
    if (level == FL_DIAG_ERROR)
        c->n++;
}

static bool check_one(const u8 *data, size_t len, char *why,
                      size_t why_cap)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    Counter counter;
    FlProgram prog;
    bool ok = true;

    if (len > FL_FUZZ_MAX_INPUT)
        len = FL_FUZZ_MAX_INPUT;

    (void)memset(&counter, 0, sizeof(counter));
    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, count_diag, &counter);
    (void)fl_diag_add_file(&dc, "fuzz.fl", (const char *)data, len);

    prog = fl_parse(&arena, &dc, &in, (const char *)data, len, 0U);

    /*
     * The cap is FL_PARSE_MAX_ERRORS plus the one that says it gave up.
     * Without a cap a megabyte of garbage is a megabyte of diagnostics,
     * and the useful first one scrolls away.
     */
    if (counter.n > (u32)FL_PARSE_MAX_ERRORS + 1U) {
        (void)snprintf(why, why_cap, "%u diagnostics exceeds the cap of %d",
                       (unsigned)counter.n, FL_PARSE_MAX_ERRORS);
        ok = false;
    }
    /*
     * Sprint 32 reads `incomplete` as "prompt for another line" and
     * `had_error` as "print this and stop".  Both at once is a REPL
     * that waits forever for a closing token which cannot fix the
     * mistake it already found.
     */
    if (ok && prog.incomplete && prog.had_error) {
        (void)snprintf(why, why_cap,
                       "incomplete and had_error are both set");
        ok = false;
    }
    /* Every statement the program claims must actually be there. */
    if (ok && prog.n != 0U && prog.stmts == NULL) {
        (void)snprintf(why, why_cap, "%u statements with a NULL array",
                       (unsigned)prog.n);
        ok = false;
    }
    if (ok && arena.next_block_size > (size_t)FL_FUZZ_ARENA_LIMIT) {
        (void)snprintf(why, why_cap,
                       "arena grew past %d MiB on %zu bytes of input",
                       FL_FUZZ_ARENA_LIMIT / (1024 * 1024), len);
        ok = false;
    }

    if (ok) {
        /*
         * The same bytes through the pure-literal door.  Its contract
         * is total: any input either yields a value or reports and
         * returns NULL -- never both, and never neither.
         */
        Counter pl;
        FlNode *v;

        (void)memset(&pl, 0, sizeof(pl));
        fl_diag_set_sink(&dc, count_diag, &pl);
        v = fl_parse_literal(&arena, &dc, &in, (const char *)data, len, 0U);
        if (v != NULL && pl.n != 0U) {
            (void)snprintf(why, why_cap,
                           "pure-literal returned a value AND reported "
                           "%u errors", (unsigned)pl.n);
            ok = false;
        }
        if (ok && v == NULL && pl.n == 0U && len != 0U) {
            (void)snprintf(why, why_cap,
                           "pure-literal rejected %zu bytes silently", len);
            ok = false;
        }
        /* §12.1: no production may reach a call, an identifier used as
         * a value, a function literal or a motion block.  The node
         * kinds are the checkable form of that list. */
        if (ok && v != NULL) {
            static const FlAstKind banned[] = {
                FL_A_CALL, FL_A_IDENT, FL_A_FN_EXPR, FL_A_FN,
                FL_A_MOTION_BLOCK, FL_A_MOTION, FL_A_BINOP, FL_A_UNOP,
                FL_A_IMPORT, FL_A_ASSIGN, FL_A_INDEX, FL_A_FIELD
            };
            size_t i;

            for (i = 0U; i < sizeof(banned) / sizeof(banned[0]); i++) {
                if ((FlAstKind)v->kind == banned[i]) {
                    (void)snprintf(why, why_cap,
                                   "pure-literal produced banned node "
                                   "kind %d", (int)v->kind);
                    ok = false;
                    break;
                }
            }
        }
    }

    interner_free(&in);
    arena_free_all(&arena);
    return ok;
}

static bool check_fl_parse(const u8 *data, size_t len, char *why,
                           size_t why_cap)
{
    static const u8 prefix[] =
        "{name:\"";
    static const u8 suffix[] =
        "\",version:\"1.0.0\",api:1,entry:\"src/main.fl\","
        "capabilities:[],events:[]}";
    u8 *manifest;
    size_t framed_len;
    bool ok;

    if (!check_one(data, len, why, why_cap))
        return false;
    if (len > FL_FUZZ_MAX_INPUT)
        len = FL_FUZZ_MAX_INPUT;
    framed_len = (sizeof(prefix) - 1U) + len + (sizeof(suffix) - 1U);
    manifest = malloc(framed_len);
    if (manifest == NULL) {
        (void)snprintf(why, why_cap, "cannot allocate manifest frame");
        return false;
    }
    (void)memcpy(manifest, prefix, sizeof(prefix) - 1U);
    (void)memcpy(manifest + sizeof(prefix) - 1U, data, len);
    (void)memcpy(manifest + sizeof(prefix) - 1U + len, suffix,
                 sizeof(suffix) - 1U);
    ok = check_one(manifest, framed_len, why, why_cap);
    free(manifest);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_fl_parse",
                         "tests/fuzz/corpus/fl_parse", check_fl_parse);
}
