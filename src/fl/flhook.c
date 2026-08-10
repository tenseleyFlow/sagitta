#include "fl/flhook.h"

#include <stdlib.h>
#include <string.h>

#include "fl/vm.h"
#include "util/log.h"

static const char *const EVENT_NAMES[FL_EV__N] = {
    "buf.open",    "buf.change", "buf.save",   "buf.saved",
    "buf.close",   "mode.enter", "mode.leave", "win.focus",
    "cursor.move", "ws.open",    "ws.close",   "ed.idle"
};

const char *fl_event_name(u32 event)
{
    return event < (u32)FL_EV__N ? EVENT_NAMES[event] : NULL;
}

bool fl_event_parse(const char *name, u32 len, u32 *out)
{
    u32 i;

    if (name == NULL)
        return false;
    for (i = 0U; i < (u32)FL_EV__N; i++) {
        size_t n = strlen(EVENT_NAMES[i]);

        if (n == (size_t)len && memcmp(name, EVENT_NAMES[i], n) == 0) {
            if (out != NULL)
                *out = i;
            return true;
        }
    }
    return false;
}

void fl_hook_table_init(FlHookTable *t, const FlHookOps *ops, void *ctx)
{
    if (t == NULL)
        SAG_BUG("fletch hooks: NULL table");
    (void)memset(t, 0, sizeof(*t));
    if (ops != NULL)
        t->ops = *ops;
    t->ctx = ctx;
    t->error_limit = (u32)SAG_HOOK_ERROR_LIMIT_DEFAULT;
}

void fl_hook_table_free(FlHookTable *t)
{
    if (t == NULL)
        return;
    free(t->v);
    free(t->ledger.v);
    (void)memset(t, 0, sizeof(*t));
}

void fl_hook_error_limit(FlHookTable *t, u32 limit)
{
    if (t == NULL)
        SAG_BUG("fletch hooks: NULL table");
    t->error_limit = limit == 0U ? 1U : limit;
}

u32 fl_reg_add(FlRegLedger *l, u32 origin_id, RegKind kind, u32 handle)
{
    FlRegistration *r;
    u32 i;
    u32 want;

    if (l == NULL)
        SAG_BUG("fletch registration ledger: NULL");
    for (i = 0U; i < l->n; i++) {
        if (!l->v[i].active) {
            r = &l->v[i];
            r->origin_id = origin_id;
            r->kind = (u8)kind;
            r->handle = handle;
            r->active = true;
            return i + 1U;
        }
    }
    if (l->n == l->cap) {
        want = l->cap == 0U ? 8U : l->cap * 2U;
        l->v = (FlRegistration *)sag_xreallocarray(l->v, want,
                                                    sizeof(*l->v));
        l->cap = want;
    }
    r = &l->v[l->n++];
    r->origin_id = origin_id;
    r->kind = (u8)kind;
    r->handle = handle;
    r->active = true;
    /* Zero is reserved as "no registration". */
    return l->n;
}

bool fl_reg_remove(FlRegLedger *l, u32 ledger_id)
{
    FlRegistration *r;

    if (l == NULL || ledger_id == 0U || ledger_id > l->n)
        return false;
    r = &l->v[ledger_id - 1U];
    if (!r->active)
        return false;
    r->active = false;
    return true;
}

u32 fl_hook_add(FlHookTable *t, u32 origin, u32 event, FlValue fn)
{
    FlHook *h;
    u32 slot;
    u32 want;
    u32 ledger_id;

    if (t == NULL || event >= (u32)FL_EV__N)
        SAG_BUG("fletch hooks: invalid add");
    if (fn.t != (u8)FL_CLOSURE && fn.t != (u8)FL_NATIVE)
        SAG_BUG("fletch hooks: callback is not callable");
    for (slot = 0U; slot < t->n; slot++)
        if (!t->v[slot].active)
            break;
    if (slot == t->n && t->n == t->cap) {
        want = t->cap == 0U ? 8U : t->cap * 2U;
        t->v = (FlHook *)sag_xreallocarray(t->v, want, sizeof(*t->v));
        t->cap = want;
    }
    /* The handle is the insertion index plus one and stays stable because
     * hook slots are tombstoned, never compacted during callback dispatch. */
    ledger_id = fl_reg_add(&t->ledger, origin, REG_HOOK, slot + 1U);
    h = &t->v[slot];
    h->event = event;
    h->origin = origin;
    h->fn = fn;
    h->errs = 0U;
    h->disabled = false;
    h->ledger_id = ledger_id;
    h->active = true;
    if (slot == t->n)
        t->n++;
    return ledger_id;
}

