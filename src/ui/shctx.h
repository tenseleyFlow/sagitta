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
    /*
     * Sprint 57.32 §1: where the caret's command will run, relative to
     * the directory `:!` commands start in ("" = unchanged), or
     * absolute.  Lexically normalised (`a/../b` -> `b`), the way a
     * shell's LOGICAL `cd` (the default, -L) resolves `..`.  Valid only
     * when `cwd_known`; "" otherwise.
     *
     * `dirv` parallels `argv`: each operand as `cd` would receive it --
     * `~`, `~user`, $HOME and $OLDPWD expanded -- or NULL where only the
     * shell knows its value (any other expansion, a glob).  The caret's
     * own word is NULL.  A change-directory flag's value reads it.
     */
    char *cwd;
    bool cwd_known;
    char **dirv;
} YewShCtx;

/*
 * Sprint 57.32 §1: what the lexer may know about the environment the
 * `:!` shell starts with.  `get` answers a variable (NULL: unset) --
 * HOME, OLDPWD and CDPATH are all it asks.  `base` is the absolute
 * directory `:!` commands start in, for CDPATH's existence test; NULL
 * makes a CDPATH search unknown.
 */
typedef struct YewShEnv {
    const char *(*get)(void *ud, const char *name);
    void *ud;
    const char *base;
} YewShEnv;

/* All strings live in `a`.  Never fails on any byte sequence: an input it
 * cannot classify yields YEW_SH_POS_NONE, not false.  Returns false only
 * for cursor > len (or NULL arguments).  Offsets are into `line`. */
bool yew_shctx_at(const char *line, size_t len, size_t cursor, Arena *a,
                  YewShCtx *out);

/*
 * Sprint 57.24 §1: one precommand wrapper as a completion spec describes
 * it.  `consumes` is NULL-terminated (NULL for none): the flags that take
 * the next word.  `operands` words follow the flags before the command
 * (timeout's duration); `skips_assign` lets NAME=value words through
 * (env, sudo).
 */
typedef struct YewShWrapper {
    const char *const *consumes;
    u32 operands;
    bool skips_assign;
} YewShWrapper;

/*
 * Is `name` a precommand wrapper?  1: yes, `out` filled (its strings must
 * outlive the call); 0: no -- a spec exists and has no `precommand`,
 * which un-wraps a table row; -1: no opinion, use §2's built-in table.
 */
typedef int (*YewShWrapperLookup)(void *ud, const char *name,
                                  YewShWrapper *out);

/* yew_shctx_at with the wrapper table replaceable by `lookup` (NULL keeps
 * the built-in table, which is all yew_shctx_at uses). */
bool yew_shctx_at_with(const char *line, size_t len, size_t cursor,
                       Arena *a, YewShWrapperLookup lookup, void *ud,
                       YewShCtx *out);

/*
 * Sprint 57.32: yew_shctx_at_with and the environment `env` (NULL: an
 * empty one -- `cd ~` and `$HOME` are then unknown, CDPATH unset).
 *
 * THE MODEL (§1).  Every command list tracks, per execution path, where
 * the shell is: a directory, UNKNOWN, or unreachable (after `exit`).  A
 * `cd` moves the path on which it SUCCEEDED.  `&&` runs the next
 * pipeline on the success path, `||` on the failure path, and a list
 * ending (`;`, newline) continues from the success path -- a failure
 * nothing branches on is assumed not to happen, so `cd a; x` runs x in
 * a, while `cd a || echo no; x` joins both paths and is UNKNOWN.
 * `exit` and `return` end their path: `cd a || exit; x` is a.  Pipeline
 * stages and `&` lists are subshells (the zsh last-stage exception only
 * counts where bash agrees); `( … )`, `$( … )`, backticks and process
 * substitutions start from a copy and are discarded at their close; a
 * `{ … }` group shares its parent's shell; `if`/`while`/`until`/`for`/
 * `case` join their branches, and a loop that moves the shell is
 * UNKNOWN inside and after (its next iteration starts elsewhere).  A
 * function body runs when called, so its start is UNKNOWN.  Paths join
 * to a directory only when they agree: whenever the shell's path to the
 * caret's command is data-dependent, the answer is UNKNOWN, never a
 * guess.
 *
 * Named limits: a `cd` inside a function, alias, `source`d file or
 * `eval` the line runs is invisible; so is a `cd` AFTER the caret in an
 * enclosing loop's body (the lexer never reads past the caret), and
 * shell options that change cd (bash cdable_vars, zsh auto_pushd) from
 * the shell's own startup files.
 */
bool yew_shctx_at_env(const char *line, size_t len, size_t cursor, Arena *a,
                      YewShWrapperLookup lookup, void *ud,
                      const YewShEnv *env, YewShCtx *out);

/* `operand` resolved against `dir` (NULL or "": relative stays
 * relative) and lexically normalised, as a logical `cd` would. */
char *yew_sh_dir_join(Arena *a, const char *dir, const char *operand);

/* The directory the caret's command runs in, as a path the filesystem
 * can open: ctx->cwd against `base` (the `:!` start directory).  NULL
 * when it is unknown. */
char *yew_shctx_dir(const YewShCtx *ctx, const char *base, Arena *a);

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
