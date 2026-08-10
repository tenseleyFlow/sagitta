/*
 * Sprint 31 deliverable 1: native registration, argument helpers, the
 * capability check.
 */
#include "fl/flapi.h"
#include "fl/flruntime.h"
#include "fl/std.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/bind.h"
#include "edit/option.h"
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
    &fl_mod_io, &fl_mod_re,
    /* Sprint 34: the editor API.  Registered unconditionally -- a
     * headless `yew fl` still sees `buf`, and its natives raise "no
     * editor" rather than reporting an undefined name for something that
     * merely has no editor attached (invariant 3). */
    &fl_mod_buf, &fl_mod_win, &fl_mod_cur, &fl_mod_span, &fl_mod_opt,
    &fl_mod_ed
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
        nat->name_id = yew_intern(vm->in, qual, (size_t)(qn < 0 ? 0 : qn));
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

/*
 * §9: `error(v)` raises.  A STRING argument becomes
 * `{kind: "user", msg: v}`; a MAP is raised as it stands, so a caller
 * may add fields of its own and a handler can read them.
 *
 * The one name in the prelude, because §9 spells it unqualified and
 * an `import` requirement would make the only documented way to raise
 * cost a line of ceremony in every file that validates anything.
 */
static bool fl_error(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n;
    (void)out;
    if (a[0].t == (u8)FL_MAP) {
        /*
         * Raised as it stands, but the kind still has to be a string:
         * `fl_trace_render` and every `e.kind` comparison read it as
         * one, and `{kind: 7}` would otherwise surface as "?" at the
         * top of a report with no hint of why.
         */
        FlValue k = FL_NIL_V;
        FlStr *key = fl_str_new(vm, "kind", 4U);

        if (!fl_map_get((FlMap *)a[0].as.o, FL_OBJ_V(FL_STR, key), &k))
            return fl_raise(vm, "type", "error: the map has no 'kind'");
        if (k.t != (u8)FL_STR)
            return fl_raise(vm, "type",
                            "error: 'kind' must be a str, found %s",
                            fl_type_name((FlType)k.t));
        vm->err = a[0];
        return false;
    }
    if (a[0].t != (u8)FL_STR)
        return fl_raise(vm, "type",
                        "error: argument 1 must be a str or a map, "
                        "found %s", fl_type_name((FlType)a[0].t));
    {
        const FlStr *s = (const FlStr *)a[0].as.o;

        return fl_raise(vm, "user", "%.*s", (int)s->len, s->b);
    }
}

static bool parse_named_register(FlVm *vm, FlValue value,
                                 const char *native, const FlStr **out)
{
    const FlStr *name = NULL;

    if (value.t != (u8)FL_STR)
        return fl_raise(vm, "type", "%s: argument 1 must be a str, found %s",
                        native, fl_type_name((FlType)value.t));
    name = (const FlStr *)value.as.o;
    if (name->len != 1U ||
        !((name->b[0] >= 'a' && name->b[0] <= 'z') ||
          (name->b[0] >= 'A' && name->b[0] <= 'Z')))
        return fl_raise(vm, "type", "%s: register must be one letter a-z/A-Z",
                        native);
    *out = name;
    return true;
}

static bool invoke_macro_command(FlVm *vm, const char *command,
                                 const FlStr *name, u32 count,
                                 bool count_given, FlValue *out)
{
    CmdCtx cx = {0};
    CmdId id;
    CmdStatus status;

    if (vm->ed == NULL)
        return fl_raise(vm, "handle", "%s: no editor is attached",
                        command);
    id = yew_cmd_lookup(command, (u32)strlen(command));
    if (id.v == 0U)
        return fl_raise(vm, "name", "%s is unavailable", command);
    cx.ed = vm->ed;
    cx.win = vm->ed->win;
    cx.count = count;
    cx.count_given = count_given;
    cx.sarg = name->b;
    cx.sarg_len = name->len;
    cx.source = fl_runtime_cmd_source(vm);
    status = yew_ed_invoke(vm->ed, id, &cx);
    if (status != YEW_CMD_OK)
        return fl_raise(vm, "user", "%s failed", command);
    *out = FL_NIL_V;
    return true;
}

static bool fl_record(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *name = NULL;
    (void)n;

    if (!parse_named_register(vm, a[0], "record", &name))
        return false;
    return invoke_macro_command(vm, "ed.macro.record", name, 1U, false,
                                out);
}

static bool fl_replay(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *name = NULL;
    i64 count = 1;

    if (!parse_named_register(vm, a[0], "replay", &name))
        return false;
    if (n == 2U) {
        if (a[1].t != (u8)FL_INT)
            return fl_raise(vm, "type",
                            "replay: argument 2 must be an int, found %s",
                            fl_type_name((FlType)a[1].t));
        count = a[1].as.i;
        if (count < 1 || count > (i64)YEW_COUNT_MAX)
            return fl_raise(vm, "type", "replay: count must be 1..%u",
                            (unsigned)YEW_COUNT_MAX);
    }
    return invoke_macro_command(vm, "ed.macro.replay", name, (u32)count,
                                n == 2U, out);
}

static void register_prelude_one(FlVm *vm, const char *name,
                                 FlNativeFn fn, u8 min_ar, u8 max_ar)
{
    FlNative *native = fl_gc_alloc(vm, sizeof(*native), FL_NATIVE);

    native->fn = fn;
    native->name_id = yew_intern(vm->in, name, strlen(name));
    native->min_ar = min_ar;
    native->max_ar = max_ar;
    native->caps = 0U;
    fl_gc_protect(vm, FL_OBJ_V(FL_NATIVE, native));
    (void)fl_map_set(vm, vm->prelude, FL_INT_V((i64)native->name_id),
                     FL_OBJ_V(FL_NATIVE, native));
    fl_gc_release(vm, 1U);
}

static void register_prelude(FlVm *vm)
{
    /*
     * Keyed by INTERNED ID, not by a string: GET_GLOBAL looks the
     * prelude up with the same constant it uses for globals, and
     * globals are keyed by id.  Keying this map by string would make
     * every lookup miss and `error` would stay invisible.
     */
    register_prelude_one(vm, "error", fl_error, 1U, 1U);
    register_prelude_one(vm, "on", fl_runtime_on, 2U, 2U);
    register_prelude_one(vm, "record", fl_record, 1U, 1U);
    register_prelude_one(vm, "replay", fl_replay, 1U, 2U);
    register_prelude_one(vm, "set", fl_api_set_options, 1U, 1U);
    register_prelude_one(vm, "bind", fl_bind_native, 3U, 4U);
    register_prelude_one(vm, "unbind", fl_unbind_native, 2U, 2U);
}

void fl_std_register(FlVm *vm)
{
    size_t i;

    fl_api_init();
    for (i = 0U; i < YEW_ARRAY_LEN(FL_MODULES); i++)
        register_module(vm, FL_MODULES[i]);
    register_prelude(vm);
}

u32 fl_std_list_natives(const FlVm *vm, Bytebuf *out)
{
    size_t i;
    u32 n = 0U;

    (void)vm;
    for (i = 0U; i < YEW_ARRAY_LEN(FL_MODULES); i++) {
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
    for (i = 0U; i < YEW_ARRAY_LEN(FL_MODULES); i++) {
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
    const char *nm = yew_intern_str(vm->in, vm->cur_native);

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

/* fl_cap_origin, fl_cap_name, fl_origin_name and fl_cap_check moved to
 * fl/origin.c in Sprint 34, which owns the origin registry they read. */
