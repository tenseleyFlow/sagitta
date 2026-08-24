#define _POSIX_C_SOURCE 200809L

#include "mod/plug/internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flconf.h"
#include "fl/origin.h"
#include "mod/plug/pkg.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/sort.h"
#include "util/xdg.h"

typedef struct PlugDiagCapture {
    Bytebuf text;
} PlugDiagCapture;

static char *plug_strdup(const char *text)
{
    size_t len = strlen(text);
    char *copy = yew_xmalloc(len + 1U);

    (void)memcpy(copy, text, len + 1U);
    return copy;
}

static char *path_join_alloc(const char *left, const char *right)
{
    size_t nl = strlen(left);
    size_t nr = strlen(right);
    bool slash = nl != 0U && left[nl - 1U] != '/';
    char *path = yew_xmalloc(nl + (slash ? 1U : 0U) + nr + 1U);
    size_t at = 0U;

    (void)memcpy(path + at, left, nl);
    at += nl;
    if (slash)
        path[at++] = '/';
    (void)memcpy(path + at, right, nr + 1U);
    return path;
}

static int name_compare(const void *a, const void *b, void *ctx)
{
    const char *const *pa = a;
    const char *const *pb = b;

    (void)ctx;
    return strcmp(*pa, *pb);
}

static void capture_diag(void *ctx, FlDiagLevel level, FlSpan span,
                         const char *msg, const char *rendered)
{
    PlugDiagCapture *capture = ctx;

    (void)level;
    (void)span;
    (void)rendered;
    if (capture->text.len != 0U)
        bytebuf_push_u8(&capture->text, (u8)'\n');
    bytebuf_append(&capture->text, (const u8 *)msg, strlen(msg));
}

static PlugSys *plug_sys_get(Ed *ed)
{
    if (ed->plug == NULL) {
        ed->plug = yew_xcalloc(1U, sizeof(*ed->plug));
        arena_init(&ed->plug->arena);
        ed->plug->next_handle = 1U;
    }
    return ed->plug;
}

static void plug_push(PlugSys *sys, Plug *plug)
{
    if (sys->n == sys->cap) {
        u32 want = sys->cap == 0U ? 8U : sys->cap * 2U;

        if (want < sys->cap)
            YEW_BUG("plugin discovery table overflow");
        sys->v = yew_xreallocarray(sys->v, want, sizeof(*sys->v));
        sys->cap = want;
    }
    sys->v[sys->n++] = plug;
}

static YewPluginDesired desired_state(const Ed *ed,
                                      const YewTrustDb *policy,
                                      const char *name)
{
    if (policy != NULL)
        return yew_trust_plugin_desired(policy, name);
    return yew_config_plugin_desired(ed, name);
}

static void shadow_earlier(PlugSys *sys, const char *name)
{
    u32 i;

    for (i = 0U; i < sys->n; i++) {
        Plug *prior = sys->v[i];

        if (prior->mf.name_text != NULL &&
            strcmp(prior->mf.name_text, name) == 0) {
            prior->winner = false;
            prior->st = PLUG_SHADOWED;
        }
    }
}

static void intern_manifest(Ed *ed, PlugManifest *mf)
{
    u32 i;

    mf->name = yew_intern_cstr(&ed->interner, mf->name_text);
    for (i = 0U; i < mf->nevents; i++)
        mf->events[i] = yew_intern_cstr(&ed->interner,
                                        mf->event_names[i]);
}

static bool verify_on_load(Ed *ed)
{
    OptVal value;

    if (!yew_opt_get(ed, NULL, NULL, "plug.verify_on_load", 19U,
                     &value) || value.type != (u8)YEW_OPT_BOOL)
        return true;
    return value.as.b;
}

