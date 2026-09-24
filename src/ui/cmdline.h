#ifndef YEW_UI_CMDLINE_H
#define YEW_UI_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

#include "term/input.h"
#include "text/cursor.h"
#include "ui/cmdcomp.h"
#include "ui/cmdhist.h"
#include "ui/cmdparse.h"
#include "ui/menu.h"
#include "ui/win.h"

typedef struct Ed Ed;

typedef enum {
    YEW_PROMPT_CMD,
    YEW_PROMPT_SEARCH_F,
    YEW_PROMPT_SEARCH_B,
    YEW_PROMPT_INPUT
} YewPromptKind;

/* `text` is valid only for the duration of the callback.  The callback runs
 * after the prompt has closed and the editor mode has been restored. */
typedef void (*YewCmdlineInputDone)(Ed *ed, bool accepted,
                                    const u8 *text, size_t len, void *ctx);

typedef struct CmdLine {
    YewPromptKind kind;
    bool active;
    /* Monotonic identity for one prompt lifetime.  The CmdLine storage is
     * embedded in Ed and reused, so pointer identity cannot distinguish a
     * prompt that a command replaced while Enter was being handled. */
    u64 generation;
    TextBuf *buf;
    Cursor cur;
    Menu menu;
    Arena comp_arena;
    /* §4: the cached candidate set `comp_arena` backs. */
    CompFilter filter;
    /*
     * Sprint 57.30 §2: the history walk Up/Down drive.  Its term froze
     * when it began; any edit ends it.  `walk_bang`: the walk reads the
     * 57.26 snapshot and puts entries after the first `walk_prefix`
     * bytes of its draft (the `!`, `r !`, `%!` the user typed).
     */
    YewHistWalk walk;
    size_t walk_prefix;
    bool walk_bang;
    /*
     * Sprint 57.30 §4: C-r's history search owns the pager -- its rows
     * are history entries matching the line (§2's source and rule),
     * refiltered as the line is edited.  `hsearch_line` is the line as
     * it was when C-r was pressed, which Esc restores.
     */
    bool hsearch;
    char *hsearch_line;
    CmdErr err;
    /* §9: what the parser currently understands.  Empty when it
     * understands nothing -- silence, never a guess. */
    char hint[160];
    u16 scroll;

    /* §8: the menu row a click last selected, so a second click on the
     * same row accepts it.  -1 when none.  A row index rather than a
     * timer keeps it deterministic for the goldens. */
    i32 click_row;
    void *target;
    CmdHist *history;
    /* Histories that could not or must not persist remain session-lived.
     * Slots are command, search, and input respectively. */
    CmdHist *memory_history[3];
    u8 return_mode;
    u32 comp_total;
    /*
     * Sprint 57.24 §5: Tab asked and the answer is a generator still in
     * flight with nothing to show yet.  Its arrival may then open the
     * menu even on an empty word -- the user did ask.  Any edit clears it.
     */
    bool comp_asked;
    char *menu_stem;
    Span menu_original;
    /*
     * Sprint 57.26 §3: the `:!` history snapshot the ghost reads -- taken
     * the first time this prompt holds a bang body with the caret at its
     * end, and fixed from then until the prompt closes (invariant 5).
     */
    YewHistSuggest suggest;
    bool suggest_loaded;
    YewCmdlineInputDone input_done;
    void *input_ctx;
    /* Sprint 57.28 §4: the last A-.: its dispatcher sequence number, the
     * history index it took a word from and where that word now sits. */
    u64 last_arg_seq;
    u64 last_arg_gen;
    size_t last_arg_entry;
    Span last_arg_span;
} CmdLine;

void yew_cmdline_open(Ed *ed, YewPromptKind kind, const char *seed);
void yew_cmdline_open_input(Ed *ed, const char *seed,
                            YewCmdlineInputDone done, void *ctx);
void yew_cmdline_close(Ed *ed, bool accepted);
void yew_cmdline_dispose(Ed *ed);
bool yew_cmdline_key(Ed *ed, const Key *key);
void yew_cmdline_paste(Ed *ed, const u8 *bytes, size_t len);
void yew_cmdline_draw(Ed *ed, Rect rect);
Win *yew_cmdline_target(Ed *ed);
void yew_cmdline_sync(Ed *ed);
/* Appends the prompt's current text to `out`. */
void yew_cmdline_text(Ed *ed, Bytebuf *out);
void yew_cmdline_edited(Ed *ed);
/*
 * Sprint 57.28: `n` bytes bound for `win`, as they may land there.  The
 * prompt is one line: a newline run folds to one blank, as a paste into
 * it always has, and a NUL is refused (false, with the message).  Any
 * other Win takes the bytes verbatim.  `out` is appended to.
 */
bool yew_cmdline_clean(Ed *ed, const Win *win, const u8 *b, size_t n,
                       Bytebuf *out);

