#define _POSIX_C_SOURCE 200809L

#include "fl/flconf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/bind.h"
#include "edit/option.h"
#include "fl/compile.h"
#include "fl/flruntime.h"
#include "fl/flruntime_int.h"
#include "fl/gc.h"
#include "fl/module.h"
#include "fl/macrolib.h"
#include "fl/origin.h"
#include "fl/parse.h"
#include "fl/trace.h"
#include "fl/value.h"
#include "fl/vm.h"
#if YEW_WITH_AI
#include "mod/ai/config.h"
#endif
#include "ui/message.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/xdg.h"
#include "ws/trust.h"
#include "ws/trust_prompt.h"

#ifndef YEW_RUNTIME_DIR_DEFAULT
#define YEW_RUNTIME_DIR_DEFAULT "/usr/local/share/yew/runtime"
#endif

enum {
    YEW_CFG_BUILTIN = 0,
    YEW_CFG_USER,
    YEW_CFG_WORKSPACE,
    YEW_CFG_SOURCE_N,
    YEW_CFG_MAX_BYTES = 8U * 1024U * 1024U
};

typedef struct CfgUnit {
    char *path;
    FlValue closure;
    u32 origin;
    u32 file_id;
    u8 kind;
    bool rooted;
    bool ran;
} CfgUnit;

struct YewConfigState {
    CfgUnit unit[YEW_CFG_SOURCE_N];
    char *config_path;
    YewTrustDb trust_db;
    YewTrustProbe trust_probe;
    u32 spare_file_id[YEW_CFG_SOURCE_N];
    char once_workspace[PATH_MAX];
    u64 once_hash;
    dev_t once_dev;
    ino_t once_ino;
    bool workspace_once;
    bool trust_probe_ready;
    bool clean;
    bool no_workspace_config;
    bool trust_workspace;
};

typedef enum ReadStatus {
    CFG_READ_OK,
    CFG_READ_MISSING,
    CFG_READ_ERROR
} ReadStatus;

static void trust_prompt_done(Ed *ed, YewTrustAnswer answer, void *ctx);

static char *cfg_dup(const char *s)
{
    size_t n;
    char *copy;

    if (s == NULL)
        return NULL;
    n = strlen(s);
    copy = yew_xmalloc(n + 1U);
    (void)memcpy(copy, s, n + 1U);
    return copy;
}

static char *path_join(const char *dir, const char *tail)
{
    size_t dn;
    size_t tn;
    bool slash;
    char *path;

    if (dir == NULL || tail == NULL)
        return NULL;
    dn = strlen(dir);
    tn = strlen(tail);
    slash = dn != 0U && dir[dn - 1U] != '/';
    if (dn > SIZE_MAX - tn - (slash ? 2U : 1U))
        return NULL;
    path = yew_xmalloc(dn + tn + (slash ? 2U : 1U));
    (void)memcpy(path, dir, dn);
    if (slash)
        path[dn++] = '/';
    (void)memcpy(path + dn, tail, tn + 1U);
    return path;
}

static char *runtime_path(void)
{
    const char *dir = getenv("YEW_RUNTIME_DIR");
    char *path;

    if (dir != NULL && dir[0] != '\0')
        return path_join(dir, "init.fl");
    path = path_join(YEW_RUNTIME_DIR_DEFAULT, "init.fl");
    if (path != NULL && access(path, R_OK) == 0)
        return path;
    free(path);
    /* An uninstalled build is run from the repository root by the test and
     * development targets.  Installed binaries still resolve the compiled
     * prefix first, so this does not weaken the shipped-artifact check. */
    if (access("runtime/init.fl", R_OK) == 0)
        return cfg_dup("runtime/init.fl");
    return path_join(YEW_RUNTIME_DIR_DEFAULT, "init.fl");
}

static char *user_path(const YewConfigState *state)
{
    char *dir;
    char *path;

    if (state != NULL && state->config_path != NULL)
        return cfg_dup(state->config_path);
    dir = yew_xdg_config_dir();
    if (dir == NULL)
        return NULL;
    path = path_join(dir, "init.fl");
    free(dir);
    return path;
}

