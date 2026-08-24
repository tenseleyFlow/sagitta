#include "mod/plug/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/bind.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flapi.h"
#include "fl/flruntime.h"
#include "fl/gc.h"
#include "fl/handle.h"
#include "fl/origin.h"
#include "syn/defs.h"
#include "ui/message.h"
#include "util/intern.h"

static bool callable(FlValue value)
{
    return value.t == (u8)FL_CLOSURE || value.t == (u8)FL_NATIVE;
}

static bool callable_arity(FlValue value, u8 arity)
{
    if (value.t == (u8)FL_CLOSURE)
        return ((const FlClosure *)value.as.o)->fn->arity == arity;
    if (value.t == (u8)FL_NATIVE) {
        const FlNative *native = (const FlNative *)value.as.o;

        return native->min_ar <= arity &&
               (native->max_ar == 255U || native->max_ar >= arity);
    }
    return false;
}

static bool owner_args(FlVm *vm, FlValue *args, u32 nargs,
                       u32 visible_min, u32 visible_max, u32 *owner)
{
    u32 current;

    if (vm == NULL || vm->ed == NULL || owner == NULL || nargs == 0U ||
        args[0].t != (u8)FL_INT || args[0].as.i <= 0 ||
        (u64)args[0].as.i > (u64)UINT32_MAX)
        return vm == NULL ? false :
               fl_raise(vm, "handle", "invalid plugin context receiver");
    if (nargs - 1U < visible_min || nargs - 1U > visible_max)
        return fl_raise(vm, "arity", "invalid plugin context call");
    *owner = (u32)args[0].as.i;
    current = fl_origin_of_frame(vm);
    if (current != *owner)
        return fl_raise(vm, "capability",
                        "plugin context cannot be used by another origin");
    return true;
}

static FlStr *arg_string(FlVm *vm, FlValue value, const char *what)
{
    if (value.t != (u8)FL_STR) {
        (void)fl_raise(vm, "type", "%s must be a string", what);
        return NULL;
    }
    return (FlStr *)value.as.o;
}

static bool ctx_on(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    Plug *plug;
    FlStr *name;
    u32 owner;
    u32 event;
    u32 ledger_id;

    if (!owner_args(vm, args, nargs, 2U, 2U, &owner))
        return false;
    name = arg_string(vm, args[1], "ctx.on event");
    if (name == NULL || !callable(args[2]))
        return name == NULL ? false :
               fl_raise(vm, "type", "ctx.on callback must be a function");
    if (!fl_event_parse(name->b, name->len, &event))
        return fl_raise(vm, "name", "unknown editor event '%.*s'",
                        (int)name->len, name->b);
    plug = yew_plug_by_origin(vm->ed, owner);
    if (plug == NULL)
        return fl_raise(vm, "handle", "plugin context owner is gone");
    if (!yew_plug_event_allowed(vm->ed, owner, name->b, name->len))
        return fl_raise(vm, "capability",
                        "plugin \"%s\" did not declare event %.*s",
                        plug->mf.name_text,
                        (int)name->len, name->b);
    ledger_id = fl_hook_add(&vm->ed->hooks, owner, event, args[2]);
    (void)ledger_id;
    *out = FL_NIL_V;
    return true;
}

static void command_segment(const char *name, char out[33])
{
    size_t i;
    size_t len = strlen(name);

    if (len > 32U)
        len = 32U;
    for (i = 0U; i < len; i++)
        out[i] = name[i] == '-' ? '_' : name[i];
    out[len] = '\0';
}

static void command_push(PlugSys *sys, CmdId id, u32 owner, FlValue fn)
{
    PlugCmd *entry;
    u32 i;

    for (i = 0U; i < sys->ncmds; i++) {
        entry = &sys->cmds[i];
        if (!entry->active) {
            *entry = (PlugCmd){id, owner, fn, true};
            return;
        }
    }

    if (sys->ncmds == sys->capcmds) {
        u32 want = sys->capcmds == 0U ? 8U : sys->capcmds * 2U;

        if (want < sys->capcmds)
            YEW_BUG("plugin command table overflow");
        sys->cmds = yew_xreallocarray(sys->cmds, want,
                                      sizeof(*sys->cmds));
        sys->capcmds = want;
    }
    entry = &sys->cmds[sys->ncmds++];
    *entry = (PlugCmd){id, owner, fn, true};
}

