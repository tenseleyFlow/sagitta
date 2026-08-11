#ifndef YEW_EDIT_ED_H
#define YEW_EDIT_ED_H

#include <stdbool.h>

#include "edit/buf.h"
#include "edit/dispatch.h"
#include "edit/job.h"
#include "fl/handle.h"
#include "fl/flhook.h"
#include "fl/origin.h"
#include "fl/record.h"
#include "edit/jumplist.h"
#include "edit/pane_cmds.h"
#include "edit/ws_cmds.h"
#include "ui/groups.h"
#include "ui/tabs.h"
#include "edit/search_cmds.h"
#include "edit/keymap.h"
#include "edit/loop.h"
#include "edit/mode.h"
#include "term/grid.h"
#include "term/input.h"
#include "term/render.h"
#include "term/tty.h"
#include "text/edit.h"
#include "text/register.h"
#include "search/searchui.h"
#include "syn/theme.h"
#include "ui/cmdline.h"
#include "ui/message.h"
#include "ui/mouse.h"
#include "ui/win.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/strmap.h"
#include "ws/state.h"
#include "ws/trust_prompt.h"

typedef enum {
    YEW_PROMPT_NONE,
    YEW_PROMPT_RECOVER,
    YEW_PROMPT_QUIT_DIRTY,
    YEW_PROMPT_OVERWRITE,
    /* Sprint 25 §9: ed.ws.forget.  Its own kind because every other
     * prompt here asks about unsaved BYTES, and answering "delete a
     * cache" must never share a keystroke with those. */
    YEW_PROMPT_WS_FORGET,
    YEW_PROMPT_WORKSPACE_TRUST
} PromptKind;

typedef struct FlRuntime FlRuntime;
typedef struct MacroLib MacroLib;
typedef struct OptProvider OptProvider;
typedef struct YewConfigState YewConfigState;
typedef struct YewOptHistory YewOptHistory;
struct OptStored;

typedef struct YewEdStartup {
    const char *config_path;
    const char *theme;
    bool clean;
    bool no_workspace_config;
    bool trust_workspace;
} YewEdStartup;

typedef struct FlPendingChange {
    u32 buffer_id;
    u64 lo;
    u64 hi;
} FlPendingChange;

/* Buffers are referenced by pointer from every Win, so the list holds
 * pointers, never values: a growing value array would relocate under the
 * windows pointing into it.  Slot 0 is always the document buffer
 * (&ed->buffer, not owned); every other slot is heap-owned by the list. */
typedef struct Workspace {
    Buffer **bufs;
    u32 nbufs;
    u32 cap;
    u32 next_buf_id;
    char *dir;
} Workspace;

struct Ed {
    Arena arena;
    Interner interner;
    Tty tty;
    In in;
    Grid grid;
    Render render;
    Bytebuf frame;
    Bytebuf paste;

    Workspace ws;
    /* Sprint 25 §5.  Zeroed = stateless; the driver opts in with
     * yew_state_open, so nothing that merely builds an Ed touches a
     * filesystem. */
    WsState state;
    Registers regs;
    Buffer buffer;
    Win single_win;
    Win *win;
    /*
     * Sprint 22: the pane tree.  `focus` is a LEAF pointer, revalidated
     * after any tree mutation.  Sprint 23 gives each tab its own root.
     */
    struct Pane *pane_root;
    struct Pane *focus;
    /* Per-frame tables the region payloads index into (§6). */
    struct Pane *leaf_tab[YEW_PANE_MAX_LEAVES];
    struct Pane *split_tab[YEW_PANE_MAX_LEAVES];
    u32 nleaf_tab;
    u32 nsplit_tab;
    PaneDrag drag;
    /* Sprint 27 §1: the router's gesture state.  One per editor: a
     * terminal has one pointer. */
    MouseState mouse;
    Tabs tabs;
    Groups groups;
    TabPrompt tab_prompt;
    WsPrompt ws_prompt;
    YewTrustPrompt trust_prompt;
    Rect footer_rect;
    Rect tab_strip_rect;

    Mode mode;
    Mode prev_unit;
    Keymap mode_keys[YEW_MODE__N];
    Keymap user_keys;
    Keymap bind_keys[YEW_MODE__N];
    struct YewBindings *bindings;
    KeyStack keys;
    Chord chord;
    CmdId capture_cmd;
    u32 capture_count;
    bool capture_count_given;
    u32 chord_timeout_ms;
    CmdId last_cmd;
    CmdStatus last_status;
    u64 dispatch_count;
    char dispatch_message[192];