static bool verify_managed_candidate(Ed *ed, Plug *plug,
                                     bool use_live_config, DiagCtx *dc)
{
    char expected[YEW_PKG_TREE_HEX + 1U];
    char actual[YEW_PKG_TREE_HEX + 1U];
    bool managed = false;
    u32 dropped = 0U;

    if (plug->source != PLUG_SOURCE_DATA || !verify_on_load(ed))
        return true;
    if (!yew_pkg_expected_tree(plug->mf.name_text, expected, &managed, dc))
        return false;
    if (!managed)
        return true;
    if (!yew_pkg_tree_hash(plug->mf.dir, actual, dc))
        return false;
    if (memcmp(expected, actual, YEW_PKG_TREE_HEX) == 0)
        return true;

    yew_msg(ed, YEW_MSG_WARN,
            "plugin \"%s\" changed on disk since install "
            "(yew pkg doctor %s)",
            plug->mf.name_text, plug->mf.name_text);
    yew_log(YEW_LOG_WARN,
            "plugin \"%s\" changed on disk since install "
            "(yew pkg doctor %s)",
            plug->mf.name_text, plug->mf.name_text);
    plug->session_allow = 0U;
    plug->session_deny = 0U;
    if (!(use_live_config ?
          yew_config_plugin_drop_grants(ed, plug->mf.name_text, &dropped) :
          yew_trust_plugin_revoke_persisted(plug->mf.name_text,
                                             &dropped))) {
        if (dc != NULL)
            fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                         "cannot revoke persisted grants for plugin %s",
                         plug->mf.name_text);
        return false;
    }
    if (dropped != 0U)
        yew_log(YEW_LOG_WARN,
                "revoked %u persisted grant(s) for \"%s\": code changed",
                dropped, plug->mf.name_text);
    return true;
}

static bool add_candidate(Ed *ed, PlugSource source, const char *dir,
                          const char *entry_name, bool workspace_trusted,
                          const YewTrustDb *policy)
{
    PlugSys *sys = plug_sys_get(ed);
    Plug *plug = yew_xcalloc(1U, sizeof(*plug));
    PlugDiagCapture capture;
    DiagCtx local_dc;
    bool valid;

    bytebuf_init(&capture.text);
    fl_diag_init(&local_dc, &sys->arena);
    fl_diag_set_sink(&local_dc, capture_diag, &capture);
    valid = yew_plug_manifest_read(&sys->arena, dir, &plug->mf, &local_dc);
    if (!valid) {
        plug->mf.dir = arena_strdup(&sys->arena, dir);
        plug->mf.name_text = arena_strdup(&sys->arena, entry_name);
        plug->mf.version = arena_strdup(&sys->arena, "");
        if (capture.text.len == 0U)
            bytebuf_append(&capture.text,
                           (const u8 *)"invalid plugin manifest",
                           sizeof("invalid plugin manifest") - 1U);
        bytebuf_push_u8(&capture.text, 0U);
        plug->last_error = plug_strdup((const char *)capture.text.data);
        yew_log(YEW_LOG_WARN, "plugin %s: %s", entry_name,
                plug->last_error);
    }
    bytebuf_free(&capture.text);

    intern_manifest(ed, &plug->mf);
    shadow_earlier(sys, plug->mf.name_text);
    plug->source = source;
    plug->winner = true;
    plug->origin_id = fl_origin_register(ed, FL_ORIGIN_PLUGIN,
                                         plug->mf.name_text, 0U);
    if (!valid)
        plug->st = PLUG_ERROR;
    else if (source == PLUG_SOURCE_WORKSPACE && !workspace_trusted)
        plug->st = PLUG_BLOCKED;
    else if (desired_state(ed, policy, plug->mf.name_text) ==
             YEW_PLUGIN_DESIRED_DISABLED)
        plug->st = PLUG_DISABLED;
    else
        plug->st = PLUG_DISCOVERED;
    plug_push(sys, plug);
    return true;
}

static bool scan_root(Ed *ed, const char *root, PlugSource source,
                      bool workspace_trusted, const YewTrustDb *policy,
                      DiagCtx *dc)
{
    DIR *dir;
    struct dirent *ent;
    char **names = NULL;
    u32 n = 0U;
    u32 cap = 0U;
    u32 i;
    bool ok = true;

    if (root == NULL)
        return true;
    dir = opendir(root);
    if (dir == NULL) {
        if (errno == ENOENT || errno == ENOTDIR)
            return true;
        if (dc != NULL)
            fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                         "cannot scan plugin directory %s: %s", root,
                         strerror(errno));
        return false;
    }
    errno = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0)
            continue;
        if (n == cap) {
            u32 want = cap == 0U ? 16U : cap * 2U;

            if (want < cap)
                YEW_BUG("plugin directory entry overflow");
            names = yew_xreallocarray(names, want, sizeof(*names));
            cap = want;
        }
        names[n++] = plug_strdup(ent->d_name);
    }
    if (errno != 0) {
        if (dc != NULL)
            fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                         "cannot read plugin directory %s: %s", root,
                         strerror(errno));
        ok = false;
    }
    if (closedir(dir) != 0)
        ok = false;
    yew_sort_stable(names, n, sizeof(*names), name_compare, NULL);
    for (i = 0U; ok && i < n; i++) {
        char *candidate = path_join_alloc(root, names[i]);
        char *manifest = path_join_alloc(candidate, "plugin.fl");
        struct stat st;
        struct stat mst;

        if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode)) {
            yew_log(YEW_LOG_WARN, "plugin discovery: skipping non-directory %s",
                    candidate);
        } else if (stat(manifest, &mst) != 0 && errno == ENOENT) {
            yew_log(YEW_LOG_WARN, "plugin discovery: skipping %s without plugin.fl",
                    candidate);
        } else {
            ok = add_candidate(ed, source, candidate, names[i],
                               workspace_trusted, policy);
        }
        free(manifest);
        free(candidate);
    }
    for (i = 0U; i < n; i++)
        free(names[i]);
    free(names);
    return ok;
}