static FlHook *hook_by_ledger(FlHookTable *t, u32 ledger_id)
{
    FlRegistration *r;
    u32 i;

    if (ledger_id == 0U || ledger_id > t->ledger.n)
        return NULL;
    r = &t->ledger.v[ledger_id - 1U];
    if (!r->active || r->kind != (u8)REG_HOOK || r->handle == 0U)
        return NULL;
    i = r->handle - 1U;
    if (i >= t->n || !t->v[i].active ||
        t->v[i].ledger_id != ledger_id)
        return NULL;
    return &t->v[i];
}

bool fl_hook_remove(FlHookTable *t, u32 ledger_id)
{
    FlHook *h;

    if (t == NULL)
        return false;
    h = hook_by_ledger(t, ledger_id);
    if (h == NULL)
        return false;
    h->active = false;
    h->fn = FL_NIL_V;
    (void)fl_reg_remove(&t->ledger, ledger_id);
    return true;
}

static void notice(FlHookTable *t, FlHookNotice what, u32 event,
                   u32 ledger_id, u32 errs, FlValue err)
{
    if (t->ops.notice != NULL)
        t->ops.notice(t->ctx, what, event, ledger_id, errs, err);
}

static void fault(FlHookTable *t, FlHook *h, FlHookNotice why, FlValue err)
{
    u32 id = h->ledger_id;

    h->errs++;
    notice(t, why, h->event, id, h->errs, err);
    if (h->errs >= t->error_limit) {
        h->disabled = true;
        notice(t, FL_HOOK_NOTICE_DISABLED, h->event, id, h->errs, err);
    }
}

static bool default_call(void *ctx, FlVm *vm, FlValue fn,
                         const FlValue *args, u8 nargs, FlValue *err)
{
    FlValue ignored = FL_NIL_V;

    (void)ctx;
    if (vm == NULL)
        SAG_BUG("fletch hooks: default call without VM");
    if (fl_call(vm, fn, args, (u32)nargs, &ignored))
        return true;
    if (err != NULL)
        *err = vm->err;
    return false;
}

static bool origin_is_masked(const FlHookTable *t, u32 origin)
{
    return t->ops.masked != NULL && t->ops.masked(t->ctx, origin);
}

static void fire_pass(FlHookTable *t, FlVm *vm, u32 event,
                      const FlValue *args, u8 nargs, bool config,
                      u32 snapshot)
{
    u32 i;

    for (i = 0U; i < snapshot; i++) {
        FlHook *h = &t->v[i];
        FlValue err = FL_NIL_V;
        u32 id;
        bool ok;

        if (!h->active || h->disabled || h->event != event ||
            (h->origin == 0U) != config || origin_is_masked(t, h->origin))
            continue;
        id = h->ledger_id;
        t->active_ledger[t->depth - 1U] = id;
        ok = t->ops.call != NULL
                 ? t->ops.call(t->ctx, vm, h->fn, args, nargs, &err)
                 : default_call(t->ctx, vm, h->fn, args, nargs, &err);
        /* The callback may append and reallocate the vector or remove this
         * hook, so never retain h across it. */
        h = hook_by_ledger(t, id);
        if (!ok && h != NULL)
            fault(t, h, FL_HOOK_NOTICE_ERROR, err);
        t->active_ledger[t->depth - 1U] = 0U;
    }
}

void fl_hook_fire(FlHookTable *t, FlVm *vm, u32 event,
                  const FlValue *args, u8 nargs)
{
    u32 snapshot;

    if (t == NULL || event >= (u32)FL_EV__N)
        return;
    if (t->in_flight[event] != 0U) {
        if (!t->warned_reentrant[event]) {
            t->warned_reentrant[event] = true;
            notice(t, FL_HOOK_NOTICE_REENTRANT, event, 0U, 0U, FL_NIL_V);
        }
        return;
    }
    if (t->depth >= (u8)SAG_HOOK_DEPTH_MAX) {
        FlHook *caller = hook_by_ledger(t, t->active_ledger[t->depth - 1U]);

        if (caller != NULL)
            fault(t, caller, FL_HOOK_NOTICE_DEPTH, FL_NIL_V);
        return;
    }
    t->depth++;
    t->in_flight[event]++;
    snapshot = t->n;
    /* Config is privileged only in ordering, never by a second table. */
    fire_pass(t, vm, event, args, nargs, true, snapshot);
    fire_pass(t, vm, event, args, nargs, false, snapshot);
    t->in_flight[event]--;
    t->depth--;
}

void fl_hook_mark(FlVm *vm, void *ctx)
{
    FlHookTable *t = (FlHookTable *)ctx;
    u32 i;

    if (vm == NULL || t == NULL)
        SAG_BUG("fletch hooks: NULL mark provider");
    for (i = 0U; i < t->n; i++) {
        if (t->v[i].active)
            fl_gc_mark_value(vm, t->v[i].fn);
    }
}
