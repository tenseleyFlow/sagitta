#ifndef YEW_EDIT_CMD_H
#define YEW_EDIT_CMD_H

#include <stdbool.h>

#include "text/coords.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;
typedef struct OptVal OptVal;

typedef struct CmdArgv {
    char **v;
    u32 n;
} CmdArgv;

typedef enum {
    YEW_RANGE_NONE,
    YEW_RANGE_LINES,
    YEW_RANGE_BUFFER,
    YEW_RANGE_SELECTION
} YewRangeKind;

typedef struct CmdRange {
    YewRangeKind kind;
    LineNo lo;
    LineNo hi;
    bool given;
    Span tok;
} CmdRange;

/* Transient cursor snapshots used by aggregate registry commands.  The
 * caller owns the array for the duration of yew_cmd_invoke(). */
typedef struct CmdCursorArg {
    ByteOff pos;
    ByteOff anchor;
    u64 goal_col;
} CmdCursorArg;

typedef enum {
    YEW_RP_FORBID,
    YEW_RP_OPT,
    YEW_RP_LINE,
    YEW_RP_BUFFER,
    YEW_RP_REQUIRED
} CmdRangePolicy;

typedef struct YewCmdInvoke {
    CmdRange range;
    CmdArgv argv;
    i64 count;
    bool bang;
    Win *win;
} YewCmdInvoke;

typedef struct {
    u32 v;
} CmdId;

#define YEW_CMD_NONE ((CmdId){0U})

typedef enum {
    YEW_SRC_KEY,
    YEW_SRC_CMDLINE,
    YEW_SRC_FLETCH,
    YEW_SRC_REPLAY,
    YEW_SRC_MOUSE,
    YEW_SRC_TEST
} CmdSource;

typedef struct CmdCtx {
    Ed *ed;
    Win *win;
    /* Stable registry identity of the command currently being invoked. */
    CmdId invoked_id;
    CmdRange range;
    CmdArgv argv;
    u32 cursor_index;
    bool cursor_given;
    u32 count;
    bool count_given;
    bool bang;
    i64 iarg;
    const char *sarg;
    u32 sarg_len;
    const CmdCursorArg *cursor_args;
    u32 cursor_args_len;
    const OptVal *opt_in;
    OptVal *opt_out;
    const char *opt_error_msg;
    u8 opt_error;
    CmdSource source;
} CmdCtx;

enum {
    YEW_OPT_ERROR_NONE = 0,
    YEW_OPT_ERROR_NAME,
    YEW_OPT_ERROR_TYPE
};

typedef enum {
    YEW_CMD_OK = 0,
    YEW_CMD_ERR_ARG,
    YEW_CMD_ERR_STATE,
    YEW_CMD_ERR_IO,
    YEW_CMD_ERR_DEFERRED
} CmdStatus;

typedef CmdStatus (*CmdFn)(CmdCtx *cx);

typedef enum {
    YEW_ARITY_NONE,
    YEW_ARITY_INT,
    YEW_ARITY_STR,
    YEW_ARITY_OPT_INT,
    YEW_ARITY_OPT_STR
} CmdArity;

enum {
    YEW_CMD_REPEATABLE = 1U << 0,
    YEW_CMD_TAKES_COUNT = 1U << 1,
    YEW_CMD_RECORDABLE = 1U << 2,
    YEW_CMD_NEEDS_WIN = 1U << 3,
    YEW_CMD_CHANGES_BUFFER = 1U << 4,
    YEW_CMD_PROMPTS = 1U << 5,
    YEW_CMD_DEFERRED = 1U << 6,
    /* Command consumes the cursor set instead of running per cursor. */
    YEW_CMD_MULTI_AGGREGATE = 1U << 7,
    /* A key binding without sarg captures the next text-producing key. */
    YEW_CMD_CAPTURES_TEXT = 1U << 8,
    /* Keymap plumbing that must not resolve as a typed E command. */
    YEW_CMD_INTERNAL = 1U << 9,
    YEW_CMD_INTERACTIVE = 1U << 10
};

typedef struct CmdDesc {
    const char *name;
    CmdFn fn;
    u8 arity;
    u32 flags;
    const char *help;
    /*
     * Sprint 34 §3: the motion-space CMDWORD -- "yank", "del_line" --
     * or NULL when this command is not one.
     *
     * `[a-z][a-z0-9_]{0,15}`, globally unique, and REQUIRED whenever
     * YEW_CMD_RECORDABLE is set.  That bijection is what Sprint 35's
     * round-trip law stands on: a recorded macro is a motion block, and
     * a motion block is what the recorder emits, so word -> command and
     * command -> word must be inverse.  Enforcing it at REGISTRATION
     * means the law cannot be broken by adding a command -- which is
     * the only way anyone would ever break it.
     *
     * Last field so the 200-odd descriptors that are not recordable
     * keep their five-element initializers and read as they did.
     */
    const char *word;
} CmdDesc;

/* CmdDesc remains the key/Fletch execution descriptor.  CmdEntry is the
 * singular registry record and adds E-mode grammar metadata without forcing
 * every earlier static descriptor initializer to grow Sprint 18 fields. */
typedef struct CmdEntry {
    CmdDesc cmd;
    const char *argspec;
    u8 range_policy;
    const char *abbrev;
} CmdEntry;

typedef void (*CmdRecordTap)(CmdId id, const CmdCtx *cx);

void yew_cmd_init(void);
void yew_cmd_shutdown(void);
CmdId yew_cmd_register(const CmdDesc *d);
CmdId yew_cmd_register_entry(const CmdEntry *entry);
bool yew_cmd_register_plugin(const char *plugin_segment, const char *local,
                             CmdFn fn, const char *help, CmdId *out,
                             char *err, size_t errcap);
bool yew_cmd_unregister(CmdId id);
CmdId yew_cmd_lookup(const char *name, u32 len);
/*
 * Sprint 34 §8: the motion-space word -> command map, built at
 * registration.  YEW_CMD_NONE when no command carries that word.
 */
CmdId yew_cmd_by_word(const char *word, u32 len);
const CmdDesc *yew_cmd_desc(CmdId id);
const CmdEntry *yew_cmd_entry(CmdId id);
CmdStatus yew_cmd_prepare(CmdId id, CmdCtx *cx, const CmdDesc **out);
CmdStatus yew_cmd_invoke(CmdId id, CmdCtx *cx);
u32 yew_cmd_count(void);
u32 yew_cmd_active_count(void);
const CmdDesc *yew_cmd_at(u32 i);
void yew_cmd_set_record_tap(CmdRecordTap tap);

#endif
