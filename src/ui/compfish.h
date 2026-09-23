#ifndef YEW_UI_COMPFISH_H
#define YEW_UI_COMPFISH_H

/*
 * Sprint 57.26 §2: fish as a completion oracle (Amendment S57.26-A1).
 *
 * fish carries hand-written completions for a very large number of
 * tools and answers over a plain pipe: `complete -C` prints
 * `candidate<TAB>description`, the pager's two columns.  When fish is on
 * $PATH and `shell.complete_fish` is `auto`, an operand of a command
 * with no spec asks it -- rung 2 of 57.25's ladder, between the spec and
 * the `--help` tree.
 *
 * The rules, each tested:
 *   - the user's line travels ONLY as argv (`$argv[1]`), never inside
 *     the `-c` script: a `:!` line is arbitrary text;
 *   - fish sees the line REBUILT from the shell context (each word
 *     escaped for fish), not the raw `:!` text, whose grammar differs;
 *   - `fish -N --private`: no config.fish, no history written; the
 *     user's completion and function directories are prepended by the
 *     script itself;
 *   - an answer counts only when `complete -c <cmd>` has rules after the
 *     query autoloaded them -- for a command fish knows nothing about it
 *     lists the cwd's files, which is not knowledge;
 *   - nothing spawns on the keystroke path: a lookup that misses QUEUES
 *     a request, and yew_compfish_idle -- the loop's input-free turn --
 *     spawns it.  At most one fish job is in flight, and it counts toward
 *     the four completion jobs;
 *   - a lookup never writes the message line or the log.
 */

#include <stdbool.h>
#include <stddef.h>

#include "ui/shctx.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct Ed Ed;

enum {
    /* §2: an answer is fresh this long; a stale one is served and
     * refreshed. */
    YEW_COMPFISH_FRESH_MS = 5000,
    YEW_COMPFISH_MAX_LINES = 5000,
    YEW_COMPFISH_TIMEOUT_MS = 2000,
    /* "fish:" + 16 hex digits + NUL. */
    YEW_COMPFISH_KEY_LEN = 22
};

#define YEW_COMPFISH_COLLECT_MAX (1024U * 1024U)

/* The one script fish runs.  Fixed text; the user's words are argv. */
extern const char YEW_COMPFISH_QUERY[];

typedef struct YewFishRow {
    const char *text; /* the candidate, a directory's `/` removed        */
    const char *desc; /* NULL when fish gave none                        */
    bool is_dir;
} YewFishRow;

typedef enum YewFishState {
    YEW_FISH_OFF,     /* not asked: off, no fish, disabled, not a slot  */
    YEW_FISH_PENDING, /* queued or in flight, nothing cached yet        */
    YEW_FISH_ROWS,    /* the gate is open: fish's rows answer           */
    YEW_FISH_CLOSED   /* fish has no rules here: the next rung answers  */
} YewFishState;

typedef struct YewFishLookup {
    YewFishState state;
    /* Valid until the next cache change (an arrival or a reset). */
    const YewFishRow *rows;
    u32 n;
    /* The request that answers (or answered) this slot; empty for OFF. */
    char key[YEW_COMPFISH_KEY_LEN];
} YewFishLookup;

/* ------------------------------------------------------------------ */
/* The line fish sees                                                  */
/* ------------------------------------------------------------------ */

/* Append `word` escaped for fish: space, tab and `$ * ? ~ # ( ) { } [ ]
 * < > & | ; " ' \` backslashed; newline as `\n`, other control bytes as
 * `\xHH`. */
void yew_compfish_escape(Bytebuf *out, const char *word);

/*
 * The query stem for the caret's real stem: "" (every candidate for the
 * slot; yew's ranker filters), "-" for a flag stem (fish lists flags only
 * for one), `--name=` for a flag's value, "." for a leading dot (fish
 * lists dotfiles only for one).  Heap-owned.
 */
char *yew_compfish_query_stem(const char *stem);

/* argv[0] .. argv[arg_index-1] escaped, space-joined, then a space and
 * the query stem.  Heap-owned; NULL when the context has no command. */
char *yew_compfish_line(const YewShCtx *ctx);

/* ------------------------------------------------------------------ */
/* Lookups (keystroke path: never spawn)                               */
/* ------------------------------------------------------------------ */

/* `shell.complete_fish` is `auto` (the default) and a fish was found and
 * has not failed this session. */
bool yew_compfish_enabled(Ed *ed);
/* The fish this session uses, or NULL: $YEW_TEST_FISH when set (empty
 * means none), else `fish` on the absolute elements of $PATH. */
const char *yew_compfish_path(void);

/*
 * Fish's answer for the caret in `ctx` (ARGUMENT position; the caller
 * has already let the shape rows win).  A miss queues a request.
 */
YewFishState yew_compfish_lookup(Ed *ed, const YewShCtx *ctx,
                                 YewFishLookup *out);

/* Queued or in flight with no answer cached: the pager's `…`. */
bool yew_compfish_awaiting(const char *key);
u32 yew_compfish_inflight(void);
u32 yew_compfish_queued(void);

/* ------------------------------------------------------------------ */
/* The idle path                                                       */
/* ------------------------------------------------------------------ */

/* A request is queued and could be spawned now (the caps allow). */
bool yew_compfish_idle_ready(void);
/* Spawn the next queued request.  Returns the jobs started (0 or 1).
 * Call only on a turn with no input. */
u32 yew_compfish_idle(Ed *ed);
/* The prompt closed: unspawned requests are dropped (an answer on its
 * way still lands in the cache). */
void yew_compfish_prompt_closed(void);

/* ------------------------------------------------------------------ */
/* Parsing (a test seam; the job's callback uses it)                   */
/* ------------------------------------------------------------------ */

/* Split fish's output into rows (at most YEW_COMPFISH_MAX_LINES; first
 * TAB separates; control bytes drawn as `·`; a trailing `/` marks a
 * directory; duplicates dropped).  Rows and strings are heap-owned; free
 * with yew_compfish_rows_free. */
YewFishRow *yew_compfish_parse(const char *data, size_t len, u32 *count);
void yew_compfish_rows_free(YewFishRow *rows, u32 n);

/* ------------------------------------------------------------------ */
/* Test seams                                                          */
/* ------------------------------------------------------------------ */

/* Forget the resolution, the cache, the queue and the disabled flag. */
void yew_compfish_reset(void);
u32 yew_compfish_test_spawns(void);
/* Age every cached answer by `ms` (freshness tests). */
void yew_compfish_test_age(i64 ms);
/* Whether a failure disabled the oracle this session. */
bool yew_compfish_test_disabled(void);

#endif
