#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/bind.h"
#include "edit/ed.h"
#include "fl/flruntime.h"
#include "fl/module.h"
#include "mod/plug/internal.h"
#include "util/buf.h"
#include "util/xdg.h"
#include "ws/trust.h"

typedef struct LifecycleCounts {
    u32 commands;
    u32 binds;
    u32 hooks;
    u32 ledger;
    u32 plug_commands;
    u32 plug_values;
} LifecycleCounts;

typedef struct LifecycleFix {
    char root[256];
    char data[320];
    char config[320];
    char state[320];
    char workspace[320];
    char plugins[384];
    char plugin_dir[448];
    char source_dir[480];
    char entry[512];
    char helper[512];
    char manifest[512];
    char *old_data;
    char *old_config;
    char *old_state;
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    YewTrustDb trust;
    Plug *plug;
} LifecycleFix;

static char *life_strdup(const char *text)
{
    size_t len;
    char *copy;

    if (text == NULL)
        return NULL;
    len = strlen(text);
    copy = malloc(len + 1U);
    YEW_ASSERT_NOT_NULL(copy);
    (void)memcpy(copy, text, len + 1U);
    return copy;
}

static void life_restore_env(const char *name, const char *value)
{
    if (value == NULL)
        YEW_ASSERT_EQ_I64(unsetenv(name), 0);
    else
        YEW_ASSERT_EQ_I64(setenv(name, value, 1), 0);
}

static void life_remove_tree(const char *path)
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
            life_remove_tree(child);
        else
            YEW_ASSERT_EQ_I64(unlink(child), 0);
    }
    YEW_ASSERT_EQ_I64(closedir(dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(path), 0);
}

static void life_write(const char *path, const char *source)
{
    FILE *fp = fopen(path, "wb");
    size_t len = strlen(source);

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(source, 1U, len, fp), len);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
}

static void life_add_plugin(LifecycleFix *f, const char *name,
                            const char *events, const char *entry_source)
{
    char plugin_dir[512];
    char source_dir[544];
    char entry[576];
    char manifest[576];
    char manifest_source[1024];
    int n;

    n = snprintf(plugin_dir, sizeof(plugin_dir), "%s/%s", f->plugins, name);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(plugin_dir));
    n = snprintf(source_dir, sizeof(source_dir), "%s/src", plugin_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(source_dir));
    n = snprintf(entry, sizeof(entry), "%s/main.fl", source_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(entry));
    n = snprintf(manifest, sizeof(manifest), "%s/plugin.fl", plugin_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(manifest));
    YEW_ASSERT(yew_mkdirs(source_dir, 0700U));
    life_write(entry, entry_source);
    n = snprintf(manifest_source, sizeof(manifest_source),
                 "{name: \"%s\", version: \"1.0.0\", api: 1, "
                 "entry: \"src/main.fl\", capabilities: [], events: %s, "
                 "description: \"lifecycle fixture\"}\n",
                 name, events);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(manifest_source));
    life_write(manifest, manifest_source);
}

static u32 life_active_ledger(const Ed *ed)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < ed->hooks.ledger.n; i++)
        if (ed->hooks.ledger.v[i].active)
            active++;
    return active;
}

static u32 life_active_hooks(const Ed *ed)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; i < ed->hooks.n; i++)
        if (ed->hooks.v[i].active)
            active++;
    return active;
}

static u32 life_active_plug_commands(const PlugSys *sys)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; sys != NULL && i < sys->ncmds; i++)
        if (sys->cmds[i].active)
            active++;
    return active;
}

static u32 life_active_plug_values(const PlugSys *sys)
{
    u32 active = 0U;
    u32 i;

    for (i = 0U; sys != NULL && i < sys->nregs; i++)
        if (sys->regs[i].active)
            active++;
    return active;
}

static LifecycleCounts life_counts(const LifecycleFix *f)
{
    LifecycleCounts counts;

    counts.commands = yew_cmd_active_count();
    counts.binds = yew_bind_active_count(&f->ed);
    counts.hooks = life_active_hooks(&f->ed);
    counts.ledger = life_active_ledger(&f->ed);
    counts.plug_commands = life_active_plug_commands(f->ed.plug);
    counts.plug_values = life_active_plug_values(f->ed.plug);
    return counts;
}

