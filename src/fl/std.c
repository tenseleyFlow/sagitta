/*
 * Sprint 31 deliverable 1: native registration, argument helpers, the
 * capability check.
 */
#include "fl/std.h"

#include <stdio.h>
#include <string.h>

#include "fl/gc.h"
#include "util/intern.h"

/* Each module's table lives in its own translation unit and is pulled
 * in here, so registration order is a single readable list rather than
 * a scatter of per-file init hooks. */
extern const FlModuleDef fl_mod_str;
extern const FlModuleDef fl_mod_list;
extern const FlModuleDef fl_mod_map;
extern const FlModuleDef fl_mod_math;
extern const FlModuleDef fl_mod_fmt;
extern const FlModuleDef fl_mod_io;
extern const FlModuleDef fl_mod_re;

/*
 * REGISTRATION ORDER IS PART OF THE CONTRACT.  fl_std_list_natives
 * walks this array and each table in turn, and s33's coverage ledger
 * diffs against that output -- so the order is spec §11's own listing
 * order, and a module added out of order shows up as a ledger diff
 * rather than a silent reshuffle.
 *
 * The array grows as each module lands rather than being pre-filled
 * with empties: a registered module with no functions would answer
 * `import` successfully and then fail every call with a bare "name"
 * error, which reads like a broken stdlib rather than an unfinished
 * one.
 */
static const FlModuleDef *const FL_MODULES[] = {
    &fl_mod_str, &fl_mod_list, &fl_mod_map, &fl_mod_math, &fl_mod_fmt,
    &fl_mod_io, &fl_mod_re
};

/* ---------------------------------------------------------------- */
/* Registration                                                     */
/* ---------------------------------------------------------------- */

static FlValue key_str(FlVm *vm, const char *s)
{
    return FL_OBJ_V(FL_STR, fl_str_new(vm, s, (u32)strlen(s)));
}

static void register_module(FlVm *vm, const FlModuleDef *md)
{
    FlMap *m = fl_map_new(vm);
    u32 i;

    /* Protected for the whole build: every fl_str_new and every native
     * below allocates, and until the map is in vm->builtins nothing
     * else points at it (gc.h rule 2). */
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, m));
    for (i = 0U; i < md->n; i++) {
        const FlNativeDef *d = &md->defs[i];
        FlNative *nat = fl_gc_alloc(vm, sizeof(*nat), FL_NATIVE);

        char qual[64];
        int qn = snprintf(qual, sizeof(qual), "%s.%s", md->name, d->name);

        nat->fn = d->fn;
        /* QUALIFIED: every message a native produces names it the way
         * the user wrote it.  "sqrt: argument 1 ..." makes a reader
         * hunt for which module's sqrt. */
        nat->name_id = sag_intern(vm->in, qual, (size_t)(qn < 0 ? 0 : qn));
        nat->min_ar = d->min_ar;
        nat->max_ar = d->max_ar;
        nat->caps = d->caps;
        (void)fl_map_set(vm, m, key_str(vm, d->name),
                         FL_OBJ_V(FL_NATIVE, nat));
    }
    for (i = 0U; i < md->nconsts; i++)
        (void)fl_map_set(vm, m, key_str(vm, md->consts[i].name),
                         md->consts[i].v);
    /*
     * Frozen once the fields are in.  The flag is what the VM's
     * FIELD_SET and INDEX_SET consult -- fl_map_set itself does not
     * check it, because the collector and the module loader both need
     * to write maps that a script may not.
     */
    m->h.oflags |= (u16)FL_OF_FROZEN;
    (void)fl_map_set(vm, vm->builtins, key_str(vm, md->name),
                     FL_OBJ_V(FL_MAP, m));
    fl_gc_release(vm, 1U);
}

void fl_std_register(FlVm *vm)
{
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(FL_MODULES); i++)
        register_module(vm, FL_MODULES[i]);
}

u32 fl_std_list_natives(const FlVm *vm, Bytebuf *out)
{
    size_t i;
    u32 n = 0U;

    (void)vm;
    for (i = 0U; i < SAG_ARRAY_LEN(FL_MODULES); i++) {
        const FlModuleDef *md = FL_MODULES[i];
        u32 k;

        for (k = 0U; k < md->n; k++) {
            bytebuf_append(out, md->name, strlen(md->name));
            bytebuf_push_u8(out, (u8)'.');
            bytebuf_append(out, md->defs[k].name, strlen(md->defs[k].name));
            bytebuf_push_u8(out, (u8)'\n');
            n++;
        }
    }
    return n;
}

