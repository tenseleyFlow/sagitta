#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.25: completions learned from `--help`.
 *
 * The parser runs against help text CAPTURED ONCE from real tools
 * (the .txt files in tests/unit/fixtures/help/, each with the tree it
 * must parse to in a sibling .expect); no test runs those tools.  Everything that does
 * run a program runs build/help_fixture -- a native executable whose
 * `--help` output, and whose record of how it was invoked, live in files
 * beside the copy a test puts on a fixture PATH.
 *
 * YEW_HELP_EXPECT_UPDATE=1 rewrites the .expect files; read every diff
 * before committing one.
 */

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "compspec_fix.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/option.h"
#include "fl/data.h"
#include "fl/diag.h"
#include "fl/vm.h"
#include "ui/cmdcomp.h"
#include "ui/cmdline.h"
#include "ui/cmdparse.h"
#include "ui/compgen.h"
#include "ui/comphelp.h"
#include "ui/compspec.h"
#include "util/intern.h"
#include "util/xdg.h"

#ifndef YEW_TEST_HELPFIX
#define YEW_TEST_HELPFIX "build/help_fixture"
#endif

#define HELP_FIXTURES "tests/unit/fixtures/help/"

/* ------------------------------------------------------------------ */
/* The tree as text                                                    */
/* ------------------------------------------------------------------ */

static const char *const kind_name[YEW_SPEC_ARG__N] = {
    "path", "dir", "exec", "command", "var", "user", "host", "pid",
    "signal", "values", "generator", "none"
};

static void dump_node(Bytebuf *b, const YewSpecNode *n)
{
    u32 i;
    u32 k;

    for (i = 0U; i < n->n_subs; i++) {
        const YewSpecNode *s = &n->subs[i];

        bytebuf_printf(b, "sub %s", s->name);
        for (k = 0U; k < s->n_aliases; k++)
            bytebuf_printf(b, "%s%s", k == 0U ? " alias=" : ",",
                           s->aliases[k]);
        bytebuf_printf(b, " | %s\n", s->desc == NULL ? "" : s->desc);
    }
    for (i = 0U; i < n->n_flags; i++) {
        const YewSpecFlag *f = &n->flags[i];

        bytebuf_printf(b, "flag");
        if (f->shrt != NULL)
            bytebuf_printf(b, " -%s", f->shrt);
        if (f->lng != NULL)
            bytebuf_printf(b, " --%s", f->lng);
        if (f->arg != NULL) {
            bytebuf_printf(b, " arg=%s", kind_name[f->arg->kind]);
            for (k = 0U; k < f->arg->n_values; k++)
                bytebuf_printf(b, "%s%s", k == 0U ? ":" : ",",
                               f->arg->values[k].value);
        }
        if (f->arg_optional)
            bytebuf_printf(b, " optional");
        if (f->global)
            bytebuf_printf(b, " global");
        bytebuf_printf(b, " | %s\n", f->desc == NULL ? "" : f->desc);
    }
}

