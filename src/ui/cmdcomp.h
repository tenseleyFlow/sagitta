#ifndef YEW_UI_CMDCOMP_H
#define YEW_UI_CMDCOMP_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "text/coords.h"
#include "ui/shctx.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/vec.h"
#include "ws/finder.h"

typedef struct Ed Ed;
/* cmdparse.h includes this header, so the tolerant parse result is
 * forward-declared rather than included back. */
typedef struct CmdParsePoint CmdParsePoint;

enum {
    YEW_COMP_MAX = 500,
    /*
     * Sprint 18.5 §4 / DoD 10: how many directory entries the path source
     * will HOLD so that later keystrokes re-rank instead of re-scanning.
     *
     * The ranked set cannot answer a narrowed pattern once it is capped --
     * an entry that missed the top YEW_COMP_MAX for "ent" may be the best
     * match for "entry9" -- so the cache has to key on the DIRECTORY and
     * keep everything in it.  50 000 short names cost a few megabytes and
     * cover every real tree including node_modules; past that we go back
     * to scanning per keystroke rather than let a runaway directory become
     * unbounded memory, and Sprint 26's finder owns that case with its own
     * budget and async walk.
     */
    YEW_COMP_LIST_MAX = 50000
};

typedef enum {
    YEW_COMP_CMD,
    YEW_COMP_PATH,
    YEW_COMP_BUFFER,
    YEW_COMP_OPTION,
    YEW_COMP_VALUE,
    YEW_COMP_PLUGIN,
    /* Sprint 57.18 §2: an executable on $PATH.  Appended rather than
     * inserted -- the source registry is indexed by this enum. */
    YEW_COMP_EXEC,
    /*
     * Sprint 57.23, appended for the same reason.  SHELL is the `:!`
     * dispatcher (§5): ONE registered source that reads the caret's shell
     * context, routes it (§3), and merges the sub-sources below.  Its
     * items keep their SUB-source's kind, so provenance styling and
     * yew_comp_sole(items, kind) still see EXEC, PATH, VAR, ….
     */
    YEW_COMP_SHELL,
    YEW_COMP_VAR,     /* environment names a `:!` child will see (§4)  */
    YEW_COMP_USER,    /* `~name/` from the password database (§4)      */
    YEW_COMP_BUILTIN, /* sh-family builtins and reserved words (§4)    */
    YEW_COMP_KIND__N
} YewCompKind;

/*
 * Sprint 57.23 §4: CompReq.path_filter.  Applied after readdir, so the
 * sliced DirListing and its cache are shared by every mask.
 */
enum {
    YEW_PATH_ANY = 0,
    YEW_PATH_DIRS = 1,
    YEW_PATH_EXEC_OR_DIR = 2
};

typedef struct {
    const char *text;
    const char *detail;
    u8 kind;
    bool is_dir;
    /*
     * YEW_CMD_DEFERRED: the command exists but hard-errors naming its
     * sprint.  Offering a row and then refusing it is worse than either
     * hiding it or marking it, so the menu keeps the row, draws it dim,
     * and puts the sprint in `detail` (invariant 3).
     */
    bool deferred;
    i32 score;
    /*
     * Which bytes of `text` matched, for the menu's highlighting.
     * n_pos == 0 means "do not highlight" rather than "nothing matched":
     * a path that needed quoting has no honest byte mapping back to the
     * ranked name, and a wrong highlight is worse than none.
     */
    FzMatch m;
    /*
     * The string that was RANKED -- the bare entry name for a path, and
     * `text` itself for everything else.  §4 re-ranks a cached candidate
     * set against a longer pattern without going back to the source, and
     * it has to score the same string the source scored.
     */
    const char *match;
    /*
     * Byte offset of `match` within `text`, or YEW_COMP_NO_HIGHLIGHT
     * when the two cannot be aligned (a quoted path).
     */
    u16 match_off;
    /*
     * Sprint 57.23 §6: bytes appended after `text` when this row is
     * COMMITTED as the whole word (a sole survivor, an accept) and is not
     * a directory -- the closing quote of a word typed inside `"…`, and
     * the `}` of a `${NAME`.  The usual trailing space follows it.  NULL
     * (every other source) means nothing extra.  A menu row that is only
     * being LOOKED at inserts `text` alone.
     */
    const char *suffix;
} CompItem;

enum { YEW_COMP_NO_HIGHLIGHT = 0xFFFFU };

VEC_DECL(Vec_CompItem, CompItem);

/*
 * Sprint 18.5 §3: what a source is asked for.  Passing a struct rather
 * than a widening argument list is what lets §4 add a budget, and later
 * Fletch add a source, without touching every enumerator again.
 */