static char *workspace_path(Ed *ed)
{
    return path_join(yew_ws_root(ed), ".yew.fl");
}

static void state_paths(YewConfigState *state, Ed *ed)
{
    state->unit[YEW_CFG_BUILTIN].path = runtime_path();
    state->unit[YEW_CFG_BUILTIN].kind = (u8)FL_ORIGIN_BUILTIN;
    state->unit[YEW_CFG_USER].path = user_path(state);
    state->unit[YEW_CFG_USER].kind = (u8)FL_ORIGIN_CONFIG;
    state->unit[YEW_CFG_WORKSPACE].path = workspace_path(ed);
    state->unit[YEW_CFG_WORKSPACE].kind = (u8)FL_ORIGIN_WORKSPACE;
}

static YewConfigState *state_new(Ed *ed, const YewConfigState *policy)
{
    YewConfigState *state = yew_xcalloc(1U, sizeof(*state));
    u32 i;

    for (i = 0U; i < YEW_CFG_SOURCE_N; i++) {
        state->unit[i].file_id = UINT32_MAX;
        state->spare_file_id[i] = UINT32_MAX;
    }

    if (policy != NULL) {
        state->config_path = cfg_dup(policy->config_path);
        state->clean = policy->clean;
        state->no_workspace_config = policy->no_workspace_config;
        state->trust_workspace = policy->trust_workspace;
        state->once_hash = policy->once_hash;
        state->once_dev = policy->once_dev;
        state->once_ino = policy->once_ino;
        state->workspace_once = policy->workspace_once;
        (void)snprintf(state->once_workspace,
                       sizeof(state->once_workspace), "%s",
                       policy->once_workspace);
        for (i = 0U; i < YEW_CFG_SOURCE_N; i++)
            state->unit[i].file_id = policy->spare_file_id[i];
    }
    yew_trust_db_init(&state->trust_db);
    yew_trust_probe_init(&state->trust_probe);
    if (!yew_trust_db_load(&state->trust_db))
        yew_log(YEW_LOG_WARN,
                "workspace trust database could not be loaded; refusing saved grants");
    state_paths(state, ed);
    return state;
}

static void unit_unroot(Ed *ed, CfgUnit *unit)
{
    FlVm *vm = yew_fl_vm(ed);

    if (unit->rooted && vm != NULL)
        fl_gc_host_root_remove(vm, &unit->closure);
    unit->rooted = false;
    unit->ran = false;
    unit->closure = FL_NIL_V;
}

static void state_dispose(Ed *ed, YewConfigState *state)
{
    u32 i;

    if (state == NULL)
        return;
    if (ed != NULL && ed->trust_prompt.active &&
        ed->trust_prompt.ctx == state)
        yew_trust_prompt_cancel(ed);
    for (i = 0U; i < YEW_CFG_SOURCE_N; i++) {
        unit_unroot(ed, &state->unit[i]);
        free(state->unit[i].path);
    }
    yew_trust_probe_free(&state->trust_probe);
    yew_trust_db_free(&state->trust_db);
    free(state->config_path);
    free(state);
}

void yew_config_init(Ed *ed, const YewEdStartup *startup)
{
    YewConfigState policy = {0};
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < YEW_CFG_SOURCE_N; i++)
        policy.spare_file_id[i] = UINT32_MAX;
    yew_config_free(ed);
    if (startup != NULL) {
        policy.config_path = (char *)startup->config_path;
        policy.clean = startup->clean;
        policy.no_workspace_config = startup->no_workspace_config;
        policy.trust_workspace = startup->trust_workspace;
    }
    ed->clean = policy.clean;
    ed->config = state_new(ed, &policy);
}

void yew_config_free(Ed *ed)
{
    if (ed == NULL)
        return;
    state_dispose(ed, ed->config);
    ed->config = NULL;
}

