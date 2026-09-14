#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.16: insert-mode comfort must not make typing slower.
 *
 * Two things are measured.  The first is a COUNT, not a time: the syntax
 * query behind auto-pairing heap-allocates a span array and re-lexes the
 * caret's line, so it must be asked once per opener keystroke and never for
 * an ordinary character.  The second is the per-keystroke wall clock over a
 * thousand characters with autoindent and autopair on, against invariant
 * 4's budget.
 */

#include "perf_policy.h"

#include "edit/block.h"
#include "edit/ed.h"
#include "edit/multicursor.h"
#include "syn/defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    TYPED_KEYS = 1000,
    /* Every tenth key is an opener, and the one after it a closer. */
    PAIR_EVERY = 10,
    ROUNDS = 5
};

/* One keystroke including the pair decision, the edit, undo bookkeeping
 * and damage.  Rendering is a separate lane. */
#define KEY_BUDGET_NS INT64_C(2000000)

static i64 now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (i64)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

typedef struct Fixture {
    Ed ed;
    Win win;
    SynEngine *engine;
} Fixture;

static bool fixture_init(Fixture *fx)
{
    static const char seed[] = "int main(void)\n";
    u32 lang = yew_syn_lang_named("c");
    const SynDef *def;
    Cursor cursor;
    SynSettleReport report;

    (void)memset(fx, 0, sizeof(*fx));
    if (lang == YEW_LANG_NONE)
        return false;
    def = yew_syn_def_for(lang);
    if (def == NULL)
        return false;
    yew_syn_buf_init(&fx->ed.buffer.syn);
    fx->ed.buffer.tb = yew_textbuf_from_bytes((const u8 *)seed,
                                              (u64)sizeof(seed) - 1U);
    fx->ed.buffer.tabwidth = 4U;
    fx->ed.buffer.undo = yew_undo_new(fx->ed.buffer.tb);
    fx->ed.buffer.marks = yew_marks_new();
    fx->ed.buffer.lang = "c";
    yew_timers_init(&fx->ed.timers);
    fx->engine = yew_syn_engine_new((SynDef *)def);
    if (fx->engine == NULL)
        return false;
    yew_syn_buf_bind(&fx->ed.buffer.syn, fx->engine);
    yew_syn_attach(&fx->ed.buffer.syn, lang, fx->ed.buffer.tb);
    yew_syn_settle(&fx->ed.buffer.syn, fx->ed.buffer.tb, LINENO(0U),
                   LINENO(yew_textbuf_line_count(fx->ed.buffer.tb)),
                   INT64_MAX, &report);
    cursor.pos = BYTEOFF(yew_textbuf_len(fx->ed.buffer.tb));
    cursor.anchor = cursor.pos;
    cursor.goal_col = (CCol){0U};
    fx->win.buf = &fx->ed.buffer;
    yew_cset_init(&fx->win.cs, cursor);
    fx->ed.win = &fx->win;
    fx->ed.model_ready = true;
    return true;
}

static void fixture_free(Fixture *fx)
{
    yew_msg_clear(&fx->ed);
    yew_timers_free(&fx->ed.timers);
    yew_cset_free(&fx->win.cs);
    yew_syn_detach(&fx->ed.buffer.syn);
    if (fx->engine != NULL)
        yew_syn_engine_free(fx->engine);
    yew_pairs_clear(&fx->ed.buffer);
    yew_marks_free(fx->ed.buffer.marks);
    yew_undo_free(fx->ed.buffer.undo);
    yew_textbuf_free(fx->ed.buffer.tb);
}

static bool type_one(Fixture *fx, CmdId id, const char *byte)
{
    CmdCtx cx = {0};

    cx.win = &fx->win;
    cx.count = 1U;
    cx.sarg = byte;
    cx.sarg_len = 1U;
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(&fx->ed, id, &cx) == YEW_CMD_OK;
}

/* Key i: an opener every PAIR_EVERY keys, its closer next, letters else. */
static const char *key_at(u32 i)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static char one[2] = {0, 0};

    if (i % PAIR_EVERY == 0U)
        return "(";
    if (i % PAIR_EVERY == 1U)
        return ")";
    one[0] = letters[i % (u32)(sizeof(letters) - 1U)];
    return one;
}

static u32 expected_openers(void)
{
    u32 i;
    u32 n = 0U;

    for (i = 0U; i < (u32)TYPED_KEYS; i++) {
        if (i % PAIR_EVERY == 0U)
            n++;
    }
    return n;
}

static bool measure(void)
{
    CmdId id = yew_cmd_lookup("ed.edit.insert.text", 19U);
    i64 worst = 0;
    u64 queries;
    u32 openers = expected_openers();
    int round;

    if (id.v == 0U) {
        (void)fprintf(stderr, "perf-insert: no insert command\n");
        return false;
    }
    for (round = 0; round < ROUNDS; round++) {
        Fixture fx;
        u32 i;

        if (!fixture_init(&fx)) {
            (void)fprintf(stderr, "perf-insert: fixture failed\n");
            return false;
        }
        yew_syn_in_string_or_comment_calls_reset();
        for (i = 0U; i < (u32)TYPED_KEYS; i++) {
            i64 started = now_ns();
            i64 elapsed;

            if (!type_one(&fx, id, key_at(i))) {
                (void)fprintf(stderr, "perf-insert: keystroke %u failed\n",
                              (unsigned)i);
                fixture_free(&fx);
                return false;
            }
            elapsed = now_ns() - started;
            if (elapsed > worst)
                worst = elapsed;
        }
        queries = yew_syn_in_string_or_comment_calls();
        fixture_free(&fx);
        if (queries > (u64)openers) {
            (void)fprintf(stderr,
                          "perf-insert: syntax queries %llu exceed %u "
                          "openers FAIL\n",
                          (unsigned long long)queries, (unsigned)openers);
            return false;
        }
    }
    (void)printf("perf-insert: %d keys, syntax queries %llu <= %u openers "
                 "ok\n", (int)TYPED_KEYS, (unsigned long long)queries,
                 (unsigned)openers);
    (void)printf("perf-insert: worst keystroke %lld ns budget %lld ns%s\n",
                 (long long)worst, (long long)KEY_BUDGET_NS,
                 yew_perf_timing_verdict((uint64_t)(worst > 0 ? worst : 0),
                                         (uint64_t)KEY_BUDGET_NS,
                                         yew_perf_advisory()));
    return !yew_perf_timing_failed((uint64_t)(worst > 0 ? worst : 0),
                                   (uint64_t)KEY_BUDGET_NS,
                                   yew_perf_advisory());
}

static int selftest_policy(void)
{
    const uint64_t budget = (uint64_t)KEY_BUDGET_NS;

    if (yew_perf_timing_failed(budget, budget, false) ||
        !yew_perf_timing_failed(budget + 1U, budget, false) ||
        yew_perf_timing_failed(budget + 1U, budget, true) ||
        !yew_perf_timing_failed(0U, budget, true)) {
        (void)fprintf(stderr, "perf-insert-policy: failed\n");
        return 1;
    }
    (void)printf("perf-insert-policy: strict/advisory/sanity ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest-policy") == 0)
        return selftest_policy();
    if (argc != 1) {
        (void)fprintf(stderr, "usage: %s [--selftest-policy]\n", argv[0]);
        return 2;
    }
    return measure() ? 0 : 1;
}
