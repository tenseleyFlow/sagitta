#ifndef YEW_EDIT_BIND_H
#define YEW_EDIT_BIND_H

/* Sprint 36: persistent, origin-owned rows above the frozen mode maps. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "edit/mode.h"
#include "fl/value.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct FlVm FlVm;

void yew_bind_init(Ed *ed);
void yew_bind_free(Ed *ed);

u32 yew_bind_add(Ed *ed, u32 origin, Mode mode, const char *seq,
                 CmdId cmd, i64 iarg, const char *sarg, FlValue fn);
bool yew_bind_remove(Ed *ed, u32 ledger_id);
void yew_bind_rebuild(Ed *ed);

/* Config loading brackets registrations so N bind calls freeze once. */
void yew_bind_batch_begin(Ed *ed);
void yew_bind_batch_end(Ed *ed);

const char *yew_bind_error(const Ed *ed);
u32 yew_bind_active_count(const Ed *ed);
u32 yew_bind_origin_count(const Ed *ed, u32 origin_id);
u32 yew_bind_rebuild_count(const Ed *ed);

bool fl_bind_native(FlVm *vm, FlValue *args, u32 nargs, FlValue *out);
bool fl_unbind_native(FlVm *vm, FlValue *args, u32 nargs, FlValue *out);
CmdStatus yew_bind_closure_cmd(CmdCtx *cx);
CmdStatus yew_bind_cmd_map(CmdCtx *cx);

#endif /* YEW_EDIT_BIND_H */
