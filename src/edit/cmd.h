#ifndef SAG_EDIT_CMD_H
#define SAG_EDIT_CMD_H

#include <stdbool.h>

#include "text/coords.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Win Win;

typedef struct CmdArgv {
    char **v;
    u32 n;
} CmdArgv;

typedef enum {
    SAG_RANGE_NONE,
    SAG_RANGE_LINES,
    SAG_RANGE_BUFFER,
    SAG_RANGE_SELECTION
} SagRangeKind;

typedef struct CmdRange {
    SagRangeKind kind;
    LineNo lo;
    LineNo hi;
    bool given;
    Span tok;
} CmdRange;

typedef enum {
    SAG_RP_FORBID,
    SAG_RP_OPT,
    SAG_RP_LINE,
    SAG_RP_BUFFER,
    SAG_RP_REQUIRED
} CmdRangePolicy;

typedef struct SagCmdInvoke {
    CmdRange range;
    CmdArgv argv;
    i64 count;
    bool bang;
    Win *win;
} SagCmdInvoke;

typedef struct {
    u32 v;
} CmdId;

#define SAG_CMD_NONE ((CmdId){0U})

typedef enum {
    SAG_SRC_KEY,
    SAG_SRC_CMDLINE,
    SAG_SRC_FLETCH,
    SAG_SRC_REPLAY,
    SAG_SRC_MOUSE,
    SAG_SRC_TEST
} CmdSource;

typedef struct CmdCtx {
    Ed *ed;
    Win *win;
    CmdRange range;
    CmdArgv argv;
    u32 cursor_index;
    u32 count;
    bool count_given;
    bool bang;
    i64 iarg;
    const char *sarg;
    u32 sarg_len;
    CmdSource source;
} CmdCtx;

typedef enum {
    SAG_CMD_OK = 0,
    SAG_CMD_ERR_ARG,
    SAG_CMD_ERR_STATE,
    SAG_CMD_ERR_IO,
    SAG_CMD_ERR_DEFERRED
} CmdStatus;

typedef CmdStatus (*CmdFn)(CmdCtx *cx);

typedef enum {
    SAG_ARITY_NONE,
    SAG_ARITY_INT,
    SAG_ARITY_STR,
    SAG_ARITY_OPT_INT,
    SAG_ARITY_OPT_STR
} CmdArity;

enum {
    SAG_CMD_REPEATABLE = 1U << 0,
    SAG_CMD_TAKES_COUNT = 1U << 1,
    SAG_CMD_RECORDABLE = 1U << 2,
    SAG_CMD_NEEDS_WIN = 1U << 3,
    SAG_CMD_CHANGES_BUFFER = 1U << 4,
    SAG_CMD_PROMPTS = 1U << 5,
    SAG_CMD_DEFERRED = 1U << 6,
    /* Command consumes the cursor set instead of running per cursor. */
    SAG_CMD_MULTI_AGGREGATE = 1U << 7,
    /* A key binding without sarg captures the next text-producing key. */
    SAG_CMD_CAPTURES_TEXT = 1U << 8,
    /* Keymap plumbing that must not resolve as a typed E command. */
    SAG_CMD_INTERNAL = 1U << 9
};

typedef struct CmdDesc {
    const char *name;
    CmdFn fn;
    u8 arity;
    u32 flags;
    const char *help;
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

void sag_cmd_init(void);
void sag_cmd_shutdown(void);
CmdId sag_cmd_register(const CmdDesc *d);
CmdId sag_cmd_register_entry(const CmdEntry *entry);
CmdId sag_cmd_lookup(const char *name, u32 len);
const CmdDesc *sag_cmd_desc(CmdId id);
const CmdEntry *sag_cmd_entry(CmdId id);
CmdStatus sag_cmd_prepare(CmdId id, CmdCtx *cx, const CmdDesc **out);
CmdStatus sag_cmd_invoke(CmdId id, CmdCtx *cx);
u32 sag_cmd_count(void);
const CmdDesc *sag_cmd_at(u32 i);
void sag_cmd_set_record_tap(CmdRecordTap tap);

#endif