static char *dump_spec(const YewCompSpec *spec)
{
    Bytebuf b;
    char *out;

    bytebuf_init(&b);
    if (spec == NULL)
        bytebuf_printf(&b, "negative\n");
    else
        dump_node(&b, yew_compspec_root(spec));
    out = yew_xmalloc(b.len + 1U);
    (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

static char *read_all(const char *path)
{
    Bytebuf b;
    char *out;

    bytebuf_init(&b);
    spec_read_file(path, &b);
    out = yew_xmalloc(b.len + 1U);
    if (b.len != 0U)
        (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

static void write_all(const char *path, const void *data, size_t n,
                      mode_t mode)
{
    FILE *fp = fopen(path, "wb");

    YEW_ASSERT_NOT_NULL(fp);
    if (n != 0U)
        YEW_ASSERT_EQ_U64(fwrite(data, 1U, n, fp), n);
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_I64(chmod(path, mode), 0);
}

static YewCompSpec *parse_file(const char *file, const char *const *words,
                               u32 n_words)
{
    char path[256];
    Bytebuf b;
    YewCompSpec *spec;

    SPEC_FMT(path, sizeof(path), "%s%s", HELP_FIXTURES, file);
    bytebuf_init(&b);
    spec_read_file(path, &b);
    spec = yew_comphelp_parse("help:test", words, n_words,
                              b.data == NULL ? "" : (const char *)b.data,
                              b.len);
    bytebuf_free(&b);
    return spec;
}

/* ------------------------------------------------------------------ */
/* §3: every layout against its captured text                          */
/* ------------------------------------------------------------------ */

static const struct {
    const char *file;
    const char *words[2];
    u32 n_words;
} layouts[] = {
    /* The user's own tools (DoD 1). */
    {"shithub.txt", {"shithub", NULL}, 1U},
    {"shithub-auth.txt", {"shithub", "auth"}, 2U},
    {"lupin.txt", {"lupin", NULL}, 1U},
    {"fac.txt", {"fac", NULL}, 1U},
    {"fackr.txt", {"fackr", NULL}, 1U},
    /* One per layout row of §3's table. */
    {"clap-starship.txt", {"starship", NULL}, 1U},
    {"clap-uv.txt", {"uv", NULL}, 1U},
    {"cobra-colima.txt", {"colima", NULL}, 1U},
    {"cobra-colima-start.txt", {"colima", "start"}, 2U},
    {"cobra-kubectl.txt", {"kubectl", NULL}, 1U},
    {"argparse-wheel.txt", {"wheel3", NULL}, 1U},
    {"argparse-rapid-mlx.txt", {"rapid-mlx", NULL}, 1U},
    {"click-app.txt", {"clickapp", NULL}, 1U},
    {"commander-vsce.txt", {"vsce", NULL}, 1U},
    {"gnu-ls.txt", {"gls", NULL}, 1U},
    {"goflag-gofmt.txt", {"gofmt", NULL}, 1U},
    {"custom-wolf.txt", {"wolf", NULL}, 1U},
    {"yargs-opencode.txt", {"opencode", NULL}, 1U},
    {"bsd-cp.txt", {"cp", NULL}, 1U},
    /* Hostile inputs. */
    {"hostile-ansi.txt", {"clickapp", NULL}, 1U},
    {"hostile-overstrike.txt", {"tool", NULL}, 1U},
    {"hostile-crlf.txt", {"clickapp", NULL}, 1U},
    {"hostile-utf8.txt", {"tool", NULL}, 1U},
};

void test_comphelp_parses_every_layout_fixture(void)
{
    size_t i;
    bool update = getenv("YEW_HELP_EXPECT_UPDATE") != NULL;

    for (i = 0U; i < YEW_ARRAY_LEN(layouts); i++) {
        char path[256];
        YewCompSpec *spec = parse_file(layouts[i].file, layouts[i].words,
                                       layouts[i].n_words);
        char *got = dump_spec(spec);

        SPEC_FMT(path, sizeof(path), "%s%s.expect", HELP_FIXTURES,
                 layouts[i].file);
        if (update) {
            write_all(path, got, strlen(got), 0644);
        } else {
            char *want = read_all(path);

            if (strcmp(got, want) != 0)
                (void)fprintf(stderr, "comphelp: %s parsed to\n%s",
                              layouts[i].file, got);
            YEW_ASSERT_EQ_STR(got, want);
            yew_xfree(want);
        }
        yew_xfree(got);
        yew_compspec_free(spec);
    }
}

static u32 count_subs(const YewCompSpec *spec)
{
    return spec == NULL ? 0U : yew_compspec_root(spec)->n_subs;
}

static const YewSpecFlag *flag_long(const YewCompSpec *spec, const char *l)
{
    const YewSpecNode *n = yew_compspec_root(spec);
    u32 i;

    for (i = 0U; i < n->n_flags; i++) {
        if (n->flags[i].lng != NULL && strcmp(n->flags[i].lng, l) == 0)
            return &n->flags[i];
    }
    return NULL;
}

static const YewSpecNode *sub_named(const YewCompSpec *spec, const char *s)
{
    const YewSpecNode *n = yew_compspec_root(spec);
    u32 i;

    for (i = 0U; i < n->n_subs; i++) {
        if (strcmp(n->subs[i].name, s) == 0)
            return &n->subs[i];
    }
    return NULL;
}

/*
 * DoD 1, by count and by the rows that make each layout hard: shithub
 * yields >= 20 subcommands (all 28, `attestation` included -- cobra pads
 * it to a single space), fackr is a NEGATIVE result, fac's usage rows
 * give flags and never `fac` itself as a subcommand.
 */
void test_comphelp_user_tools_behave_as_stated(void)
{
    static const char *const shithub[] = {"shithub"};
    static const char *const lupin[] = {"lupin"};
    static const char *const fac[] = {"fac"};
    static const char *const fackr[] = {"fackr"};
    YewCompSpec *s = parse_file("shithub.txt", shithub, 1U);
    YewCompSpec *l = parse_file("lupin.txt", lupin, 1U);
    YewCompSpec *f = parse_file("fac.txt", fac, 1U);
    YewCompSpec *k = parse_file("fackr.txt", fackr, 1U);

    YEW_ASSERT_NOT_NULL(s);
    YEW_ASSERT(count_subs(s) >= 20U);
    YEW_ASSERT_EQ_U64(count_subs(s), 28U);
    YEW_ASSERT_NOT_NULL(sub_named(s, "attestation"));
    YEW_ASSERT_NOT_NULL(sub_named(s, "ssh-key"));
    YEW_ASSERT_NOT_NULL(l);
    YEW_ASSERT_EQ_U64(count_subs(l), 13U);
    YEW_ASSERT_NOT_NULL(sub_named(l, "conform-run"));
    YEW_ASSERT_NOT_NULL(f);
    YEW_ASSERT_EQ_U64(count_subs(f), 0U);
    YEW_ASSERT_NOT_NULL(flag_long(f, "version"));
    YEW_ASSERT_NULL(k);
    yew_compspec_free(s);
    yew_compspec_free(l);
    yew_compspec_free(f);
}

/* The classifications §3 names, one assertion each. */
void test_comphelp_classifies_terms(void)
{
    static const char *const uv[] = {"uv"};
    static const char *const gofmt[] = {"gofmt"};
    static const char *const gls[] = {"gls"};
    static const char *const vsce[] = {"vsce"};
    static const char *const rapid[] = {"rapid-mlx"};
    static const char *const colima[] = {"colima", "start"};
    YewCompSpec *spec;
    const YewSpecFlag *fl;
    const YewSpecNode *sub;

    spec = parse_file("clap-uv.txt", uv, 1U);
    YEW_ASSERT_NOT_NULL(spec);
    fl = flag_long(spec, "color");
    YEW_ASSERT_NOT_NULL(fl);
    YEW_ASSERT_NOT_NULL(fl->arg);
    YEW_ASSERT_EQ_U64(fl->arg->kind, YEW_SPEC_ARG_VALUES);
    YEW_ASSERT_EQ_U64(fl->arg->n_values, 3U);
    YEW_ASSERT_EQ_STR(fl->arg->values[1].value, "always");
    fl = flag_long(spec, "directory");
    YEW_ASSERT(fl != NULL && fl->arg != NULL &&
               fl->arg->kind == YEW_SPEC_ARG_DIR);
    fl = flag_long(spec, "config-file");
    YEW_ASSERT(fl != NULL && fl->arg != NULL &&
               fl->arg->kind == YEW_SPEC_ARG_PATH);
    /* `-q, --quiet...`: repeatable, takes no value. */
    fl = flag_long(spec, "quiet");
    YEW_ASSERT(fl != NULL && fl->arg == NULL && fl->shrt != NULL &&
               strcmp(fl->shrt, "q") == 0 && fl->global);
    yew_compspec_free(spec);

    /* Go's flag package: `-name type` then a tab-indented line. */
    spec = parse_file("goflag-gofmt.txt", gofmt, 1U);
    YEW_ASSERT_NOT_NULL(spec);
    fl = flag_long(spec, "cpuprofile");
    YEW_ASSERT(fl != NULL && fl->arg != NULL &&
               fl->arg->kind == YEW_SPEC_ARG_NONE && fl->desc != NULL);
    yew_compspec_free(spec);

    /* GNU: `--color[=WHEN]` is arg_optional. */
    spec = parse_file("gnu-ls.txt", gls, 1U);
    YEW_ASSERT_NOT_NULL(spec);
    fl = flag_long(spec, "color");
    YEW_ASSERT(fl != NULL && fl->arg != NULL && fl->arg_optional);
    fl = flag_long(spec, "block-size");
    YEW_ASSERT(fl != NULL && fl->arg != NULL && !fl->arg_optional);
    YEW_ASSERT_EQ_U64(count_subs(spec), 0U);
    yew_compspec_free(spec);

    /* commander: `package|pack [options] [version]`. */
    spec = parse_file("commander-vsce.txt", vsce, 1U);
    sub = sub_named(spec, "package");
    YEW_ASSERT(sub != NULL && sub->n_aliases == 1U &&
               strcmp(sub->aliases[0], "pack") == 0);
    yew_compspec_free(spec);

    /* argparse: `chat (run)` -- `run` is also in the choice set, and is
     * an alias, not an 18th subcommand -- and nothing from Examples. */
    spec = parse_file("argparse-rapid-mlx.txt", rapid, 1U);
    sub = sub_named(spec, "chat");
    YEW_ASSERT(sub != NULL && sub->n_aliases == 1U &&
               strcmp(sub->aliases[0], "run") == 0);
    YEW_ASSERT_EQ_U64(count_subs(spec), 17U);
    yew_compspec_free(spec);

    /* cobra: Global Flags are global; Examples rows are not rows. */
    spec = parse_file("cobra-colima-start.txt", colima, 2U);
    YEW_ASSERT_NOT_NULL(spec);
    YEW_ASSERT_EQ_U64(count_subs(spec), 0U);
    fl = flag_long(spec, "verbose");
    YEW_ASSERT(fl != NULL && fl->global);
    fl = flag_long(spec, "arch");
    YEW_ASSERT(fl != NULL && !fl->global && fl->arg != NULL);
    yew_compspec_free(spec);
}

/* Hostile inputs that are not worth a file: a 300 KiB input is cut at
 * the collect cap (rows past it are never read) and a line of 10 000
 * spaces is only a blank line. */
void test_comphelp_survives_huge_and_blank_inputs(void)
{
    static const char *const words[] = {"tool"};
    Bytebuf b;
    YewCompSpec *spec;
    u32 i;

    bytebuf_init(&b);
    bytebuf_printf(&b, "Commands:\n");
    for (i = 0U; b.len < 300U * 1024U; i++)
        bytebuf_printf(&b, "  cmd%05u    row number %u of many\n",
                       (unsigned)i, (unsigned)i);
    bytebuf_printf(&b, "  beyond      past the cap\n");
    spec = yew_comphelp_parse("help:big", words, 1U,
                              (const char *)b.data, b.len);
    YEW_ASSERT_NOT_NULL(spec);
    YEW_ASSERT(count_subs(spec) <= 512U);
    YEW_ASSERT_NULL(sub_named(spec, "beyond"));
    yew_compspec_free(spec);
    bytebuf_free(&b);

    bytebuf_init(&b);
    bytebuf_printf(&b, "Commands:\n  build");
    for (i = 0U; i < 10000U; i++)
        bytebuf_push_u8(&b, ' ');
    bytebuf_printf(&b, "compile it\n");
    for (i = 0U; i < 10000U; i++)
        bytebuf_push_u8(&b, ' ');
    bytebuf_printf(&b, "\n  run   run it\n");
    spec = yew_comphelp_parse("help:spaces", words, 1U,
                              (const char *)b.data, b.len);
    YEW_ASSERT_NOT_NULL(spec);
    YEW_ASSERT_EQ_U64(count_subs(spec), 2U);
    YEW_ASSERT_EQ_STR(sub_named(spec, "build")->desc, "compile it");
    yew_compspec_free(spec);
    bytebuf_free(&b);
    /* Nothing at all, and NUL bytes, are negative, not a crash. */
    YEW_ASSERT_NULL(yew_comphelp_parse("help:empty", words, 1U, "", 0U));
    YEW_ASSERT_NULL(yew_comphelp_parse("help:nul", words, 1U,
                                       "\0\0\0\n\0", 5U));
}

/* The node's data form round-trips through the SAME reader a spec file
 * uses -- the cache has no second schema. */
void test_comphelp_node_data_form_round_trips(void)
{
    static const char *const uv[] = {"uv"};
    YewCompSpec *spec = parse_file("clap-uv.txt", uv, 1U);
    YewCompSpec *back;
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    FlValue v;
    Bytebuf out;
    char *a;
    char *b;

    YEW_ASSERT_NOT_NULL(spec);
    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    (void)fl_vm_init(&vm, &arena, &in, &dc);
    bytebuf_init(&out);
    fl_data_write(&out, yew_compspec_write_node(&vm,
                                                yew_compspec_root(spec)),
                  0U);
    v = fl_data_read(&vm, (const char *)out.data, out.len, &dc);
    YEW_ASSERT_EQ_U64(fl_diag_errors(&dc), 0U);
    back = yew_compspec_read_node("help:rt", &v, NULL, 0U);
    YEW_ASSERT_NOT_NULL(back);
    a = dump_spec(spec);
    b = dump_spec(back);
    YEW_ASSERT_EQ_STR(b, a);
    yew_xfree(a);
    yew_xfree(b);
    /* A node that is not one -- a top-level spec key -- is refused. */
    {
        char err[256];

        bytebuf_free(&out);
        bytebuf_init(&out);
        bytebuf_printf(&out, "{ completion: 1, flags: [] }");
        v = fl_data_read(&vm, (const char *)out.data, out.len, &dc);
        YEW_ASSERT_NULL(yew_compspec_read_node("help:bad", &v, err,
                                               sizeof(err)));
        YEW_ASSERT_NOT_NULL(strstr(err, "completion"));
    }
    bytebuf_free(&out);
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
    yew_compspec_free(back);
    yew_compspec_free(spec);
}

/* ------------------------------------------------------------------ */
/* A PATH with tools on it                                             */
/* ------------------------------------------------------------------ */

typedef struct HelpFix {
    SpecFix spec;
    char bin[160];
    char cache[160];
    char state[160];
    char *old_cache;
    char *old_state;
    char *old_path;
    Ed ed;
} HelpFix;

static void help_fix_init(HelpFix *h)
{
    spec_fix_init(&h->spec);
    SPEC_FMT(h->bin, sizeof(h->bin), "%s/bin", h->spec.root);
    SPEC_FMT(h->cache, sizeof(h->cache), "%s/cache", h->spec.root);
    SPEC_FMT(h->state, sizeof(h->state), "%s/state", h->spec.root);
    YEW_ASSERT_EQ_I64(mkdir(h->bin, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(h->state, 0700), 0);
    h->old_cache = spec_env_copy("XDG_CACHE_HOME");
    h->old_state = spec_env_copy("XDG_STATE_HOME");
    h->old_path = spec_env_copy("PATH");
    YEW_ASSERT_EQ_I64(setenv("XDG_CACHE_HOME", h->cache, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", h->state, 1), 0);
    YEW_ASSERT_EQ_I64(setenv("PATH", h->bin, 1), 0);
    yew_comphelp_reset();
    yew_comphelp_test_set_timeout_ms(0);
    yew_ed_init(&h->ed);
    YEW_ASSERT(yew_ed_open_scratch(&h->ed));
    yew_test_load_runtime(&h->ed);
    h->ed.ws.dir = h->spec.root;
}

static void help_fix_drop(HelpFix *h)
{
    if (h->ed.cmdline.active)
        yew_cmdline_close(&h->ed, false);
    h->ed.ws.dir = NULL;
    yew_ed_free(&h->ed);
    yew_comphelp_reset();
    yew_comphelp_test_set_timeout_ms(0);
    spec_env_restore("XDG_CACHE_HOME", h->old_cache);
    spec_env_restore("XDG_STATE_HOME", h->old_state);
    spec_env_restore("PATH", h->old_path);
    spec_fix_drop(&h->spec);
}

/* Copy the native fixture onto the PATH as `name`, with `help` as what
 * `name --help` prints (NULL: prints nothing). */
static void tool_install(const HelpFix *h, const char *name, const char *help)
{
    char path[256];
    Bytebuf b;

    bytebuf_init(&b);
    spec_read_file(YEW_TEST_HELPFIX, &b);
    YEW_ASSERT(b.len > 4U);
    SPEC_FMT(path, sizeof(path), "%s/%s", h->bin, name);
    write_all(path, b.data, b.len, 0755);
    bytebuf_free(&b);
    if (help != NULL) {
        SPEC_FMT(path, sizeof(path), "%s/%s.help", h->bin, name);
        write_all(path, help, strlen(help), 0644);
    }
}

static void tool_help_file(const HelpFix *h, const char *file,
                           const char *text)
{
    char path[256];

    SPEC_FMT(path, sizeof(path), "%s/%s", h->bin, file);
    write_all(path, text, strlen(text), 0644);
}

static void script_install(const HelpFix *h, const char *name)
{
    char path[256];
    char body[512];

    SPEC_FMT(path, sizeof(path), "%s/%s", h->bin, name);
    SPEC_FMT(body, sizeof(body),
             "#!/bin/sh\necho \"$0 $*\" >> \"$0.log\"\n"
             "printf 'Commands:\\n  build   compile it\\n'\n");
    write_all(path, body, strlen(body), 0755);
}

/* What the tool logged (every invocation), or "" when it never ran. */
static char *tool_log(const HelpFix *h, const char *name)
{
    char path[256];

    SPEC_FMT(path, sizeof(path), "%s/%s.log", h->bin, name);
    if (access(path, F_OK) != 0)
        return yew_xstrdup("");
    return read_all(path);
}

static void pump(Ed *ed, int timeout_ms)
{
    struct pollfd pfd[YEW_JOB_MAX * 4U];
    u32 n = 0U;

    yew_job_collect_fds(ed, pfd, &n);
    if (n != 0U)
        (void)poll(pfd, (nfds_t)n, timeout_ms);
    else if (timeout_ms > 0)
        (void)poll(NULL, 0, timeout_ms);
    yew_job_pump(ed, pfd, n);
    yew_job_reap(ed);
    yew_job_tick(ed, yew_now_ms());
    (void)yew_job_settle(ed);
}

/* The idle path, then the jobs, until every help request is answered
 * (bounded). */
static void settle(Ed *ed)
{
    i64 start = yew_now_ms();

    while ((yew_comphelp_inflight() != 0U || yew_comphelp_idle_ready()) &&
           yew_now_ms() - start < 10000) {
        (void)yew_comphelp_idle(ed);
        pump(ed, 10);
    }
    YEW_ASSERT_EQ_U64(yew_comphelp_inflight(), 0U);
}

static void set_policy(Ed *ed, const char *value)
{
    OptVal v;
    const char *err = NULL;

    (void)memset(&v, 0, sizeof(v));
    v.type = (u8)YEW_OPT_ENUM;
    v.as.str.s = value;
    v.as.str.len = (u32)strlen(value);
    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_SCOPE_DECLARED, "shell.complete_help",
                           19U, &v, &err));
}

static const char clap_help[] =
    "A fixture tool.\n\n"
    "Usage: tool [OPTIONS] <COMMAND>\n\n"
    "Commands:\n"
    "  build  Compile the project\n"
    "  bench  Run the benchmarks\n"
    "  auth   Manage credentials\n"
    "  help   Print this message or the help of the given subcommand(s)\n\n"
    "Options:\n"
    "  -o, --output <FILE>   Write here\n"
    "  -v, --verbose         More output\n"
    "  -h, --help            Print help\n";

static const char auth_help[] =
    "Usage: tool auth <COMMAND>\n\n"
    "Commands:\n"
    "  login   Sign in\n"
    "  logout  Sign out\n\n"
    "Options:\n"
    "  --token <TOKEN>  Use this token\n";

/* ------------------------------------------------------------------ */
/* §1: policy                                                          */
/* ------------------------------------------------------------------ */

void test_comphelp_magic_bytes_decide_native(void)
{
    HelpFix h;
    char path[256];

    help_fix_init(&h);
    tool_install(&h, "native", NULL);
    script_install(&h, "script");
    SPEC_FMT(path, sizeof(path), "%s/data", h.bin);
    write_all(path, "hello world", 11U, 0644);
    SPEC_FMT(path, sizeof(path), "%s/blob", h.bin);
    write_all(path, "\x01\x02\x03\x04rest", 8U, 0755);
    SPEC_FMT(path, sizeof(path), "%s/native", h.bin);
    YEW_ASSERT_EQ_U64(yew_comphelp_exe_kind(path), YEW_HELP_EXE_NATIVE);
    SPEC_FMT(path, sizeof(path), "%s/script", h.bin);
    YEW_ASSERT_EQ_U64(yew_comphelp_exe_kind(path), YEW_HELP_EXE_SCRIPT);
    SPEC_FMT(path, sizeof(path), "%s/data", h.bin);
    YEW_ASSERT_EQ_U64(yew_comphelp_exe_kind(path), YEW_HELP_EXE_NONE);
    SPEC_FMT(path, sizeof(path), "%s/blob", h.bin);
    YEW_ASSERT_EQ_U64(yew_comphelp_exe_kind(path), YEW_HELP_EXE_OTHER);
    YEW_ASSERT_EQ_U64(yew_comphelp_exe_kind(h.bin), YEW_HELP_EXE_NONE);
    YEW_ASSERT(yew_comphelp_kind_allowed(YEW_HELP_POLICY_NATIVE,
                                         YEW_HELP_EXE_NATIVE));
    YEW_ASSERT(!yew_comphelp_kind_allowed(YEW_HELP_POLICY_NATIVE,
                                          YEW_HELP_EXE_SCRIPT));
    YEW_ASSERT(!yew_comphelp_kind_allowed(YEW_HELP_POLICY_NATIVE,
                                          YEW_HELP_EXE_OTHER));
    YEW_ASSERT(yew_comphelp_kind_allowed(YEW_HELP_POLICY_ALL,
                                         YEW_HELP_EXE_SCRIPT));
    YEW_ASSERT(yew_comphelp_kind_allowed(YEW_HELP_POLICY_ALL,
                                         YEW_HELP_EXE_OTHER));
    YEW_ASSERT(!yew_comphelp_kind_allowed(YEW_HELP_POLICY_ALL,
                                          YEW_HELP_EXE_NONE));
    YEW_ASSERT(!yew_comphelp_kind_allowed(YEW_HELP_POLICY_OFF,
                                          YEW_HELP_EXE_NATIVE));
    /* The default is native. */
    YEW_ASSERT_EQ_U64(yew_comphelp_policy(&h.ed), YEW_HELP_POLICY_NATIVE);
    YEW_ASSERT_EQ_U64(yew_comphelp_policy(NULL), YEW_HELP_POLICY_OFF);
    help_fix_drop(&h);
}

/* Ask for `name`'s help and run whatever that queued; returns whether a
 * tree came back. */
static bool ask_and_run(HelpFix *h, const char *name)
{
    YewHelpLookup hl;

    if (yew_comphelp_lookup(&h->ed, name, &hl))
        return true;
    settle(&h->ed);
    return yew_comphelp_lookup(&h->ed, name, &hl);
}

/*
 * §1 under each mode: a native tool runs under `native` and `all`; a
 * `#!` script only under `all`; nothing under `off`.  What ran is read
 * from the tools' own logs.
 */
void test_comphelp_policy_native_all_off(void)
{
    HelpFix h;
    char *log;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    script_install(&h, "script");
    YEW_ASSERT(ask_and_run(&h, "tool"));
    YEW_ASSERT(!ask_and_run(&h, "script"));
    log = tool_log(&h, "script");
    YEW_ASSERT_EQ_STR(log, "");
    yew_xfree(log);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 1U);

    yew_comphelp_reset();
    set_policy(&h.ed, "all");
    YEW_ASSERT(ask_and_run(&h, "script"));
    log = tool_log(&h, "script");
    YEW_ASSERT(strstr(log, "--help") != NULL);
    yew_xfree(log);

    yew_comphelp_reset();
    set_policy(&h.ed, "off");
    {
        YewHelpLookup hl;

        /* Nothing is asked, and a cached answer is not used either. */
        YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "tool", &hl));
        YEW_ASSERT(!hl.pending);
        YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "script", &hl));
        YEW_ASSERT_EQ_U64(yew_comphelp_queued(), 0U);
    }
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 0U);
    help_fix_drop(&h);
}