/*
 * Continue a sliced completion scan on the idle path; true while more
 * remains.  The loop calls this beside yew_picker_tick, after input is
 * drained, for the reason given there.
 *
 * `scanning` is the same condition the tick acts on, exposed so
 * yew_loop_deadline can refuse to sleep on exactly it.  Use it rather
 * than re-deriving the test — the two drifting apart is a busy loop in
 * one direction and a stalled menu in the other.
 */
bool yew_cmdline_comp_tick(Ed *ed);
bool yew_cmdline_comp_scanning(const Ed *ed);

/*
 * Sprint 57.25 §6: the IDLE half of completion -- the loop calls it only
 * on a turn that carried no input, so a subprocess spawn (~1 ms) is never
 * charged to a keystroke.  It asks for the help of the command whose
 * argument the caret sits in (the prewarm) and spawns one queued help
 * request.  Returns the jobs it started.
 *
 * `idle_pending` is the condition it would act on, for
 * yew_loop_deadline: the same pairing rule as `scanning` above.
 */
u32 yew_cmdline_comp_idle(Ed *ed);
bool yew_cmdline_comp_idle_pending(const Ed *ed);

/* Sprint 57.24 §5.4: a completion generator's answer for `key` landed in
 * the cache.  Refilters an open `:` menu that asked for exactly that key;
 * never edits the prompt's text. */
void yew_cmdline_compgen_arrived(Ed *ed, const char *key);

CmdStatus yew_cmdline_cmd_hist_prev(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_hist_next(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_complete_next(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_complete_prev(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_insert_register(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_literal_next(CmdCtx *cx);
/* §8: click selects, a second click on the same row accepts. */
bool yew_cmdline_menu_click(Ed *ed, i32 row);
bool yew_cmdline_menu_scroll(Ed *ed, i32 delta);

/*
 * Sprint 57.17 §2: the pager's pure preview moves -- the selection
 * changes, the prompt text does not.  Bound to nothing by default; the
 * arrow dispatchers below and Fletch reach them.
 */
CmdStatus yew_cmdline_cmd_menu_next(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_menu_prev(CmdCtx *cx);
/* Sprint 57.30 §1: `<up>` / `<down>` (and C-p / C-n): rows of the table
 * once Tab has entered it, the smart history walk otherwise. */
CmdStatus yew_cmdline_cmd_up(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_down(CmdCtx *cx);
/*
 * Sprint 57.30 §2: the bytes of the line the history highlight covers --
 * the first occurrence of the walk's term in the walked entry.  STATE,
 * never text.  False while no walked entry is on the line or the term
 * is empty.
 */
bool yew_cmdline_hist_match(Ed *ed, Span *out);
/* Sprint 57.30 §4: C-r -- open the pager on history matching the line,
 * or, open already, move to the next OLDER match. */
CmdStatus yew_cmdline_cmd_hist_search(CmdCtx *cx);

CmdStatus yew_cmdline_cmd_menu_page_next(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_menu_page_prev(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_menu_accept(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_menu_dismiss(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_ghost_accept(CmdCtx *cx);
/* The ghost the prompt draws after the caret (dim), or NULL: what
 * `<right>` would accept.  Never part of the prompt's text. */
const char *yew_cmdline_ghost(Ed *ed, size_t *len);
/* Sprint 57.26 §3: the ghost up to and including the next run of
 * unquoted whitespace; a word motion when there is no ghost. */
CmdStatus yew_cmdline_cmd_ghost_accept_word(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_accept(CmdCtx *cx);
CmdStatus yew_cmdline_cmd_cancel(CmdCtx *cx);
/* Sprint 57.28 §3: the whole ghost, or the line end without one. */
CmdStatus yew_cmdline_cmd_ghost_accept_line(CmdCtx *cx);
/* Sprint 57.28 §4: A-., the previous entry's last word. */
CmdStatus yew_cmdline_cmd_last_arg(CmdCtx *cx);

/*
 * Sprint 57.29 §1: the prompt's selection is [min(anchor,pos),
 * max(anchor,pos)) of its Win's primary cursor -- STATE, never text: it
 * is not in yew_cmdline_text(), the history draft, the parse point or
 * the ghost.  False, and `out` untouched, when it is empty or there is no
 * prompt.
 */
bool yew_cmdline_selection(Ed *ed, Span *out);
/*
 * The collapse table, applied by the dispatcher around EVERY command it
 * runs on the prompt Win (yew_ed_invoke): `after` false runs before the
 * command, inside its undo transaction, and returns true when the
 * selection was the whole event (the command must not run); `after`
 * true runs once it has, and leaves the selection collapsed unless the
 * command was a Shift+motion.
 */
bool yew_cmdline_sel(Ed *ed, const char *command, bool after);
/* C-c: copy a selection and stay; none, exactly ed.cmdline.cancel. */
CmdStatus yew_cmdline_cmd_copy_or_cancel(CmdCtx *cx);

#endif
