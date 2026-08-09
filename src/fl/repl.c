/* Sprint 32 §2-§5: the interactive `sag fl` prompt. */
#include "fl/repl.h"

#include <stdio.h>

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
