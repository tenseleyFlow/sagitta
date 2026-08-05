#ifndef SAG_UI_CMDCOMP_H
#define SAG_UI_CMDCOMP_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "text/coords.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/vec.h"
#include "ws/finder.h"

typedef struct Ed Ed;

enum { SAG_COMP_MAX = 500 };

typedef enum {
    SAG_COMP_CMD,
    SAG_COMP_PATH,
    SAG_COMP_BUFFER,
    SAG_COMP_OPTION,
    SAG_COMP_VALUE,
    SAG_COMP_KIND__N
} SagCompKind;

typedef struct {
    const char *text;
    const char *detail;
    u8 kind;
    bool is_dir;
    /*
     * SAG_CMD_DEFERRED: the command exists but hard-errors naming its
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
     * Byte offset of `match` within `text`, or SAG_COMP_NO_HIGHLIGHT
     * when the two cannot be aligned (a quoted path).
     */
    u16 match_off;
} CompItem;

enum { SAG_COMP_NO_HIGHLIGHT = 0xFFFFU };

VEC_DECL(Vec_CompItem, CompItem);

/*
 * Sprint 18.5 §3: what a source is asked for.  Passing a struct rather
 * than a widening argument list is what lets §4 add a budget, and later
 * Fletch add a source, without touching every enumerator again.
 */
typedef struct CompReq {
    SagCompKind kind;
    const char *stem; /* decoded token text at the cursor */
    Ed *ed;
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
} CompReq;

enum {
    /* The result is reusable while the stem's directory head is
     * unchanged, so §4 re-ranks instead of re-enumerating. */
    SAG_COMP_SRC_CACHEABLE = 1U << 0,
    /* May exceed its budget; §4 slices it across idle ticks. */
    SAG_COMP_SRC_SLOW = 1U << 1
};

typedef struct CompSource {
    SagCompKind kind;
    const char *name; /* stable id, for logs and (Sprint 34) Fletch */
    u32 (*enumerate)(const CompReq *req, Vec_CompItem *out);
    u32 flags;
} CompSource;

typedef struct CompMenu {
    Vec_CompItem items;
    i32 sel;
    Span replace;
    bool cycling;
} CompMenu;

typedef struct SagCompQuery {
    SagCompKind kind;
    const CompSource *source;
    const char *stem;
    Span replace;
} SagCompQuery;

/*
 * Sprint 18.5 §4: the live filter's cached candidate set.
 *
 * The expensive part of completing a path is the opendir, not the
 * ranking -- so the set is cached and re-ranked while the user keeps
 * typing, and only re-enumerated when the answer could actually change.
 */
typedef struct CompFilter {
    Vec_CompItem base; /* strings live in the caller's completion arena */
    SagCompKind kind;
    char *head;    /* the directory this set came from ("" for non-paths) */
    char *pattern; /* the pattern it was enumerated with                  */
    u32 total;     /* pre-cap total, for the footer                       */
    bool capped;   /* the source had more than SAG_COMP_MAX matches       */
    bool valid;
} CompFilter;

void sag_comp_filter_init(CompFilter *f);
/* Drops the cached set; the strings belong to the caller's arena. */
void sag_comp_filter_invalidate(CompFilter *f);
void sag_comp_filter_free(CompFilter *f);

/*
 * Rank the candidates for `q` into `out`, re-enumerating only when the
 * cache cannot answer.  `arena` owns the enumerated strings and is reset
 * whenever this re-enumerates, so `out` and `f->base` from a previous
 * call are both invalid afterwards.  Returns the pre-cap match total.
 */
u32 sag_comp_filter_run(Ed *ed, CompFilter *f, Arena *arena,
                        const SagCompQuery *q, i64 budget_us,
                        Vec_CompItem *out);

/* Resolve an argspec position. token_index is zero for the command name. */
bool sag_comp_kind_for(const CmdEntry *entry, u32 token_index,
                       SagCompKind *kind);

/* Tolerant command-line source selection at the cursor. */
bool sag_comp_query(Ed *ed, const char *line, size_t len, size_t cursor,
                    Arena *scratch, SagCompQuery *out);

/*
 * Registration is idempotent BY KIND: registering a kind that already
 * has a source replaces it.  That is how a plugin overrides the built-in
 * path source without a removal API, and without leaving two sources
 * fighting over one kind.
 */
void sag_comp_source_register(const CompSource *src);
const CompSource *sag_comp_source(SagCompKind kind);
u32 sag_comp_source_count(void);

/* The full form; `sag_comp_enumerate` is the unbudgeted convenience. */
u32 sag_comp_request(const CompReq *req, Vec_CompItem *out);
u32 sag_comp_enumerate(Ed *ed, SagCompKind kind, const char *stem,
                       Vec_CompItem *out);

/*
 * Bytes of `stem` that name the directory, including the trailing '/'
 * (0 when there is none).  The path source splits here, and §4 keys its
 * cache on the same answer -- two split rules would let the cache serve
 * one directory's entries while the source read another's.
 */
size_t sag_comp_path_head_len(const char *stem);

/* Quote one completion so the Sprint 18 tokenizer reads one argv element. */
char *sag_comp_quote(Arena *arena, const char *text);
char *sag_comp_lcp(Arena *arena, const Vec_CompItem *items);

void sag_comp_menu_init(CompMenu *menu);
void sag_comp_menu_free(CompMenu *menu);

/* Unit-test seam: exercise the required DT_UNKNOWN/lstat path. */
void sag_comp_test_force_dtype_unknown(bool force);
u32 sag_comp_test_lstat_count(void);
/* Unit-test seam: how often the live filter went back to the source. */
void sag_comp_test_reset_enumerate_count(void);
u32 sag_comp_test_enumerate_count(void);

#endif