/* §1's denylist: by typed name, by `mkfs.*`, and by the REAL name behind
 * a symlink -- in every mode. */
void test_comphelp_denylist_never_runs(void)
{
    static const char *const modes[] = {"native", "all"};
    HelpFix h;
    size_t m;

    help_fix_init(&h);
    tool_install(&h, "rm", clap_help);
    tool_install(&h, "mkfs.fake", clap_help);
    tool_install(&h, "shutdown", clap_help);
    {
        char link[256];
        char target[256];

        SPEC_FMT(target, sizeof(target), "%s/rm", h.bin);
        SPEC_FMT(link, sizeof(link), "%s/tidy", h.bin);
        YEW_ASSERT_EQ_I64(symlink(target, link), 0);
    }
    YEW_ASSERT(yew_comphelp_denied("rm"));
    YEW_ASSERT(yew_comphelp_denied("mkfs.ext4"));
    YEW_ASSERT(yew_comphelp_denied("sudo"));
    YEW_ASSERT(!yew_comphelp_denied("fac"));
    for (m = 0U; m < YEW_ARRAY_LEN(modes); m++) {
        static const char *const names[] = {"rm", "mkfs.fake", "shutdown",
                                            "tidy"};
        size_t i;

        yew_comphelp_reset();
        set_policy(&h.ed, modes[m]);
        for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
            YewHelpLookup hl;

            YEW_ASSERT(!yew_comphelp_lookup(&h.ed, names[i], &hl));
            YEW_ASSERT(!hl.pending);
        }
        YEW_ASSERT_EQ_U64(yew_comphelp_queued(), 0U);
        settle(&h.ed);
    }
    {
        char *log = tool_log(&h, "rm");

        YEW_ASSERT_EQ_STR(log, "");
        yew_xfree(log);
        log = tool_log(&h, "mkfs.fake");
        YEW_ASSERT_EQ_STR(log, "");
        yew_xfree(log);
    }
    help_fix_drop(&h);
}