    /* The loop's clock, handed in with each key; nothing in the core
     * reads a clock itself (invariant 5). */
    i64 now_ms;
    /* Last syntax buffer serviced.  Idle settle ticks resume after it;
     * an input-bearing tick deliberately overrides this with focus. */
    u32 syn_rr_last_buf_id;
    bool syn_rr_last_valid;
    TimerHeap timers;
    JobTable jobs;
    Msg msg;
    SearchOpts search_opts;
    SearchState search;
    SearchConfirm confirm;
    CmdLine cmdline;
    PromptKind prompt;
    bool quit_after_save;
    bool insert_txn;
    bool durability_failed;
    bool paste_active;
    bool quit;
    int exit_code;
    bool layout_dirty;
    bool full_damage;
    bool footer_dirty;
    bool cursor_follow_pending;
    u16 doc_damage_lo;
    u16 doc_damage_hi;
    LineNo drawn_top;
    u32 drawn_top_sub;
    CCol drawn_left;
    bool drawn_wrap;
    LineNo drawn_cursor_line;
    bool drawn_cursor_line_valid;
    bool drawn_top_valid;

    /*
     * Sprint 34: the origin registry (§2).  A value member rather than
     * a pointer so a plain yew_ed_init has one -- a capability question
     * asked of a half-built editor must have an answer, and "the
     * registry has not been allocated yet" is not one.
     */
    FlOriginReg origins;
    /* Sprint 34 §1: every editor handle a script holds. */
    FlHandleTable handles;
    /* Persistent editor-side Fletch state.  Kept opaque here so the editor
     * model does not expose VM internals to every translation unit that
     * includes ed.h. */
    FlRuntime *fl;
    /* Sprint 38: host-owned, reload-safe named macro registry. */
    MacroLib *macrolib;
    FlHookTable hooks;
    /* Sprint 35: resolved-command macro recorder. */
    Rec rec;
    FlPendingChange *fl_changes;
    u32 fl_changes_len;
    u32 fl_changes_cap;
    i64 fl_idle_since_ms;
    bool fl_flushing_change;
    bool fl_idle_fired;
    bool fl_model_teardown;
    const OptProvider *opt_provider;
    struct OptStored *opt_globals;
    bool *opt_inflight;
    YewOptHistory *opt_history;
    YewConfigState *config;
    bool clean;
    Theme theme;
    char *theme_last_dark;
    char *theme_last_light;
    bool theme_option_inflight;
    /* Sprint 37: model/runtime without terminal, input, grid, or loop. */
    bool headless;
    bool batch_stdin_claimed;
    bool undo_break_on_newline;
    bool errorbells;
    bool ambiguous_wide;
    u32 next_win_id;

    bool dispatch_ready;
    bool model_ready;
    bool tty_ready;
    bool input_ready;
    bool probe_seeded;
    bool grid_ready;
    bool render_ready;
};

void yew_ed_init(Ed *ed);
void yew_ed_free(Ed *ed);
YewLoadErr yew_ed_open(Ed *ed, const char *path);
bool yew_ed_open_scratch(Ed *ed);
/* A byte-exact, initially-clean scratch buffer for `--batch ... -`. */
bool yew_ed_open_memory(Ed *ed, const u8 *bytes, size_t len,
                        const char *name);
int yew_ed_driver(const char *path);
int yew_ed_driver_opts(const char *path, const YewEdStartup *startup);
const char *yew_ws_root(const Ed *ed);

bool yew_buf_dirty(const Buffer *b);
bool yew_buf_readonly(const Buffer *b);
u64 yew_buf_len(const Buffer *b);
u64 yew_buf_line_count(const Buffer *b);
Span yew_buf_line_span(const Buffer *b, LineNo line);
LineNo yew_buf_line_of(const Buffer *b, ByteOff off);
/* THE document: the buffer the focused window is showing.  Everything
 * that writes bytes or names a file asks this, never &ed->buffer — see
 * ed.c for why the two stopped being the same object. */
Buffer *yew_ed_doc(Ed *ed);
const char *yew_buf_label(const Buffer *b);

/* Scratch buffers (Sprint 19: job output and the *jobs* table).  The
 * returned pointer is stable for the buffer's lifetime — windows hold it. */
