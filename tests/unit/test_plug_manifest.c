#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mod/plug/manifest.h"

typedef struct ManifestFix {
    Arena arena;
    DiagCtx dc;
    char root[256];
    char dir[320];
    char src_dir[384];
    char manifest[384];
    char entry[448];
    char diag[4096];
    size_t ndiag;
} ManifestFix;

static void manifest_diag(void *ctx, FlDiagLevel level, FlSpan span,
                          const char *msg, const char *rendered)
{
    ManifestFix *f = ctx;
    size_t n = strlen(msg);

    (void)level;
    (void)span;
    (void)rendered;
    if (f->ndiag != 0U && f->ndiag + 1U < sizeof(f->diag))
        f->diag[f->ndiag++] = '\n';
    if (n > sizeof(f->diag) - f->ndiag - 1U)
        n = sizeof(f->diag) - f->ndiag - 1U;
    (void)memcpy(f->diag + f->ndiag, msg, n);
    f->ndiag += n;
    f->diag[f->ndiag] = '\0';
}

static void manifest_write(const char *path, const char *text)
{
    size_t len = strlen(text);
    size_t off = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    YEW_ASSERT(fd >= 0);
    while (fd >= 0 && off < len) {
        ssize_t n = write(fd, text + off, len - off);

        YEW_ASSERT(n > 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    if (fd >= 0)
        YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void manifest_fix_init(ManifestFix *f, const char *dirname,
                              const char *source)
{
    int n;

    (void)memset(f, 0, sizeof(*f));
    (void)memcpy(f->root, "/tmp/yew-plug-manifest-XXXXXX",
                 sizeof("/tmp/yew-plug-manifest-XXXXXX"));
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    n = snprintf(f->dir, sizeof(f->dir), "%s/%s", f->root, dirname);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->dir));
    n = snprintf(f->src_dir, sizeof(f->src_dir), "%s/src", f->dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->src_dir));
    n = snprintf(f->manifest, sizeof(f->manifest), "%s/plugin.fl", f->dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->manifest));
    n = snprintf(f->entry, sizeof(f->entry), "%s/main.fl", f->src_dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->entry));
    YEW_ASSERT_EQ_I64(mkdir(f->dir, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(f->src_dir, 0700), 0);
    manifest_write(f->entry, "{ value: 1 }\n");
    manifest_write(f->manifest, source);
    arena_init(&f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, manifest_diag, f);
}

static void manifest_fix_done(ManifestFix *f)
{
    (void)unlink(f->entry);
    (void)unlink(f->manifest);
    YEW_ASSERT_EQ_I64(rmdir(f->src_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
    arena_free_all(&f->arena);
}

static bool manifest_read(ManifestFix *f, PlugManifest *out)
{
    return yew_plug_manifest_read(&f->arena, f->dir, out, &f->dc);
}

static const char VALID_MANIFEST[] =
    "{\n"
    "  name: \"valid-plugin\",\n"
    "  version: \"1.2.3-alpha.1+build.7\",\n"
    "  api: 1,\n"
    "  entry: \"src/main.fl\",\n"
    "  capabilities: [\"fs\", \"clipboard\"],\n"
    "  events: [\"buf.open\", \"plug.disable\"],\n"
    "  description: \"a valid plugin\",\n"
    "}\n";

void test_plug_manifest_valid_literal_is_owned_and_uninterned(void)
{
    ManifestFix f;
    PlugManifest mf;

    manifest_fix_init(&f, "valid-plugin", VALID_MANIFEST);
    YEW_ASSERT(manifest_read(&f, &mf));
    YEW_ASSERT_EQ_STR(mf.name_text, "valid-plugin");
    YEW_ASSERT_EQ_STR(mf.version, "1.2.3-alpha.1+build.7");
    YEW_ASSERT_EQ_STR(mf.entry, "src/main.fl");
    YEW_ASSERT_EQ_STR(mf.desc, "a valid plugin");
    YEW_ASSERT_EQ_U64(mf.api, 1U);
    YEW_ASSERT_EQ_U64(mf.name, 0U);
    YEW_ASSERT_EQ_U64(mf.nevents, 2U);
    YEW_ASSERT_EQ_U64(mf.events[0], 0U);
    YEW_ASSERT_EQ_U64(mf.events[1], 0U);
    YEW_ASSERT_EQ_STR(mf.event_names[0], "buf.open");
    YEW_ASSERT_EQ_STR(mf.event_names[1], "plug.disable");
    YEW_ASSERT((mf.caps_wanted & (1U << YEW_CAP_FS)) != 0U);
    YEW_ASSERT((mf.caps_wanted & (1U << YEW_CAP_CLIPBOARD)) != 0U);
    YEW_ASSERT(mf.dir[0] == '/');
    YEW_ASSERT_EQ_U64(fl_diag_errors(&f.dc), 0U);
    manifest_fix_done(&f);
}

void test_plug_manifest_pure_literal_executes_nothing(void)
{
    ManifestFix f;
    PlugManifest mf = {.api = 99U};

    manifest_fix_init(&f, "valid-plugin", "io.read(\"plugin.fl\")\n");
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT_EQ_U64(mf.api, 0U);
    YEW_ASSERT(fl_diag_errors(&f.dc) != 0U);
    manifest_fix_done(&f);
}

void test_plug_manifest_unknown_key_suggests_one_edit(void)
{
    ManifestFix f;
    PlugManifest mf;
    static const char source[] =
        "{ name: \"valid-plugin\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilites: [], capabilities: [], "
        "events: [] }\n";

    manifest_fix_init(&f, "valid-plugin", source);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "capabilites") != NULL);
    YEW_ASSERT(strstr(f.diag, "did you mean 'capabilities'") != NULL);
    YEW_ASSERT_NULL(mf.name_text);
    manifest_fix_done(&f);
}

