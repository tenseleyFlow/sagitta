#ifndef SAG_EDIT_OPT_H
#define SAG_EDIT_OPT_H

/* Sprint 34's typed bridge over the editor options that already exist as
 * C state.  Sprint 36 replaces the provider with the scoped option table;
 * callers keep using this seam and never learn that storage changed. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "util/base.h"

typedef struct Ed Ed;

typedef enum OptValType {
    SAG_OPT_BOOL = 0,
    SAG_OPT_INT,
    SAG_OPT_STR,
    SAG_OPT_ENUM
} OptValType;

typedef struct OptVal {
    u8 type;                       /* OptValType */
    union {
        bool b;
        i64 i;
        struct {
            const char *s;         /* borrowed for the provider call */
            u32 len;
        } str;
    } as;
} OptVal;

typedef struct OptProvider {
    bool (*get)(Ed *, const char *name, u32 len, OptVal *out);
    bool (*set)(Ed *, const char *name, u32 len, const OptVal *value,
                const char **err);
    u32 (*list)(Ed *, const char **out, u32 max);
} OptProvider;

void sag_opt_provider_set(Ed *ed, const OptProvider *provider);
const OptProvider *sag_opt_provider(const Ed *ed);

CmdStatus sag_opt_cmd_get(CmdCtx *cx);
CmdStatus sag_opt_cmd_set(CmdCtx *cx);
CmdStatus sag_fl_cmd_eval(CmdCtx *cx);

#endif /* SAG_EDIT_OPT_H */