/* A command with a spec never runs (57.24 wins) -- by its own name or
 * under another name for the same program. */
void test_comphelp_spec_wins_over_help(void)
{
    HelpFix h;
    YewHelpLookup hl;
    char *log;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    spec_fix_user(&h.spec, "tool",
                  "# test 1\n{ completion: 1, command: \"tool\" }\n");
    YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "tool", &hl));
    YEW_ASSERT(!hl.pending);
    {
        char link[256];
        char target[256];

        SPEC_FMT(target, sizeof(target), "%s/tool", h.bin);
        SPEC_FMT(link, sizeof(link), "%s/alias", h.bin);
        YEW_ASSERT_EQ_I64(symlink(target, link), 0);
    }
    YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "alias", &hl));
    YEW_ASSERT_EQ_U64(yew_comphelp_queued(), 0U);
    settle(&h.ed);
    log = tool_log(&h, "tool");
    YEW_ASSERT_EQ_STR(log, "");
    yew_xfree(log);
    help_fix_drop(&h);
}

/*
 * §2: argv[0] is the RESOLVED path (not the bare name), the request
 * carries only --help, stdin is at EOF at once, and nothing -- not the
 * lookup, not the keystroke -- spawns before the idle path.
 */
void test_comphelp_runs_the_resolved_path_without_stdin(void)
{
    HelpFix h;
    YewHelpLookup hl;
    char want[512];
    char *log;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "tool", &hl));
    YEW_ASSERT(hl.pending);
    YEW_ASSERT_EQ_U64(strlen(hl.key), 16U);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 0U);
    YEW_ASSERT_EQ_U64(h.ed.jobs.len, 0U);
    settle(&h.ed);
    SPEC_FMT(want, sizeof(want), "%s/tool --help", h.bin);
    YEW_ASSERT_EQ_STR(yew_comphelp_test_last_argv(), want);
    log = tool_log(&h, "tool");
    SPEC_FMT(want, sizeof(want), "%s/tool --help stdin=eof\n", h.bin);
    YEW_ASSERT_EQ_STR(log, want);
    yew_xfree(log);
    help_fix_drop(&h);
}

