#ifndef YEW_FL_REPL_H
#define YEW_FL_REPL_H

/*
 * Sprint 32 §2-§5: the interactive `yew fl` prompt.
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
void yew_fl_print_result(FlVm *vm, FlValue v, Bytebuf *out);

/*
 * What the accumulated buffer is, after each line.
 *
 * Sprint 32 §3, and three pitfalls live in this one decision:
 *
 *   - THE WHOLE TEXT IS RE-PARSED, never a line at a time.
 *     `incomplete` is a property of the full source, and deciding it
 *     per line makes a `}` on its own line look like an error.
 *   - THE TRIAL PARSE IS SILENCED.  Printing its diagnostics means
 *     every multiline entry emits a bogus `unexpected end of input` on
 *     each line as it is typed.
 *   - `incomplete` IS NOT TRUSTED ALONE.  s29 pins that a syntax error
 *     inside an open bracket sets `had_error`, not `incomplete`, so a
 *     REPL reading only `incomplete` waits forever for a `)` the user
 *     already typed wrong.  had_error is checked FIRST.
 */
/* The two prompts.  `... ` is the same width as `fl> ` so a
 * continued entry stays column-aligned with the line above it. */
#define FL_REPL_PROMPT "fl> "
#define FL_REPL_CONT   "... "

typedef enum {
    FL_REPL_RUN = 0,     /* complete and clean: compile, run, print   */
    FL_REPL_CONTINUE,    /* still open: show the `... ` prompt        */
    FL_REPL_ERROR        /* a real syntax error: re-parse loudly      */
} FlReplVerdict;

/*
 * Classifies the accumulated text WITHOUT emitting anything.  The
 * caller re-parses with its printing sink when the verdict is
 * FL_REPL_ERROR, which is the only path that should produce output.
 */
FlReplVerdict yew_fl_repl_classify(Arena *arena, Interner *in,
                                   const char *text, size_t len);

/*
 * The prompt's own state: the VM it evaluates into, and the one thing a
 * `:`-command remembers between lines.
 */
typedef struct FlRepl {
    FlVm *vm;
    Arena *arena;
    Interner *in;
    DiagCtx *dc;
    char last_load[1024];
    bool has_load;
} FlRepl;

/*
 * Handles a `:`-command, appending its output to `out`.
 *
 * Returns false when `line` is not one, in which case the caller treats
 * it as source.  Recognised ONLY when the pending buffer is empty and
 * the first non-space byte is `:` -- a `.fl` file starting with `:` is
 * an ordinary syntax error, which is correct and needs no special case.
 */
bool yew_fl_repl_command(FlRepl *r, const char *line, size_t len,
                         Bytebuf *out, bool *quit);

/* The interactive prompt.  Returns a YEW_EXIT_* code. */
int yew_fl_repl(void);

/*
 * Opens the prompt exactly as yew_fl_repl does -- tty, raw mode,
 * yew_bug prehook -- then breaks a chunk on purpose so §9's report
 * fires with the terminal live.  Hidden behind --selftest-fl-bug; the
 * pty golden for invariant 6 is the only caller.
 */
int yew_fl_repl_selftest_bug(void);

#endif /* YEW_FL_REPL_H */
