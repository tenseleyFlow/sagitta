#define _POSIX_C_SOURCE 200809L

#include "mod/plug/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/bind.h"
#include "edit/ed.h"
#include "fl/flconf.h"
#include "fl/flruntime.h"
#include "fl/gc.h"
#include "fl/module.h"
#include "fl/origin.h"
#include "fl/trace.h"
#include "term/input.h"
#include "ui/message.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"

enum { PLUG_ERROR_LIMIT_DEFAULT = 5U, PLUG_PICK_TEXT = 512U };

static u32 error_limit(const Ed *ed)
{
    if (ed == NULL || ed->plug_error_limit == 0U)
        return PLUG_ERROR_LIMIT_DEFAULT;
    return ed->plug_error_limit;
}

void yew_plug_error_limit_set(Ed *ed, u32 limit)
{
    if (ed == NULL)
        return;
    ed->plug_error_limit = limit == 0U ? PLUG_ERROR_LIMIT_DEFAULT : limit;
}

static char *plug_strdup(const char *text)
{
    size_t len = text == NULL ? 0U : strlen(text);
    char *copy = yew_xmalloc(len + 1U);

    if (len != 0U)
        (void)memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static char *entry_path(const Plug *plug)
{
    size_t ndir = strlen(plug->mf.dir);
    size_t nentry = strlen(plug->mf.entry);
    bool slash = ndir != 0U && plug->mf.dir[ndir - 1U] != '/';
    char *path = yew_xmalloc(ndir + (slash ? 1U : 0U) + nentry + 1U);
    size_t at = 0U;

    (void)memcpy(path + at, plug->mf.dir, ndir);
    at += ndir;
    if (slash)
        path[at++] = '/';
    (void)memcpy(path + at, plug->mf.entry, nentry + 1U);
    return path;
}

static void set_last_error(Plug *plug, const char *text)
{
    free(plug->last_error);
    plug->last_error = plug_strdup(text == NULL ? "plugin error" : text);
}

static void capture_error(Ed *ed, Plug *plug, FlValue error)
{
    Bytebuf trace;

    if (ed == NULL || plug == NULL)
        return;
    bytebuf_init(&trace);
    if (error.t == (u8)FL_MAP)
        fl_trace_render(yew_fl_vm(ed), error, &trace);
    if (trace.len == 0U) {
        const char *diag = fl_runtime_last_diag(ed->fl, NULL);

        if (diag != NULL)
            bytebuf_append(&trace, diag, strlen(diag));
    }
    if (trace.len == 0U)
        bytebuf_append(&trace, "plugin error", sizeof("plugin error") - 1U);
    bytebuf_push_u8(&trace, 0U);
    set_last_error(plug, (const char *)trace.data);
    bytebuf_free(&trace);
}

static const char *first_line(const char *text, char *out, size_t cap)
{
    size_t n = 0U;

    if (cap == 0U)
        return "plugin error";
    while (text != NULL && text[n] != '\0' && text[n] != '\n' &&
           n + 1U < cap) {
        out[n] = text[n];
        n++;
    }
    out[n] = '\0';
    return n == 0U ? "plugin error" : out;
}

Plug *yew_plug_by_origin(Ed *ed, u32 origin_id)
{
    u32 i;

    if (ed == NULL || ed->plug == NULL)
        return NULL;
    for (i = 0U; i < ed->plug->n; i++) {
        Plug *plug = ed->plug->v[i];

        if (plug != NULL && plug->origin_id == origin_id)
            return plug;
    }
    return NULL;
}

const char *yew_plug_state_name(PlugState state)
{
    switch (state) {
    case PLUG_DISCOVERED: return "discovered";
    case PLUG_LOADED: return "loaded";
    case PLUG_ENABLED: return "enabled";
    case PLUG_DISABLED: return "disabled";
    case PLUG_ERROR: return "error";
    case PLUG_BLOCKED: return "blocked";
    case PLUG_SHADOWED: return "shadowed";
    default: return "error";
    }
}

const char *yew_plug_source_name(PlugSource source)
{
    switch (source) {
    case PLUG_SOURCE_DATA: return "data";
    case PLUG_SOURCE_CONFIG: return "config";
    case PLUG_SOURCE_WORKSPACE: return "workspace";
    default: return "data";
    }
}

u32 yew_plug_count(const Ed *ed)
{
    return ed == NULL || ed->plug == NULL ? 0U : ed->plug->n;
}

Plug *yew_plug_at(Ed *ed, u32 index)
{
    return ed == NULL || ed->plug == NULL || index >= ed->plug->n ?
           NULL : ed->plug->v[index];
}

Plug *yew_plug_find(Ed *ed, const char *name)
{
    u32 i;

    if (ed == NULL || ed->plug == NULL || name == NULL)
        return NULL;
    for (i = ed->plug->n; i != 0U; i--) {
        Plug *plug = ed->plug->v[i - 1U];

        if (plug != NULL && plug->winner && plug->mf.name_text != NULL &&
            strcmp(plug->mf.name_text, name) == 0)
            return plug;
    }
    return NULL;
}

static Plug *find_len(Ed *ed, const char *name, size_t len)
{
    u32 i;

    if (ed == NULL || ed->plug == NULL || name == NULL)
        return NULL;
    for (i = ed->plug->n; i != 0U; i--) {
        Plug *plug = ed->plug->v[i - 1U];

        if (plug != NULL && plug->winner && plug->mf.name_text != NULL &&
            strlen(plug->mf.name_text) == len &&
            memcmp(plug->mf.name_text, name, len) == 0)
            return plug;
    }
    return NULL;
}

static void ensure_provider(Ed *ed)
{
    FlVm *vm;

    if (ed == NULL || ed->plug == NULL || ed->plug->gc_registered ||
        (vm = yew_fl_vm(ed)) == NULL)
        return;
    fl_gc_root_provider(vm, yew_plug_mark, ed->plug);
    ed->plug->gc_registered = true;
}

static void fire_lifecycle(Ed *ed, FlEvent event, Plug *plug)
{
    FlVm *vm = yew_fl_vm(ed);
    FlValue name;

    if (vm == NULL || plug == NULL || plug->mf.name_text == NULL)
        return;
    name = FL_OBJ_V(FL_STR,
                    fl_str_new(vm, plug->mf.name_text,
                               (u32)strlen(plug->mf.name_text)));
    yew_fl_hook_fire(ed, event, &name, 1U);
}

static bool callable_arity_one(FlValue value)
{
    if (value.t == (u8)FL_CLOSURE)
        return ((const FlClosure *)value.as.o)->fn->arity == 1U;
    if (value.t == (u8)FL_NATIVE) {
        const FlNative *native = (const FlNative *)value.as.o;

        return native->min_ar <= 1U &&
               (native->max_ar == 255U || native->max_ar >= 1U);
    }
    return false;
}

#ifndef NDEBUG
static u32 active_hooks(const Ed *ed)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < ed->hooks.n; i++)
        if (ed->hooks.v[i].active)
            active++;
    return active;
}

