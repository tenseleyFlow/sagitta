/*
 * Sprint 32 §8: the VM fuzzer.
 *
 * SOURCE IN, not bytecode.  §8's decision is that no code path can
 * deliver foreign bytecode -- there is no loader, no .flc, no cache --
 * so fuzzing random bytecode would exercise a door that does not
 * exist.  Every byte an adversary controls passes through the parser
 * first, and that is where this points.
 *
 * The per-input invariants, all of them checked rather than assumed:
 *
 *   - the run ends in exactly one outcome: a value, or a raise whose
 *     kind is one of §9's twelve;
 *   - AFTER AN UNCAUGHT RAISE THE VM IS REUSABLE -- sp back at the
 *     stack floor, no frames, no handlers.  The unwinder leaving one
 *     behind is invisible until the next program returns into a dead
 *     chunk, which is exactly the bug Sprint 31 shipped for a while;
 *   - the step limit is what stops a hang, and the alarm is the
 *     backstop for a hang somewhere the step limit cannot see.
 */
#define _POSIX_C_SOURCE 200809L

#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fl/compile.h"
#include "fl/parse.h"
#include "fl/std.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

enum {
    FL_VM_FUZZ_MAX_INPUT = 64U * 1024U,
    /* Enough for any honest program in the corpus, small enough that a
     * pathological loop is cut off in milliseconds. */
    FL_VM_FUZZ_STEPS = 5000000
};

/* §9.1, closed for 1.0.  Amendment A1 added "limit". */
static bool known_kind(const char *k)
{
    static const char *const KINDS[] = {
        "type", "arity", "name", "index", "key", "div",
        "capability", "io", "import", "motion", "user", "limit"
    };
    size_t i;

    for (i = 0U; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        if (strcmp(k, KINDS[i]) == 0)
            return true;
    }
    return false;
}

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

static void err_kind(FlVm *vm, FlValue err, char *out, size_t cap)
{
    FlValue got = FL_NIL_V;
    FlStr *k;

    out[0] = '\0';
    if (err.t != (u8)FL_MAP)
        return;
    k = fl_str_new(vm, "kind", 4U);
    if (!fl_map_get((FlMap *)err.as.o, FL_OBJ_V(FL_STR, k), &got))
        return;
    if (got.t != (u8)FL_STR)
        return;
    (void)snprintf(out, cap, "%.*s", (int)((const FlStr *)got.as.o)->len,
                   ((const FlStr *)got.as.o)->b);
}

static bool check_fl_vm(const u8 *data, size_t len, char *why, size_t why_cap)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    Counter counter;
    FlProgram p;
    FlFn *fn;
    FlValue out = FL_NIL_V;
    FlOrigin origin;
    bool ok = true;

    if (len > (size_t)FL_VM_FUZZ_MAX_INPUT)
        len = (size_t)FL_VM_FUZZ_MAX_INPUT;
    /* The backstop for a hang the step limit cannot see -- a loop
     * inside a native, say.  Rearmed per input. */
    (void)alarm(5U);

    (void)memset(&counter, 0, sizeof(counter));
    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, count_diag, &counter);
    fl_vm_init(&vm, &arena, &in, &dc);
    fl_std_register(&vm);
    fl_vm_set_step_limit(&vm, (u64)FL_VM_FUZZ_STEPS);
    /*
     * DENY-ALL.  Zero capabilities means io is unreachable, so the
     * campaign cannot write a file, and the null host means a motion
     * block raises rather than touching an editor that is not there.
     */
    origin.kind = (u8)FL_ORIGIN_CLI;
    origin.path_id = 0U;
    origin.caps = 0U;
    vm.root_origin = origin;
    /* GC stress on one input in sixteen: a native building an
     * intermediate without protection fails there and nowhere else. */
    if (len != 0U && (data[0] & 15U) == 0U)
        vm.gc.stress = true;

    (void)fl_diag_add_file(&dc, "fuzz.fl", (const char *)data, len);
    p = fl_parse(&arena, &dc, &in, (const char *)data, len, 0U);
    /* The cap is FL_PARSE_MAX_ERRORS PLUS the one that says it gave
     * up, which is what s29 pins and what fuzz_fl_parse asserts.  This
     * read the bare cap and a nest of bad `@[` blocks tripped it at 21
     * -- the parser was right and the bound was wrong. */
    if (counter.n > (u32)FL_PARSE_MAX_ERRORS + 1U) {
        (void)snprintf(why, why_cap, "%u diagnostics, cap is %d + 1",
                       (unsigned)counter.n, FL_PARSE_MAX_ERRORS);
        ok = false;
        goto done;
    }
    if (p.had_error || p.incomplete)
        goto done;                 /* a parse failure is a fine outcome */
    fn = fl_compile(&vm, &dc, &p, 0U, origin);
    if (fn == NULL)
        goto done;                 /* so is a compile failure */
    if (!fl_vm_run(&vm, fn, &out)) {
        char kind[64];

        err_kind(&vm, out, kind, sizeof(kind));
        if (kind[0] == '\0') {
            (void)snprintf(why, why_cap, "raised without an error value");
            ok = false;
            goto done;
        }
        if (!known_kind(kind)) {
            (void)snprintf(why, why_cap, "raised unknown kind '%s'", kind);
            ok = false;
            goto done;
        }
        /*
         * THE VM IS REUSABLE.  An unwinder that left a frame or a
         * handler behind is invisible until the next program returns
         * into a dead chunk -- which is a bug this project has actually
         * shipped, so it is checked on every input rather than trusted.
         */
        if (vm.sp != vm.stack || vm.nframes != 0U || vm.nhandlers != 0U) {
            (void)snprintf(why, why_cap,
                           "after an uncaught raise: sp+%ld frames=%u "
                           "handlers=%u", (long)(vm.sp - vm.stack),
                           (unsigned)vm.nframes, (unsigned)vm.nhandlers);
            ok = false;
        }
    }
done:
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
    (void)alarm(0U);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_fl_vm",
                         "tests/fuzz/corpus/fl_vm", check_fl_vm);
}
