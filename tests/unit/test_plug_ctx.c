#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "mod/plug/internal.h"
#include "util/xdg.h"
#include "ws/trust.h"

typedef struct PlugCtxFix {
    char root[256];
    char data[320];
    char config[320];
    char state[320];
    char workspace[320];
    char source_dir[512];
    char manifest[544];
    char entry[544];
    char *old_data;
    char *old_config;
    char *old_state;
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    YewTrustDb trust;
    Plug *plug;
} PlugCtxFix;

static char *ctx_env_copy(const char *name)
{
    const char *value = getenv(name);
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;
    len = strlen(value);
    copy = yew_xmalloc(len + 1U);
    (void)memcpy(copy, value, len + 1U);
    return copy;
}

static void ctx_env_restore(const char *name, const char *value)
{
    if (value == NULL)
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    else
        YEW_ASSERT_EQ_I64(setenv(name, value, 1), 0);
}

static void ctx_remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *ent;

    if (dir == NULL) {
        (void)unlink(path);
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char child[768];
        struct stat st;
        int n;

        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0)
            continue;
        n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        YEW_ASSERT(n > 0 && (size_t)n < sizeof(child));
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            ctx_remove_tree(child);
        else
            YEW_ASSERT_EQ_I64(unlink(child), 0);
    }
    YEW_ASSERT_EQ_I64(closedir(dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
}

static void ctx_write(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    size_t len = strlen(text);

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, len, fp), len);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static void ctx_open(PlugCtxFix *f, const char *source)
{
    char plugin_dir[448];
    int n;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-plug-ctx-XXXXXX",
                 sizeof("/tmp/yew-plug-ctx-XXXXXX"));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    n = snprintf(f->data, sizeof(f->data), "%s/data", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->data));
    n = snprintf(f->config, sizeof(f->config), "%s/config", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->config));
    n = snprintf(f->state, sizeof(f->state), "%s/state", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->state));
    n = snprintf(f->workspace, sizeof(f->workspace), "%s/work", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->workspace));
    n = snprintf(plugin_dir, sizeof(plugin_dir),
                 "%s/yew/plugins/ctx-surface", f->data);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(plugin_dir));
    n = snprintf(f->source_dir, sizeof(f->source_dir), "%s/src", plugin_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->source_dir));
    n = snprintf(f->manifest, sizeof(f->manifest), "%s/plugin.fl",
                 plugin_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->manifest));
    n = snprintf(f->entry, sizeof(f->entry), "%s/main.fl", f->source_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->entry));
    YEW_ASSERT(yew_mkdirs(f->source_dir, 0700U));
    YEW_ASSERT(yew_mkdirs(f->config, 0700U));
    YEW_ASSERT(yew_mkdirs(f->state, 0700U));
    YEW_ASSERT(yew_mkdirs(f->workspace, 0700U));
    ctx_write(f->manifest,
              "{name: \"ctx-surface\", version: \"1.0.0\", api: 1, "
              "entry: \"src/main.fl\", capabilities: [], "
              "events: [\"ed.idle\"], description: \"ctx fixture\"}\n");
    ctx_write(f->entry, source);
    f->old_data = ctx_env_copy("XDG_DATA_HOME");
    f->old_config = ctx_env_copy("XDG_CONFIG_HOME");
    f->old_state = ctx_env_copy("XDG_STATE_HOME");
    YEW_ASSERT_EQ_I64(setenv("XDG_DATA_HOME", f->data, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_CONFIG_HOME", f->config, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state, 1), 0);
    yew_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->workspace);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    arena_init(&f->diag_arena);
    fl_diag_init(&f->dc, &f->diag_arena);
    yew_trust_db_init(&f->trust);
    YEW_ASSERT(yew_plug_discover_with_policy(&f->ed, true, &f->trust,
                                              &f->dc));
    f->plug = yew_plug_find(&f->ed, "ctx-surface");
    YEW_ASSERT_NOT_NULL(f->plug);
}

static void ctx_close(PlugCtxFix *f)
{
    yew_trust_db_free(&f->trust);
    arena_free_all(&f->diag_arena);
    yew_ed_free(&f->ed);
    ctx_env_restore("XDG_DATA_HOME", f->old_data);
    ctx_env_restore("XDG_CONFIG_HOME", f->old_config);
    ctx_env_restore("XDG_STATE_HOME", f->old_state);
    free(f->old_data);
    free(f->old_config);
    free(f->old_state);
    ctx_remove_tree(f->root);
}