static ReadStatus read_file(const char *path, Bytebuf *out)
{
    int fd;

    bytebuf_init(out);
    if (path == NULL)
        return CFG_READ_MISSING;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? CFG_READ_MISSING : CFG_READ_ERROR;
    for (;;) {
        u8 bytes[65536];
        ssize_t n = read(fd, bytes, sizeof(bytes));

        if (n > 0) {
            bytebuf_append(out, bytes, (size_t)n);
            if (out->len > (size_t)YEW_CFG_MAX_BYTES) {
                (void)close(fd);
                bytebuf_free(out);
                return CFG_READ_ERROR;
            }
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        (void)close(fd);
        bytebuf_free(out);
        return CFG_READ_ERROR;
    }
    if (close(fd) != 0) {
        bytebuf_free(out);
        return CFG_READ_ERROR;
    }
    return CFG_READ_OK;
}

static bool compile_unit(Ed *ed, CfgUnit *unit, const Bytebuf *source)
{
    FlRuntime *rt = ed->fl;
    FlVm *vm = yew_fl_vm(ed);
    const char *owned;
    const char *path;
    FlProgram program;
    FlOrigin origin;
    FlFn *fn;
    FlMap *globals;
    FlClosure *closure;
    u32 file_id;
    u32 caps = FL_CAP_ALL;

    if (rt == NULL || vm == NULL || unit->path == NULL)
        return false;
    owned = arena_strndup(&rt->arena, (const char *)source->data, source->len);
    path = arena_strdup(&rt->arena, unit->path);
    if (unit->file_id == UINT32_MAX) {
        unit->file_id = fl_diag_add_file(&rt->diag, path, owned,
                                         source->len);
    } else {
        if (unit->file_id >= rt->diag.nfiles)
            YEW_BUG("config diagnostic slot is outside the file table");
        rt->diag.files[unit->file_id] = (FlDiagFile){path, owned,
                                                     source->len};
    }
    file_id = unit->file_id;
    origin = (FlOrigin){unit->kind,
                        yew_intern(&rt->interner, path, strlen(path)), caps,
                        0U};
    unit->origin = unit->kind == (u8)FL_ORIGIN_CONFIG ?
                   FL_ORIGIN_ID_CONFIG :
                   fl_origin_register(ed, (FlOriginKind)unit->kind, path,
                                      caps);
    program = fl_parse(&rt->arena, &rt->diag, &rt->interner, owned,
                       source->len, file_id);
    if (program.had_error || program.incomplete)
        return false;
#if YEW_WITH_AI
    if (!yew_ai_config_validate_program(&rt->arena, &rt->diag,
                                        &rt->interner, &program))
        return false;
#endif
    fn = fl_compile(vm, &rt->diag, &program, file_id, origin);
    if (fn == NULL)
        return false;
    fl_gc_protect(vm, FL_OBJ_V(FL_FN, fn));
    globals = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, globals));
    closure = fl_gc_alloc(vm, sizeof(*closure), FL_CLOSURE);
    closure->fn = fn;
    closure->up = NULL;
    closure->nup = 0U;
    closure->globals = globals;
    unit->closure = FL_OBJ_V(FL_CLOSURE, closure);
    fl_gc_host_root_add(vm, &unit->closure);
    unit->rooted = true;
    fl_gc_release(vm, 2U);
    return true;
}

static bool source_enabled(const YewConfigState *state, u32 index)
{
    if (state->clean)
        return false;
    if (index == YEW_CFG_WORKSPACE && state->no_workspace_config)
        return false;
    return true;
}