static u32 active_ledger(const Ed *ed)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < ed->hooks.ledger.n; i++)
        if (ed->hooks.ledger.v[i].active)
            active++;
    return active;
}

static u32 active_plugin_commands(const PlugSys *sys)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < sys->ncmds; i++)
        if (sys->cmds[i].active)
            active++;
    return active;
}

static u32 active_plugin_values(const PlugSys *sys)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < sys->nregs; i++)
        if (sys->regs[i].active)
            active++;
    return active;
}

static void residue_snapshot(Ed *ed, Plug *plug)
{
    plug->residue_before[0] = yew_cmd_active_count();
    plug->residue_before[1] = yew_bind_active_count(ed);
    plug->residue_before[2] = active_hooks(ed);
    plug->residue_before[3] = active_ledger(ed);
    plug->residue_before[4] = active_plugin_commands(ed->plug);
    plug->residue_before[5] = active_plugin_values(ed->plug);
    plug->residue_snapshot = true;
}

static void residue_assert(Ed *ed, Plug *plug)
{
    u32 after[6];
    u32 i;

    if (!plug->residue_snapshot)
        return;
    after[0] = yew_cmd_active_count();
    after[1] = yew_bind_active_count(ed);
    after[2] = active_hooks(ed);
    after[3] = active_ledger(ed);
    after[4] = active_plugin_commands(ed->plug);
    after[5] = active_plugin_values(ed->plug);
    for (i = 0U; i < YEW_ARRAY_LEN(after); i++)
        if (after[i] != plug->residue_before[i])
            YEW_BUG("plugin teardown left registry residue (%u: %u != %u)",
                    (unsigned)i, (unsigned)after[i],
                    (unsigned)plug->residue_before[i]);
    plug->residue_snapshot = false;
}
#else
#define residue_snapshot(ed_, plug_) ((void)0)
#define residue_assert(ed_, plug_) ((void)0)
#endif

