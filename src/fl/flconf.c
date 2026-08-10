#define _POSIX_C_SOURCE 200809L

#include "fl/flconf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/bind.h"
#include "edit/option.h"
#include "fl/compile.h"
#include "fl/flruntime.h"
#include "fl/flruntime_int.h"
#include "fl/gc.h"
#include "fl/module.h"
#include "fl/origin.h"
#include "fl/parse.h"
#include "fl/trace.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "ui/message.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/xdg.h"
#include "ws/trust.h"
#include "ws/trust_prompt.h"

#ifndef SAG_RUNTIME_DIR_DEFAULT
#define SAG_RUNTIME_DIR_DEFAULT "/usr/local/share/sagitta/runtime"
#endif

enum {
    SAG_CFG_BUILTIN = 0,
    SAG_CFG_USER,
    SAG_CFG_WORKSPACE,
    SAG_CFG_SOURCE_N,
    SAG_CFG_MAX_BYTES = 8U * 1024U * 1024U
};

typedef struct CfgUnit {
    char *path;
    FlValue closure;
    u32 origin;
    u32 file_id;
    u8 kind;
    bool rooted;
} CfgUnit;

struct SagConfigState {
    CfgUnit unit[SAG_CFG_SOURCE_N];
    char *config_path;
    SagTrustDb trust_db;
    SagTrustProbe trust_probe;
    u32 spare_file_id[SAG_CFG_SOURCE_N];
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

static void trust_prompt_done(Ed *ed, SagTrustAnswer answer, void *ctx);

static char *cfg_dup(const char *s)
{
    size_t n;
    char *copy;

    if (s == NULL)
        return NULL;
    n = strlen(s);
    copy = sag_xmalloc(n + 1U);
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
    path = sag_xmalloc(dn + tn + (slash ? 2U : 1U));
    (void)memcpy(path, dir, dn);
    if (slash)
        path[dn++] = '/';
    (void)memcpy(path + dn, tail, tn + 1U);
    return path;
}

static char *runtime_path(void)
{
    const char *dir = getenv("SAG_RUNTIME_DIR");
    char *path;

    if (dir != NULL && dir[0] != '\0')
        return path_join(dir, "init.fl");
    path = path_join(SAG_RUNTIME_DIR_DEFAULT, "init.fl");
    if (path != NULL && access(path, R_OK) == 0)
        return path;
    free(path);
    /* An uninstalled build is run from the repository root by the test and
     * development targets.  Installed binaries still resolve the compiled
     * prefix first, so this does not weaken the shipped-artifact check. */
    if (access("runtime/init.fl", R_OK) == 0)
        return cfg_dup("runtime/init.fl");
    return path_join(SAG_RUNTIME_DIR_DEFAULT, "init.fl");
}

static char *user_path(const SagConfigState *state)
{
    char *dir;
    char *path;

    if (state != NULL && state->config_path != NULL)
        return cfg_dup(state->config_path);
    dir = sag_xdg_config_dir();
    if (dir == NULL)
        return NULL;
    path = path_join(dir, "init.fl");
    free(dir);
    return path;
}

static char *workspace_path(Ed *ed)
{
    return path_join(sag_ws_root(ed), ".sagitta.fl");
}

static void state_paths(SagConfigState *state, Ed *ed)
{
    state->unit[SAG_CFG_BUILTIN].path = runtime_path();
    state->unit[SAG_CFG_BUILTIN].kind = (u8)FL_ORIGIN_BUILTIN;
    state->unit[SAG_CFG_USER].path = user_path(state);
    state->unit[SAG_CFG_USER].kind = (u8)FL_ORIGIN_CONFIG;
    state->unit[SAG_CFG_WORKSPACE].path = workspace_path(ed);
    state->unit[SAG_CFG_WORKSPACE].kind = (u8)FL_ORIGIN_WORKSPACE;
}

static SagConfigState *state_new(Ed *ed, const SagConfigState *policy)
{
    SagConfigState *state = sag_xcalloc(1U, sizeof(*state));
    u32 i;

    for (i = 0U; i < SAG_CFG_SOURCE_N; i++) {
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
        for (i = 0U; i < SAG_CFG_SOURCE_N; i++)
            state->unit[i].file_id = policy->spare_file_id[i];
    }
    sag_trust_db_init(&state->trust_db);
    sag_trust_probe_init(&state->trust_probe);
    if (!sag_trust_db_load(&state->trust_db))
        sag_log(SAG_LOG_WARN,
                "workspace trust database could not be loaded; refusing saved grants");
    state_paths(state, ed);
    return state;
}

static void unit_unroot(Ed *ed, CfgUnit *unit)
{
    FlVm *vm = sag_fl_vm(ed);

    if (unit->rooted && vm != NULL)
        fl_gc_host_root_remove(vm, &unit->closure);
    unit->rooted = false;
    unit->closure = FL_NIL_V;
}

static void state_dispose(Ed *ed, SagConfigState *state)
{
    u32 i;

    if (state == NULL)
        return;
    if (ed != NULL && ed->trust_prompt.active &&
        ed->trust_prompt.ctx == state)
        sag_trust_prompt_cancel(ed);
    for (i = 0U; i < SAG_CFG_SOURCE_N; i++) {
        unit_unroot(ed, &state->unit[i]);
        free(state->unit[i].path);
    }
    sag_trust_probe_free(&state->trust_probe);
    sag_trust_db_free(&state->trust_db);
    free(state->config_path);
    free(state);
}

void sag_config_init(Ed *ed, const SagEdStartup *startup)
{
    SagConfigState policy = {0};
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < SAG_CFG_SOURCE_N; i++)
        policy.spare_file_id[i] = UINT32_MAX;
    sag_config_free(ed);
    if (startup != NULL) {
        policy.config_path = (char *)startup->config_path;
        policy.clean = startup->clean;
        policy.no_workspace_config = startup->no_workspace_config;
        policy.trust_workspace = startup->trust_workspace;
    }
    ed->clean = policy.clean;
    ed->config = state_new(ed, &policy);
}