static CfgStatus compile_one(Ed *ed, YewConfigState *state, u32 index)
{
    Bytebuf source;
    ReadStatus read_status;
    bool source_ready = false;

    if (!source_enabled(state, index))
        return YEW_CFG_OK;
    if (index == YEW_CFG_WORKSPACE) {
        YewTrustProbe probe;
        YewTrustDecision decision;

        yew_trust_probe_init(&probe);
        decision = yew_trust_check(&state->trust_db, yew_ws_root(ed),
                                   ed->tty.raw, state->trust_workspace,
                                   &probe);
        if (state->workspace_once && probe.has_config &&
            strcmp(state->once_workspace, probe.workspace) == 0 &&
            state->once_hash == probe.hash && state->once_dev == probe.dev &&
            state->once_ino == probe.ino)
            decision = YEW_TRUST_GRANTED;
        if (decision == YEW_TRUST_GRANTED) {
            source = probe.bytes;
            bytebuf_init(&probe.bytes);
            source_ready = true;
            free(state->unit[index].path);
            state->unit[index].path = cfg_dup(probe.config_path);
        }
        if (!source_ready) {
            if (decision == YEW_TRUST_NO_CONFIG)
                yew_log(YEW_LOG_INFO, "config missing: %s",
                        state->unit[index].path);
            else if (decision == YEW_TRUST_SKIP_NO_TTY) {
                yew_log(YEW_LOG_WARN, "%s: %s",
                        state->unit[index].path,
                        yew_trust_decision_reason(decision));
                (void)fprintf(stderr, "yew: warning: %s: %s\n",
                              state->unit[index].path,
                              yew_trust_decision_reason(decision));
            } else if (decision == YEW_TRUST_ERROR)
                yew_log(YEW_LOG_ERROR, "cannot verify workspace config: %s",
                        state->unit[index].path);
            else if (decision == YEW_TRUST_PROMPT_NEW ||
                     decision == YEW_TRUST_PROMPT_CHANGED ||
                     decision == YEW_TRUST_PROMPT_REPLACED) {
                yew_trust_probe_free(&state->trust_probe);
                state->trust_probe = probe;
                bytebuf_init(&probe.bytes);
                state->trust_probe_ready = true;
                if (!yew_trust_prompt_begin(ed, &state->trust_db,
                                            &state->trust_probe, decision,
                                            trust_prompt_done, state))
                    yew_log(YEW_LOG_WARN,
                            "workspace config trust prompt could not be opened: %s",
                            state->unit[index].path);
            }
            yew_trust_probe_free(&probe);
            if (decision == YEW_TRUST_NO_CONFIG)
                return YEW_CFG_MISSING;
            return decision == YEW_TRUST_ERROR ? YEW_CFG_RUN :
                                                 YEW_CFG_UNTRUSTED;
        }
        yew_trust_probe_free(&probe);
    } else {
        read_status = read_file(state->unit[index].path, &source);
        if (read_status == CFG_READ_MISSING) {
            yew_log(index == YEW_CFG_BUILTIN ? YEW_LOG_ERROR : YEW_LOG_INFO,
                    "config missing: %s%s", state->unit[index].path == NULL ?
                        "<unresolved>" : state->unit[index].path,
                    index == YEW_CFG_BUILTIN ?
                        " (set YEW_RUNTIME_DIR to the shipped runtime directory)" :
                        "");
            return YEW_CFG_MISSING;
        }
        if (read_status == CFG_READ_ERROR) {
            yew_log(YEW_LOG_ERROR, "cannot read config: %s",
                    state->unit[index].path == NULL ? "<unresolved>" :
                                                     state->unit[index].path);
            return YEW_CFG_RUN;
        }
        source_ready = true;
    }
    if (!source_ready)
        YEW_BUG("config source reached compile without bytes");
    if (!compile_unit(ed, &state->unit[index], &source)) {
        bytebuf_free(&source);
        return YEW_CFG_PARSE;
    }
    bytebuf_free(&source);
    return YEW_CFG_OK;
}

static CfgStatus run_one(Ed *ed, CfgUnit *unit)
{
    if (!unit->rooted)
        return YEW_CFG_OK;
    if (fl_call_value(ed->fl, unit->closure, YEW_SRC_FLETCH)) {
        unit->ran = true;
        return YEW_CFG_OK;
    }
    yew_log(YEW_LOG_ERROR, "config runtime error: %s", unit->path);
    yew_msg(ed, YEW_MSG_ERROR, "config runtime error: %s", unit->path);
    yew_origin_teardown(ed, unit->origin);
    unit_unroot(ed, unit);
    return YEW_CFG_RUN;
}