typedef struct CompReq {
    YewCompKind kind;
    const char *stem; /* decoded token text at the cursor */
    Ed *ed;
    /*
     * Sprint 32 §2: whatever is driving this completion when it is not
     * an editor.  `yew fl`'s prompt has no Ed and enumerates from an
     * FlVm, so a source that can serve both reads `ud` and leaves `ed`
     * alone.
     *
     * On the REQUEST rather than on the CompSource, which is where §2
     * suggested it: the source registry is process-global, and a VM
     * pointer parked in a global outlives the VM the first time two
     * of them exist.  A request is scoped to the call that made it.
     */
    void *ud;
    /*
     * Where the returned items' strings are allocated.  Explicit rather
     * than chosen by the source from editor state: §4 resets this arena
     * when it replaces its cached set, and a source that quietly
     * allocated somewhere else would leave the cache pointing at freed
     * strings -- or leak, depending on which way the two disagreed.
     */
    Arena *arena;
    /*
     * Advisory: 0 means "no limit" (a Tab, where the user is waiting for
     * an answer); a positive value is roughly how long a live keystroke
     * can afford.  A source that ignores it is not a bug -- §4 caps the
     * damage by slicing.  A CALLER that assumes a budgeted enumerate
     * returned everything IS a bug, and it looks like a menu that
     * silently lost rows.
     */
    i64 budget_us;
    /*
     * May the source answer from state it cached on an earlier request?
     *
     * True only on the live filter's path, where the menu is already open
     * and the whole point is that twelve keystrokes cost one opendir.  A
     * direct yew_comp_enumerate is a FRESH read: it is what a test calls
     * after touching the filesystem, and memoizing it would answer with a
     * directory that no longer exists.
     */
    bool allow_cache;
    /* Sprint 57.23 §5.  Both zero-initialised for every existing caller. */
    const YewShCtx *shell; /* non-NULL only for YEW_COMP_SHELL             */
    u32 path_filter;       /* YEW_PATH_* mask; 0 == YEW_PATH_ANY           */
} CompReq;

enum {
    /* The result is reusable while the stem's directory head is
     * unchanged, so §4 re-ranks instead of re-enumerating. */
    YEW_COMP_SRC_CACHEABLE = 1U << 0,
    /* May exceed its budget; §4 slices it across idle ticks. */
    YEW_COMP_SRC_SLOW = 1U << 1
};

typedef struct CompSource {
    YewCompKind kind;
    const char *name; /* stable id, for logs and (Sprint 34) Fletch */
    u32 (*enumerate)(const CompReq *req, Vec_CompItem *out);
    u32 flags;
} CompSource;

typedef struct YewCompQuery {
    YewCompKind kind;
    const CompSource *source;
    const char *stem;
    Span replace;
    /* Sprint 57.23: the caret's shell context when kind is SHELL, owned
     * by whatever arena the tolerant parse used.  NULL otherwise. */
    const YewShCtx *shell;
} YewCompQuery;

/*
 * Sprint 18.5 §4: the live filter's cached candidate set.
 *
 * The expensive part of completing a path is the opendir, not the
 * ranking -- so the set is cached and re-ranked while the user keeps
 * typing, and only re-enumerated when the answer could actually change.
 */
typedef struct CompFilter {
    Vec_CompItem base; /* strings live in the caller's completion arena */
    YewCompKind kind;
    char *head;    /* the directory this set came from ("" for non-paths) */
    char *pattern; /* the pattern it was enumerated with                  */
    /*
     * Sprint 57.23 §5 pitfall: `head` and `pattern` do not identify a
     * SHELL answer.  `ls | gr` and `ls gr` share both, but one is command
     * position and the other is not.  The routing row, position, quote
     * state, argv[0] and path mask go here, and any change re-enumerates.
     * NULL for every other kind.
     */
    char *ctx_key;
    u32 total;     /* pre-cap total, for the footer                       */
    bool capped;   /* the source had more than YEW_COMP_MAX matches       */
    bool valid;
} CompFilter;

void yew_comp_filter_init(CompFilter *f);
/* Drops the cached set; the strings belong to the caller's arena. */
void yew_comp_filter_invalidate(CompFilter *f);
void yew_comp_filter_free(CompFilter *f);

/*
 * Drop the path source's cached directory listing.  Call this when the
 * menu closes, not per keystroke -- holding it across a prompt is the
 * whole point, and holding it BETWEEN prompts would show a directory that
 * has since changed on disk.
 */
void yew_comp_listing_invalidate(void);

/*
 * The cached directory scan is SLICED (see DirListing in cmdcomp.c): a
 * live keystroke reads for a bounded time and leaves the rest, so the
 * one keystroke that opens a 10 000-entry directory cannot eat
 * invariant 4's whole budget by itself.
 *
 * `pending` is true while a scan has more to read; `advance` reads one
 * more slice and returns whether yet more remains.  yew_cmdline_comp_tick
 * is the only caller of `advance` — it drives them from the idle path.
 */
bool yew_comp_listing_pending(void);
bool yew_comp_listing_advance(i64 slice_us);
/* Test hook: how many opendir calls the path source has made.  DoD 10
 * asserts a COUNT, which a latency number cannot prove. */
