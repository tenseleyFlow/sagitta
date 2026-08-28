/*
 * Sprint 34 deliverable 2: the origin registry and the §13 capability
 * check.  fl_cap_origin, fl_cap_name, fl_origin_name and fl_cap_check
 * moved here from std.c unchanged -- the walk they implement is the
 * whole capability model and it belongs in the file named after it.
 */

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/origin.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "util/base.h"
#include "util/intern.h"

#ifndef YEW_WITH_PLUGINS
#define YEW_WITH_PLUGINS 0
#endif

#if YEW_WITH_PLUGINS
#include "mod/plug/plug.h"
#endif

/* ---------------------------------------------------------------- */
/* The registry                                                     */
/* ---------------------------------------------------------------- */

static char *dup_label(const char *s)
{
    size_t n = strlen(s);
    char *p = (char *)yew_xmalloc(n + 1U);

    (void)memcpy(p, s, n + 1U);
    return p;
}

static void reg_push(FlOriginReg *r, FlOriginKind k, const char *label,
                     u32 caps)
{
    if (r->n == r->cap) {
        u32 want = r->cap == 0U ? 4U : r->cap * 2U;

        r->v = (FlOriginRec *)yew_xreallocarray(r->v, want,
                                                sizeof(*r->v));
        r->cap = want;
    }
    r->v[r->n].kind = (u8)k;
    r->v[r->n].caps = caps;
    r->v[r->n].label = dup_label(label);
    r->n++;
}

void fl_origin_reg_init(FlOriginReg *r)
{
    if (r == NULL)
        YEW_BUG("origin registry: NULL");
    (void)memset(r, 0, sizeof(*r));
    /*
     * Slot 0 is the user config, seeded whether or not one is ever
     * loaded.  Sprint 54 hard-codes the number; making it depend on
     * load order would mean a session with no config gives id 0 to
     * whatever registered first.
     */
    reg_push(r, FL_ORIGIN_CONFIG, "the user config", FL_CAP_ALL);
}

void fl_origin_reg_free(FlOriginReg *r)
{
    u32 i;

    if (r == NULL)
        return;
    for (i = 0U; i < r->n; i++)
        yew_xfree(r->v[i].label);
    yew_xfree(r->v);
    yew_xfree(r->masked);
    (void)memset(r, 0, sizeof(*r));
}

static FlOriginReg *reg_of(Ed *ed)
{
    if (ed == NULL)
        return NULL;
    if (ed->origins.n == 0U)
        fl_origin_reg_init(&ed->origins);
    return &ed->origins;
}

u32 fl_origin_register(Ed *ed, FlOriginKind k, const char *label, u32 caps)
{
    FlOriginReg *r = reg_of(ed);
    u32 i;

    if (r == NULL)
        return FL_ORIGIN_ID_NONE;
    if (label == NULL)
        label = "an unnamed origin";
    /*
     * Idempotent on (kind, label).  Loading the same file twice must
     * not give it two identities: the ledger keys teardown on the id,
     * so a second id leaves the first registration's hooks live with
     * nothing left to remove them.
     */
    for (i = 0U; i < r->n; i++) {
        if (r->v[i].kind == (u8)k && strcmp(r->v[i].label, label) == 0) {
            r->v[i].caps = caps;
            return i;
        }
    }
    reg_push(r, k, label, caps);
    return r->n - 1U;
}

const char *fl_origin_label(const Ed *ed, u32 origin_id)
{
    if (ed == NULL || origin_id >= ed->origins.n)
        return "an unknown origin";
    return ed->origins.v[origin_id].label;
}

u32 fl_origin_caps(const Ed *ed, u32 origin_id)
{
    if (ed == NULL || origin_id >= ed->origins.n)
        return 0U;
    return ed->origins.v[origin_id].caps;
}

u32 fl_origin_of_frame(FlVm *vm)
{
    FlOrigin o;
    Ed *ed;

    if (vm == NULL)
        return FL_ORIGIN_ID_NONE;
    ed = vm->ed;
    if (ed == NULL)
        return FL_ORIGIN_ID_NONE;
    o = fl_cap_origin(vm);
    if (o.principal_id != 0U)
        return o.principal_id;
    /* Slot zero is the one stable identity for the user's config.  Its
     * defining FlOrigin still carries the concrete path for imports and
     * diagnostics; registrations intentionally do not mint a second id
     * merely because --config selected a different file. */
    if (o.kind == (u8)FL_ORIGIN_CONFIG)
        return FL_ORIGIN_ID_CONFIG;
    return fl_origin_register(ed, (FlOriginKind)o.kind,
                              fl_origin_name(vm, &o), o.caps);
}