static void trust_prompt_done(Ed *ed, YewTrustAnswer answer, void *ctx)
{
    YewConfigState *state = ctx;
    CfgUnit *unit;
    CfgStatus status = YEW_CFG_UNTRUSTED;

    if (ed == NULL || state == NULL || ed->config != state ||
        !state->trust_probe_ready)
        return;
    unit = &state->unit[YEW_CFG_WORKSPACE];
    if (answer == YEW_TRUST_ALWAYS || answer == YEW_TRUST_ONCE) {
        if (answer == YEW_TRUST_ONCE) {
            state->workspace_once = true;
            state->once_hash = state->trust_probe.hash;
            state->once_dev = state->trust_probe.dev;
            state->once_ino = state->trust_probe.ino;
            (void)snprintf(state->once_workspace,
                           sizeof(state->once_workspace), "%s",
                           state->trust_probe.workspace);
        }
        free(unit->path);
        unit->path = cfg_dup(state->trust_probe.config_path);
        yew_bind_batch_begin(ed);
        if (compile_unit(ed, unit, &state->trust_probe.bytes))
            status = run_one(ed, unit);
        else
            status = YEW_CFG_PARSE;
        yew_bind_batch_end(ed);
        if (status == YEW_CFG_OK)
            yew_msg(ed, YEW_MSG_INFO, "workspace configuration loaded");
        else
            yew_msg(ed, YEW_MSG_ERROR,
                    "workspace configuration failed to load");
    }
    yew_trust_probe_free(&state->trust_probe);
    yew_trust_probe_init(&state->trust_probe);
    state->trust_probe_ready = false;
}

void yew_origin_teardown(Ed *ed, u32 origin)
{
    FlRegLedger *ledger;
    u32 i;

    if (ed == NULL)
        return;
    ledger = &ed->hooks.ledger;
    fl_origin_mask(ed, origin);
    for (i = ledger->n; i != 0U; i--) {
        FlRegistration *reg = &ledger->v[i - 1U];

        if (!reg->active || reg->origin_id != origin)
            continue;
        if (reg->kind == (u8)REG_HOOK)
            (void)fl_hook_remove(&ed->hooks, i);
        else if (reg->kind == (u8)REG_BIND)
            (void)yew_bind_remove(ed, i);
        else if (reg->kind == (u8)REG_OPTION)
            (void)yew_opt_remove(ed, i);
        else
            (void)fl_reg_remove(ledger, i);
    }
    fl_origin_unmask(ed, origin);
}

static void teardown_state(Ed *ed, YewConfigState *state)
{
    u32 i = YEW_CFG_SOURCE_N;

    if (state == NULL)
        return;
    while (i != 0U) {
        CfgUnit *unit = &state->unit[--i];

        if (unit->rooted)
            yew_origin_teardown(ed, unit->origin);
    }
}

static void reset_modules(Ed *ed)
{
    FlVm *vm = yew_fl_vm(ed);

    if (vm == NULL)
        return;
    fl_mod_free(vm);
    fl_map_clear(vm->modules);
}

static CfgStatus status_merge(CfgStatus current, CfgStatus next)
{
    return next > current ? next : current;
}

CfgStatus yew_config_load_all(Ed *ed, DiagCtx *dc)
{
    YewConfigState *state;
    CfgStatus overall = YEW_CFG_OK;
    u32 i;

    (void)dc;
    if (ed == NULL || ed->fl == NULL)
        return YEW_CFG_RUN;
    if (ed->config == NULL)
        yew_config_init(ed, NULL);
    state = ed->config;
    if (state->clean)
        return YEW_CFG_OK;
    yew_bind_batch_begin(ed);
    for (i = 0U; i < YEW_CFG_SOURCE_N; i++) {
        CfgStatus status = compile_one(ed, state, i);

        if (status == YEW_CFG_MISSING && i != YEW_CFG_BUILTIN)
            continue;
        if (status == YEW_CFG_UNTRUSTED && i == YEW_CFG_WORKSPACE)
            continue;
        overall = status_merge(overall, status);
        if (status == YEW_CFG_OK)
            overall = status_merge(overall, run_one(ed, &state->unit[i]));
    }
    yew_bind_batch_end(ed);
    yew_macrolib_enable(ed);
    return overall;
}

