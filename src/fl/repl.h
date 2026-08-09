#ifndef SAG_FL_REPL_H
#define SAG_FL_REPL_H

/*
 * Sprint 32 §2-§5: the interactive `sag fl` prompt.
 *
 * Result printing lives here rather than in the CLI because `-e` and
 * the prompt must agree about it byte for byte -- they are the same
 * question asked twice, and §5's bounds are the answer.
 */

#include "fl/vm.h"
#include "util/buf.h"

enum {
    /* §5: containers nested deeper than this print an ellipsis. */
    FL_REPL_MAX_DEPTH = 8,
    /* §5: a result longer than this is elided.  A prompt that hangs
     * printing is worse than one that truncates. */
    FL_REPL_MAX_BYTES = 2000
};

/*
 * Prints `v` the way a prompt does: repr's quoting so the result can be
 * pasted back in, display's elision so a cyclic value prints, and the
 * two bounds above.  NIL PRINTS NOTHING AT ALL -- not even a blank
 * line, or every `let` and every `io.print` would spam one.
 */
void sag_fl_print_result(FlVm *vm, FlValue v, Bytebuf *out);

/* The interactive prompt.  Returns a SAG_EXIT_* code. */
int sag_fl_repl(void);

#endif /* SAG_FL_REPL_H */