static void life_assert_counts(LifecycleCounts got, LifecycleCounts want)
{
    YEW_ASSERT_EQ_U64(got.commands, want.commands);
    YEW_ASSERT_EQ_U64(got.binds, want.binds);
    YEW_ASSERT_EQ_U64(got.hooks, want.hooks);
    YEW_ASSERT_EQ_U64(got.ledger, want.ledger);
    YEW_ASSERT_EQ_U64(got.plug_commands, want.plug_commands);
    YEW_ASSERT_EQ_U64(got.plug_values, want.plug_values);
}

static void life_source_string(Bytebuf *out, const char *text)
{
    const u8 *p = (const u8 *)text;

    bytebuf_push_u8(out, (u8)'"');
    while (*p != 0U) {
        if (*p == (u8)'"' || *p == (u8)'\\')
            bytebuf_push_u8(out, (u8)'\\');
        bytebuf_push_u8(out, *p++);
    }
    bytebuf_push_u8(out, (u8)'"');
}

static void life_hostile_source(Bytebuf *source, Bytebuf *events)
{
    static const char *const named_keys[] = {
        "<left>", "<right>", "<up>", "<down>", "<esc>", "<cr>",
        "<tab>", "<bs>", "<del>", "<space>", "<home>", "<end>",
        "<pgup>", "<pgdn>", "<ins>", "<f1>", "<f2>", "<f3>",
        "<f4>", "<f5>", "<f6>", "<f7>", "<f8>", "<f9>",
        "<f10>", "<f11>", "<f12>", "<lt>"
    };
    u32 i;

    bytebuf_init(source);
    bytebuf_init(events);
    bytebuf_append(source,
                   "fn boom() { error(\"hostile callback\") }\n"
                   "fn init(ctx) {\n",
                   strlen("fn boom() { error(\"hostile callback\") }\n"
                          "fn init(ctx) {\n"));
    for (i = 0U; i < 100U; i++)
        bytebuf_printf(source, "  ctx.command(\"c%03u\", boom)\n",
                       (unsigned)i);

    bytebuf_push_u8(events, (u8)'[');
    for (i = 0U; i < (u32)FL_EV__N; i++) {
        const char *name = fl_event_name(i);

        YEW_ASSERT_NOT_NULL(name);
        if (!yew_plug_event_valid(name, strlen(name)))
            continue;
        if (events->len != 1U)
            bytebuf_append(events, ", ", 2U);
        life_source_string(events, name);
        bytebuf_append(source, "  ctx.on(", 9U);
        life_source_string(source, name);
        bytebuf_append(source, ", boom)\n", 8U);
    }
    bytebuf_push_u8(events, (u8)']');

    for (i = 32U; i <= 126U; i++) {
        char token[2] = {(char)i, '\0'};
        const char *seq = token;

        if (i == 32U)
            seq = "<space>";
        else if (i == (u32)'<')
            seq = "<lt>";
        bytebuf_append(source, "  ctx.bind(\"L\", ", 16U);
        life_source_string(source, seq);
        bytebuf_append(source, ", boom)\n", 8U);
    }
    for (i = 0U; i < (u32)YEW_ARRAY_LEN(named_keys); i++) {
        bytebuf_append(source, "  ctx.bind(\"W\", ", 16U);
        life_source_string(source, named_keys[i]);
        bytebuf_append(source, ", boom)\n", 8U);
    }
    bytebuf_append(source, "}\n", 2U);
    bytebuf_push_u8(source, 0U);
    bytebuf_push_u8(events, 0U);
}