static void teardown(Ed *ed, Plug *plug, PlugState state, bool notify)
{
    FlVm *vm;

    if (ed == NULL || plug == NULL)
        return;
    vm = yew_fl_vm(ed);
    fl_origin_mask(ed, plug->origin_id);
    yew_plug_drop_origin_regs(ed, plug->origin_id);
    if (vm != NULL)
        fl_module_drop_principal(vm, plug->origin_id);
    plug->module = FL_NIL_V;
    plug->rooted = false;
    if (vm != NULL)
        fl_gc_collect(vm);
    residue_assert(ed, plug);
    plug->st = state;
    if (notify)
        fire_lifecycle(ed, FL_EV_PLUG_DISABLE, plug);
    fl_origin_unmask(ed, plug->origin_id);
}

bool yew_plug_enable(Ed *ed, Plug *plug, DiagCtx *dc)
{
    FlVm *vm;
    FlOrigin origin;
    FlValue init;
    FlValue ctx;
    FlValue ignored = FL_NIL_V;
    FlValue key;
    char *path;
    bool ok;

    (void)dc;
    if (ed == NULL || plug == NULL || !plug->winner ||
        plug->st == PLUG_BLOCKED || plug->st == PLUG_SHADOWED)
        return false;
    if (plug->st == PLUG_ENABLED)
        return true;
    vm = yew_fl_vm(ed);
    if (vm == NULL || ed->plug == NULL)
        return false;
    if (plug->mf.entry == NULL || plug->mf.dir == NULL) {
        plug->st = PLUG_ERROR;
        set_last_error(plug, "invalid plugin manifest");
        return false;
    }
    ensure_provider(ed);
    teardown(ed, plug, PLUG_DISCOVERED, false);
    residue_snapshot(ed, plug);
    plug->err_count = 0U;
    path = entry_path(plug);
    origin = (FlOrigin){
        (u8)FL_ORIGIN_PLUGIN,
        yew_intern(vm->in, path, strlen(path)),
        0U,
        plug->origin_id
    };
    ok = fl_module_load_path(vm, path, origin, &plug->module);
    free(path);
    if (!ok) {
        capture_error(ed, plug, vm->err);
        teardown(ed, plug, PLUG_ERROR, false);
        return false;
    }
    plug->rooted = true;
    plug->st = PLUG_LOADED;
    key = FL_OBJ_V(FL_STR, fl_str_new(vm, "init", 4U));
    if (plug->module.t != (u8)FL_MAP ||
        !fl_map_get((FlMap *)plug->module.as.o, key, &init)) {
        set_last_error(plug, "plugin module must export fn init(ctx)");
        teardown(ed, plug, PLUG_ERROR, false);
        return false;
    }
    if (!callable_arity_one(init)) {
        set_last_error(plug, "plugin init must be a function taking ctx");
        teardown(ed, plug, PLUG_ERROR, false);
        return false;
    }
    if (!yew_plug_context_build(ed, plug, &ctx)) {
        capture_error(ed, plug, vm->err);
        teardown(ed, plug, PLUG_ERROR, false);
        return false;
    }
    ok = fl_call_value_args(ed->fl, init, &ctx, 1U, YEW_SRC_FLETCH,
                            &ignored);
    if (!ok) {
        capture_error(ed, plug, vm->err);
        if (ed->plug->prompt.active && ed->plug->prompt.plug == plug)
            ed->plug->prompt.retry_enable = true;
        teardown(ed, plug, PLUG_ERROR, false);
        return false;
    }
    free(plug->last_error);
    plug->last_error = NULL;
    plug->st = PLUG_ENABLED;
    fl_origin_mask(ed, plug->origin_id);
    fire_lifecycle(ed, FL_EV_PLUG_ENABLE, plug);
    fl_origin_unmask(ed, plug->origin_id);
    return true;
}

