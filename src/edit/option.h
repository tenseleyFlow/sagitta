#ifndef SAG_EDIT_OPTION_H
#define SAG_EDIT_OPTION_H

/* Sprint 36: the single typed option table shared by Fletch and E mode. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "util/base.h"
#include "util/strmap.h"

typedef struct Buffer Buffer;
typedef struct Ed Ed;
typedef struct FlVm FlVm;
typedef struct FlValue FlValue;
typedef struct Win Win;

typedef enum OptValType {
    SAG_OPT_BOOL = 0,
    SAG_OPT_INT,
    SAG_OPT_STR,
    SAG_OPT_ENUM
} OptValType;

typedef enum OptScope {
    SAG_OPT_GLOBAL = 0,
    SAG_OPT_BUFFER,
    SAG_OPT_WINDOW,
    SAG_OPT_SCOPE_DECLARED = 255
} OptScope;

typedef struct OptVal {
    u8 type;                       /* OptValType */
    union {
        bool b;
        i64 i;
        struct {
            const char *s;         /* borrowed for the duration of a call */
            u32 len;
        } str;
    } as;
} OptVal;

typedef struct OptDesc {
    const char *name;
    u8 type;                       /* OptValType */
    u8 scope;                      /* OptScope */
    OptVal dflt;
    const char *const *enums;      /* NUL-terminated; ENUM only */
    i64 imin;
    i64 imax;
    bool (*validate)(const OptVal *value, const char **err);
    void (*on_change)(Ed *ed, const struct OptDesc *desc,
                      const OptVal *old, const OptVal *nu);
    const char *help;
} OptDesc;

extern const OptDesc sag_opts[];
extern const u32 sag_opts_len;

/* The Sprint 34 provider remains a narrow compatibility/test seam. */
typedef struct OptProvider {
    bool (*get)(Ed *, const char *name, u32 len, OptVal *out);
    bool (*set)(Ed *, const char *name, u32 len, const OptVal *value,
                const char **err);
    u32 (*list)(Ed *, const char **out, u32 max);
} OptProvider;

void sag_opt_init(Ed *ed);
void sag_opt_free(Ed *ed);
void sag_opt_scope_free(Strmap *map);
void sag_opt_scope_clone(Strmap *dst, const Strmap *src);
void sag_opt_reset(Ed *ed);

const OptDesc *sag_opt_desc(const char *name, u32 len);
bool sag_opt_validate(Ed *ed, u8 scope_hint, const char *name, u32 len,
                      const OptVal *value, const char **err);
bool sag_opt_get(Ed *ed, Buffer *buffer, Win *win,
                 const char *name, u32 len, OptVal *out);
bool sag_opt_set(Ed *ed, u8 scope_hint, const char *name, u32 len,
                 const OptVal *value, const char **err);
/* Snapshot/commit is the transactional registration seam used by set(map).
 * A committed checkpoint is removed by its shared registration-ledger id. */
u32 sag_opt_checkpoint(Ed *ed, const char *name, u32 len,
                       const char **err);
u32 sag_opt_commit(Ed *ed, u32 origin_id, u32 checkpoint);
void sag_opt_discard(Ed *ed, u32 checkpoint);
bool sag_opt_remove(Ed *ed, u32 ledger_id);
u32 sag_opt_list(const char **out, u32 max);

void sag_opt_provider_set(Ed *ed, const OptProvider *provider);
const OptProvider *sag_opt_provider(const Ed *ed);

CmdStatus sag_opt_cmd_get(CmdCtx *cx);
CmdStatus sag_opt_cmd_set(CmdCtx *cx);
CmdStatus sag_opt_cmdline_set(CmdCtx *cx);
CmdStatus sag_fl_cmd_eval(CmdCtx *cx);

/* Native behind the unqualified set({...}) Fletch prelude entry. */
bool fl_api_set_options(FlVm *vm, FlValue *args, u32 nargs, FlValue *out);

#endif /* SAG_EDIT_OPTION_H */
