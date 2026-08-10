#ifndef SAG_FL_RECORD_H
#define SAG_FL_RECORD_H

#include <stdbool.h>

#include "edit/cmd.h"
#include "edit/mode.h"
#include "term/input.h"
#include "text/register.h"
#include "util/buf.h"
#include "util/vec.h"

typedef struct Ed Ed;

typedef enum RecRangeKind {
    SAG_REC_RANGE_NONE,
    SAG_REC_RANGE_LINES,
    SAG_REC_RANGE_BUFFER,
    SAG_REC_RANGE_SELECTION,
    SAG_REC_RANGE_SPAN
} RecRangeKind;

typedef struct RecEvent {
    CmdId cmd;
    u32 count;
    bool count_given;
    bool bang;
    i64 iarg;
    u64 range_lo;
    u64 range_hi;
    u32 sarg_at;
    u32 sarg_len;
    u8 mode;
    u8 src;
    u8 range_kind;
    bool range_given;
} RecEvent;

VEC_DECL(RecEventVec, RecEvent);

typedef struct Rec {
    RecEventVec ev;
    Bytebuf blob;
    u8 reg;
    u8 mode_at_start;
    u8 last_reg;
    u32 txn_depth_at_start;
    bool active;
    bool in_prompt;
    bool append;
    bool import_ed;
} Rec;

typedef struct RecStatus {
    bool active;
    u8 reg;
    u32 nevents;
    u8 last_reg;
} RecStatus;

void sag_record_init(Rec *rec);
void sag_record_free(Rec *rec);
bool sag_record_start(Ed *ed, u8 reg);
CmdStatus sag_record_stop(Ed *ed);
bool sag_record_active(const Ed *ed);
CmdStatus sag_record_preflight(CmdId id, const CmdCtx *cx);
void sag_record_tap(CmdId id, const CmdCtx *cx);
void sag_record_key(Ed *ed, Key key);
void sag_record_emit(const Rec *rec, const Ed *ed, Bytebuf *out);
u32 sag_record_status(const Ed *ed, RecStatus *out);

CmdStatus sag_macro_replay(Ed *ed, u8 reg, u32 count);
u32 sag_macro_list(const Ed *ed, RegInfo *out, u32 max);
/* Reads the deterministic recorder metadata; zero when absent/malformed. */
u32 sag_macro_event_count(const u8 *source, size_t len);

CmdStatus sag_record_cmd_record(CmdCtx *cx);
CmdStatus sag_record_cmd_stop(CmdCtx *cx);
CmdStatus sag_record_cmd_replay(CmdCtx *cx);
CmdStatus sag_record_cmd_replay_last(CmdCtx *cx);
CmdStatus sag_record_cmd_list(CmdCtx *cx);
CmdStatus sag_record_cmd_repeat(CmdCtx *cx);

#endif /* SAG_FL_RECORD_H */