static void life_open(LifecycleFix *f, const char *name,
                      const char *events, const char *entry_source,
                      const char *helper_source)
{
    char manifest_source[1024];
    int n;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-plug-life-XXXXXX",
                 sizeof("/tmp/yew-plug-life-XXXXXX"));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    n = snprintf(f->data, sizeof(f->data), "%s/data", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->data));
    n = snprintf(f->config, sizeof(f->config), "%s/config", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->config));
    n = snprintf(f->state, sizeof(f->state), "%s/state", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->state));
    n = snprintf(f->workspace, sizeof(f->workspace), "%s/work", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->workspace));
    n = snprintf(f->plugins, sizeof(f->plugins), "%s/yew/plugins", f->data);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->plugins));
    n = snprintf(f->plugin_dir, sizeof(f->plugin_dir), "%s/%s",
                 f->plugins, name);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->plugin_dir));
    n = snprintf(f->source_dir, sizeof(f->source_dir), "%s/src",
                 f->plugin_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->source_dir));
    n = snprintf(f->entry, sizeof(f->entry), "%s/main.fl", f->source_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->entry));
    n = snprintf(f->helper, sizeof(f->helper), "%s/helper.fl",
                 f->source_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->helper));
    n = snprintf(f->manifest, sizeof(f->manifest), "%s/plugin.fl",
                 f->plugin_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->manifest));
    YEW_ASSERT(yew_mkdirs(f->source_dir, 0700U));
    YEW_ASSERT(yew_mkdirs(f->config, 0700U));
    YEW_ASSERT(yew_mkdirs(f->state, 0700U));
    YEW_ASSERT(yew_mkdirs(f->workspace, 0700U));
    life_write(f->entry, entry_source);
    if (helper_source != NULL)
        life_write(f->helper, helper_source);
    n = snprintf(manifest_source, sizeof(manifest_source),
                 "{name: \"%s\", version: \"1.0.0\", api: 1, "
                 "entry: \"src/main.fl\", capabilities: [], events: %s, "
                 "description: \"lifecycle fixture\"}\n",
                 name, events);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(manifest_source));
    life_write(f->manifest, manifest_source);

    f->old_data = life_strdup(getenv("XDG_DATA_HOME"));
    f->old_config = life_strdup(getenv("XDG_CONFIG_HOME"));
    f->old_state = life_strdup(getenv("XDG_STATE_HOME"));
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
    f->plug = yew_plug_find(&f->ed, name);
    YEW_ASSERT_NOT_NULL(f->plug);
    YEW_ASSERT_EQ_U64(f->plug->st, PLUG_DISCOVERED);
}

static void life_close(LifecycleFix *f)
{
    yew_trust_db_free(&f->trust);
    arena_free_all(&f->diag_arena);
    yew_ed_free(&f->ed);
    life_restore_env("XDG_DATA_HOME", f->old_data);
    life_restore_env("XDG_CONFIG_HOME", f->old_config);
    life_restore_env("XDG_STATE_HOME", f->old_state);
    free(f->old_data);
    free(f->old_config);
    free(f->old_state);
    life_remove_tree(f->root);
}

static u32 life_principal_modules(const LifecycleFix *f, u8 state)
{
    const FlVm *vm = yew_fl_vm((Ed *)&f->ed);
    u32 count = 0U;
    u32 i;

    for (i = 0U; i < vm->mods.n; i++)
        if (vm->mods.v[i].origin.principal_id == f->plug->origin_id &&
            vm->mods.v[i].state == (u8)state)
            count++;
    return count;
}

static void life_rediscover_caps(LifecycleFix *f, const char *name,
                                 const char *caps, const char *events,
                                 const char *source)
{
    char manifest_source[1024];
    int n;

    life_write(f->entry, source);
    n = snprintf(manifest_source, sizeof(manifest_source),
                 "{name: \"%s\", version: \"1.0.0\", api: 1, "
                 "entry: \"src/main.fl\", capabilities: %s, events: %s, "
                 "description: \"lifecycle fixture\"}\n",
                 name, caps, events);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(manifest_source));
    life_write(f->manifest, manifest_source);
    yew_plug_free(&f->ed);
    YEW_ASSERT(yew_plug_discover_with_policy(&f->ed, true, &f->trust,
                                              &f->dc));
    f->plug = yew_plug_find(&f->ed, name);
    YEW_ASSERT_NOT_NULL(f->plug);
}