static bool command_flag(const FlStr *name, u32 *flag)
{
    static const struct {
        const char *name;
        u32 flag;
    } table[] = {
        {"repeatable", YEW_CMD_REPEATABLE},
        {"takes_count", YEW_CMD_TAKES_COUNT},
        {"needs_win", YEW_CMD_NEEDS_WIN},
        {"changes_buffer", YEW_CMD_CHANGES_BUFFER},
        {"prompts", YEW_CMD_PROMPTS}
    };
    u32 i;

    for (i = 0U; i < YEW_ARRAY_LEN(table); i++)
        if (name->len == strlen(table[i].name) &&
            memcmp(name->b, table[i].name, name->len) == 0) {
            *flag = table[i].flag;
            return true;
        }
    return false;
}

static bool command_forbidden_flag(const FlStr *name)
{
    return (name->len == 10U &&
            memcmp(name->b, "recordable", 10U) == 0) ||
           (name->len == 8U && memcmp(name->b, "deferred", 8U) == 0);
}

static bool command_flags(FlVm *vm, FlValue value, u32 *out)
{
    FlMap *map;
    u32 flags = 0U;
    u32 i;

    if (value.t != (u8)FL_MAP)
        return fl_raise(vm, "type", "ctx.command opts must be a map");
    map = (FlMap *)value.as.o;
    for (i = 0U; i < map->n; i++) {
        const FlMapEnt *entry = &map->ent[i];
        FlStr *name;
        u32 flag;

        if (entry->dead)
            continue;
        if (entry->k.t != (u8)FL_STR || entry->v.t != (u8)FL_BOOL)
            return fl_raise(vm, "type",
                            "ctx.command opts must map names to booleans");
        name = (FlStr *)entry->k.as.o;
        if (!command_flag(name, &flag)) {
            if (command_forbidden_flag(name) && entry->v.as.b)
                return fl_raise(vm, "value",
                                "ctx.command flag '%.*s' is host-only",
                                (int)name->len, name->b);
            if (command_forbidden_flag(name))
                continue;
            return fl_raise(vm, "name", "unknown ctx.command flag '%.*s'",
                            (int)name->len, name->b);
        }
        if (entry->v.as.b)
            flags |= flag;
    }
    if ((flags & YEW_CMD_REPEATABLE) != 0U &&
        (flags & YEW_CMD_TAKES_COUNT) != 0U)
        return fl_raise(vm, "value",
                        "ctx.command repeatable and takes_count conflict");
    *out = flags;
    return true;
}

static CmdStatus plugin_command_invoke(CmdCtx *cx)
{
    PlugSys *sys;
    FlValue ignored = FL_NIL_V;
    u32 i;

    if (cx == NULL || cx->ed == NULL || cx->ed->plug == NULL)
        return YEW_CMD_ERR_STATE;
    sys = cx->ed->plug;
    for (i = 0U; i < sys->ncmds; i++) {
        PlugCmd *entry = &sys->cmds[i];
        FlValue fn;
        u32 origin_id;

        if (!entry->active || entry->id.v != cx->invoked_id.v)
            continue;
        origin_id = entry->origin_id;
        fn = entry->fn;
        if (fl_origin_masked(cx->ed, origin_id))
            return YEW_CMD_ERR_STATE;
        if (!fl_call_value_args(cx->ed->fl, fn, NULL, 0U,
                                cx->source, &ignored)) {
            yew_plug_hook_error(cx->ed, origin_id,
                                yew_fl_vm(cx->ed)->err);
            yew_plug_drain_pending(cx->ed);
            return YEW_CMD_ERR_STATE;
        }
        return YEW_CMD_OK;
    }
    return YEW_CMD_ERR_STATE;
}

