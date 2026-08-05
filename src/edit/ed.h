#ifndef SAG_EDIT_ED_H
#define SAG_EDIT_ED_H

#include <stdbool.h>

#include "edit/dispatch.h"
#include "edit/job.h"
#include "edit/jumplist.h"
#include "edit/keymap.h"
#include "edit/loop.h"
#include "edit/mode.h"
#include "term/grid.h"
#include "term/input.h"
#include "term/render.h"
#include "term/tty.h"
#include "text/edit.h"
#include "text/register.h"
#include "ui/cmdline.h"
#include "ui/message.h"
#include "ui/win.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

typedef enum {
    SAG_PROMPT_NONE,
    SAG_PROMPT_RECOVER,
    SAG_PROMPT_QUIT_DIRTY,
    SAG_PROMPT_OVERWRITE
} PromptKind;

enum {
    /* No file behind it: save refuses, the journal never opens.  Sprint 19
     * §4 — job output must never be mistaken for a document. */
    SAG_BUF_SCRATCH = 1U << 0,
    /* Appends bypass the undo tree.  Recording a million streamed appends
     * as undo ops would also journal them (s08); undo in such a buffer is
     * a no-op with a message rather than a lie. */
    SAG_BUF_NOUNDO = 1U << 1
};

typedef struct Buffer {
    /*
     * Stable for the buffer's lifetime and never reused.  Workspace
     * indices shift when a buffer is dropped, so anything that outlives
     * a close — Sprint 21's jumplist entries, Sprint 25's persisted
     * state — names buffers by this instead.
     */
    u32 id;
    TextBuf *tb;
    FileMeta meta;
    char *path;
    /* Display name for buffers with no path ("*job:3 make*").  NULL for
     * ordinary file buffers, which display their path. */
    char *name;
    u32 flags;
    u32 tabwidth;
    UndoTree *undo;
    Journal *jrn;
    MarkSet *marks;
    /* Where this TEXT was edited; reachable from any view of it. */
    ChangeList changes;
} Buffer;

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
    Registers regs;
    Buffer buffer;
    Win single_win;
    Win *win;
    Rect footer_rect;

    Mode mode;
    Mode prev_unit;
    Keymap mode_keys[SAG_MODE__N];
    Keymap user_keys;
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
    TimerHeap timers;
    JobTable jobs;
    Msg msg;
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

    bool dispatch_ready;
    bool model_ready;
    bool tty_ready;
    bool input_ready;
    bool probe_seeded;
    bool grid_ready;
    bool render_ready;
};

void sag_ed_init(Ed *ed);
void sag_ed_free(Ed *ed);
SagLoadErr sag_ed_open(Ed *ed, const char *path);
bool sag_ed_open_scratch(Ed *ed);
int sag_ed_driver(const char *path);
const char *sag_ws_root(const Ed *ed);

bool sag_buf_dirty(const Buffer *b);
const char *sag_buf_label(const Buffer *b);

/* Scratch buffers (Sprint 19: job output and the *jobs* table).  The
 * returned pointer is stable for the buffer's lifetime — windows hold it. */
Buffer *sag_ws_scratch_new(Ed *ed, const char *name, u32 flags);
Buffer *sag_ws_scratch_find(Ed *ed, const char *name);
/* NULL when the buffer has been closed since the id was recorded. */
Buffer *sag_ws_buf_by_id(Ed *ed, u32 id);
void sag_ws_scratch_drop(Ed *ed, Buffer *b);
/* Points the focused window at `b` with a fresh cursor set and viewport.
 * Returns false when `b` is not in the workspace. */
bool sag_ed_show_buffer(Ed *ed, Buffer *b);
EditCtx sag_ed_edit_ctx(Ed *ed);
EditCtx sag_ed_edit_ctx_for(Ed *ed, Win *win);
void sag_ed_finish_edit(Ed *ed, const EditCtx *ec);
Cursor *sag_ed_cursor(Ed *ed);
void sag_ed_insert_barrier(Ed *ed);
CmdStatus sag_ed_invoke(Ed *ed, CmdId id, CmdCtx *cx);
CmdStatus sag_ed_invoke_parsed(Ed *ed, CmdId id,
                               const SagCmdInvoke *invoke);
CmdStatus sag_ed_file_save(Ed *ed, bool force);
CmdStatus sag_ed_file_write_to(Ed *ed, const char *path, bool force);
CmdStatus sag_ed_request_quit(Ed *ed, bool force);

void sag_ed_handle_key(Ed *ed, Key key, i64 now_ms);
void sag_ed_handle_paste(Ed *ed, const u8 *bytes, size_t len, bool end);
void sag_ed_resize(Ed *ed, bool resumed);
void sag_ed_layout(Ed *ed);
void sag_ed_render(Ed *ed);
void sag_ed_damage_rows(Ed *ed, u16 lo, u16 hi);
void sag_ed_damage_line(Ed *ed, LineNo line, bool line_count_changed);
void sag_ed_damage_document(Ed *ed);

void sag_ed_prompt(Ed *ed, PromptKind prompt);

#endif
