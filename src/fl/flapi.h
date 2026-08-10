#ifndef SAG_FL_FLAPI_H
#define SAG_FL_FLAPI_H

/* Sprint 34: the table-driven editor surface. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "fl/handle.h"
#include "fl/std.h"

typedef enum FlArgSlot {
    FL_ARG_NONE = 0,
    FL_ARG_BOOL,
    FL_ARG_INT,
    FL_ARG_STR,
    FL_ARG_COUNT,
    FL_ARG_HANDLE_WIN,
    FL_ARG_HANDLE_BUF,
    FL_ARG_HANDLE_CUR,
    FL_ARG_HANDLE_SPAN,
    FL_ARG_LIST,
    FL_ARG_MAP,
    FL_ARG_VALUE
} FlArgSlot;

typedef struct FlBindDesc {
    const char *fl_name;
    const char *cmd;       /* NULL exactly when query is non-NULL. */
    CmdId resolved_id;     /* resolved by fl_api_init, never per call */
    u8 recv;               /* FlHandleKind; FL_H_NONE for a free function */
    u8 nmin;
    u8 nmax;
    u8 argmap[3];
    u32 caps;
    FlNativeFn query;
} FlBindDesc;

extern FlBindDesc fl_api[];
extern const u32 fl_api_len;

extern const FlModuleDef fl_mod_buf;
extern const FlModuleDef fl_mod_win;
extern const FlModuleDef fl_mod_cur;
extern const FlModuleDef fl_mod_span;
extern const FlModuleDef fl_mod_opt;
extern const FlModuleDef fl_mod_ed;

/* Resolve every command row.  A miss is a startup invariant failure. */
void fl_api_init(void);

/* Attach the typed editor pointer and its VM host as one operation. */
void fl_ed_attach(FlVm *vm, Ed *ed, const FlHost *host);
void fl_ed_detach(FlVm *vm);

/* Shared entry points used by module natives and receiver sugar in vm.c. */
const FlBindDesc *fl_api_find(const char *name, u32 len);
const FlBindDesc *fl_api_find_receiver(FlValue recv,
                                       const char *member, u32 len);
bool fl_api_bind_receiver(FlVm *vm, FlValue recv,
                          const char *member, u32 len, FlValue *out);
bool fl_api_invoke(FlVm *vm, const FlBindDesc *d,
                   FlValue *argv, u32 argc, FlValue *out);

/* Generic registry access; public so the marshalling unit test does not
 * need to reach through a registered module map. */
bool fl_api_ed_run(FlVm *vm, FlValue *argv, u32 argc, FlValue *out);
bool fl_api_ed_commands(FlVm *vm, FlValue *argv, u32 argc, FlValue *out);

#endif /* SAG_FL_FLAPI_H */