static bool ctx_command(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    Plug *plug;
    FlStr *local;
    char plugin[33];
    char name[33];
    char error[128];
    CmdId id;
    u32 owner;
    u32 flags = 0U;

    if (!owner_args(vm, args, nargs, 2U, 3U, &owner))
        return false;
    local = arg_string(vm, args[1], "ctx.command name");
    if (local == NULL || local->len == 0U || local->len > 32U)
        return local == NULL ? false :
               fl_raise(vm, "name", "invalid plugin command name");
    if (!callable_arity(args[2], 0U))
        return fl_raise(vm, "arity",
                        "plugin command callback must take no arguments");
    if (nargs == 4U && !command_flags(vm, args[3], &flags))
        return false;
    plug = yew_plug_by_origin(vm->ed, owner);
    if (plug == NULL)
        return fl_raise(vm, "handle", "plugin context owner is gone");
    command_segment(plug->mf.name_text, plugin);
    (void)memcpy(name, local->b, local->len);
    name[local->len] = '\0';
    if (!yew_cmd_register_plugin_flags(plugin, name,
                                       plugin_command_invoke,
                                       "Run a plugin command", flags, &id,
                                       error, sizeof(error)))
        return fl_raise(vm, "name", "ctx.command: %s", error);
    command_push(vm->ed->plug, id, owner, args[2]);
    (void)fl_reg_add(&vm->ed->hooks.ledger, owner, REG_CMD, id.v);
    *out = FL_NIL_V;
    return true;
}

static bool ctx_bind(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    PlugSys *sys;
    u32 owner;
    bool ok;

    if (!owner_args(vm, args, nargs, 3U, 4U, &owner))
        return false;
    sys = vm->ed->plug;
    sys->ctx_origin = owner;
    sys->ctx_registration = true;
    ok = fl_bind_native(vm, args + 1, nargs - 1U, out);
    sys->ctx_registration = false;
    sys->ctx_origin = 0U;
    if (ok)
        *out = FL_NIL_V;
    return ok;
}

static bool ctx_set(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    Plug *plug;
    PlugSys *sys;
    u32 owner;
    bool ok;

    if (!owner_args(vm, args, nargs, 1U, 1U, &owner))
        return false;
    plug = yew_plug_by_origin(vm->ed, owner);
    if (plug == NULL)
        return fl_raise(vm, "handle", "plugin context owner is gone");
    sys = vm->ed->plug;
    sys->ctx_origin = owner;
    sys->ctx_registration = true;
    ok = fl_api_declare_plugin_options(vm, owner, plug->mf.name_text,
                                       (u32)strlen(plug->mf.name_text),
                                       args + 1, nargs - 1U, out);
    sys->ctx_registration = false;
    sys->ctx_origin = 0U;
    if (ok)
        *out = FL_NIL_V;
    return ok;
}

static void value_reg_push(PlugSys *sys, u32 handle, u32 owner,
                           RegKind kind, FlValue value);

static bool ctx_attr(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    PlugSys *sys;
    FlStr *name;
    u32 owner;
    u32 handle;
    u8 attr;

    if (!owner_args(vm, args, nargs, 1U, 1U, &owner))
        return false;
    name = arg_string(vm, args[1], "ctx.attr name");
    if (name == NULL)
        return false;
    if (!yew_syn_attr_id(name->b, name->len, &attr))
        return fl_raise(vm, "name", "unknown syntax attribute '%.*s'",
                        (int)name->len, name->b);
    sys = vm->ed->plug;
    handle = sys->next_handle++;
    if (handle == 0U)
        YEW_BUG("plugin registration handle overflow");
    value_reg_push(sys, handle, owner, REG_ATTR, FL_INT_V((i64)attr));
    (void)fl_reg_add(&vm->ed->hooks.ledger, owner, REG_ATTR, handle);
    *out = FL_INT_V((i64)attr);
    return true;
}