bool yew_plug_disable(Ed *ed, Plug *plug)
{
    if (ed == NULL || plug == NULL || !plug->winner)
        return false;
    if (plug->st == PLUG_SHADOWED || plug->st == PLUG_BLOCKED)
        return false;
    if (plug->st == PLUG_DISABLED && !plug->rooted)
        return true;
    teardown(ed, plug, PLUG_DISABLED, plug->st == PLUG_ENABLED);
    return true;
}

bool yew_plug_reload(Ed *ed, Plug *plug, DiagCtx *dc)
{
    PlugManifest fresh;
    bool enabled;
    u32 i;

    if (ed == NULL || ed->plug == NULL || plug == NULL ||
        plug->st == PLUG_BLOCKED || plug->st == PLUG_SHADOWED)
        return false;
    enabled = plug->st == PLUG_ENABLED;
    teardown(ed, plug, PLUG_DISABLED, enabled);
    (void)memset(&fresh, 0, sizeof(fresh));
    if (!yew_plug_manifest_read(&ed->plug->arena, plug->mf.dir,
                                &fresh, dc)) {
        plug->st = PLUG_ERROR;
        set_last_error(plug, "plugin manifest reload failed");
        return false;
    }
    fresh.name = yew_intern_cstr(&ed->interner, fresh.name_text);
    for (i = 0U; i < fresh.nevents; i++)
        fresh.events[i] = yew_intern_cstr(&ed->interner,
                                          fresh.event_names[i]);
    plug->mf = fresh;
    plug->st = enabled ? PLUG_DISCOVERED : PLUG_DISABLED;
    return !enabled || yew_plug_enable(ed, plug, dc);
}

bool yew_plug_enable_desired(Ed *ed, DiagCtx *dc)
{
    bool ok = true;
    u32 i;

    if (ed == NULL)
        return false;
    if (ed->plug == NULL)
        return true;
    ensure_provider(ed);
    for (i = 0U; i < ed->plug->n; i++) {
        Plug *plug = ed->plug->v[i];

        if (plug == NULL || !plug->winner || plug->st != PLUG_DISCOVERED)
            continue;
        if (!yew_plug_enable(ed, plug, dc))
            ok = false;
    }
    return ok;
}

bool yew_plug_boot(Ed *ed)
{
    bool discovered;
    bool enabled;

    if (ed == NULL)
        return false;
    if (ed->clean)
        return true;
    if (ed->plug != NULL && ed->plug->booted)
        return true;
    discovered = yew_plug_discover(ed, NULL);
    if (!discovered)
        return false;
    if (ed->plug == NULL)
        return true;
    ensure_provider(ed);
    enabled = yew_plug_enable_desired(ed, NULL);
    ed->plug->booted = true;
    return enabled;
}

void yew_plug_free(Ed *ed)
{
    PlugSys *sys;
    FlVm *vm;
    u32 i;

    if (ed == NULL || ed->plug == NULL)
        return;
    sys = ed->plug;
    for (i = sys->n; i != 0U; i--) {
        Plug *plug = sys->v[i - 1U];

        if (plug != NULL && (plug->rooted || plug->st == PLUG_ENABLED ||
                             plug->st == PLUG_LOADED))
            teardown(ed, plug, PLUG_DISABLED, false);
    }
    vm = yew_fl_vm(ed);
    if (sys->gc_registered && vm != NULL)
        fl_gc_root_provider_remove(vm, yew_plug_mark, sys);
    for (i = 0U; i < sys->n; i++) {
        if (sys->v[i] != NULL) {
            free(sys->v[i]->last_error);
            free(sys->v[i]);
        }
    }
    free(sys->v);
    free(sys->cmds);
    free(sys->regs);
    free(sys->pick_items);
    free(sys->pick_text);
    arena_free_all(&sys->arena);
    free(sys);
    ed->plug = NULL;
}