/* ------------------------------------------------------------------ */
/* §4: laziness                                                        */
/* ------------------------------------------------------------------ */

static const YewSpecNode *lookup_node(HelpFix *h, const char *name,
                                      const char *sub)
{
    YewHelpLookup hl;
    const YewSpecNode *root;
    u32 i;

    if (!yew_comphelp_lookup(&h->ed, name, &hl))
        return NULL;
    root = yew_compspec_root(hl.spec);
    for (i = 0U; i < root->n_subs; i++) {
        if (strcmp(root->subs[i].name, sub) == 0)
            return &root->subs[i];
    }
    return NULL;
}

/*
 * `tool auth ` asks `tool auth --help` because the root's help listed
 * `auth`; `tool unlisted ` asks nothing -- through the real prompt, Tab
 * and all.
 */
void test_comphelp_descends_only_into_listed_subcommands(void)
{
    HelpFix h;
    char *log;
    char want[512];
    const YewSpecNode *auth;
    CmdCtx cx;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    tool_help_file(&h, "tool.auth.help", auth_help);
    YEW_ASSERT(ask_and_run(&h, "tool"));
    auth = lookup_node(&h, "tool", "auth");
    YEW_ASSERT(auth != NULL && auth->n_subs == 0U);

    yew_cmdline_open(&h.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&h.ed, (const u8 *)"!tool unlisted ", 15U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = &h.ed;
    (void)yew_cmdline_cmd_complete_next(&cx);
    (void)yew_cmdline_comp_idle(&h.ed);
    settle(&h.ed);
    log = tool_log(&h, "tool");
    SPEC_FMT(want, sizeof(want), "%s/tool --help stdin=eof\n", h.bin);
    YEW_ASSERT_EQ_STR(log, want);
    yew_xfree(log);
    yew_cmdline_close(&h.ed, false);

    yew_cmdline_open(&h.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&h.ed, (const u8 *)"!tool auth ", 11U);
    (void)yew_cmdline_cmd_complete_next(&cx);
    YEW_ASSERT(h.ed.cmdline.filter.gen_pending);
    (void)yew_cmdline_comp_idle(&h.ed);
    settle(&h.ed);
    log = tool_log(&h, "tool");
    SPEC_FMT(want, sizeof(want),
             "%s/tool --help stdin=eof\n%s/tool auth --help stdin=eof\n",
             h.bin, h.bin);
    YEW_ASSERT_EQ_STR(log, want);
    yew_xfree(log);
    /* The answer refilled the menu: `login` and `logout`. */
    YEW_ASSERT(!h.ed.cmdline.filter.gen_pending);
    YEW_ASSERT_EQ_U64(h.ed.cmdline.menu.items.len, 2U);
    auth = lookup_node(&h, "tool", "auth");
    YEW_ASSERT(auth != NULL && auth->n_subs == 2U && auth->n_flags == 1U);
    YEW_ASSERT(auth->subs[0].parent == auth);
    yew_cmdline_close(&h.ed, false);
    help_fix_drop(&h);
}

/* ------------------------------------------------------------------ */
/* §5: the cache                                                       */
/* ------------------------------------------------------------------ */

static char *cache_file_for(const HelpFix *h, const char *name,
                            const char *const *subs, u32 n_subs)
{
    char exe[256];
    char *real;
    char key[17];
    char path[512];
    struct stat st;

    SPEC_FMT(exe, sizeof(exe), "%s/%s", h->bin, name);
    real = yew_xrealpath(exe);
    YEW_ASSERT_NOT_NULL(real);
    YEW_ASSERT_EQ_I64(stat(real, &st), 0);
    yew_comphelp_key(real, (i64)st.st_mtime, (i64)st.st_size, subs, n_subs,
                     key);
    yew_xfree(real);
    SPEC_FMT(path, sizeof(path), "%s/yew/completions/help/%s.fl", h->cache,
             key);
    return yew_xstrdup(path);
}

static void touch_mtime(const char *path, time_t when)
{
    struct timeval tv[2];

    tv[0].tv_sec = when;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = when;
    tv[1].tv_usec = 0;
    YEW_ASSERT_EQ_I64(utimes(path, tv), 0);
}

/*
 * Hit after the first parse (a fresh process -- memory dropped -- reads
 * the disk and spawns nothing); an upgrade (the executable's mtime
 * moves) misses and re-learns; the file holds what §5 says.
 */
void test_comphelp_cache_hits_and_upgrades_miss(void)
{
    HelpFix h;
    YewHelpLookup hl;
    char *path;
    char *text;
    char exe[256];
    char want[512];

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    /* Created on first write only. */
    {
        struct stat st;

        YEW_ASSERT(stat(h.cache, &st) != 0);
    }
    YEW_ASSERT(ask_and_run(&h, "tool"));
    path = cache_file_for(&h, "tool", NULL, 0U);
    text = read_all(path);
    SPEC_FMT(exe, sizeof(exe), "%s/tool", h.bin);
    {
        char *real = yew_xrealpath(exe);

        SPEC_FMT(want, sizeof(want), "exe: \"%s\"", real);
        yew_xfree(real);
    }
    YEW_ASSERT_NOT_NULL(strstr(text, "help_cache: 1"));
    YEW_ASSERT_NOT_NULL(strstr(text, want));
    YEW_ASSERT_NOT_NULL(strstr(text, "empty: false"));
    YEW_ASSERT_NOT_NULL(strstr(text, "name: \"build\""));
    yew_xfree(text);

    /* "Restart": memory gone, the disk answers, nothing runs. */
    yew_comphelp_reset();
    YEW_ASSERT(yew_comphelp_lookup(&h.ed, "tool", &hl));
    YEW_ASSERT_EQ_U64(yew_comphelp_queued(), 0U);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 0U);

    /* Upgrade: a new mtime is a new identity. */
    yew_comphelp_reset();
    touch_mtime(exe, time(NULL) - 3600);
    YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "tool", &hl));
    YEW_ASSERT(hl.pending);
    settle(&h.ed);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 1U);
    YEW_ASSERT(yew_comphelp_lookup(&h.ed, "tool", &hl));
    yew_xfree(path);
    help_fix_drop(&h);
}