static void verify_managed_winners(Ed *ed, bool use_live_config,
                                   DiagCtx *dc)
{
    PlugSys *sys = ed->plug;
    u32 i;

    if (sys == NULL)
        return;
    for (i = 0U; i < sys->n; i++) {
        Plug *plug = sys->v[i];

        if (!plug->winner || plug->source != PLUG_SOURCE_DATA ||
            plug->st == PLUG_ERROR ||
            verify_managed_candidate(ed, plug, use_live_config, dc))
            continue;
        plug->last_error = plug_strdup(
            "cannot verify managed plugin integrity");
        plug->st = PLUG_ERROR;
        yew_log(YEW_LOG_ERROR, "plugin %s: %s", plug->mf.name_text,
                plug->last_error);
    }
}

bool yew_plug_discover_with_policy(Ed *ed, bool workspace_trusted,
                                   const YewTrustDb *policy, DiagCtx *dc)
{
    char *data;
    char *config;
    char *data_plugins = NULL;
    char *config_plugins = NULL;
    char *ws_meta = NULL;
    char *ws_plugins = NULL;
    bool ok;

    if (ed == NULL)
        return false;
    if (ed->plug != NULL && ed->plug->n != 0U)
        return true;
    data = yew_xdg_data_dir();
    config = yew_xdg_config_dir();
    if (data != NULL)
        data_plugins = path_join_alloc(data, "plugins");
    if (config != NULL)
        config_plugins = path_join_alloc(config, "plugins");
    ws_meta = path_join_alloc(yew_ws_root(ed), ".yew");
    ws_plugins = path_join_alloc(ws_meta, "plugins");

    ok = scan_root(ed, data_plugins, PLUG_SOURCE_DATA, workspace_trusted,
                   policy, dc) &&
         scan_root(ed, config_plugins, PLUG_SOURCE_CONFIG,
                   workspace_trusted, policy, dc) &&
         scan_root(ed, ws_plugins, PLUG_SOURCE_WORKSPACE,
                   workspace_trusted, policy, dc);
    if (ok)
        verify_managed_winners(ed, policy == NULL && ed->config != NULL,
                               dc);
    free(ws_plugins);
    free(ws_meta);
    free(config_plugins);
    free(data_plugins);
    free(config);
    free(data);
    return ok;
}

bool yew_plug_discover(Ed *ed, DiagCtx *dc)
{
    if (ed == NULL || ed->clean)
        return ed != NULL;
    return yew_plug_discover_with_policy(
        ed, yew_config_workspace_plugins_trusted(ed), NULL, dc);
}

static const char *cli_state(const Plug *plug)
{
    switch (plug->st) {
    case PLUG_DISABLED: return "disabled";
    case PLUG_ERROR: return "error";
    case PLUG_BLOCKED: return "blocked";
    case PLUG_SHADOWED: return "shadowed";
    default: return "enabled";
    }
}

static const char *cli_source(PlugSource source)
{
    switch (source) {
    case PLUG_SOURCE_CONFIG: return "config";
    case PLUG_SOURCE_WORKSPACE: return "workspace";
    default: return "data";
    }
}

static Plug *cli_find(Ed *ed, const char *name)
{
    u32 i;

    if (ed->plug == NULL)
        return NULL;
    for (i = ed->plug->n; i != 0U; i--) {
        Plug *plug = ed->plug->v[i - 1U];

        if (plug->winner && strcmp(plug->mf.name_text, name) == 0)
            return plug;
    }
    return NULL;
}

