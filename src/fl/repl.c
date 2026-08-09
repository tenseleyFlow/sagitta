/* Sprint 32 §2-§5: the interactive `sag fl` prompt. */
#include "fl/repl.h"

#include <stdio.h>

#include "fl/parse.h"
#include "fl/std.h"
#include "util/base.h"

void sag_fl_print_result(FlVm *vm, FlValue v, Bytebuf *out)
{
    Bytebuf bb;

    if (v.t == (u8)FL_NIL)
        return;
    bytebuf_init(&bb);
    if (!fl_fmt_repl(vm, &bb, v, (u32)FL_REPL_MAX_DEPTH)) {
        /* fl_fmt_repl elides rather than refusing, so the only way here
         * is a limit the renderer itself hit; say so instead of
         * printing half a value. */
        bytebuf_free(&bb);
        bytebuf_append(out, "<unprintable>\n", 14U);
        return;
    }
    if (bb.len > (size_t)FL_REPL_MAX_BYTES) {
        bytebuf_append(out, bb.data, (size_t)FL_REPL_MAX_BYTES);
        bytebuf_printf(out, "\xe2\x80\xa6 (%zu more bytes)",
                       bb.len - (size_t)FL_REPL_MAX_BYTES);
    } else {
        bytebuf_append(out, bb.data, bb.len);
    }
    bytebuf_push_u8(out, (u8)'\n');
    bytebuf_free(&bb);
}

/* Counts and discards: the trial parse must be silent. */
static void quiet_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    u32 *n = ctx;

    (void)sp;
    (void)msg;
    (void)rendered;
    if (level == FL_DIAG_ERROR)
        (*n)++;
}

FlReplVerdict sag_fl_repl_classify(Arena *arena, Interner *in,
                                   const char *text, size_t len)
{
    DiagCtx dc;
    FlProgram p;
    u32 nerr = 0U;

    /*
     * A THROWAWAY context, not the caller's with its sink swapped:
     * fl_diag_add_file appends, and a prompt that classified forty
     * lines would fill the caller's file table and trip a SAG_BUG about
     * source files -- a very confusing way to learn that someone typed
     * a lot.
     */
    fl_diag_init(&dc, arena);
    fl_diag_set_sink(&dc, quiet_sink, &nerr);
    (void)fl_diag_add_file(&dc, "<repl>", text, len);
    p = fl_parse(arena, &dc, in, text, len, 0U);
    /* had_error FIRST: an error inside an open bracket sets both, and
     * reading `incomplete` first waits forever for a fixed `)`. */
    if (p.had_error || nerr != 0U)
        return FL_REPL_ERROR;
    if (p.incomplete)
        return FL_REPL_CONTINUE;
    return FL_REPL_RUN;
}

int sag_fl_repl(void)
{
    /*
     * No silent stub: the prompt is Sprint 32 §2-§4 and is not built in
     * this tree yet.  Naming the section rather than returning a usage
     * message keeps `sag fl` on a tty from looking like a typo.
     */
    (void)fprintf(stderr,
                  "sagitta: the interactive REPL lands in Sprint 32 §2;\n"
                  "         `sag fl FILE`, `-e`, and stdin all work today.\n");
    return SAG_EXIT_ERR;
}