/* `empty: true` suppresses a rerun: fackr is asked once. */
void test_comphelp_negative_result_is_cached(void)
{
    HelpFix h;
    YewHelpLookup hl;
    char *path;
    char *text;

    help_fix_init(&h);
    tool_install(&h, "fackr", NULL);
    tool_help_file(&h, "fackr.help2", "Error: Device not configured\n");
    YEW_ASSERT(!ask_and_run(&h, "fackr"));
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 1U);
    path = cache_file_for(&h, "fackr", NULL, 0U);
    text = read_all(path);
    YEW_ASSERT_NOT_NULL(strstr(text, "empty: true"));
    YEW_ASSERT_NULL(strstr(text, "node:"));
    yew_xfree(text);
    yew_xfree(path);
    YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "fackr", &hl));
    YEW_ASSERT(!hl.pending);
    yew_comphelp_reset();
    YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "fackr", &hl));
    YEW_ASSERT(!hl.pending);
    settle(&h.ed);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 0U);
    help_fix_drop(&h);
}

/* A torn or tampered cache file is a miss, never a spec, never a crash
 * -- and the answer that replaces it is written whole. */
void test_comphelp_truncated_cache_is_a_miss(void)
{
    HelpFix h;
    YewHelpLookup hl;
    char *path;
    char *text;
    size_t cut;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    YEW_ASSERT(ask_and_run(&h, "tool"));
    path = cache_file_for(&h, "tool", NULL, 0U);
    text = read_all(path);
    for (cut = 1U; cut < strlen(text); cut += strlen(text) / 7U + 1U) {
        write_all(path, text, cut, 0600);
        yew_comphelp_reset();
        YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "tool", &hl));
        YEW_ASSERT(hl.pending);
    }
    /* Tampered: another executable's entry under this key. */
    {
        char *bad = yew_xstrdup(text);
        char *at = strstr(bad, "exe: \"/");

        YEW_ASSERT_NOT_NULL(at);
        at[7] = 'X';
        write_all(path, bad, strlen(bad), 0600);
        yew_xfree(bad);
        yew_comphelp_reset();
        YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "tool", &hl));
        YEW_ASSERT(hl.pending);
    }
    settle(&h.ed);
    yew_xfree(text);
    text = read_all(path);
    YEW_ASSERT_NOT_NULL(strstr(text, "name: \"build\""));
    yew_xfree(text);
    yew_xfree(path);
    help_fix_drop(&h);
}