void yew_plug_hook_error(Ed *ed, u32 origin_id, FlValue error)
{
    Plug *plug = yew_plug_by_origin(ed, origin_id);
    char line[256];
    u32 limit;

    if (plug == NULL || plug->st != PLUG_ENABLED)
        return;
    capture_error(ed, plug, error);
    plug->err_count++;
    limit = error_limit(ed);
    yew_log(YEW_LOG_ERROR, "plugin \"%s\" error (%u/%u): %s",
            plug->mf.name_text, (unsigned)plug->err_count,
            (unsigned)limit, plug->last_error);
    yew_msg(ed, YEW_MSG_WARN,
            "plugin \"%s\" error (%u/%u): %s — :PlugInfo %s for trace",
            plug->mf.name_text, (unsigned)plug->err_count,
            (unsigned)limit,
            first_line(plug->last_error, line, sizeof(line)),
            plug->mf.name_text);
    if (plug->err_count >= limit &&
        ed->plug->pending_disable_origin == 0U)
        ed->plug->pending_disable_origin = origin_id;
}

void yew_plug_drain_pending(Ed *ed)
{
    PlugSys *sys;
    u32 origin;
    u32 limit;
    Plug *plug;

    if (ed == NULL || ed->plug == NULL)
        return;
    sys = ed->plug;
    if (sys->draining || sys->pending_disable_origin == 0U)
        return;
    sys->draining = true;
    limit = error_limit(ed);
    origin = sys->pending_disable_origin;
    sys->pending_disable_origin = 0U;
    plug = yew_plug_by_origin(ed, origin);
    if (plug != NULL) {
        (void)yew_plug_disable(ed, plug);
        yew_msg(ed, YEW_MSG_WARN, "plugin \"%s\" disabled after %u errors",
                plug->mf.name_text, (unsigned)limit);
    }
    sys->draining = false;
}

static const char *picker_glyph(PlugState state)
{
    switch (state) {
    case PLUG_ENABLED: return "●";
    case PLUG_ERROR: return "✗";
    case PLUG_BLOCKED: return "⊘";
    default: return "○";
    }
}

static void picker_build(Ed *ed)
{
    PlugSys *sys = ed->plug;
    u32 i;

    free(sys->pick_items);
    free(sys->pick_text);
    sys->pick_items = NULL;
    sys->pick_text = NULL;
    sys->pick_n = 0U;
    if (sys->n == 0U)
        return;
    sys->pick_items = yew_xcalloc(sys->n, sizeof(*sys->pick_items));
    sys->pick_text = yew_xcalloc(sys->n, PLUG_PICK_TEXT);
    for (i = 0U; i < sys->n; i++) {
        Plug *plug = sys->v[i];
        char *slot = sys->pick_text + (size_t)i * PLUG_PICK_TEXT;
        char *detail = slot + PLUG_PICK_TEXT / 2U;

        (void)snprintf(slot, PLUG_PICK_TEXT / 2U, "%s %s %s",
                       picker_glyph(plug->st), plug->mf.name_text,
                       plug->mf.version == NULL ? "" : plug->mf.version);
        (void)snprintf(detail, PLUG_PICK_TEXT / 2U, "%s",
                       plug->mf.desc == NULL ? "" : plug->mf.desc);
        sys->pick_items[sys->pick_n++] =
            (PickItem){slot, detail, (i32)i, 0U};
    }
}

static const PickItem *picker_items(void *ctx, u32 *n)
{
    PlugSys *sys = ctx;

    *n = sys == NULL ? 0U : sys->pick_n;
    return sys == NULL ? NULL : sys->pick_items;
}

static void plugin_info(Ed *ed, Plug *plug)
{
    yew_msg(ed, plug->st == PLUG_ERROR ? YEW_MSG_ERROR : YEW_MSG_INFO,
            "%s %s — %s — %s%s%s",
            plug->mf.name_text,
            plug->mf.version == NULL ? "" : plug->mf.version,
            yew_plug_state_name(plug->st),
            plug->mf.desc == NULL ? "" : plug->mf.desc,
            plug->last_error == NULL ? "" : " — ",
            plug->last_error == NULL ? "" : plug->last_error);
}

