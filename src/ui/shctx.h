#ifndef YEW_UI_SHCTX_H
#define YEW_UI_SHCTX_H

/*
 * Sprint 57.23 §1: the shell completion context engine.
 *
 * A completion-grade lexer for the sh/bash/zsh family.  It READS a `:!`
 * body and describes the caret; it never produces anything that is
 * executed.  yew_cmd_parse still hands the whole body to `sh -c` as ONE
 * verbatim argument (finish_bang), because that is what makes pipes,
 * quotes and redirection work; nothing decided here reaches execution.
 *
 * The lexer runs from the body start to the CARET, never past it: text
 * after the caret cannot change the caret's context, and scanning it
 * would let an unterminated quote later in the line reclassify the word
 * being completed.  `replace` is therefore always [word start, caret).
 *
 * fish is not lexed.  A $SHELL of fish still RUNS `:!` commands
 * correctly -- execution is untouched -- but completion context may
 * misread fish-only syntax such as `(cmd)`, `and`/`or` and `$argv[1]`;
 * `:!` targets the sh family (Sprint 57.23 §8).
 *
 * Named limits, reported as YEW_SH_POS_NONE rather than guessed at:
 * case-statement bodies, here-document bodies, arithmetic, and
 * `NAME() {` function definitions (whose words are ordinary words).
 */

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/arena.h"
#include "util/base.h"

enum {
    /* §1 pitfall: the nesting stack is bounded.  `$(((((…` is a fuzz
     * input, not a command line; past this the lexer reports NONE rather
     * than growing. */
    YEW_SH_DEPTH_MAX = 64
};

typedef enum {
    YEW_SH_POS_COMMAND,   /* the word the shell would execute            */
    YEW_SH_POS_ARGUMENT,  /* an operand of the simple command            */
    YEW_SH_POS_REDIRECT,  /* the target of > >> < <> >| &> &>> n> n<     */
    YEW_SH_POS_ASSIGN,    /* the value side of NAME=<caret>              */
    YEW_SH_POS_VARIABLE,  /* $NA<caret> or ${NA<caret>                   */
    YEW_SH_POS_NONE       /* comment, heredoc delimiter, arithmetic, case */
} YewShPos;

typedef enum {
    YEW_SH_Q_NONE,
    YEW_SH_Q_SINGLE,      /* '...'                                        */
    YEW_SH_Q_DOUBLE,      /* "..."                                        */
    YEW_SH_Q_DOLLAR       /* $'...' (bash, zsh)                           */
} YewShQuote;

typedef struct YewShCtx {
    YewShPos pos;
    YewShQuote quote;     /* quoting state AT the caret                    */
    Span replace;         /* [word start, caret): bytes a completion replaces */
    char *stem;           /* decoded text of `replace`, quotes removed     */
    /*
     * The simple command the caret belongs to, AFTER assignment prefixes
     * and precommand wrappers (§2) are stripped.  argv[0] is the command
     * word; argv[arg_index] is the caret's word (its decoded stem), so
     * argv[1 .. arg_index-1] are the operands already typed.  argc == 0
     * and arg_index == 0 in command position.  argv[0] is "" when the
     * operands belong to no command (`for x in a b`, `fi > out`).
     */
    char **argv;
    u32 argc;
    u32 arg_index;
    bool dashdash;        /* an unquoted `--` precedes the caret's word    */
    bool brace_var;       /* VARIABLE opened with `${`                      */
    u32 depth;            /* open $( ` ( <( >( { at the caret, for tests   */
    /*
     * Not in §1's sketch, and both needed to insert safely.
     *
     * `tilde`: the replaced text starts with a LEADING, UNQUOTED `~`, so
     * the shell will expand it and the completer may too.  A quoted `~`
     * is a literal directory name (§4 "Tilde").
     *
     * `expands`: the replaced text holds an active expansion -- `$x`,
     * `$(…)`, a backtick.  The stem then carries the expansion's SOURCE,
     * not its value, and completing against it would insert a name the
     * shell never looks up (and re-quote the `$` into a literal).
     */
    bool tilde;
    bool expands;
} YewShCtx;

/* All strings live in `a`.  Never fails on any byte sequence: an input it
 * cannot classify yields YEW_SH_POS_NONE, not false.  Returns false only
 * for cursor > len (or NULL arguments).  Offsets are into `line`. */
bool yew_shctx_at(const char *line, size_t len, size_t cursor, Arena *a,
                  YewShCtx *out);

/*
 * §6: quote `text` for insertion at a caret whose state is `q`.  Returns
 * the bytes to insert; `closing` receives the quote to append when the
 * word is complete (sole match, non-directory), or "" for none.
 *
 * This is the invariant-1 half of the sprint: a file named `$HOME` or
 * `a*b` inserted unescaped is a word the shell EXPANDS, and `rm` then
 * removes something else.  The unit suite round-trips random bytes
 * through real /bin/sh, zsh and bash to prove every state.
 */
char *yew_shq_quote(Arena *a, const char *text, size_t len, YewShQuote q,
                    const char **closing);

/*
 * The whole replacement for a word whose caret state is `q`: the first
 * `literal_prefix` bytes of `text` verbatim (a `~user/` the shell must
 * still see unquoted, so it expands), then the opener `q` needs (`'`,
 * `"`, `$'`, or nothing), then yew_shq_quote of the rest.  This is what
 * lands in the prompt, so it is what the round-trip test drives.
 */
char *yew_shq_insert(Arena *a, const char *text, size_t len,
                     size_t literal_prefix, YewShQuote q,
                     const char **closing);

#endif