void sag_config_free(Ed *ed)
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
            if (out->len > (size_t)SAG_CFG_MAX_BYTES) {
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
    FlVm *vm = sag_fl_vm(ed);
    const char *owned;
    const char *path;
    FlProgram program;
    FlOrigin origin;
    FlFn *fn;
    FlMap *globals;
    FlClosure *closure;
    u32 file_id;
    u32 caps = (u32)FL_CAP_FS_READ | (u32)FL_CAP_FS_WRITE |
               (u32)FL_CAP_SHELL | (u32)FL_CAP_NET;

    if (rt == NULL || vm == NULL || unit->path == NULL)
        return false;
    owned = arena_strndup(&rt->arena, (const char *)source->data, source->len);
    path = arena_strdup(&rt->arena, unit->path);
    if (unit->file_id == UINT32_MAX) {
        if (rt->diag.nfiles >= FL_DIAG_MAX_FILES)
            return false;
        unit->file_id = fl_diag_add_file(&rt->diag, path, owned,
                                         source->len);
    } else {
        if (unit->file_id >= rt->diag.nfiles)
            SAG_BUG("config diagnostic slot is outside the file table");
        rt->diag.files[unit->file_id] = (FlDiagFile){path, owned,
                                                     source->len};
    }
    file_id = unit->file_id;
    origin = (FlOrigin){unit->kind,
                        sag_intern(&rt->interner, path, strlen(path)), caps};
    unit->origin = unit->kind == (u8)FL_ORIGIN_CONFIG ?
                   FL_ORIGIN_ID_CONFIG :
                   fl_origin_register(ed, (FlOriginKind)unit->kind, path,
                                      caps);
    program = fl_parse(&rt->arena, &rt->diag, &rt->interner, owned,
                       source->len, file_id);
    if (program.had_error || program.incomplete)
        return false;
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

static bool source_enabled(const SagConfigState *state, u32 index)
{
    if (state->clean)
        return false;
    if (index == SAG_CFG_WORKSPACE && state->no_workspace_config)
        return false;
    return true;
}

static CfgStatus compile_one(Ed *ed, SagConfigState *state, u32 index)
{
    Bytebuf source;
    ReadStatus read_status;
    bool source_ready = false;

    if (!source_enabled(state, index))
        return SAG_CFG_OK;
    if (index == SAG_CFG_WORKSPACE) {
        SagTrustProbe probe;
        SagTrustDecision decision;

        sag_trust_probe_init(&probe);
        decision = sag_trust_check(&state->trust_db, sag_ws_root(ed),
                                   ed->tty.raw, state->trust_workspace,
                                   &probe);
        if (state->workspace_once && probe.has_config &&
            strcmp(state->once_workspace, probe.workspace) == 0 &&
            state->once_hash == probe.hash && state->once_dev == probe.dev &&
            state->once_ino == probe.ino)
            decision = SAG_TRUST_GRANTED;
        if (decision == SAG_TRUST_GRANTED) {
            source = probe.bytes;
            bytebuf_init(&probe.bytes);
            source_ready = true;
            free(state->unit[index].path);
            state->unit[index].path = cfg_dup(probe.config_path);
        }
        if (!source_ready) {
            if (decision == SAG_TRUST_NO_CONFIG)
                sag_log(SAG_LOG_INFO, "config missing: %s",
                        state->unit[index].path);
            else if (decision == SAG_TRUST_SKIP_NO_TTY) {
                sag_log(SAG_LOG_WARN, "%s: %s",
                        state->unit[index].path,
                        sag_trust_decision_reason(decision));
                (void)fprintf(stderr, "sagitta: warning: %s: %s\n",
                              state->unit[index].path,
                              sag_trust_decision_reason(decision));
            } else if (decision == SAG_TRUST_ERROR)
                sag_log(SAG_LOG_ERROR, "cannot verify workspace config: %s",
                        state->unit[index].path);
            else if (decision == SAG_TRUST_PROMPT_NEW ||
                     decision == SAG_TRUST_PROMPT_CHANGED ||
                     decision == SAG_TRUST_PROMPT_REPLACED) {
                sag_trust_probe_free(&state->trust_probe);
                state->trust_probe = probe;
                bytebuf_init(&probe.bytes);
                state->trust_probe_ready = true;
                if (!sag_trust_prompt_begin(ed, &state->trust_db,
                                            &state->trust_probe, decision,
                                            trust_prompt_done, state))
                    sag_log(SAG_LOG_WARN,
                            "workspace config trust prompt could not be opened: %s",
                            state->unit[index].path);
            }
            sag_trust_probe_free(&probe);
            if (decision == SAG_TRUST_NO_CONFIG)
                return SAG_CFG_MISSING;
            return decision == SAG_TRUST_ERROR ? SAG_CFG_RUN :
                                                 SAG_CFG_UNTRUSTED;
        }
        sag_trust_probe_free(&probe);
    } else {
        read_status = read_file(state->unit[index].path, &source);
        if (read_status == CFG_READ_MISSING) {
            sag_log(index == SAG_CFG_BUILTIN ? SAG_LOG_ERROR : SAG_LOG_INFO,
                    "config missing: %s%s", state->unit[index].path == NULL ?
                        "<unresolved>" : state->unit[index].path,
                    index == SAG_CFG_BUILTIN ?
                        " (set SAG_RUNTIME_DIR to the shipped runtime directory)" :
                        "");
            return SAG_CFG_MISSING;
        }
        if (read_status == CFG_READ_ERROR) {
            sag_log(SAG_LOG_ERROR, "cannot read config: %s",
                    state->unit[index].path == NULL ? "<unresolved>" :
                                                     state->unit[index].path);
            return SAG_CFG_RUN;
        }
        source_ready = true;
    }
    if (!source_ready)
        SAG_BUG("config source reached compile without bytes");
    if (!compile_unit(ed, &state->unit[index], &source)) {
        bytebuf_free(&source);
        return SAG_CFG_PARSE;
    }
    bytebuf_free(&source);
    return SAG_CFG_OK;
}

static CfgStatus run_one(Ed *ed, CfgUnit *unit)
{
    if (!unit->rooted)
        return SAG_CFG_OK;
    if (fl_call_value(ed->fl, unit->closure, SAG_SRC_FLETCH))
        return SAG_CFG_OK;
    sag_log(SAG_LOG_ERROR, "config runtime error: %s", unit->path);
    sag_msg(ed, SAG_MSG_ERROR, "config runtime error: %s", unit->path);
    sag_origin_teardown(ed, unit->origin);
    unit_unroot(ed, unit);
    return SAG_CFG_RUN;
}

static void trust_prompt_done(Ed *ed, SagTrustAnswer answer, void *ctx)
{
    SagConfigState *state = ctx;
    CfgUnit *unit;
    CfgStatus status = SAG_CFG_UNTRUSTED;

    if (ed == NULL || state == NULL || ed->config != state ||
        !state->trust_probe_ready)
        return;
    unit = &state->unit[SAG_CFG_WORKSPACE];
    if (answer == SAG_TRUST_ALWAYS || answer == SAG_TRUST_ONCE) {
        if (answer == SAG_TRUST_ONCE) {
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
        sag_bind_batch_begin(ed);
        if (compile_unit(ed, unit, &state->trust_probe.bytes))
            status = run_one(ed, unit);
        else
            status = SAG_CFG_PARSE;
        sag_bind_batch_end(ed);
        if (status == SAG_CFG_OK)
            sag_msg(ed, SAG_MSG_INFO, "workspace configuration loaded");
        else
            sag_msg(ed, SAG_MSG_ERROR,
                    "workspace configuration failed to load");
    }
    sag_trust_probe_free(&state->trust_probe);
    sag_trust_probe_init(&state->trust_probe);
    state->trust_probe_ready = false;
}

void sag_origin_teardown(Ed *ed, u32 origin)
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
            (void)sag_bind_remove(ed, i);
        else if (reg->kind == (u8)REG_OPTION)
            (void)sag_opt_remove(ed, i);
        else
            (void)fl_reg_remove(ledger, i);
    }
    fl_origin_unmask(ed, origin);
}

