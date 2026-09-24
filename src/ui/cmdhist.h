#ifndef YEW_UI_CMDHIST_H
#define YEW_UI_CMDHIST_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

enum {
    YEW_HIST_MAX = 1000,
    YEW_HIST_LINE_MAX = 4096
};

typedef struct CmdHist CmdHist;

/* Persistent history uses the XDG state directory and never fails hard. */
CmdHist *yew_hist_open(const char *kind);

/*
 * Sprint 25 §8: per-workspace history, closing s18's deferral.
 *
 * `ws_dir` is a workspace state directory (<ws_dir>/history/<kind>), or
 * NULL to behave exactly like yew_hist_open.
 *
 * READS MERGE, WRITES DO NOT.  Both files are loaded — global first,
 * workspace second, so the most local entries land newest under s18's
 * newest-last convention and win the dedupe.  Appends and compaction go
 * only to the SCOPE's file.
 *
 * The pitfall this signature exists to prevent: merging at SAVE instead
 * of at load.  Writing the merged list back would copy every global
 * entry into the workspace file, and then into the next one, until
 * every workspace held everybody's history and none of it meant
 * anything.
 */
CmdHist *yew_hist_open_scoped(const char *kind, const char *ws_dir,
                              bool workspace_scope);
/* Where writes go.  NULL for an in-memory history. */
const char *yew_hist_path(const CmdHist *h);
/* In-memory histories are used by --clean and --batch. */
CmdHist *yew_hist_open_memory(void);
void yew_hist_close(CmdHist *h);

void yew_hist_add(CmdHist *h, const char *line);
void yew_hist_flush(CmdHist *h);

size_t yew_hist_len(const CmdHist *h);
const char *yew_hist_at(const CmdHist *h, size_t index);
bool yew_hist_is_memory(const CmdHist *h);

typedef struct HistCur {
    i32 idx;
    char *stem;
    char *draft;
} HistCur;

/* Reset before a new walk. The first prev freezes draft as the search stem. */
void yew_hist_cur_reset(HistCur *c, const char *draft);
void yew_hist_cur_dispose(HistCur *c);
const char *yew_hist_prev(CmdHist *h, HistCur *c);
const char *yew_hist_next(CmdHist *h, HistCur *c);

/*
 * Sprint 57.26 §3: history suggestions for `:!` bodies.
 *
 * A snapshot of command bodies, NEWEST FIRST, deduplicated keeping the
 * newest, capped at YEW_HIST_SUGGEST_MAX.  It is taken once per prompt
 * and never changes under the prompt, so the ghost is a pure function of
 * (snapshot, line) -- invariant 5 -- and the per-keystroke work is one
 * prefix memcmp per entry, the loading having happened already
 * (invariant 4).
 *
 * Refused on entry, whatever the source:
 *   - a multi-line command (the prompt is one line);
 *   - one with a control byte (a ghost is drawn as it is);
 *   - one holding a `NAME=value` word whose NAME yew_secret_name()
 *     calls a secret: the ghost is on screen, and screens are shared.
 */
enum {
    YEW_HIST_SUGGEST_MAX = 20000,
    /* A shell history larger than this is read from its tail. */
    YEW_HIST_SHELL_READ_MAX = 8 * 1024 * 1024
};

typedef struct YewHistSuggest {
    /* Every body, NUL-terminated, in one pool: loading 20 000 entries
     * is one growing buffer, not 20 000 allocations. */
    char *pool;
    size_t pool_len;
    size_t pool_cap;
    u32 *off;
    u32 *lens;
    u32 *hashes;
    u32 n;
    u32 cap;
    /* Open-addressed set of the bodies held, for the dedupe. */
    u32 *slots;
    u32 n_slots;
} YewHistSuggest;

void yew_hist_suggest_init(YewHistSuggest *s);
void yew_hist_suggest_free(YewHistSuggest *s);
/* Offer one body.  Call newest first: the first offer of a text wins.
 * Leading blanks are dropped (the shell ignores them).  Returns whether
 * it was kept. */
bool yew_hist_suggest_add(YewHistSuggest *s, const char *text, size_t len);
/* Entry `i` (0 is the newest), NUL-terminated; valid until the next add
 * or the free. */