void test_plug_lifecycle_capability_preflight_runs_bytecode_once(void)
{
    LifecycleFix f;
    char sentinel[512];
    char source[1400];
    char bytes[32] = {0};
    FILE *fp;
    int n;

    life_open(&f, "life-preflight", "[]", "fn init(ctx) nil\n", NULL);
    n = snprintf(sentinel, sizeof(sentinel), "%s/preflight-sentinel",
                 f.root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(sentinel));
    n = snprintf(source, sizeof(source),
                 "import io\n"
                 "io.write(\"%s\", \"top\\n\")\n"
                 "fn init(ctx) { io.append(\"%s\", \"init\\n\") }\n",
                 sentinel, sentinel);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(source));
    life_rediscover_caps(&f, "life-preflight", "[\"fs\", \"shell\"]",
                         "[]", source);

    /* Enable is accepted but no module bytecode runs before both decisions. */
    YEW_ASSERT(yew_plug_enable_desired(&f.ed, &f.dc));
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_PLUGIN_CAP);
    YEW_ASSERT(f.ed.plug->enable_desired_pending);
    YEW_ASSERT_EQ_I64(f.plug->st, PLUG_DISCOVERED);
    YEW_ASSERT_EQ_I64(access(sentinel, F_OK), -1);
    YEW_ASSERT_EQ_U64(f.ed.plug->prompt.cap, YEW_CAP_FS);

    YEW_ASSERT(yew_plug_prompt_key(&f.ed, (u32)'a'));
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_PLUGIN_CAP);
    YEW_ASSERT_EQ_U64(f.ed.plug->prompt.cap, YEW_CAP_SHELL);
    YEW_ASSERT_EQ_I64(f.plug->st, PLUG_DISCOVERED);
    YEW_ASSERT_EQ_I64(access(sentinel, F_OK), -1);

    /* A denial is a settled decision, not a refusal to load. */
    YEW_ASSERT(yew_plug_prompt_key(&f.ed, (u32)'d'));
    YEW_ASSERT_EQ_I64(f.ed.prompt, YEW_PROMPT_NONE);
    YEW_ASSERT(!f.ed.plug->enable_desired_pending);
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
    YEW_ASSERT((f.plug->session_allow & (1U << YEW_CAP_FS)) != 0U);
    YEW_ASSERT((f.plug->session_deny & (1U << YEW_CAP_SHELL)) != 0U);
    fp = fopen(sentinel, "rb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fread(bytes, 1U, sizeof(bytes), fp), 9U);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_STR(bytes, "top\ninit\n");
    life_close(&f);
}

