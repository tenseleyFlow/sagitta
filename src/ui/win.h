#ifndef YEW_UI_WIN_H
#define YEW_UI_WIN_H

#include "edit/motion.h"
#include "edit/completion.h"
#include "edit/jumplist.h"
#include "edit/shadow.h"
/* Sprint 22 moved Rect here, as this file said it would. */
#include "ui/layout.h"
#include "ui/panel.h"
#include "search/overlay.h"
#include "syn/engine.h"
#include "edit/multicursor.h"
#include "edit/select.h"
#include "text/coords.h"
#include "ui/gutter.h"
#include "ui/viewport.h"
#include "unicode/coords.h"
#include "util/base.h"
#include "util/strmap.h"

typedef struct Buffer Buffer;

typedef struct LspHighlightState {
    /* Separate from Win.overlay: search and LSP responses have independent
     * ownership, invalidation, and paint roles. */
    MatchOverlay read;
    MatchOverlay write;
    TimerId timer;
    u64 request;
    u64 seq;
    u64 buf_gen;
    ByteOff cursor;
    u32 server_id;
    u32 buf_id;
    bool cursor_valid;
} LspHighlightState;

typedef struct Win {
    /*
     * Sprint 34: stable for this window's lifetime and never reused.
     *
     * Buffers have carried one since Sprint 14 for the same reason a
     * window needs one now: a Fletch handle outlives the pane tree
     * mutation that freed the Win, and an id that is absent from the
     * tree fails cleanly where a dangling pointer does not.  Assigned
     * by ed.c, which owns the counter; 0 is never handed out.
     */
    u32 id;
    Buffer *buf;
    CursorSet cs;
    HState h;
    Viewport vp;
    /* Navigation history of this VIEW; two panes on one file keep
     * separate ones (Sprint 21 §5). */
    JumpList jumps;
    /* Sprint 21 $3: match highlighting for THIS view. */
    MatchOverlay overlay;
    /* Reused by the allocation-free syntax draw path. */
    SynSpan *syn_spans;
    u32 syn_spans_cap;
    WrapCache wrap_cache;
    Rect rect;
    NumStyle number_style;
    u16 gutter_width;
    GutterSigns gutter_signs;
    CCol wrap_goal;
    bool wrap_goal_valid;
    /* Sprint 36: sparse values explicitly set at window scope. */
    Strmap opt_overrides;
    /* Sprint 43: display-only completion state belongs to the view. */
    Shadow shadow;
    ComplMenu compl;
    /* Sprint 47: one transient floating panel belongs to this view. */
    Panel panel;
    /* Opaque async producer state.  Core does not interpret these fields;
     * the LSP module uses stable window ids plus this sequence to reject
     * late hover/signature responses. */
    u64 panel_source_request;
    u64 panel_source_seq;
    u32 panel_source_server;
    /* Definition-family/reference requests use a separate cancellation and
     * staleness lane from panels, so a hover never cancels a pending goto. */
    u64 nav_source_request;
    u64 nav_source_seq;
    u32 nav_source_server;
    u64 symbol_source_request;
    u64 symbol_source_seq;
    u32 symbol_source_server;
    /* Sprint 47: passive document highlights debounce and reject stale
     * responses independently of every command-driven LSP request. */
    LspHighlightState lsp_highlight;
    /* Sprint 53 view-local Git presentation. */
    u32 scroll_link;
    u64 git_sign_gen;
    u32 git_sign_buf;
    SpanVec git_diff_intra;
    bool git_diff_intra_add;
    bool git_blame;
} Win;

/* Sprint 14 compatibility names; new code uses the yew_vp_* API. */
void yew_win_follow_cursor(Win *w);
LineNo yew_win_view_top(const Win *w);
bool yew_win_view_row(const Win *w, LineNo line, u16 *row);

/* Sprint 22 §7: click-to-focus lands the cursor on the clicked
 * grapheme.  Conversions go through src/unicode/, never here. */
void yew_win_click_to_cursor(Win *w, u16 grid_x, u16 grid_y);

#endif