/* §5: past 2000 files a write prunes the oldest down to 1800. */
void test_comphelp_cache_prunes_at_2000(void)
{
    HelpFix h;
    char dir[256];
    char *path;
    u32 i;
    u32 left = 0U;
    DIR *d;
    struct dirent *e;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    SPEC_FMT(dir, sizeof(dir), "%s/yew/completions/help", h.cache);
    YEW_ASSERT(yew_mkdirs(dir, 0700));
    for (i = 0U; i < 2000U; i++) {
        char file[512];

        SPEC_FMT(file, sizeof(file), "%s/%016x.fl", dir, i + 1U);
        write_all(file, "{}", 2U, 0600);
        touch_mtime(file, (time_t)(1000000000 + (time_t)i));
    }
    /* Not ours: never counted, never removed. */
    path = yew_xmalloc(512U);
    SPEC_FMT(path, 512U, "%s/README", dir);
    write_all(path, "x", 1U, 0600);
    YEW_ASSERT(ask_and_run(&h, "tool"));
    d = opendir(dir);
    YEW_ASSERT_NOT_NULL(d);
    while ((e = readdir(d)) != NULL) {
        if (strlen(e->d_name) == 19U)
            left++;
    }
    YEW_ASSERT_EQ_I64(closedir(d), 0);
    YEW_ASSERT_EQ_U64(left, (u64)YEW_COMPHELP_CACHE_KEEP);
    YEW_ASSERT_EQ_I64(access(path, F_OK), 0);
    yew_xfree(path);
    /* The oldest went, the newest (ours included) stayed. */
    {
        char file[512];
        char *mine = cache_file_for(&h, "tool", NULL, 0U);

        SPEC_FMT(file, sizeof(file), "%s/%016x.fl", dir, 1U);
        YEW_ASSERT(access(file, F_OK) != 0);
        SPEC_FMT(file, sizeof(file), "%s/%016x.fl", dir, 2000U);
        YEW_ASSERT_EQ_I64(access(file, F_OK), 0);
        YEW_ASSERT_EQ_I64(access(mine, F_OK), 0);
        yew_xfree(mine);
    }
    help_fix_drop(&h);
}

/* Run one E-mode line through the real parse and dispatch. */
static CmdStatus run_line(Ed *ed, const char *line)
{
    Arena scratch;
    CmdParse parsed;
    YewCmdInvoke invoke;
    CmdStatus status;

    arena_init(&scratch);
    if (!yew_cmd_parse(ed, line, strlen(line), &scratch, &parsed)) {
        arena_free_all(&scratch);
        return YEW_CMD_ERR_ARG;
    }
    invoke = (YewCmdInvoke){parsed.range, parsed.argv, 0, parsed.bang,
                            ed->win};
    status = yew_ed_invoke_parsed(ed, parsed.command, &invoke);
    arena_free_all(&scratch);
    return status;
}

/*
 * `ed.shell.complete_forget fac` deletes only fac's entries (disk and
 * memory); with no argument, every entry.  Reachable by its E-mode
 * spelling (invariant 9); not recordable, so no CMDWORD.
 */
void test_comphelp_forget_deletes_only_that_command(void)
{
    static const char *const auth[] = {"auth"};
    HelpFix h;
    YewHelpLookup hl;
    char *fac;
    char *other;
    char *sub;
    CmdId id;
    const CmdDesc *desc;

    help_fix_init(&h);
    tool_install(&h, "fac", clap_help);
    tool_install(&h, "other", clap_help);
    tool_help_file(&h, "fac.auth.help", auth_help);
    YEW_ASSERT(ask_and_run(&h, "fac"));
    YEW_ASSERT(ask_and_run(&h, "other"));
    /* A subcommand entry of fac's too. */
    {
        const YewSpecNode *node = lookup_node(&h, "fac", "auth");
        char key[17];

        YEW_ASSERT_NOT_NULL(node);
        YEW_ASSERT(yew_comphelp_lookup(&h.ed, "fac", &hl));
        YEW_ASSERT_EQ_U64(yew_comphelp_descend(&h.ed, hl.spec, node, key),
                          YEW_HELP_DESCEND_PENDING);
        settle(&h.ed);
    }
    fac = cache_file_for(&h, "fac", NULL, 0U);
    sub = cache_file_for(&h, "fac", auth, 1U);
    other = cache_file_for(&h, "other", NULL, 0U);
    YEW_ASSERT_EQ_I64(access(fac, F_OK), 0);
    YEW_ASSERT_EQ_I64(access(sub, F_OK), 0);

    id = yew_cmd_lookup("ed.shell.complete_forget", 24U);
    YEW_ASSERT(id.v != 0U);
    desc = yew_cmd_desc(id);
    YEW_ASSERT((desc->flags & YEW_CMD_RECORDABLE) == 0U);
    YEW_ASSERT_NULL(desc->word);
    YEW_ASSERT_EQ_I64(run_line(&h.ed, ":compforget fac"), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(h.ed.msg.text, "fac (2 cache files)"));
    YEW_ASSERT(access(fac, F_OK) != 0);
    YEW_ASSERT(access(sub, F_OK) != 0);
    YEW_ASSERT_EQ_I64(access(other, F_OK), 0);
    /* Memory too: fac re-learns in this session, other does not. */
    YEW_ASSERT(!yew_comphelp_lookup(&h.ed, "fac", &hl));
    YEW_ASSERT(hl.pending);
    YEW_ASSERT(yew_comphelp_lookup(&h.ed, "other", &hl));
    YEW_ASSERT_EQ_I64(run_line(&h.ed, ":shell.complete_forget"),
                      YEW_CMD_OK);
    YEW_ASSERT(access(other, F_OK) != 0);
    YEW_ASSERT_NOT_NULL(strstr(h.ed.msg.text, "forgot all"));
    yew_xfree(fac);
    yew_xfree(sub);
    yew_xfree(other);
    help_fix_drop(&h);
}