CfgStatus yew_config_reload(Ed *ed, DiagCtx *dc)
{
    YewConfigState *candidate;
    YewConfigState *old;
    CfgStatus overall = YEW_CFG_OK;
    u32 i;

    (void)dc;
    if (ed == NULL || ed->fl == NULL)
        return YEW_CFG_RUN;
    if (ed->config == NULL)
        yew_config_init(ed, NULL);
    old = ed->config;
    if (old->clean)
        return YEW_CFG_OK;
    candidate = state_new(ed, old);
    for (i = 0U; i < YEW_CFG_SOURCE_N; i++) {
        CfgStatus status = compile_one(ed, candidate, i);

        if (status == YEW_CFG_MISSING && i != YEW_CFG_BUILTIN)
            continue;
        if (status == YEW_CFG_UNTRUSTED && i == YEW_CFG_WORKSPACE)
            continue;
        if (status != YEW_CFG_OK) {
            state_dispose(ed, candidate);
            return status;
        }
    }
    yew_bind_batch_begin(ed);
    teardown_state(ed, old);
    yew_opt_reset(ed);
    reset_modules(ed);
    for (i = 0U; i < YEW_CFG_SOURCE_N; i++)
        candidate->spare_file_id[i] = old->unit[i].file_id;
    ed->config = candidate;
    state_dispose(ed, old);
    fl_gc_collect(yew_fl_vm(ed));
    for (i = 0U; i < YEW_CFG_SOURCE_N; i++)
        overall = status_merge(overall, run_one(ed, &candidate->unit[i]));
    yew_bind_batch_end(ed);
    (void)yew_macrolib_scan(ed, dc);
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
    return overall;
}

bool yew_config_get_global(const Ed *ed, const char *name, size_t name_len,
                           FlValue *out)
{
    const YewConfigState *state;
    const FlRuntime *rt;
    void *found;
    FlValue key;
    u32 i;

    if (ed == NULL || name == NULL || out == NULL || ed->config == NULL ||
        ed->fl == NULL)
        return false;
    state = ed->config;
    rt = ed->fl;
    found = strmap_get(&rt->interner.map, name, name_len);
    if (found == NULL)
        return false;
    key = FL_INT_V((i64)(u32)(uintptr_t)found);
    i = YEW_CFG_SOURCE_N;
    while (i != 0U) {
        const CfgUnit *unit = &state->unit[--i];
        const FlClosure *closure;

        if (!unit->rooted || !unit->ran || unit->closure.t != FL_CLOSURE)
            continue;
        closure = (const FlClosure *)unit->closure.as.o;
        if (fl_map_get(closure->globals, key, out))
            return true;
    }
    return false;
}

AiWsGrant yew_config_ai_workspace_grant(Ed *ed)
{
    if (ed == NULL || ed->config == NULL)
        return YEW_AI_WS_UNSET;
    return yew_trust_ai_grant(&ed->config->trust_db, yew_ws_root(ed));
}

bool yew_config_ai_workspace_set(Ed *ed, AiWsGrant grant)
{
    time_t now;

    if (ed == NULL || ed->config == NULL)
        return false;
    now = time(NULL);
    if (now == (time_t)-1 ||
        !yew_trust_ai_set(&ed->config->trust_db, yew_ws_root(ed), grant,
                          now))
        return false;
    return yew_trust_db_write(&ed->config->trust_db, now,
                              YEW_TRUST_PRUNE_DAYS_DEFAULT);
}