static void teardown_state(Ed *ed, SagConfigState *state)
{
    u32 i = SAG_CFG_SOURCE_N;

    if (state == NULL)
        return;
    while (i != 0U) {
        CfgUnit *unit = &state->unit[--i];

        if (unit->rooted)
            sag_origin_teardown(ed, unit->origin);
    }
}

static void reset_modules(Ed *ed)
{
    FlVm *vm = sag_fl_vm(ed);

    if (vm == NULL)
        return;
    fl_mod_free(vm);
    fl_map_clear(vm->modules);
}

static CfgStatus status_merge(CfgStatus current, CfgStatus next)
{
    return next > current ? next : current;
}

CfgStatus sag_config_load_all(Ed *ed, DiagCtx *dc)
{
    SagConfigState *state;
    CfgStatus overall = SAG_CFG_OK;
    u32 i;

    (void)dc;
    if (ed == NULL || ed->fl == NULL)
        return SAG_CFG_RUN;
    if (ed->config == NULL)
        sag_config_init(ed, NULL);
    state = ed->config;
    if (state->clean)
        return SAG_CFG_OK;
    sag_bind_batch_begin(ed);
    for (i = 0U; i < SAG_CFG_SOURCE_N; i++) {
        CfgStatus status = compile_one(ed, state, i);

        if (status == SAG_CFG_MISSING && i != SAG_CFG_BUILTIN)
            continue;
        if (status == SAG_CFG_UNTRUSTED && i == SAG_CFG_WORKSPACE)
            continue;
        overall = status_merge(overall, status);
        if (status == SAG_CFG_OK)
            overall = status_merge(overall, run_one(ed, &state->unit[i]));
    }
    sag_bind_batch_end(ed);
    return overall;
}