/* ------------------------------------------------------------------ */
/* §6: prewarm, and §7 through the prompt                              */
/* ------------------------------------------------------------------ */

/*
 * DoD 4: entering `tool ` spawns NOTHING on the keystroke path -- the
 * refilter, a Tab -- and exactly one job on the idle tick.  At most one
 * help job is ever in flight.
 */
void test_comphelp_prewarm_only_on_the_idle_tick(void)
{
    HelpFix h;
    CmdCtx cx;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    tool_install(&h, "second", clap_help);
    tool_help_file(&h, "tool.hold", "");
    yew_cmdline_open(&h.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&h.ed, (const u8 *)"!tool ", 6U);
    YEW_ASSERT_EQ_U64(h.ed.jobs.len, 0U);
    YEW_ASSERT_EQ_U64(yew_comphelp_inflight(), 0U);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = &h.ed;
    (void)yew_cmdline_cmd_complete_next(&cx);
    YEW_ASSERT_EQ_U64(h.ed.jobs.len, 0U);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 0U);
    YEW_ASSERT(yew_cmdline_comp_idle_pending(&h.ed));
    YEW_ASSERT_EQ_U64(yew_cmdline_comp_idle(&h.ed), 1U);
    YEW_ASSERT_EQ_U64(yew_comphelp_inflight(), 1U);
    YEW_ASSERT_EQ_U64(h.ed.jobs.len, 1U);
    /* A second command's prewarm waits: one help job at a time. */
    yew_cmdline_paste(&h.ed, (const u8 *)"; second ", 9U);
    YEW_ASSERT_EQ_U64(yew_cmdline_comp_idle(&h.ed), 0U);
    YEW_ASSERT_EQ_U64(yew_comphelp_inflight(), 1U);
    YEW_ASSERT_EQ_U64(yew_comphelp_queued(), 1U);
    YEW_ASSERT(!yew_cmdline_comp_idle_pending(&h.ed));
    {
        char hold[256];

        SPEC_FMT(hold, sizeof(hold), "%s/tool.hold", h.bin);
        YEW_ASSERT_EQ_I64(unlink(hold), 0);
    }
    settle(&h.ed);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 2U);
    yew_cmdline_close(&h.ed, false);
    help_fix_drop(&h);
}

static char *prompt_text(Ed *ed)
{
    Bytebuf b;
    char *out;

    bytebuf_init(&b);
    yew_cmdline_text(ed, &b);
    out = yew_xmalloc(b.len + 1U);
    if (b.len != 0U)
        (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

/*
 * DoD 3 at unit level: the first `!tool bu<Tab>` shows the pending
 * marker and edits nothing; the answer refills the menu (never the
 * line); the second Tab completes `build `.  After a "restart" (memory
 * dropped) the first Tab completes at once from the disk cache.
 */
void test_comphelp_first_tab_pends_second_completes(void)
{
    HelpFix h;
    CmdCtx cx;
    char *text;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = &h.ed;
    yew_cmdline_open(&h.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&h.ed, (const u8 *)"!tool bu", 8U);
    (void)yew_cmdline_cmd_complete_next(&cx);
    YEW_ASSERT(h.ed.cmdline.filter.gen_pending);
    text = prompt_text(&h.ed);
    YEW_ASSERT_EQ_STR(text, "!tool bu");
    yew_xfree(text);
    (void)yew_cmdline_comp_idle(&h.ed);
    settle(&h.ed);
    text = prompt_text(&h.ed);
    YEW_ASSERT_EQ_STR(text, "!tool bu");
    yew_xfree(text);
    YEW_ASSERT(!h.ed.cmdline.filter.gen_pending);
    (void)yew_cmdline_cmd_complete_next(&cx);
    text = prompt_text(&h.ed);
    YEW_ASSERT_EQ_STR(text, "!tool build ");
    yew_xfree(text);
    yew_cmdline_close(&h.ed, false);

    yew_comphelp_reset();
    yew_cmdline_open(&h.ed, YEW_PROMPT_CMD, NULL);
    /* Flags come from the tree too; then the subcommand after them. */
    yew_cmdline_paste(&h.ed, (const u8 *)"!tool --verb", 12U);
    (void)yew_cmdline_cmd_complete_next(&cx);
    text = prompt_text(&h.ed);
    YEW_ASSERT_EQ_STR(text, "!tool --verbose ");
    yew_xfree(text);
    yew_cmdline_paste(&h.ed, (const u8 *)"bu", 2U);
    (void)yew_cmdline_cmd_complete_next(&cx);
    text = prompt_text(&h.ed);
    YEW_ASSERT_EQ_STR(text, "!tool --verbose build ");
    yew_xfree(text);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 0U);
    yew_cmdline_close(&h.ed, false);
    help_fix_drop(&h);
}

/* A usage-only tool is NEGATIVE: 57.23's paths answer, no marker. */
void test_comphelp_negative_falls_back_to_paths(void)
{
    HelpFix h;
    CmdCtx cx;
    char file[256];
    char *text;

    help_fix_init(&h);
    tool_install(&h, "usage", "usage: usage [-abc] file ...\n");
    SPEC_FMT(file, sizeof(file), "%s/readme-only.txt", h.spec.root);
    write_all(file, "x", 1U, 0600);
    YEW_ASSERT(!ask_and_run(&h, "usage"));
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = &h.ed;
    yew_cmdline_open(&h.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&h.ed, (const u8 *)"!usage read", 11U);
    (void)yew_cmdline_cmd_complete_next(&cx);
    YEW_ASSERT(!h.ed.cmdline.filter.gen_pending);
    text = prompt_text(&h.ed);
    YEW_ASSERT_EQ_STR(text, "!usage readme-only.txt ");
    yew_xfree(text);
    yew_cmdline_close(&h.ed, false);
    help_fix_drop(&h);
}

/* The loop and the idle path agree on "work pending" (the same pairing
 * rule as the scan tick's). */
void test_comphelp_loop_deadline_shares_the_idle_condition(void)
{
    HelpFix h;

    help_fix_init(&h);
    tool_install(&h, "tool", clap_help);
    YEW_ASSERT(!yew_cmdline_comp_idle_pending(&h.ed));
    yew_cmdline_open(&h.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&h.ed, (const u8 *)"!tool ", 6U);
    YEW_ASSERT(yew_cmdline_comp_idle_pending(&h.ed));
    YEW_ASSERT_EQ_I64(yew_loop_deadline(&h.ed, yew_now_ms()), 0);
    settle(&h.ed);
    (void)yew_cmdline_comp_idle(&h.ed);
    settle(&h.ed);
    YEW_ASSERT(!yew_cmdline_comp_idle_pending(&h.ed));
    yew_cmdline_close(&h.ed, false);
    YEW_ASSERT(!yew_cmdline_comp_idle_pending(&h.ed));
    help_fix_drop(&h);
}