static void print_long_fields(const Plug *plug)
{
    u32 cap;
    u32 event;
    bool first = true;

    (void)putchar('\t');
    for (cap = 0U; cap < (u32)YEW_CAP__N; cap++) {
        if ((plug->mf.caps_wanted & (1U << cap)) == 0U)
            continue;
        if (!first)
            (void)putchar(',');
        (void)fputs(yew_cap_name((YewCap)cap), stdout);
        first = false;
    }
    (void)putchar('\t');
    for (event = 0U; event < plug->mf.nevents; event++) {
        if (event != 0U)
            (void)putchar(',');
        (void)fputs(plug->mf.event_names[event], stdout);
    }
    (void)putchar('\t');
    (void)fputs(plug->mf.desc == NULL ? "" : plug->mf.desc, stdout);
}

static int plug_cli_run(int argc, char **argv)
{
    Ed ed;
    Arena diag_arena;
    DiagCtx dc;
    YewTrustDb trust;
    const char *command = argc >= 2 ? argv[1] : NULL;
    const char *name = argc >= 3 ? argv[2] : NULL;
    bool long_list = argc == 3 && strcmp(argv[2], "--long") == 0;
    int result = YEW_EXIT_OK;
    u32 i;

    if (command == NULL ||
        (strcmp(command, "list") != 0 && strcmp(command, "enable") != 0 &&
         strcmp(command, "disable") != 0 &&
         strcmp(command, "reload") != 0)) {
        (void)fputs("yew: error: usage: yew plug list [--long] | "
                    "enable NAME | disable NAME | reload NAME\n", stderr);
        return YEW_EXIT_ERR;
    }
    if ((strcmp(command, "list") == 0 && argc != 2 && !long_list) ||
        (strcmp(command, "list") != 0 && argc != 3)) {
        (void)fputs("yew: error: invalid yew plug arguments\n", stderr);
        return YEW_EXIT_ERR;
    }

    yew_trust_db_init(&trust);
    if (!yew_trust_db_load(&trust)) {
        yew_trust_db_free(&trust);
        (void)fputs("yew: error: cannot read plugin trust database\n",
                    stderr);
        return YEW_EXIT_IO;
    }
    yew_ed_init(&ed);
    ed.headless = true;
    arena_init(&diag_arena);
    fl_diag_init(&dc, &diag_arena);
    if (!yew_plug_discover_with_policy(&ed, false, &trust, &dc)) {
        result = YEW_EXIT_IO;
        goto done;
    }
    if (strcmp(command, "list") == 0) {
        for (i = 0U; ed.plug != NULL && i < ed.plug->n; i++) {
            const Plug *plug = ed.plug->v[i];

            (void)printf("%s\t%s\t%s\t%s", plug->mf.name_text,
                         cli_state(plug),
                         plug->mf.version == NULL ? "" : plug->mf.version,
                         cli_source(plug->source));
            if (long_list)
                print_long_fields(plug);
            (void)putchar('\n');
        }
    } else {
        Plug *plug = cli_find(&ed, name);

        if (plug == NULL) {
            (void)fprintf(stderr, "yew: error: unknown plugin %s\n", name);
            result = YEW_EXIT_ERR;
        } else if (strcmp(command, "reload") == 0) {
            (void)printf("yew: reload %s in a running editor; no changes made\n",
                         name);
        } else {
            YewPluginDesired desired = strcmp(command, "enable") == 0 ?
                YEW_PLUGIN_DESIRED_ENABLED : YEW_PLUGIN_DESIRED_DISABLED;
            time_t now = time(NULL);

            if (now == (time_t)-1 ||
                !yew_trust_plugin_set_desired(&trust, name, desired) ||
                !yew_trust_db_write(&trust, now,
                                    YEW_TRUST_PRUNE_DAYS_DEFAULT)) {
                (void)fprintf(stderr,
                              "yew: error: cannot persist plugin %s state\n",
                              name);
                result = YEW_EXIT_IO;
            }
        }
    }

done:
    arena_free_all(&diag_arena);
    yew_ed_free(&ed);
    yew_trust_db_free(&trust);
    return result;
}

int yew_plug_main(int argc, char **argv)
{
    return plug_cli_run(argc, argv);
}