void test_plug_ctx_frozen_returns_flags_ws_attr_and_message(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  if ctx.on(\"ed.idle\", fn() nil) != nil { error(\"on return\") }\n"
        "  if ctx.command(\"pulse\", fn() nil, {repeatable: true, needs_win: true}) != nil { error(\"command return\") }\n"
        "  if ctx.bind(\"L\", \"z\", fn() nil) != nil { error(\"bind return\") }\n"
        "  if ctx.set({enabled: true}) != nil { error(\"set return\") }\n"
        "  if ctx.attr(\"warning\") == nil { error(\"attr return\") }\n"
        "  if ctx.overlay(fn(w, b, lo, hi) []) != nil { error(\"overlay return\") }\n"
        "  if ctx.ws.root() == nil { error(\"root return\") }\n"
        "  if ctx.ws.state_dir() != nil { error(\"state dir return\") }\n"
        "  ctx.msg(\"ready\", \"warn\")\n"
        "}\n";
    PlugCtxFix f;
    CmdId id;
    const CmdDesc *desc;
    const char *message;
    u32 attrs = 0U;
    u32 i;

    ctx_open(&f, source);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    id = yew_cmd_lookup("ed.plug.ctx_surface.pulse",
                        (u32)strlen("ed.plug.ctx_surface.pulse"));
    YEW_ASSERT(id.v != YEW_CMD_NONE.v);
    desc = yew_cmd_desc(id);
    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT_EQ_U64(desc->flags,
                      YEW_CMD_REPEATABLE | YEW_CMD_NEEDS_WIN);
    message = f.ed.msg.full == NULL ? f.ed.msg.text : f.ed.msg.full;
    YEW_ASSERT_EQ_STR(message, "[ctx-surface] ready");
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_WARN);
    for (i = 0U; i < f.ed.hooks.ledger.n; i++)
        if (f.ed.hooks.ledger.v[i].active &&
            f.ed.hooks.ledger.v[i].origin_id == f.plug->origin_id &&
            f.ed.hooks.ledger.v[i].kind == (u8)REG_ATTR)
            attrs++;
    YEW_ASSERT_EQ_U64(attrs, 1U);
    YEW_ASSERT(yew_plug_disable(&f.ed, f.plug));
    YEW_ASSERT_EQ_U64(f.ed.plug->ncmds, 0U);
    YEW_ASSERT_EQ_U64(f.ed.plug->nregs, 0U);
    ctx_close(&f);
}

void test_plug_ctx_many_cycles_leave_raw_registries_at_baseline(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.command(\"cycle\", fn() nil)\n"
        "  ctx.attr(\"warning\")\n"
        "  ctx.overlay(fn(w, b, lo, hi) [])\n"
        "}\n";
    PlugCtxFix f;
    u32 command_base;
    u32 value_base;
    u32 i;

    ctx_open(&f, source);
    command_base = f.ed.plug->ncmds;
    value_base = f.ed.plug->nregs;
    for (i = 0U; i < 100U; i++) {
        YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
        YEW_ASSERT_EQ_U64(f.ed.plug->ncmds, command_base + 1U);
        YEW_ASSERT_EQ_U64(f.ed.plug->nregs, value_base + 2U);
        YEW_ASSERT(yew_plug_disable(&f.ed, f.plug));
        YEW_ASSERT_EQ_U64(f.ed.plug->ncmds, command_base);
        YEW_ASSERT_EQ_U64(f.ed.plug->nregs, value_base);
    }
    ctx_close(&f);
}

void test_plug_ctx_rejects_unknown_message_level(void)
{
    static const char source[] =
        "fn init(ctx) { ctx.msg(\"bad\", \"debug\") }\n";
    PlugCtxFix f;

    ctx_open(&f, source);
    YEW_ASSERT(!yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_NOT_NULL(f.plug->last_error);
    YEW_ASSERT_NOT_NULL(strstr(f.plug->last_error,
                               "must be info, warn, or error"));
    YEW_ASSERT_EQ_U64(f.ed.plug->ncmds, 0U);
    YEW_ASSERT_EQ_U64(f.ed.plug->nregs, 0U);
    ctx_close(&f);
}

void test_plug_ctx_ws_state_dir_is_string_when_stateful(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  if ctx.ws.state_dir() == nil { error(\"missing state dir\") }\n"
        "}\n";
    PlugCtxFix f;
    int n;

    ctx_open(&f, source);
    n = snprintf(f.ed.state.key.dir, sizeof(f.ed.state.key.dir),
                 "%s/workspace-state", f.state);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f.ed.state.key.dir));
    f.ed.state.key.stateless = false;
    f.ed.state.ready = true;
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    ctx_close(&f);
}