void test_plug_lifecycle_enable_passes_ctx_and_registers_command_and_hook(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  if ctx.name != \"life-basic\" { error(\"wrong ctx\") }\n"
        "  ctx.command(\"ping\", fn() nil)\n"
        "  ctx.on(\"ed.idle\", fn() nil)\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    CmdId command;

    life_open(&f, "life-basic", "[\"ed.idle\"]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
    YEW_ASSERT(f.plug->rooted);
    YEW_ASSERT_EQ_U64(f.plug->module.t, FL_MAP);
    command = yew_cmd_lookup("ed.plug.life_basic.ping",
                             strlen("ed.plug.life_basic.ping"));
    YEW_ASSERT(command.v != YEW_CMD_NONE.v);
    YEW_ASSERT_EQ_U64(life_counts(&f).commands, before.commands + 1U);
    YEW_ASSERT_EQ_U64(life_counts(&f).hooks, before.hooks + 1U);
    YEW_ASSERT_EQ_U64(life_counts(&f).ledger, before.ledger + 2U);
    life_close(&f);
}

void test_plug_lifecycle_failing_init_leaves_zero_residue_and_trace(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.command(\"partial\", fn() nil)\n"
        "  ctx.on(\"ed.idle\", fn() nil)\n"
        "  error(\"init exploded\")\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    CmdId info;
    CmdCtx cx = {0};
    const char *shown;

    life_open(&f, "life-failing", "[\"ed.idle\"]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(!yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ERROR);
    YEW_ASSERT(!f.plug->rooted);
    YEW_ASSERT_EQ_U64(f.plug->module.t, FL_NIL);
    YEW_ASSERT_NOT_NULL(f.plug->last_error);
    YEW_ASSERT(strstr(f.plug->last_error, "init exploded") != NULL);
    YEW_ASSERT(strstr(f.plug->last_error, "at init") != NULL);
    YEW_ASSERT(strstr(f.plug->last_error, "^") != NULL);
    life_assert_counts(life_counts(&f), before);

    info = yew_cmd_lookup("ed.plug.info", strlen("ed.plug.info"));
    YEW_ASSERT(info.v != YEW_CMD_NONE.v);
    cx.source = YEW_SRC_TEST;
    cx.count = 1U;
    cx.sarg = "life-failing";
    cx.sarg_len = strlen(cx.sarg);
    YEW_ASSERT_EQ_U64(yew_ed_invoke(&f.ed, info, &cx), YEW_CMD_OK);
    shown = f.ed.msg.full == NULL ? f.ed.msg.text : f.ed.msg.full;
    YEW_ASSERT_EQ_U64(f.ed.msg.sev, YEW_MSG_ERROR);
    YEW_ASSERT_NOT_NULL(strstr(shown, "state: error"));
    YEW_ASSERT_NOT_NULL(strstr(shown, "last error:"));
    YEW_ASSERT_NOT_NULL(strstr(shown, "init exploded"));
    YEW_ASSERT_NOT_NULL(strstr(shown, "at init"));
    YEW_ASSERT_NOT_NULL(strstr(shown, "^"));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ERROR);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_ctx_on_rejects_event_missing_from_manifest(void)
{
    static const char source[] =
        "fn init(ctx) { ctx.on(\"ed.idle\", fn() nil) }\n";
    LifecycleFix f;
    LifecycleCounts before;

    life_open(&f, "life-events", "[]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(!yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ERROR);
    YEW_ASSERT_NOT_NULL(f.plug->last_error);
    YEW_ASSERT(strstr(f.plug->last_error,
                      "did not declare event ed.idle") != NULL);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_reverse_disable_leaves_zero_registry_residue(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.command(\"remove\", fn() nil)\n"
        "  ctx.on(\"ed.idle\", fn() nil)\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    u32 first = 0U;
    u32 second = 0U;
    u32 i;

    life_open(&f, "life-disable", "[\"ed.idle\"]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    for (i = 0U; i < f.ed.hooks.ledger.n; i++) {
        const FlRegistration *reg = &f.ed.hooks.ledger.v[i];

        if (!reg->active || reg->origin_id != f.plug->origin_id)
            continue;
        if (first == 0U)
            first = i + 1U;
        else
            second = i + 1U;
    }
    YEW_ASSERT(first != 0U);
    YEW_ASSERT(second != 0U);
    YEW_ASSERT_EQ_U64(f.ed.hooks.ledger.v[first - 1U].kind, REG_CMD);
    YEW_ASSERT_EQ_U64(f.ed.hooks.ledger.v[second - 1U].kind, REG_HOOK);
    YEW_ASSERT(yew_plug_disable(&f.ed, f.plug));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_reenable_keeps_imported_helper_principal(void)
{
    static const char source[] =
        "import \"helper.fl\" as helper\n"
        "fn init(ctx) {\n"
        "  if helper.answer != 54 { error(\"bad helper\") }\n"
        "  ctx.command(\"again\", fn() nil)\n"
        "  ctx.on(\"ed.idle\", fn() nil)\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    LifecycleCounts enabled;
    u32 principal;
    u32 principal_modules;

    life_open(&f, "life-reenable", "[\"ed.idle\"]", source,
              "let answer = 54\n");
    before = life_counts(&f);
    principal = f.plug->origin_id;
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    enabled = life_counts(&f);
    principal_modules = life_principal_modules(&f, FL_MOD_READY);
    YEW_ASSERT(principal_modules >= 2U);
    YEW_ASSERT(yew_plug_disable(&f.ed, f.plug));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    YEW_ASSERT_EQ_U64(f.plug->origin_id, principal);
    YEW_ASSERT_EQ_U64(life_principal_modules(&f, FL_MOD_READY), 0U);
    YEW_ASSERT_EQ_U64(life_principal_modules(&f, FL_MOD_DROPPED),
                      principal_modules);
    life_assert_counts(life_counts(&f), before);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
    YEW_ASSERT_EQ_U64(f.plug->origin_id, principal);
    YEW_ASSERT_EQ_U64(life_principal_modules(&f, FL_MOD_READY),
                      principal_modules);
    life_assert_counts(life_counts(&f), enabled);
    life_close(&f);
}

void test_plug_lifecycle_fifth_hook_error_auto_disables_plugin(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.on(\"ed.idle\", fn() error(\"hook exploded\"))\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    u32 i;

    life_open(&f, "life-errors", "[\"ed.idle\"]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    for (i = 0U; i < 4U; i++) {
        yew_fl_hook_fire(&f.ed, FL_EV_ED_IDLE, NULL, 0U);
        YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
        YEW_ASSERT_EQ_U64(f.plug->err_count, i + 1U);
    }
    yew_fl_hook_fire(&f.ed, FL_EV_ED_IDLE, NULL, 0U);
    YEW_ASSERT_EQ_U64(f.plug->err_count, 5U);
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    YEW_ASSERT_NOT_NULL(f.plug->last_error);
    YEW_ASSERT(strstr(f.plug->last_error, "hook exploded") != NULL);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_command_errors_share_plugin_limit(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.command(\"boom\", fn() error(\"command exploded\"))\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    CmdId command;
    CmdCtx cx = {0};
    u32 i;

    life_open(&f, "life-command-errors", "[]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    command = yew_cmd_lookup("ed.plug.life_command_errors.boom",
                             strlen("ed.plug.life_command_errors.boom"));
    YEW_ASSERT(command.v != YEW_CMD_NONE.v);
    cx.source = YEW_SRC_TEST;
    cx.count = 1U;
    for (i = 0U; i < 5U; i++)
        YEW_ASSERT_EQ_U64(yew_ed_invoke(&f.ed, command, &cx),
                          YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(f.plug->err_count, 5U);
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    YEW_ASSERT(strstr(f.plug->last_error, "command exploded") != NULL);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_generic_hook_limit_cannot_preempt_plugin_limit(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.on(\"ed.idle\", fn() error(\"hook exploded\"))\n"
        "}\n";
    LifecycleFix f;
    u32 i;

    life_open(&f, "life-hook-limits", "[\"ed.idle\"]", source, NULL);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    fl_hook_error_limit(&f.ed.hooks, 1U);
    for (i = 0U; i < 4U; i++) {
        yew_fl_hook_fire(&f.ed, FL_EV_ED_IDLE, NULL, 0U);
        YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
        YEW_ASSERT_EQ_U64(f.plug->err_count, i + 1U);
    }
    yew_fl_hook_fire(&f.ed, FL_EV_ED_IDLE, NULL, 0U);
    YEW_ASSERT_EQ_U64(f.plug->err_count, 5U);
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    life_close(&f);
}

void test_plug_lifecycle_drains_every_plugin_reaching_limit_in_one_event(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.on(\"ed.idle\", fn() error(\"hook exploded\"))\n"
        "}\n";
    LifecycleFix f;
    Plug *first;
    Plug *second;
    u32 i;

    life_open(&f, "life-queue-a", "[\"ed.idle\"]", source, NULL);
    life_add_plugin(&f, "life-queue-b", "[\"ed.idle\"]", source);
    yew_plug_free(&f.ed);
    YEW_ASSERT(yew_plug_discover_with_policy(&f.ed, true, &f.trust,
                                              &f.dc));
    first = yew_plug_find(&f.ed, "life-queue-a");
    second = yew_plug_find(&f.ed, "life-queue-b");
    YEW_ASSERT_NOT_NULL(first);
    YEW_ASSERT_NOT_NULL(second);
    YEW_ASSERT(yew_plug_enable(&f.ed, first, &f.dc));
    YEW_ASSERT(yew_plug_enable(&f.ed, second, &f.dc));
    for (i = 0U; i < 4U; i++)
        yew_fl_hook_fire(&f.ed, FL_EV_ED_IDLE, NULL, 0U);
    YEW_ASSERT_EQ_U64(first->st, PLUG_ENABLED);
    YEW_ASSERT_EQ_U64(second->st, PLUG_ENABLED);
    yew_fl_hook_fire(&f.ed, FL_EV_ED_IDLE, NULL, 0U);
    YEW_ASSERT_EQ_U64(first->err_count, 5U);
    YEW_ASSERT_EQ_U64(second->err_count, 5U);
    YEW_ASSERT_EQ_U64(first->st, PLUG_DISABLED);
    YEW_ASSERT_EQ_U64(second->st, PLUG_DISABLED);
    life_close(&f);
}

void test_plug_lifecycle_bound_errors_share_plugin_limit(void)
{
    static const char source[] =
        "fn init(ctx) {\n"
        "  ctx.bind(\"L\", \"z\", fn() error(\"bind exploded\"))\n"
        "}\n";
    LifecycleFix f;
    LifecycleCounts before;
    CmdId closure;
    CmdCtx cx = {0};
    u32 ledger_id = 0U;
    u32 i;

    life_open(&f, "life-bind-errors", "[]", source, NULL);
    before = life_counts(&f);
    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    for (i = 0U; i < f.ed.hooks.ledger.n; i++)
        if (f.ed.hooks.ledger.v[i].active &&
            f.ed.hooks.ledger.v[i].origin_id == f.plug->origin_id &&
            f.ed.hooks.ledger.v[i].kind == (u8)REG_BIND) {
            ledger_id = i + 1U;
            break;
        }
    YEW_ASSERT(ledger_id != 0U);
    closure = yew_cmd_lookup("ed.fl.closure", strlen("ed.fl.closure"));
    YEW_ASSERT(closure.v != YEW_CMD_NONE.v);
    cx.source = YEW_SRC_TEST;
    cx.count = 1U;
    cx.iarg = (i64)ledger_id;
    for (i = 0U; i < 5U; i++)
        YEW_ASSERT_EQ_U64(yew_ed_invoke(&f.ed, closure, &cx),
                          YEW_CMD_ERR_STATE);
    YEW_ASSERT_EQ_U64(f.plug->err_count, 5U);
    YEW_ASSERT_EQ_U64(f.plug->st, PLUG_DISABLED);
    YEW_ASSERT(strstr(f.plug->last_error, "bind exploded") != NULL);
    life_assert_counts(life_counts(&f), before);
    life_close(&f);
}

void test_plug_lifecycle_hostile_surface_fires_and_tears_down_cleanly(void)
{
    Bytebuf source;
    Bytebuf events;
    LifecycleFix f;
    LifecycleCounts before;
    LifecycleCounts enabled;
    u32 raw_commands;
    u32 raw_values;
    u32 event;
    u32 registered_events = 0U;
    u32 fired_errors = 0U;

    life_hostile_source(&source, &events);
    life_open(&f, "life-hostile", (const char *)events.data,
              (const char *)source.data, NULL);
    bytebuf_free(&events);
    bytebuf_free(&source);
    before = life_counts(&f);
    for (event = 0U; event < (u32)FL_EV__N; event++) {
        const char *name = fl_event_name(event);

        YEW_ASSERT_NOT_NULL(name);
        if (yew_plug_event_valid(name, strlen(name)))
            registered_events++;
    }
    raw_commands = f.ed.plug->ncmds;
    raw_values = f.ed.plug->nregs;
    yew_plug_error_limit_set(&f.ed, (u32)FL_EV__N + 1U);

    YEW_ASSERT(yew_plug_enable(&f.ed, f.plug, &f.dc));
    enabled = life_counts(&f);
    YEW_ASSERT_EQ_U64(enabled.commands, before.commands + 100U);
    YEW_ASSERT_EQ_U64(enabled.binds, before.binds + 123U);
    YEW_ASSERT_EQ_U64(enabled.hooks, before.hooks + registered_events);
    YEW_ASSERT_EQ_U64(enabled.ledger,
                      before.ledger + 100U + 123U + registered_events);
    YEW_ASSERT_EQ_U64(enabled.plug_commands,
                      before.plug_commands + 100U);
    YEW_ASSERT_EQ_U64(enabled.plug_values, before.plug_values);
    YEW_ASSERT_EQ_U64(f.ed.plug->ncmds, raw_commands + 100U);
    YEW_ASSERT_EQ_U64(f.ed.plug->nregs, raw_values);

    for (event = 0U; event < (u32)FL_EV__N; event++) {
        const char *name = fl_event_name(event);

        yew_fl_hook_fire(&f.ed, (FlEvent)event, NULL, 0U);
        if (yew_plug_event_valid(name, strlen(name)))
            fired_errors++;
        YEW_ASSERT_EQ_U64(f.plug->st, PLUG_ENABLED);
        YEW_ASSERT_EQ_U64(f.plug->err_count, fired_errors);
    }
    YEW_ASSERT_EQ_U64(fired_errors, registered_events);
    YEW_ASSERT_NOT_NULL(f.plug->last_error);
    YEW_ASSERT_NOT_NULL(strstr(f.plug->last_error, "hostile callback"));

    YEW_ASSERT(yew_plug_disable(&f.ed, f.plug));
    life_assert_counts(life_counts(&f), before);
    YEW_ASSERT_EQ_U64(f.ed.plug->ncmds, raw_commands);
    YEW_ASSERT_EQ_U64(f.ed.plug->nregs, raw_values);
    life_close(&f);
}