CfgStatus sag_config_reload(Ed *ed, DiagCtx *dc)
{
    SagConfigState *candidate;
    SagConfigState *old;
    CfgStatus overall = SAG_CFG_OK;
    u32 i;

    (void)dc;
    if (ed == NULL || ed->fl == NULL)
        return SAG_CFG_RUN;
    if (ed->config == NULL)
        sag_config_init(ed, NULL);
    old = ed->config;
    if (old->clean)
        return SAG_CFG_OK;
    candidate = state_new(ed, old);
    for (i = 0U; i < SAG_CFG_SOURCE_N; i++) {
        CfgStatus status = compile_one(ed, candidate, i);

        if (status == SAG_CFG_MISSING && i != SAG_CFG_BUILTIN)
            continue;
        if (status == SAG_CFG_UNTRUSTED && i == SAG_CFG_WORKSPACE)
            continue;
        if (status != SAG_CFG_OK) {
            state_dispose(ed, candidate);
            return status;
        }
    }
    sag_bind_batch_begin(ed);
    teardown_state(ed, old);
    sag_opt_reset(ed);
    reset_modules(ed);
    for (i = 0U; i < SAG_CFG_SOURCE_N; i++)
        candidate->spare_file_id[i] = old->unit[i].file_id;
    ed->config = candidate;
    state_dispose(ed, old);
    fl_gc_collect(sag_fl_vm(ed));
    for (i = 0U; i < SAG_CFG_SOURCE_N; i++)
        overall = status_merge(overall, run_one(ed, &candidate->unit[i]));
    sag_bind_batch_end(ed);
    ed->layout_dirty = true;
    ed->full_damage = true;
    ed->footer_dirty = true;
    return overall;
}

const char *sag_config_user_path(Ed *ed)
{
    if (ed == NULL || ed->config == NULL)
        return NULL;
    return ed->config->unit[SAG_CFG_USER].path;
}

CmdStatus sag_config_cmd_reload(CmdCtx *cx)
{
    CfgStatus status;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    status = sag_config_reload(cx->ed, NULL);
    if (status != SAG_CFG_OK) {
        sag_msg(cx->ed, SAG_MSG_ERROR, "configuration reload failed");
        return SAG_CMD_ERR_STATE;
    }
    sag_msg(cx->ed, SAG_MSG_INFO, "configuration reloaded");
    return SAG_CMD_OK;
}

CmdStatus sag_config_cmd_edit(CmdCtx *cx)
{
    const char *path;
    SagLoadErr load;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    path = sag_config_user_path(cx->ed);
    if (path == NULL)
        return SAG_CMD_ERR_IO;
    load = sag_ed_open(cx->ed, path);
    return load == SAG_LOAD_OK || load == SAG_LOAD_ENOENT ? SAG_CMD_OK :
                                                            SAG_CMD_ERR_IO;
}