static bool picker_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    PlugSys *sys = ctx;
    Plug *plug;

    (void)how;
    if (sys == NULL || payload < 0 || (u32)payload >= sys->n)
        return false;
    plug = sys->v[(u32)payload];
    if (plug->st == PLUG_ENABLED) {
        if (!yew_plug_disable(ed, plug))
            return false;
        (void)yew_config_plugin_set_desired(
            ed, plug->mf.name_text, YEW_PLUGIN_DESIRED_DISABLED);
    } else {
        if (!yew_plug_enable(ed, plug, NULL))
            return false;
        (void)yew_config_plugin_set_desired(
            ed, plug->mf.name_text, YEW_PLUGIN_DESIRED_ENABLED);
    }
    return true;
}

static bool picker_action(Ed *ed, void *ctx, i32 payload, const Key *key)
{
    PlugSys *sys = ctx;
    Plug *plug;

    if (sys == NULL || key == NULL || payload < 0 ||
        (u32)payload >= sys->n || key->ntext != 1U)
        return false;
    plug = sys->v[(u32)payload];
    if (key->text[0] == (u8)'r') {
        (void)yew_plug_reload(ed, plug, NULL);
        picker_build(ed);
        yew_picker_refilter(ed);
        return true;
    }
    if (key->text[0] == (u8)'i') {
        plugin_info(ed, plug);
        return true;
    }
    return false;
}

CmdStatus yew_plug_cmd_list(CmdCtx *cx)
{
    PickerSpec spec;

    if (cx == NULL || cx->ed == NULL || cx->ed->plug == NULL)
        return YEW_CMD_ERR_STATE;
    picker_build(cx->ed);
    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Plugins";
    spec.items = picker_items;
    spec.accept = picker_accept;
    spec.action = picker_action;
    spec.footer = "enter toggle . r reload . i info . / filter . esc";
    spec.filter_requires_slash = true;
    spec.ctx = cx->ed->plug;
    yew_picker_open(cx->ed, &spec);
    return yew_picker_active(cx->ed) ? YEW_CMD_OK : YEW_CMD_ERR_STATE;
}

static Plug *cmd_plugin(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->sarg_len == 0U)
        return NULL;
    return find_len(cx->ed, cx->sarg, cx->sarg_len);
}

CmdStatus yew_plug_cmd_enable(CmdCtx *cx)
{
    Plug *plug = cmd_plugin(cx);

    if (plug == NULL)
        return YEW_CMD_ERR_ARG;
    if (!yew_plug_enable(cx->ed, plug, NULL))
        return YEW_CMD_ERR_STATE;
    return yew_config_plugin_set_desired(
               cx->ed, plug->mf.name_text, YEW_PLUGIN_DESIRED_ENABLED) ?
           YEW_CMD_OK : YEW_CMD_ERR_IO;
}

CmdStatus yew_plug_cmd_disable(CmdCtx *cx)
{
    Plug *plug = cmd_plugin(cx);

    if (plug == NULL)
        return YEW_CMD_ERR_ARG;
    if (!yew_plug_disable(cx->ed, plug))
        return YEW_CMD_ERR_STATE;
    return yew_config_plugin_set_desired(
               cx->ed, plug->mf.name_text, YEW_PLUGIN_DESIRED_DISABLED) ?
           YEW_CMD_OK : YEW_CMD_ERR_IO;
}

CmdStatus yew_plug_cmd_reload(CmdCtx *cx)
{
    Plug *plug = cmd_plugin(cx);

    return plug != NULL && yew_plug_reload(cx->ed, plug, NULL) ?
           YEW_CMD_OK : YEW_CMD_ERR_ARG;
}

CmdStatus yew_plug_cmd_info(CmdCtx *cx)
{
    Plug *plug = cmd_plugin(cx);

    if (plug == NULL)
        return YEW_CMD_ERR_ARG;
    plugin_info(cx->ed, plug);
    return YEW_CMD_OK;
}