u64 yew_comp_listing_opendirs(void);

/*
 * Rank the candidates for `q` into `out`, re-enumerating only when the
 * cache cannot answer.  `arena` owns the enumerated strings and is reset
 * whenever this re-enumerates, so `out` and `f->base` from a previous
 * call are both invalid afterwards.  Returns the pre-cap match total.
 */
u32 yew_comp_filter_run(Ed *ed, CompFilter *f, Arena *arena,
                        const YewCompQuery *q, i64 budget_us,
                        Vec_CompItem *out);

/*
 * Sprint 57.17 §1: did the filter leave EXACTLY ONE row?
 *
 * The predicate the menu shows and the parser lacks.  `resolve_name`'s
 * "exactly one" is a unique PREFIX and stays that way -- a name that
 * already resolves by prefix must keep winning, or this would change
 * the meaning of commands that work today.  This is the OTHER "exactly
 * one": one survivor of the RANKED set, which is what the user is
 * looking at when the list has narrowed to a single row.
 *
 * Returns that sole item, or NULL when zero or more than one survived.
 * `kind` restricts the answer to one completion kind; pass
 * YEW_COMP_KIND__N to accept whatever the set holds.  One definition,
 * because two would disagree the first time either grew a rule.
 */
const CompItem *yew_comp_sole(const Vec_CompItem *items, YewCompKind kind);

/*
 * Resolve an argspec position. token_index is zero for the command name.
 *
 * Sprint 57.18 §3: `bang_body` says the caret is inside a `:!` body, in
 * which case the argspec has nothing to say -- ed.shell.run's single
 * 's' means "an arbitrary command line".  Sprint 57.23 §5: every word of
 * a body is YEW_COMP_SHELL, whose dispatcher reads the caret's whole
 * shell context (CmdParsePoint.shell) rather than a word index -- the
 * index could not tell `ls | gr` from `ls gr`.
 *
 * Keyed off the bang body rather than off ed.shell.run, deliberately:
 * `:r !cmd` and `:%!cmd` are the same situation under different command
 * ids, and an ordinary 's' argument elsewhere must stay uncompleted.
 */
bool yew_comp_kind_for(const CmdEntry *entry, u32 token_index,
                       bool bang_body, YewCompKind *kind);

/*
 * Sprint 57.23 §3: route a shell context to its sources -- shape beats
 * position, position refines.  Returns the table row that matched (1-9;
 * 0 for the active-expansion guard), fills `sources` with a bit per
 * YewCompKind (1U << kind) and `path_filter` with the YEW_PATH_* mask the
 * PATH source applies.  An empty `sources` offers nothing.
 */
u32 yew_comp_shell_route(const YewShCtx *ctx, u32 *sources, u32 *path_filter);

/* Tolerant command-line source selection at the cursor. */
bool yew_comp_query(Ed *ed, const char *line, size_t len, size_t cursor,
                    Arena *scratch, YewCompQuery *out);
/*
 * The same, from a parse the caller already has.  §9's hint and §4's
 * filter both read one tolerant parse per keystroke: two independent
 * parses would drift, and then the hint names one command while the menu
 * completes another.
 */
bool yew_comp_query_at(Ed *ed, const CmdParsePoint *point,
                       YewCompQuery *out);

/*
 * Registration is idempotent BY KIND: registering a kind that already
 * has a source replaces it.  That is how a plugin overrides the built-in
 * path source without a removal API, and without leaving two sources
 * fighting over one kind.
 */
void yew_comp_source_register(const CompSource *src);
const CompSource *yew_comp_source(YewCompKind kind);
u32 yew_comp_source_count(void);

/* The full form; `yew_comp_enumerate` is the unbudgeted convenience. */
u32 yew_comp_request(const CompReq *req, Vec_CompItem *out);
u32 yew_comp_enumerate(Ed *ed, YewCompKind kind, const char *stem,
                       Vec_CompItem *out);

/*
 * Bytes of `stem` that name the directory, including the trailing '/'
 * (0 when there is none).  The path source splits here, and §4 keys its
 * cache on the same answer -- two split rules would let the cache serve
 * one directory's entries while the source read another's.
 */
size_t yew_comp_path_head_len(const char *stem);

/* Quote one completion so the Sprint 18 tokenizer reads one argv element. */
char *yew_comp_quote(Arena *arena, const char *text);
char *yew_comp_lcp(Arena *arena, const Vec_CompItem *items);

/* Unit-test seam: exercise the required DT_UNKNOWN/lstat path. */
/* 0 restores YEW_COMP_LIST_MAX.  Retires the cache, since the listing
 * held under the old limit was built to a different rule. */
void yew_comp_test_set_list_max(u32 max);
void yew_comp_test_force_dtype_unknown(bool force);
u32 yew_comp_test_lstat_count(void);
/* Unit-test seam: how often the live filter went back to the source. */
void yew_comp_test_reset_enumerate_count(void);
u32 yew_comp_test_enumerate_count(void);

#endif