static void value_reg_push(PlugSys *sys, u32 handle, u32 owner,
                           RegKind kind, FlValue value)
{
    PlugValueReg *entry;
    u32 i;

    for (i = 0U; i < sys->nregs; i++) {
        entry = &sys->regs[i];
        if (!entry->active) {
            *entry = (PlugValueReg){handle, owner, (u8)kind, value, true};
            return;
        }
    }

    if (sys->nregs == sys->capregs) {
        u32 want = sys->capregs == 0U ? 8U : sys->capregs * 2U;

        if (want < sys->capregs)
            YEW_BUG("plugin value registry overflow");
        sys->regs = yew_xreallocarray(sys->regs, want,
                                      sizeof(*sys->regs));
        sys->capregs = want;
    }
    entry = &sys->regs[sys->nregs++];
    *entry = (PlugValueReg){handle, owner, (u8)kind, value, true};
}

static bool ctx_overlay(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    PlugSys *sys;
    u32 owner;
    u32 handle;

    if (!owner_args(vm, args, nargs, 1U, 1U, &owner))
        return false;
    if (!callable(args[1]))
        return fl_raise(vm, "type", "ctx.overlay expects a function");
    sys = vm->ed->plug;
    handle = sys->next_handle++;
    if (handle == 0U)
        YEW_BUG("plugin registration handle overflow");
    value_reg_push(sys, handle, owner, REG_OVERLAY, args[1]);
    (void)fl_reg_add(&vm->ed->hooks.ledger, owner, REG_OVERLAY, handle);
    *out = FL_NIL_V;
    return true;
}

static bool ctx_msg(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    Plug *plug;
    FlStr *message;
    MsgSev severity = YEW_MSG_INFO;
    u32 owner;

    if (!owner_args(vm, args, nargs, 1U, 2U, &owner))
        return false;
    message = arg_string(vm, args[1], "ctx.msg text");
    if (message == NULL)
        return false;
    if (nargs == 3U) {
        FlStr *level = arg_string(vm, args[2], "ctx.msg level");

        if (level == NULL)
            return false;
        if (level->len == 4U && memcmp(level->b, "info", 4U) == 0)
            severity = YEW_MSG_INFO;
        else if (level->len == 4U && memcmp(level->b, "warn", 4U) == 0)
            severity = YEW_MSG_WARN;
        else if (level->len == 5U && memcmp(level->b, "error", 5U) == 0)
            severity = YEW_MSG_ERROR;
        else
            return fl_raise(vm, "value",
                            "ctx.msg level must be info, warn, or error");
    }
    plug = yew_plug_by_origin(vm->ed, owner);
    if (plug == NULL)
        return fl_raise(vm, "handle", "plugin context owner is gone");
    yew_msg(vm->ed, severity, "[%s] %.*s", plug->mf.name_text,
            (int)message->len, message->b);
    *out = FL_NIL_V;
    return true;
}

static bool ctx_ws_root(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    const char *root;
    u32 owner;

    if (!owner_args(vm, args, nargs, 0U, 0U, &owner))
        return false;
    (void)owner;
    root = yew_ws_root(vm->ed);
    *out = FL_OBJ_V(FL_STR, fl_str_new(vm, root, (u32)strlen(root)));
    return true;
}

static bool ctx_ws_state_dir(FlVm *vm, FlValue *args, u32 nargs,
                             FlValue *out)
{
    u32 owner;

    if (!owner_args(vm, args, nargs, 0U, 0U, &owner))
        return false;
    (void)owner;
    if (vm->ed->clean || vm->ed->headless || !vm->ed->state.ready ||
        vm->ed->state.key.stateless) {
        *out = FL_NIL_V;
        return true;
    }
    *out = FL_OBJ_V(FL_STR,
                    fl_str_new(vm, vm->ed->state.key.dir,
                               (u32)strlen(vm->ed->state.key.dir)));
    return true;
}