const char *fl_std_sig(const char *qualified)
{
    size_t i;

    if (qualified == NULL)
        return NULL;
    for (i = 0U; i < SAG_ARRAY_LEN(FL_MODULES); i++) {
        const FlModuleDef *md = FL_MODULES[i];
        size_t mn = strlen(md->name);
        u32 k;

        if (strncmp(qualified, md->name, mn) != 0 || qualified[mn] != '.')
            continue;
        for (k = 0U; k < md->n; k++) {
            if (strcmp(qualified + mn + 1U, md->defs[k].name) == 0)
                return md->defs[k].sig;
        }
        return NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Argument helpers                                                 */
/* ---------------------------------------------------------------- */

void fl_std_set_current(FlVm *vm, u32 name_id) { vm->cur_native = name_id; }

/*
 * `want` is spelled with fl_type_name's vocabulary -- "str", not
 * "string" -- so an error and `type_of` never disagree about what a
 * type is called.  The sprint's illustrative example writes "string";
 * one vocabulary is worth the one-word deviation.
 */
static bool arg_type_err(FlVm *vm, u32 i, const char *want, FlValue got)
{
    const char *nm = sag_intern_str(vm->in, vm->cur_native);

    /* One-based, because that is how a user counts the arguments they
     * wrote -- argv[0] is "argument 1" in every message. */
    return fl_raise(vm, "type", "%s: argument %u must be %s, found %s",
                    nm == NULL ? "?" : nm, (unsigned)(i + 1U), want,
                    fl_type_name((FlType)got.t));
}

bool fl_arg_str(FlVm *vm, FlValue *a, u32 i, const FlStr **out)
{
    if (a[i].t != (u8)FL_STR)
        return arg_type_err(vm, i, "str", a[i]);
    *out = (const FlStr *)a[i].as.o;
    return true;
}

bool fl_arg_int(FlVm *vm, FlValue *a, u32 i, i64 *out)
{
    /* int ONLY.  A float index would have to round, and every rounding
     * rule is wrong for somebody -- refusing is the answer that never
     * silently moves a cursor one cluster left. */
    if (a[i].t != (u8)FL_INT)
        return arg_type_err(vm, i, "int", a[i]);
    *out = a[i].as.i;
    return true;
}

bool fl_arg_num(FlVm *vm, FlValue *a, u32 i, double *out)
{
    if (a[i].t == (u8)FL_INT) {
        *out = (double)a[i].as.i;
        return true;
    }
    if (a[i].t == (u8)FL_FLOAT) {
        *out = a[i].as.f;
        return true;
    }
    return arg_type_err(vm, i, "number", a[i]);
}

bool fl_arg_list(FlVm *vm, FlValue *a, u32 i, FlList **out)
{
    if (a[i].t != (u8)FL_LIST)
        return arg_type_err(vm, i, "list", a[i]);
    *out = (FlList *)a[i].as.o;
    return true;
}

bool fl_arg_map(FlVm *vm, FlValue *a, u32 i, FlMap **out)
{
    if (a[i].t != (u8)FL_MAP)
        return arg_type_err(vm, i, "map", a[i]);
    *out = (FlMap *)a[i].as.o;
    return true;
}

bool fl_arg_fn(FlVm *vm, FlValue *a, u32 i, FlValue *out)
{
    if (a[i].t != (u8)FL_CLOSURE && a[i].t != (u8)FL_NATIVE)
        return arg_type_err(vm, i, "fn", a[i]);
    *out = a[i];
    return true;
}

/* ---------------------------------------------------------------- */
/* Capabilities (spec §13)                                          */
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
    default:              return "capability";
    }
}

const char *fl_origin_name(const FlVm *vm, const FlOrigin *o)
{
    const char *p;

    /* The PATH when there is one: "denied to plugin" tells a user with
     * four plugins nothing, and the whole point of the message is that
     * they can go and look at the file. */
    p = o->path_id == 0U ? NULL : sag_intern_str(vm->in, o->path_id);
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
    return fl_raise(vm, "capability", "%s denied to %s",
                    fl_cap_name(need), fl_origin_name(vm, &o));
}