Buffer *yew_ws_scratch_new(Ed *ed, const char *name, u32 flags);
Buffer *yew_ws_scratch_find(Ed *ed, const char *name);
/* NULL when the buffer has been closed since the id was recorded. */
Buffer *yew_ws_buf_by_id(Ed *ed, u32 id);
/*
 * The same contract for windows (Sprint 34 §1).  Searches every tab's
 * pane tree, not just the active one: a Fletch handle taken in one tab
 * stays valid when the user switches to another, and a script that
 * walks buf.list() across tabs would otherwise see its own windows
 * vanish.
 */
Win *yew_ed_win_by_id(Ed *ed, u32 id);
void yew_ws_scratch_drop(Ed *ed, Buffer *b);
/* Points the focused window at `b` with a fresh cursor set and viewport.
 * Returns false when `b` is not in the workspace. */
bool yew_ed_show_buffer(Ed *ed, Buffer *b);
/* Re-run language detection and bind the matching syntax engine. */
void yew_ed_syn_bind(Buffer *b);

/*
 * Sprint 24 §3: deferred file buffers.
 *
 * yew_ws_file_buf returns the buffer for `path`, creating a
 * NON-RESIDENT one (path, no text) if it is new.  Residency is asked of
 * the allocation — there is no flag to go stale.
 */
Buffer *yew_ws_file_buf(Ed *ed, const char *path);
bool yew_buf_resident(const Buffer *b);
int yew_buf_hydrate(Ed *ed, Buffer *b);  /* 0 ok; performs the read */
void yew_buf_defer(Ed *ed, Buffer *b);   /* releases text; modified off */
void yew_ed_win_set_buffer(Ed *ed, Win *w, Buffer *b);
/* Named marks.  `name` is 'a'..'z'; returns false for anything else or
 * for a name that has not been set (or whose mark has since died). */
bool yew_ed_mark_set(Ed *ed, Buffer *b, u8 name, ByteOff at);
bool yew_ed_mark_get(Ed *ed, const Buffer *b, u8 name, ByteOff *out);

/* Sprint 22 pane plumbing.  A clone shares the BUFFER and copies the
 * view (cursor, viewport); Sprint 21's jumplist is per window and so
 * deliberately starts empty in the new pane. */
struct Pane *yew_ed_pane_root(Ed *ed);
Win *yew_ed_win_clone(Ed *ed, const Win *src);
void yew_ed_win_release(Ed *ed, Win *w);

EditCtx yew_ed_edit_ctx(Ed *ed);
EditCtx yew_ed_edit_ctx_for(Ed *ed, Win *win);
void yew_ed_finish_edit(Ed *ed, const EditCtx *ec);
Cursor *yew_ed_cursor(Ed *ed);
void yew_ed_insert_barrier(Ed *ed);
/* Dispatch an already-resolved command without opening an editor-owned
 * transaction.  Fletch uses this after enlisting its outer MACRO
 * transaction; ordinary editor entry remains yew_ed_invoke(). */
CmdStatus yew_ed_dispatch_resolved(Ed *ed, CmdId id, CmdCtx *cx);
CmdStatus yew_ed_invoke(Ed *ed, CmdId id, CmdCtx *cx);
CmdStatus yew_ed_invoke_parsed(Ed *ed, CmdId id,
                               const YewCmdInvoke *invoke);
CmdStatus yew_ed_file_save(Ed *ed, bool force);
CmdStatus yew_ed_file_write_to(Ed *ed, const char *path, bool force);
CmdStatus yew_ed_file_save_win(Ed *ed, Win *win, bool force);
CmdStatus yew_ed_file_write_to_win(Ed *ed, Win *win, const char *path,
                                   bool force);
CmdStatus yew_ed_request_quit(Ed *ed, bool force);

void yew_ed_handle_key(Ed *ed, Key key, i64 now_ms);
void yew_ed_handle_paste(Ed *ed, const u8 *bytes, size_t len, bool end);
/* Mouse events go to yew_mouse_event (ui/mouse.h).  There is deliberately
 * no editor-level twin: Sprint 27 DoD 2 is that the router is the only
 * place a mouse event becomes an action. */
void yew_ed_resize(Ed *ed, bool resumed);
void yew_ed_layout(Ed *ed);
void yew_ed_render(Ed *ed);
bool yew_ed_syn_pending(const Ed *ed);
void yew_ed_syn_tick(Ed *ed, i64 budget_us, bool prioritize_focus);
void yew_ed_damage_rows(Ed *ed, u16 lo, u16 hi);
void yew_ed_damage_line(Ed *ed, LineNo line, bool line_count_changed);
void yew_ed_damage_document(Ed *ed);

void yew_ed_prompt(Ed *ed, PromptKind prompt);

#endif