static bool map_put(FlVm *vm, FlMap *map, const char *name, FlValue value)
{
    FlValue key;
    bool ok;

    fl_gc_protect(vm, value);
    key = FL_OBJ_V(FL_STR, fl_str_new(vm, name, (u32)strlen(name)));
    fl_gc_protect(vm, key);
    ok = fl_map_set(vm, map, key, value);
    fl_gc_release(vm, 2U);
    return ok;
}

static bool map_native(FlVm *vm, FlMap *map, const char *field,
                       const char *qualified, FlNativeFn fn, u8 min, u8 max,
                       u32 owner)
{
    FlNative *native = fl_gc_alloc(vm, sizeof(*native), FL_NATIVE);
    FlValue value;
    bool ok;

    native->fn = fn;
    native->name_id = yew_intern(vm->in, qualified, strlen(qualified));
    native->min_ar = min;
    native->max_ar = max;
    native->has_recv = 1U;
    native->rsv = 0U;
    native->caps = 0U;
    native->recv = FL_INT_V((i64)owner);
    value = FL_OBJ_V(FL_NATIVE, native);
    fl_gc_protect(vm, value);
    ok = map_put(vm, map, field, value);
    fl_gc_release(vm, 1U);
    return ok;
}

static bool builtin(FlVm *vm, const char *name, FlValue *out)
{
    FlValue key = FL_OBJ_V(FL_STR,
                           fl_str_new(vm, name, (u32)strlen(name)));

    return fl_map_get(vm->builtins, key, out);
}

bool yew_plug_context_build(Ed *ed, Plug *plug, FlValue *out)
{
    FlVm *vm;
    FlMap *ctx;
    FlMap *ws;
    FlValue value;
    bool ok = true;

    if (ed == NULL || plug == NULL || out == NULL ||
        (vm = yew_fl_vm(ed)) == NULL)
        return false;
    ctx = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, ctx));
    ok = map_native(vm, ctx, "on", "ctx.on", ctx_on, 2U, 2U,
                    plug->origin_id) &&
         map_native(vm, ctx, "command", "ctx.command", ctx_command, 2U, 3U,
                    plug->origin_id) &&
         map_native(vm, ctx, "bind", "ctx.bind", ctx_bind, 3U, 4U,
                    plug->origin_id) &&
         map_native(vm, ctx, "set", "ctx.set", ctx_set, 1U, 1U,
                    plug->origin_id) &&
         map_native(vm, ctx, "attr", "ctx.attr", ctx_attr, 1U, 1U,
                    plug->origin_id) &&
         map_native(vm, ctx, "overlay", "ctx.overlay", ctx_overlay, 1U, 1U,
                    plug->origin_id) &&
         map_native(vm, ctx, "msg", "ctx.msg", ctx_msg, 1U, 2U,
                    plug->origin_id);
    if (ok && builtin(vm, "buf", &value))
        ok = map_put(vm, ctx, "buf", value);
    else if (ok)
        ok = false;
    if (ok && builtin(vm, "win", &value))
        ok = map_put(vm, ctx, "win", value);
    else if (ok)
        ok = false;
    ws = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, ws));
    ok = ok && map_native(vm, ws, "root", "ctx.ws.root", ctx_ws_root,
                          0U, 0U, plug->origin_id) &&
         map_native(vm, ws, "state_dir", "ctx.ws.state_dir",
                    ctx_ws_state_dir, 0U, 0U, plug->origin_id);
    ws->h.oflags |= (u16)FL_OF_FROZEN;
    ok = ok && map_put(vm, ctx, "ws", FL_OBJ_V(FL_MAP, ws));
    fl_gc_release(vm, 1U);
    ok = ok && map_put(vm, ctx, "name",
                       FL_OBJ_V(FL_STR,
                                fl_str_new(vm, plug->mf.name_text,
                                           (u32)strlen(plug->mf.name_text))));
    ctx->h.oflags |= (u16)FL_OF_FROZEN;
    *out = ok ? FL_OBJ_V(FL_MAP, ctx) : FL_NIL_V;
    fl_gc_release(vm, 1U);
    return ok;
}