const char *yew_hist_suggest_at(const YewHistSuggest *s, u32 i);
/* Is this entry refused (multi-line, a control byte, a secret)? */
bool yew_hist_suggest_refused(const char *text, size_t len);
/*
 * The newest entry that starts with `body` (leading blanks ignored) and
 * is longer: returns its remainder and sets `*rest_len`, or NULL.
 * Byte-exact and case-sensitive.
 */
const char *yew_hist_suggest_match(const YewHistSuggest *s, const char *body,
                                   size_t len, size_t *rest_len);

/*
 * The shells' own history files, read-only.  Each parser offers its
 * entries NEWEST FIRST (the files are written oldest first).
 *   fish: `- cmd: <text>` lines; `\\` is `\`, `\n` a newline.
 *   zsh:  UNMETAFIED first (0x83 then b means b ^ 0x20); an optional
 *         `: <start>:<elapsed>;` prefix; a line ending `\` continues.
 *   bash: plain lines; `#<digits>` timestamp lines skipped.
 */
void yew_hist_parse_fish(YewHistSuggest *s, const char *data, size_t len);
void yew_hist_parse_zsh(YewHistSuggest *s, const char *data, size_t len);
void yew_hist_parse_bash(YewHistSuggest *s, const char *data, size_t len);
/* zsh's metafication undone in place; returns the new length. */
size_t yew_hist_unmetafy(char *bytes, size_t len);

/*
 * Read the history files that exist, in the order fish, zsh, bash:
 *   fish  $XDG_DATA_HOME/fish/fish_history, else
 *         $HOME/.local/share/fish/fish_history;
 *   zsh   $HISTFILE when its name mentions zsh, else $HOME/.zsh_history;
 *   bash  $HISTFILE otherwise, else $HOME/.bash_history.
 * Only the environment names a file -- never the password database -- so
 * a process without HOME reads nothing.
 */
void yew_hist_suggest_read_shells(YewHistSuggest *s);

/* Test seam: history files opened by yew_hist_suggest_read_shells. */
u32 yew_hist_test_shell_opens(void);
void yew_hist_test_reset_shell_opens(void);

/*
 * Sprint 57.30 §2: fish's "smart" history walk.
 *
 * A VIEW is the history one prompt walks: its own CmdHist, or -- on a
 * bang line -- the 57.26 snapshot, so Up and the ghost always agree.
 * Either way index 0 is the NEWEST entry.
 */
typedef struct YewHistView {
    const CmdHist *hist;            /* used when non-NULL */
    const YewHistSuggest *suggest;  /* otherwise */
} YewHistView;

u32 yew_hist_view_len(const YewHistView *v);
/* Entry `i`, 0 the newest; NULL past the end. */
const char *yew_hist_view_at(const YewHistView *v, u32 i);

/*
 * The first occurrence of `term` in `text`: case-sensitive, byte-exact.
 * An empty term is found at 0.  False, `*at` untouched, when absent.
 */
bool yew_hist_find(const char *text, size_t len, const char *term,
                   size_t term_len, size_t *at);

/*
 * One walk.  The TERM is frozen when it begins and never re-derived;
 * matching is yew_hist_find over the whole entry; an entry equal to one
 * this walk already showed is skipped; newest first.  `seen` is the
 * stack of view indices shown, oldest last -- Down pops it, so walking
 * back retraces exactly the entries walked through.
 */
typedef struct YewHistWalk {
    bool on;
    char *term;
    size_t term_len;
    /* The line as it was when the walk began: Down past the newest
     * match restores it (readline's draft). */
    char *draft;
    u32 *seen;
    u32 n_seen;
    u32 cap_seen;
} YewHistWalk;

void yew_hist_walk_begin(YewHistWalk *w, const char *draft,
                         const char *term, size_t term_len);
/* Ends the walk and frees what it held; safe on a zeroed or ended walk. */
void yew_hist_walk_end(YewHistWalk *w);
/* The next OLDER distinct match, or NULL (the walk stays put). */
const char *yew_hist_walk_older(YewHistWalk *w, const YewHistView *v);
/*
 * One step NEWER: the match before the current one, or -- past the
 * newest -- NULL with `*at_draft` set, the walk now back at its draft.
 * NULL with `*at_draft` clear when it was already there.
 */
const char *yew_hist_walk_newer(YewHistWalk *w, const YewHistView *v,
                                bool *at_draft);
/* The entry the walk shows now, or NULL at the draft. */
const char *yew_hist_walk_current(const YewHistWalk *w,
                                  const YewHistView *v);

#endif
