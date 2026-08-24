#ifndef YEW_FL_FLHOOK_H
#define YEW_FL_FLHOOK_H

/* Sprint 34: deterministic, contained Fletch hook dispatch. */

#include <stdbool.h>

#include "fl/value.h"
#include "util/base.h"

typedef struct FlVm FlVm;

/* Frozen event inventory.  Keep this order in step with fl_event_name(). */
typedef enum FlEvent {
    FL_EV_BUF_OPEN = 0,
    FL_EV_BUF_CHANGE,
    FL_EV_BUF_SAVE,
    FL_EV_BUF_SAVED,
    FL_EV_BUF_CLOSE,
    FL_EV_MODE_ENTER,
    FL_EV_MODE_LEAVE,
    FL_EV_WIN_FOCUS,
    FL_EV_CURSOR_MOVE,
    FL_EV_WS_OPEN,
    FL_EV_WS_CLOSE,
    FL_EV_PLUG_ENABLE,
    FL_EV_PLUG_DISABLE,
    FL_EV_ED_IDLE,
    FL_EV__N
} FlEvent;

enum {
    YEW_HOOK_DEPTH_MAX = 8,
    YEW_HOOK_ERROR_LIMIT_DEFAULT = 5
};

/* Sprint 36 produces the other three kinds; defining them here keeps one
 * origin-tagged ledger ABI rather than one ledger per registration surface. */
typedef enum RegKind {
    REG_HOOK = 0,
    REG_BIND,
    REG_OPTION,
    REG_CMD,
    REG_ATTR,
    REG_OVERLAY,
    REG_TIMER
} RegKind;

typedef struct FlRegistration {
    u32 origin_id;
    u8 kind;               /* RegKind */
    u32 handle;
    bool active;
} FlRegistration;

typedef struct FlRegLedger {
    FlRegistration *v;
    u32 n;
    u32 cap;
} FlRegLedger;

typedef struct FlHook {
    u32 event;
    u32 origin;
    FlValue fn;
    u32 errs;
    bool disabled;
    u32 ledger_id;
    bool active;
} FlHook;

typedef enum FlHookNotice {
    FL_HOOK_NOTICE_ERROR = 0,
    FL_HOOK_NOTICE_DISABLED,
    FL_HOOK_NOTICE_REENTRANT,
    FL_HOOK_NOTICE_DEPTH
} FlHookNotice;

/*
 * Editor integration is deliberately callbacks, not editor mutation here:
 * - call may wrap fl_call in the editor's implicit transaction;
 * - masked reads Ed.origin_mask through fl_origin_masked;
 * - notice performs the pinned log/message presentation.
 * NULL call uses fl_call; NULL masked means no origins are masked.
 */
typedef struct FlHookOps {
    bool (*call)(void *ctx, FlVm *vm, FlValue fn, const FlValue *args,
                 u8 nargs, FlValue *err);
    bool (*masked)(void *ctx, u32 origin_id);
    void (*notice)(void *ctx, FlHookNotice what, u32 event, u32 ledger_id,
                   u32 errs, FlValue err);
} FlHookOps;

typedef struct FlHookTable {
    FlHook *v;
    u32 n;
    u32 cap;
    FlRegLedger ledger;
    FlHookOps ops;
    void *ctx;
    u16 in_flight[FL_EV__N];
    bool warned_reentrant[FL_EV__N];
    u8 depth;
    u32 active_ledger[YEW_HOOK_DEPTH_MAX];
    u32 error_limit;
} FlHookTable;

const char *fl_event_name(u32 event);
bool fl_event_parse(const char *name, u32 len, u32 *out);

void fl_hook_table_init(FlHookTable *t, const FlHookOps *ops, void *ctx);
void fl_hook_table_free(FlHookTable *t);
void fl_hook_error_limit(FlHookTable *t, u32 limit);

u32 fl_reg_add(FlRegLedger *l, u32 origin_id, RegKind kind, u32 handle);
bool fl_reg_remove(FlRegLedger *l, u32 ledger_id);

/* Returns a stable, nonzero ledger id. */
u32 fl_hook_add(FlHookTable *t, u32 origin, u32 event, FlValue fn);
bool fl_hook_remove(FlHookTable *t, u32 ledger_id);

/* A nested same-event fire is dropped.  Other event recursion is bounded. */
void fl_hook_fire(FlHookTable *t, FlVm *vm, u32 event,
                  const FlValue *args, u8 nargs);

/* Root-11 provider: register with fl_gc_root_provider(vm, fl_hook_mark, t). */
void fl_hook_mark(FlVm *vm, void *ctx);

#endif /* YEW_FL_FLHOOK_H */