void yew_plug_mark(FlVm *vm, void *ctx)
{
    PlugSys *sys = ctx;
    u32 i;

    if (vm == NULL || sys == NULL)
        return;
    for (i = 0U; i < sys->n; i++) {
        Plug *plug = sys->v[i];

        if (plug != NULL && plug->rooted)
            fl_gc_mark_value(vm, plug->module);
    }
    for (i = 0U; i < sys->ncmds; i++)
        if (sys->cmds[i].active)
            fl_gc_mark_value(vm, sys->cmds[i].fn);
    for (i = 0U; i < sys->nregs; i++)
        if (sys->regs[i].active)
            fl_gc_mark_value(vm, sys->regs[i].value);
}

bool yew_plug_registration_remove(Ed *ed, const FlRegistration *reg)
{
    PlugSys *sys;
    u32 i;

    if (ed == NULL || reg == NULL || ed->plug == NULL)
        return false;
    sys = ed->plug;
    if (reg->kind == (u8)REG_CMD) {
        for (i = 0U; i < sys->ncmds; i++) {
            PlugCmd *entry = &sys->cmds[i];
            bool removed;

            if (!entry->active || entry->id.v != reg->handle)
                continue;
            removed = yew_cmd_unregister(entry->id);
            entry->active = false;
            entry->fn = FL_NIL_V;
            while (sys->ncmds != 0U &&
                   !sys->cmds[sys->ncmds - 1U].active)
                sys->ncmds--;
            return removed;
        }
        return false;
    }
    if (reg->kind == (u8)REG_OVERLAY || reg->kind == (u8)REG_ATTR ||
        reg->kind == (u8)REG_TIMER) {
        for (i = 0U; i < sys->nregs; i++) {
            PlugValueReg *entry = &sys->regs[i];

            if (!entry->active || entry->handle != reg->handle ||
                entry->kind != reg->kind)
                continue;
            entry->active = false;
            entry->value = FL_NIL_V;
            while (sys->nregs != 0U &&
                   !sys->regs[sys->nregs - 1U].active)
                sys->nregs--;
            return true;
        }
    }
    return false;
}

void yew_plug_drop_origin_regs(Ed *ed, u32 origin_id)
{
    FlRegLedger *ledger;
    u32 i;

    if (ed == NULL)
        return;
    ledger = &ed->hooks.ledger;
    for (i = ledger->n; i != 0U; i--) {
        FlRegistration *reg = &ledger->v[i - 1U];

        if (!reg->active || reg->origin_id != origin_id)
            continue;
        if (reg->kind == (u8)REG_HOOK)
            (void)fl_hook_remove(&ed->hooks, i);
        else if (reg->kind == (u8)REG_BIND)
            (void)yew_bind_remove(ed, i);
        else if (reg->kind == (u8)REG_OPTION)
            (void)yew_opt_remove(ed, i);
        else {
            (void)yew_plug_registration_remove(ed, reg);
            (void)fl_reg_remove(ledger, i);
        }
    }
}

bool yew_plug_event_allowed(Ed *ed, u32 origin_id, const char *name,
                            size_t len)
{
    Plug *plug = yew_plug_by_origin(ed, origin_id);
    u32 i;

    if (plug == NULL || name == NULL)
        return false;
    for (i = 0U; i < plug->mf.nevents; i++)
        if (strlen(plug->mf.event_names[i]) == len &&
            memcmp(plug->mf.event_names[i], name, len) == 0)
            return true;
    return false;
}

bool yew_plug_ctx_registration_allowed(const Ed *ed, u32 origin_id)
{
    return ed != NULL && ed->plug != NULL &&
           ed->plug->ctx_registration && ed->plug->ctx_origin == origin_id;
}