/* ---------------------------------------------------------------- */
/* Teardown masking (§7)                                            */
/* ---------------------------------------------------------------- */

void fl_origin_mask(Ed *ed, u32 origin_id)
{
    FlOriginReg *r = reg_of(ed);

    if (r == NULL || fl_origin_masked(ed, origin_id))
        return;
    if (r->nmasked == r->maskcap) {
        u32 want = r->maskcap == 0U ? 4U : r->maskcap * 2U;

        r->masked = (u32 *)yew_xreallocarray(r->masked, want,
                                             sizeof(*r->masked));
        r->maskcap = want;
    }
    r->masked[r->nmasked++] = origin_id;
}

void fl_origin_unmask(Ed *ed, u32 origin_id)
{
    FlOriginReg *r;
    u32 i;

    if (ed == NULL)
        return;
    r = &ed->origins;
    for (i = 0U; i < r->nmasked; i++) {
        if (r->masked[i] != origin_id)
            continue;
        r->masked[i] = r->masked[r->nmasked - 1U];
        r->nmasked--;
        return;
    }
}

bool fl_origin_masked(const Ed *ed, u32 origin_id)
{
    u32 i;

    if (ed == NULL)
        return false;
    for (i = 0U; i < ed->origins.nmasked; i++) {
        if (ed->origins.masked[i] == origin_id)
            return true;
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* The check (relocated from std.c, unchanged)                      */
/* ---------------------------------------------------------------- */

FlOrigin fl_cap_origin(const FlVm *vm)
{
    u32 i;

    /*
     * Walk DOWN from the innermost frame to the first non-builtin one.
     *
     * Builtin frames are transparent on purpose: `list.map(f, io.read)`
     * must check f's grants, not list's.  Stopping at the innermost
     * frame would let any stdlib function launder authority for its
     * caller, and reading frames[0] would give whatever started the
     * program -- both of which make §13's "a plugin calling a
     * user-config helper gains nothing" false.
     */
    for (i = vm->nframes; i-- > 0U; ) {
        FlOrigin o = vm->frames[i].cl->fn->origin;

        if (o.kind != (u8)FL_ORIGIN_BUILTIN)
            return o;
    }
    return vm->root_origin;        /* a native called by the host */
}

const char *fl_cap_name(u32 cap)
{
    switch (cap) {
    case FL_CAP_FS_READ:  return "fs.read";
    case FL_CAP_FS_WRITE: return "fs.write";
    case FL_CAP_SHELL:    return "shell";
    case FL_CAP_NET:      return "net";
    case FL_CAP_CLIPBOARD: return "clipboard";
    default:              return "capability";
    }
}

const char *fl_origin_name(const FlVm *vm, const FlOrigin *o)
{
    const char *p;

    /* The PATH when there is one: "denied to plugin" tells a user with
     * four plugins nothing, and the whole point of the message is that
     * they can go and look at the file. */
    p = o->path_id == 0U ? NULL : yew_intern_str(vm->in, o->path_id);
    if (p != NULL)
        return p;
    switch ((FlOriginKind)o->kind) {
    case FL_ORIGIN_BUILTIN:   return "a builtin";
    case FL_ORIGIN_CONFIG:    return "the user config";
    case FL_ORIGIN_WORKSPACE: return "the workspace config";
    case FL_ORIGIN_PLUGIN:    return "a plugin";
    case FL_ORIGIN_CLI:       return "the command line";
    case FL_ORIGIN_REPL:      return "the REPL";
    default:                  return "an unknown origin";
    }
}

bool fl_cap_check(FlVm *vm, u32 need)
{
    FlOrigin o = fl_cap_origin(vm);

    if ((o.caps & need) == need)
        return true;
#if YEW_WITH_PLUGINS
    if (o.kind == (u8)FL_ORIGIN_PLUGIN)
        return yew_plug_cap_check(vm, need & ~o.caps);
#endif
    return fl_raise(vm, "capability", "%s denied to %s",
                    fl_cap_name(need), fl_origin_name(vm, &o));
}
