/*
 * YEW-F-008 — an unprivileged plugin can launder authority through a macro.
 *
 * Correct behavior: a plugin declaring no capabilities cannot create a file.
 * In particular, writing a register through ed.run and replaying it must not
 * turn the register's source into user-config code.  The replay must retain
 * the plugin defining origin, so io.write raises "capability".
 *
 * Baseline failure: ed.run accepts the internal ed.reg.set command from a
 * Fletch caller.  yew_macro_replay then compiles that plugin-supplied source
 * through fl_compile_str, which assigns runtime_origin() (trusted config,
 * FL_CAP_ALL).  The replayed macro can consequently write this file although
 * the plugin manifest requests no fs.write capability.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/diag.h"
#include "mod/plug/internal.h"
#include "util/xdg.h"
#include "ws/trust.h"

typedef struct F008Fix {
    char root[128];
    char data[192];
    char config[192];
    char state[192];
    char work[192];
    char source_dir[320];
    char manifest[352];
    char entry[352];
    char escaped[192];
    char *old_data;
    char *old_config;
    char *old_state;
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    YewTrustDb trust;
    bool env_data_set;
    bool env_config_set;
    bool env_state_set;
    bool ed_ready;
    bool diag_ready;
    bool trust_ready;
} F008Fix;

static char *f008_env_copy(const char *name)
{
    const char *value = getenv(name);
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;
    len = strlen(value);
    copy = malloc(len + 1U);
    if (copy != NULL)
        (void)memcpy(copy, value, len + 1U);
    return copy;
}

static bool f008_env_set(const char *name, const char *value)
{
    return setenv(name, value, 1) == 0;
}

static void f008_env_restore(const char *name, const char *value)
{
    if (value == NULL)
        (void)unsetenv(name);
    else
        (void)setenv(name, value, 1);
}

static bool f008_write(const char *path, const char *text)
{
    FILE *file;
    size_t len = strlen(text);

    file = fopen(path, "wb");
    if (file == NULL)
        return false;
    if (fwrite(text, 1U, len, file) != len) {
        (void)fclose(file);
        return false;
    }
    return fclose(file) == 0;
}

static void f008_close(F008Fix *f)
{
    if (f->trust_ready)
        yew_trust_db_free(&f->trust);
    if (f->diag_ready)
        arena_free_all(&f->diag_arena);
    if (f->ed_ready)
        yew_ed_free(&f->ed);
    if (f->env_data_set)
        f008_env_restore("XDG_DATA_HOME", f->old_data);
    if (f->env_config_set)
        f008_env_restore("XDG_CONFIG_HOME", f->old_config);
    if (f->env_state_set)
        f008_env_restore("XDG_STATE_HOME", f->old_state);
    free(f->old_data);
    free(f->old_config);
    free(f->old_state);
    (void)unlink(f->escaped);
    (void)unlink(f->entry);
    (void)unlink(f->manifest);
    (void)rmdir(f->source_dir);
    {
        char plugin_dir[288];
        char yew_dir[224];

        (void)snprintf(plugin_dir, sizeof(plugin_dir), "%s/yew/plugins/audit-cap",
                       f->data);
        (void)snprintf(yew_dir, sizeof(yew_dir), "%s/yew/plugins", f->data);
        (void)rmdir(plugin_dir);
        (void)rmdir(yew_dir);
        (void)snprintf(yew_dir, sizeof(yew_dir), "%s/yew", f->data);
        (void)rmdir(yew_dir);
    }
    (void)rmdir(f->data);
    (void)rmdir(f->config);
    (void)rmdir(f->state);
    (void)rmdir(f->work);
    (void)rmdir(f->root);
}

static bool f008_open(F008Fix *f)
{
    static const char manifest_source[] =
        "{name: \"audit-cap\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], events: [], "
        "description: \"F08 capability audit fixture\"}\n";
    char entry_source[1024];
    int n;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-f008-XXXXXX",
                 sizeof("/tmp/yew-f008-XXXXXX"));
    if (mkdtemp(f->root) == NULL)
        return false;
    n = snprintf(f->data, sizeof(f->data), "%s/data", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->data))
        return false;
    n = snprintf(f->config, sizeof(f->config), "%s/config", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->config))
        return false;
    n = snprintf(f->state, sizeof(f->state), "%s/state", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->state))
        return false;
    n = snprintf(f->work, sizeof(f->work), "%s/work", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->work))
        return false;
    n = snprintf(f->source_dir, sizeof(f->source_dir),
                 "%s/yew/plugins/audit-cap/src", f->data);
    if (n <= 0 || (size_t)n >= sizeof(f->source_dir))
        return false;
    n = snprintf(f->manifest, sizeof(f->manifest),
                 "%s/yew/plugins/audit-cap/plugin.fl", f->data);
    if (n <= 0 || (size_t)n >= sizeof(f->manifest))
        return false;
    n = snprintf(f->entry, sizeof(f->entry), "%s/main.fl", f->source_dir);
    if (n <= 0 || (size_t)n >= sizeof(f->entry))
        return false;
    n = snprintf(f->escaped, sizeof(f->escaped), "%s/escaped.txt", f->root);
    if (n <= 0 || (size_t)n >= sizeof(f->escaped))
        return false;
    if (!yew_mkdirs(f->source_dir, 0700U) ||
        !yew_mkdirs(f->config, 0700U) || !yew_mkdirs(f->state, 0700U) ||
        !yew_mkdirs(f->work, 0700U))
        return false;
    n = snprintf(entry_source, sizeof(entry_source),
                 "import ed\n"
                 "fn init(ctx) {\n"
                 "  ed.run(\"ed.reg.set\", {iarg: 97, sarg: "
                 "\"import io\\nio.write(\\\"%s\\\", \\\"owned\\\")\\n\"})\n"
                 "  ed.run(\"ed.macro.replay\", {sarg: \"a\"})\n"
                 "}\n",
                 f->escaped);
    if (n <= 0 || (size_t)n >= sizeof(entry_source) ||
        !f008_write(f->manifest, manifest_source) ||
        !f008_write(f->entry, entry_source))
        return false;
    f->old_data = f008_env_copy("XDG_DATA_HOME");
    f->old_config = f008_env_copy("XDG_CONFIG_HOME");
    f->old_state = f008_env_copy("XDG_STATE_HOME");
    if ((getenv("XDG_DATA_HOME") != NULL && f->old_data == NULL) ||
        (getenv("XDG_CONFIG_HOME") != NULL && f->old_config == NULL) ||
        (getenv("XDG_STATE_HOME") != NULL && f->old_state == NULL) ||
        !f008_env_set("XDG_DATA_HOME", f->data))
        return false;
    f->env_data_set = true;
    if (!f008_env_set("XDG_CONFIG_HOME", f->config))
        return false;
    f->env_config_set = true;
    if (!f008_env_set("XDG_STATE_HOME", f->state))
        return false;
    f->env_state_set = true;
    yew_ed_init(&f->ed);
    f->ed_ready = true;
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->work);
    if (!yew_ed_open_scratch(&f->ed))
        return false;
    arena_init(&f->diag_arena);
    f->diag_ready = true;
    fl_diag_init(&f->dc, &f->diag_arena);
    yew_trust_db_init(&f->trust);
    f->trust_ready = true;
    return yew_plug_discover_with_policy(&f->ed, true, &f->trust, &f->dc);
}

bool test_yew_f_008(char *why, size_t why_cap)
{
    F008Fix f;
    Plug *plug;
    struct stat st;
    bool enabled;
    bool escaped;
    bool setup_ok;

    setup_ok = f008_open(&f);
    if (!setup_ok) {
        f008_close(&f);
        return true; /* Infrastructure failures must be hard XPASSes. */
    }
    plug = yew_plug_find(&f.ed, "audit-cap");
    if (plug == NULL) {
        f008_close(&f);
        return true;
    }
    enabled = yew_plug_enable(&f.ed, plug, &f.dc);
    escaped = stat(f.escaped, &st) == 0 && S_ISREG(st.st_mode) &&
              (u64)st.st_size == sizeof("owned") - 1U;
    if (enabled && escaped && why != NULL && why_cap > 0U) {
        (void)snprintf(why, why_cap,
                       "plugin with capabilities: [] wrote %s via macro replay",
                       f.escaped);
    }
    f008_close(&f);
    /* The only correct result is no unauthorized file and a failed plugin
     * initialization.  On the immutable baseline this is false (XFAIL). */
    return !enabled && !escaped;
}
