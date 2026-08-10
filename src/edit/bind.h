#ifndef SAG_EDIT_BIND_H
#define SAG_EDIT_BIND_H

/* Sprint 36: persistent, origin-owned rows above the frozen mode maps. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "edit/mode.h"
#include "fl/value.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct FlVm FlVm;

void sag_bind_init(Ed *ed);
void sag_bind_free(Ed *ed);

u32 sag_bind_add(Ed *ed, u32 origin, Mode mode, const char *seq,
                 CmdId cmd, i64 iarg, const char *sarg, FlValue fn);
bool sag_bind_remove(Ed *ed, u32 ledger_id);
void sag_bind_rebuild(Ed *ed);

/* Config loading brackets registrations so N bind calls freeze once. */
void sag_bind_batch_begin(Ed *ed);
void sag_bind_batch_end(Ed *ed);

const char *sag_bind_error(const Ed *ed);
u32 sag_bind_active_count(const Ed *ed);
u32 sag_bind_rebuild_count(const Ed *ed);

bool fl_bind_native(FlVm *vm, FlValue *args, u32 nargs, FlValue *out);
bool fl_unbind_native(FlVm *vm, FlValue *args, u32 nargs, FlValue *out);
CmdStatus sag_bind_closure_cmd(CmdCtx *cx);
CmdStatus sag_bind_cmd_map(CmdCtx *cx);

#endif /* SAG_EDIT_BIND_H */