void test_plug_manifest_name_must_match_directory(void)
{
    ManifestFix f;
    PlugManifest mf;
    static const char source[] =
        "{ name: \"other-name\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], events: [] }\n";

    manifest_fix_init(&f, "actual-name", source);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "other-name") != NULL);
    YEW_ASSERT(strstr(f.diag, "actual-name") != NULL);
    manifest_fix_done(&f);
}

void test_plug_manifest_entry_parent_escape_names_resolved_path(void)
{
    ManifestFix f;
    PlugManifest mf;
    char outside[384];
    static const char source[] =
        "{ name: \"escape\", version: \"1.0.0\", api: 1, "
        "entry: \"../outside.fl\", capabilities: [], events: [] }\n";
    int n;

    manifest_fix_init(&f, "escape", source);
    n = snprintf(outside, sizeof(outside), "%s/outside.fl", f.root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(outside));
    manifest_write(outside, "nil\n");
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "escapes directory") != NULL);
    YEW_ASSERT(strstr(f.diag, outside) != NULL);
    YEW_ASSERT_EQ_I64(unlink(outside), 0);
    manifest_fix_done(&f);
}

void test_plug_manifest_entry_symlink_escape_is_rejected(void)
{
    ManifestFix f;
    PlugManifest mf;
    char outside[384];
    static const char source[] =
        "{ name: \"escape\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], events: [] }\n";
    int n;

    manifest_fix_init(&f, "escape", source);
    n = snprintf(outside, sizeof(outside), "%s/outside.fl", f.root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(outside));
    manifest_write(outside, "nil\n");
    YEW_ASSERT_EQ_I64(unlink(f.entry), 0);
    YEW_ASSERT_EQ_I64(symlink(outside, f.entry), 0);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "escapes directory") != NULL);
    YEW_ASSERT(strstr(f.diag, outside) != NULL);
    YEW_ASSERT_EQ_I64(unlink(outside), 0);
    manifest_fix_done(&f);
}

void test_plug_manifest_capability_set_is_closed(void)
{
    ManifestFix f;
    PlugManifest mf;
    static const char source[] =
        "{ name: \"valid-plugin\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [\"process\"], events: [] }\n";

    manifest_fix_init(&f, "valid-plugin", source);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "process") != NULL);
    YEW_ASSERT_EQ_STR(yew_cap_name(YEW_CAP_FS), "fs");
    YEW_ASSERT_EQ_STR(yew_cap_name(YEW_CAP_SHELL), "shell");
    YEW_ASSERT_EQ_STR(yew_cap_name(YEW_CAP_NET), "net");
    YEW_ASSERT_EQ_STR(yew_cap_name(YEW_CAP_CLIPBOARD), "clipboard");
    YEW_ASSERT_NULL(yew_cap_name(YEW_CAP__N));
    manifest_fix_done(&f);
}

void test_plug_manifest_event_set_is_frozen(void)
{
    static const char *const accepted[] = {
        "buf.open", "buf.change", "buf.save", "buf.saved", "buf.close",
        "win.focus", "mode.enter", "mode.leave", "ws.open", "ws.close",
        "plug.enable", "plug.disable", "ed.idle"
    };
    ManifestFix f;
    PlugManifest mf;
    static const char source[] =
        "{ name: \"valid-plugin\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], "
        "events: [\"cursor.move\"] }\n";
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(accepted); i++)
        YEW_ASSERT(yew_plug_event_valid(accepted[i], strlen(accepted[i])));
    YEW_ASSERT(!yew_plug_event_valid("cursor.move", 11U));
    YEW_ASSERT(!yew_plug_event_valid("buf.unknown", 11U));
    manifest_fix_init(&f, "valid-plugin", source);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "cursor.move") != NULL);
    manifest_fix_done(&f);
}

void test_plug_manifest_rejects_newer_api(void)
{
    ManifestFix f;
    PlugManifest mf;
    static const char source[] =
        "{ name: \"valid-plugin\", version: \"1.0.0\", api: 7, "
        "entry: \"src/main.fl\", capabilities: [], events: [] }\n";

    manifest_fix_init(&f, "valid-plugin", source);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag,
                      "requires plugin API 7; this yew speaks 1") != NULL);
    manifest_fix_done(&f);
}

void test_plug_manifest_rejects_dependencies_at_one_dot_zero(void)
{
    ManifestFix f;
    PlugManifest mf;
    static const char source[] =
        "{ name: \"valid-plugin\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], events: [], "
        "dependencies: [\"other\"] }\n";

    manifest_fix_init(&f, "valid-plugin", source);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag,
                      "plugins have no dependencies at 1.0") != NULL);
    manifest_fix_done(&f);
}

void test_plug_manifest_required_shapes_are_strict(void)
{
    ManifestFix f;
    PlugManifest mf;
    static const char bad_name[] =
        "{ name: \"Upper\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], events: [] }\n";
    static const char bad_version[] =
        "{ name: \"valid-plugin\", version: \"01.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], events: [] }\n";
    static const char missing[] = "{ name: \"valid-plugin\" }\n";

    manifest_fix_init(&f, "valid-plugin", bad_name);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "[a-z0-9-]{1,32}") != NULL);
    manifest_fix_done(&f);

    manifest_fix_init(&f, "valid-plugin", bad_version);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "semver") != NULL);
    manifest_fix_done(&f);

    manifest_fix_init(&f, "valid-plugin", missing);
    YEW_ASSERT(!manifest_read(&f, &mf));
    YEW_ASSERT(strstr(f.diag, "missing a required key") != NULL);
    manifest_fix_done(&f);
}