bool yew_config_ai_workspace_forget(Ed *ed)
{
    time_t now;

    if (ed == NULL || ed->config == NULL)
        return false;
    now = time(NULL);
    if (now == (time_t)-1 ||
        !yew_trust_ai_forget(&ed->config->trust_db, yew_ws_root(ed)))
        return false;
    return yew_trust_db_write(&ed->config->trust_db, now,
                              YEW_TRUST_PRUNE_DAYS_DEFAULT);
}

YewPluginDesired yew_config_plugin_desired(const Ed *ed,
                                           const char *plugin)
{
    if (ed == NULL || ed->config == NULL)
        return YEW_PLUGIN_DESIRED_DEFAULT;
    return yew_trust_plugin_desired(&ed->config->trust_db, plugin);
}

bool yew_config_plugin_set_desired(Ed *ed, const char *plugin,
                                   YewPluginDesired desired)
{
    time_t now;

    if (ed == NULL || ed->config == NULL)
        return false;
    now = time(NULL);
    return now != (time_t)-1 &&
           yew_trust_plugin_set_desired(&ed->config->trust_db, plugin,
                                        desired) &&
           yew_trust_db_write(&ed->config->trust_db, now,
                              YEW_TRUST_PRUNE_DAYS_DEFAULT);
}

YewPluginGrant yew_config_plugin_capability(
    const Ed *ed, const char *plugin, YewPluginCapability capability)
{
    if (ed == NULL || ed->config == NULL)
        return YEW_PLUGIN_GRANT_UNSET;
    return yew_trust_plugin_capability(&ed->config->trust_db, plugin,
                                       capability);
}

bool yew_config_plugin_set_capability(Ed *ed, const char *plugin,
                                      YewPluginCapability capability,
                                      YewPluginGrant grant)
{
    time_t now;

    if (ed == NULL || ed->config == NULL)
        return false;
    now = time(NULL);
    return now != (time_t)-1 &&
           yew_trust_plugin_set_capability(&ed->config->trust_db, plugin,
                                           capability, grant) &&
           yew_trust_db_write(&ed->config->trust_db, now,
                              YEW_TRUST_PRUNE_DAYS_DEFAULT);
}

bool yew_config_plugin_drop_grants(Ed *ed, const char *plugin,
                                   u32 *dropped)
{
    u32 count = 0U;

    if (dropped != NULL)
        *dropped = 0U;
    if (ed == NULL || ed->config == NULL ||
        !yew_trust_plugin_revoke_persisted(plugin, &count))
        return false;
    if (!yew_trust_db_load(&ed->config->trust_db))
        return false;
    if (dropped != NULL)
        *dropped = count;
    return true;
}

bool yew_config_workspace_plugins_trusted(const Ed *ed)
{
    const YewConfigState *state;

    if (ed == NULL || ed->config == NULL)
        return false;
    state = ed->config;
    return state->trust_workspace || state->workspace_once ||
           state->unit[YEW_CFG_WORKSPACE].ran;
}

const char *yew_config_user_path(Ed *ed)
{
    if (ed == NULL || ed->config == NULL)
        return NULL;
    return ed->config->unit[YEW_CFG_USER].path;
}

CmdStatus yew_config_cmd_reload(CmdCtx *cx)
{
    CfgStatus status;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    status = yew_config_reload(cx->ed, NULL);
    if (status != YEW_CFG_OK) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "configuration reload failed");
        return YEW_CMD_ERR_STATE;
    }
    yew_msg(cx->ed, YEW_MSG_INFO, "configuration reloaded");
    return YEW_CMD_OK;
}

CmdStatus yew_config_cmd_edit(CmdCtx *cx)
{
    const char *path;
    YewLoadErr load;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_ARG;
    path = yew_config_user_path(cx->ed);
    if (path == NULL)
        return YEW_CMD_ERR_IO;
    load = yew_ed_open(cx->ed, path);
    return load == YEW_LOAD_OK || load == YEW_LOAD_ENOENT ? YEW_CMD_OK :
                                                            YEW_CMD_ERR_IO;
}
