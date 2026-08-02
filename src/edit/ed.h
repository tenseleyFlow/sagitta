#ifndef SAG_EDIT_ED_H
#define SAG_EDIT_ED_H

#include <stdbool.h>

#include "edit/dispatch.h"
#include "edit/keymap.h"
#include "edit/loop.h"
#include "edit/mode.h"
#include "term/grid.h"
#include "term/input.h"
#include "term/render.h"
#include "term/tty.h"
#include "text/edit.h"
#include "ui/win.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

typedef enum {
    SAG_MSG_INFO,
    SAG_MSG_WARN,
    SAG_MSG_ERROR
} MsgSev;

typedef struct Msg {
    char text[512];
    MsgSev sev;
    TimerId expiry;
    bool active;
} Msg;

typedef enum {
    SAG_PROMPT_NONE,
    SAG_PROMPT_RECOVER,
    SAG_PROMPT_QUIT_DIRTY,
    SAG_PROMPT_OVERWRITE
} PromptKind;

typedef struct Buffer {
    TextBuf *tb;
    FileMeta meta;
    char *path;
    UndoTree *undo;
    Journal *jrn;
    MarkSet *marks;
} Buffer;

typedef struct Workspace {
    Buffer *bufs;
    u32 nbufs;
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
    Buffer buffer;
    Win single_win;
    Win *win;

    Mode mode;
    Mode prev_unit;
    Keymap mode_keys[SAG_MODE__N];
    Keymap user_keys;
    KeyStack keys;
    Chord chord;
    u32 chord_timeout_ms;
    CmdId last_cmd;
    CmdStatus last_status;
    u64 dispatch_count;
    char dispatch_message[192];

    TimerHeap timers;
    Msg msg;
    PromptKind prompt;
    bool quit_after_save;
    bool insert_txn;
    bool paste_active;
    bool quit;
    int exit_code;
    bool layout_dirty;
    bool full_damage;

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

bool sag_buf_dirty(const Buffer *b);
EditCtx sag_ed_edit_ctx(Ed *ed);
void sag_ed_finish_edit(Ed *ed, const EditCtx *ec);
Cursor *sag_ed_cursor(Ed *ed);
void sag_ed_insert_barrier(Ed *ed);
CmdStatus sag_ed_invoke(Ed *ed, CmdId id, CmdCtx *cx);
CmdStatus sag_ed_file_save(Ed *ed, bool force);
CmdStatus sag_ed_request_quit(Ed *ed, bool force);

void sag_ed_handle_key(Ed *ed, Key key, i64 now_ms);
void sag_ed_handle_paste(Ed *ed, const u8 *bytes, size_t len, bool end);
void sag_ed_resize(Ed *ed, bool resumed);
void sag_ed_layout(Ed *ed);
void sag_ed_render(Ed *ed);

void sag_msg(Ed *ed, MsgSev sev, const char *fmt, ...);
void sag_msg_clear(Ed *ed);
void sag_ed_prompt(Ed *ed, PromptKind prompt);

#endif
