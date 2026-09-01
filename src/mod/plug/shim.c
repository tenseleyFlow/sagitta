#include "mod/plug/plug.h"
#include "mod/plug/pkg.h"

#include <stdio.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/mods.h"
#include "ui/message.h"
#include "util/base.h"

static CmdStatus plug_cmd_unavailable(CmdCtx *cx)
{
    char err[160];

    if (cx != NULL && cx->ed != NULL &&
        !yew_mod_require(YEW_MOD_PLUGINS, err, sizeof(err)))
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s", err);
    return YEW_CMD_ERR_STATE;
}

static int plug_main_unavailable(void)
{
    char err[160];

    if (!yew_mod_require(YEW_MOD_PLUGINS, err, sizeof(err)))
        (void)fprintf(stderr, "%s\n", err);
    return YEW_EXIT_ERR;
}

bool yew_plug_discover(Ed *ed, DiagCtx *dc)
{
    (void)ed;
    (void)dc;
    return false;
}

bool yew_plug_enable_desired(Ed *ed, DiagCtx *dc)
{
    (void)ed;
    (void)dc;
    return false;
}

bool yew_plug_boot(Ed *ed)
{
    (void)ed;
    return false;
}

bool yew_plug_startup_pending(const Ed *ed)
{
    (void)ed;
    return false;
}

void yew_plug_pump(Ed *ed)
{
    (void)ed;
}

void yew_plug_free(Ed *ed)
{
    if (ed != NULL)
        ed->plug = NULL;
}

bool yew_plug_session_grant(Ed *ed, const char *plugin, const char *cap)
{
    (void)ed;
    (void)plugin;
    (void)cap;
    return false;
}

u32 yew_plug_count(const Ed *ed)
{
    (void)ed;
    return 0U;
}

Plug *yew_plug_at(Ed *ed, u32 index)
{
    (void)ed;
    (void)index;
    return NULL;
}

Plug *yew_plug_find(Ed *ed, const char *name)
{
    (void)ed;
    (void)name;
    return NULL;
}

bool yew_plug_enable(Ed *ed, Plug *p, DiagCtx *dc)
{
    (void)ed;
    (void)p;
    (void)dc;
    return false;
}

bool yew_plug_disable(Ed *ed, Plug *p)
{
    (void)ed;
    (void)p;
    return false;
}

bool yew_plug_reload(Ed *ed, Plug *p, DiagCtx *dc)
{
    (void)ed;
    (void)p;
    (void)dc;
    return false;
}

bool yew_plug_event_allowed(Ed *ed, u32 origin_id, const char *name,
                            size_t len)
{
    (void)ed;
    (void)origin_id;
    (void)name;
    (void)len;
    return false;
}

bool yew_plug_ctx_registration_allowed(const Ed *ed, u32 origin_id)
{
    (void)ed;
    (void)origin_id;
    return false;
}

void yew_plug_hook_error(Ed *ed, u32 origin_id, FlValue err)
{
    (void)ed;
    (void)origin_id;
    (void)err;
}

void yew_plug_drain_pending(Ed *ed)
{
    (void)ed;
}

bool yew_plug_cap_check(FlVm *vm, u32 need)
{
    (void)vm;
    (void)need;
    return false;
}

bool yew_plug_registration_remove(Ed *ed, const FlRegistration *reg)
{
    (void)ed;
    (void)reg;
    return false;
}

bool yew_plug_prompt_key(Ed *ed, u32 code)
{
    (void)ed;
    (void)code;
    return false;
}

void yew_plug_error_limit_set(Ed *ed, u32 limit)
{
    (void)ed;
    (void)limit;
}

CmdStatus yew_plug_cmd_list(CmdCtx *cx) { return plug_cmd_unavailable(cx); }
CmdStatus yew_plug_cmd_enable(CmdCtx *cx) { return plug_cmd_unavailable(cx); }
CmdStatus yew_plug_cmd_disable(CmdCtx *cx) { return plug_cmd_unavailable(cx); }
CmdStatus yew_plug_cmd_reload(CmdCtx *cx) { return plug_cmd_unavailable(cx); }
CmdStatus yew_plug_cmd_info(CmdCtx *cx) { return plug_cmd_unavailable(cx); }

int yew_plug_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return plug_main_unavailable();
}

int yew_pkg_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return plug_main_unavailable();
}

void yew_pkg_git_run_init(GitRun *run)
{
    if (run == NULL)
        return;
    (void)memset(run, 0, sizeof(*run));
    bytebuf_init(&run->out);
    bytebuf_init(&run->err);
    run->status = -1;
}

void yew_pkg_git_run_free(GitRun *run)
{
    if (run == NULL)
        return;
    bytebuf_free(&run->out);
    bytebuf_free(&run->err);
    (void)memset(run, 0, sizeof(*run));
}

bool yew_pkg_git(const char *const *argv, u32 nargv, i64 timeout_ms,
                 bool c_locale, GitRun *out)
{
    (void)argv;
    (void)nargv;
    (void)timeout_ms;
    (void)c_locale;
    (void)out;
    return false;
}

bool yew_pkg_resolve_spec(const char *spec, Bytebuf *url, DiagCtx *dc)
{
    (void)spec;
    (void)url;
    (void)dc;
    return false;
}

bool yew_pkg_ref_valid(const char *ref)
{
    (void)ref;
    return false;
}

bool yew_pkg_pin_valid(const char *pin)
{
    (void)pin;
    return false;
}

void yew_pkg_lock_init(PkgLock *lock)
{
    if (lock != NULL)
        (void)memset(lock, 0, sizeof(*lock));
}

void yew_pkg_lock_free(PkgLock *lock)
{
    if (lock != NULL)
        (void)memset(lock, 0, sizeof(*lock));
}

bool yew_pkg_lock_load(PkgLock *lock, DiagCtx *dc)
{
    (void)lock;
    (void)dc;
    return false;
}

bool yew_pkg_lock_save(const PkgLock *lock, DiagCtx *dc)
{
    (void)lock;
    (void)dc;
    return false;
}

PkgEntry *yew_pkg_lock_find(PkgLock *lock, const char *name, u32 nlen)
{
    (void)lock;
    (void)name;
    (void)nlen;
    return NULL;
}

bool yew_pkg_tree_hash(const char *dir, char out[17], DiagCtx *dc)
{
    (void)dir;
    (void)dc;
    if (out != NULL)
        out[0] = '\0';
    return false;
}

bool yew_pkg_expected_tree(const char *name, char out[17], bool *managed,
                           DiagCtx *dc)
{
    (void)name;
    (void)dc;
    if (out != NULL)
        out[0] = '\0';
    if (managed != NULL)
        *managed = false;
    return false;
}

bool yew_rmtree(const char *path, const char *must_contain, DiagCtx *dc)
{
    (void)path;
    (void)must_contain;
    (void)dc;
    return false;
}
