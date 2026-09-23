#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.32: cd-aware completion -- the consumers.
 *
 * The lexer's half (which directory the caret's command runs in) is the
 * corpus in shctx_corpus.h.  This file proves every consumer of a
 * directory follows it: the path source, `./` executables, generators,
 * fish, the help layer, and every cache keyed on a directory.
 */

#include "harness.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compspec_fix.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "ui/cmdcomp.h"
#include "ui/cmdline.h"
#include "ui/cmdparse.h"
#include "ui/compgen.h"
#include "ui/compspec.h"

typedef struct CdFix {
    char root[128];
    Ed ed;
} CdFix;

static void cd_path(const CdFix *f, const char *rel, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", f->root, rel);

    YEW_ASSERT(n > 0 && (size_t)n < cap);
}

static void cd_touch(const CdFix *f, const char *rel, mode_t mode)
{
    char path[512];
    int fd;

    cd_path(f, rel, path, sizeof(path));
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void cd_mkdir(const CdFix *f, const char *rel)
{
    char path[512];

    cd_path(f, rel, path, sizeof(path));
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

static void cd_rm(const CdFix *f, const char *rel, bool dir)
{
    char path[512];

    cd_path(f, rel, path, sizeof(path));
    YEW_ASSERT_EQ_I64(dir ? rmdir(path) : unlink(path), 0);
}

static void cd_fix_init(CdFix *f)
{
    (void)strcpy(f->root, "/tmp/yew-cdaware-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    (void)memset(&f->ed, 0, sizeof(f->ed));
    arena_init(&f->ed.arena);
    f->ed.ws.dir = f->root;
}

static void cd_fix_drop(CdFix *f)
{
    arena_free_all(&f->ed.arena);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
}

static const CompItem *cd_find(const Vec_CompItem *items, const char *text)
{
    size_t i;

    for (i = 0U; i < items->len; i++) {
        if (strcmp(items->data[i].text, text) == 0)
            return &items->data[i];
    }
    return NULL;
}

/*
 * §3's pitfall, written before the fix: complete `ls s‸`, then insert
 * `cd sub && ` before `ls` with the menu still open.  Head (""), pattern
 * ("s"), row, position and argv[0] are all unchanged, so a cache that
 * does not key on the EFFECTIVE directory re-ranks the prompt
 * directory's rows -- names from the wrong place.
 */
void test_cdaware_cache_rekeys_on_inserted_cd(void)
{
    static const char before[] = ":!ls s";
    static const char after[] = ":!cd sub && ls s";
    CdFix f;
    Arena scratch;
    Arena arena;
    CompFilter filter;
    YewCompQuery q;
    Vec_CompItem rows = {0};

    cd_fix_init(&f);
    cd_mkdir(&f, "sub");
    cd_touch(&f, "sa-top", 0600);
    cd_touch(&f, "sub/sb-inner", 0600);
    arena_init(&scratch);
    arena_init(&arena);
    yew_comp_filter_init(&filter);

    YEW_ASSERT(yew_comp_query(&f.ed, before, strlen(before), strlen(before),
                              &scratch, &q));
    (void)yew_comp_filter_run(&f.ed, &filter, &arena, &q, 0, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sa-top"));
    YEW_ASSERT_NULL(cd_find(&rows, "sb-inner"));

    YEW_ASSERT(yew_comp_query(&f.ed, after, strlen(after), strlen(after),
                              &scratch, &q));
    YEW_ASSERT_EQ_STR(q.stem, "s");
    (void)yew_comp_filter_run(&f.ed, &filter, &arena, &q, 0, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sb-inner"));
    YEW_ASSERT_NULL(cd_find(&rows, "sa-top"));

    /* And back: deleting the `cd` re-keys again. */
    YEW_ASSERT(yew_comp_query(&f.ed, before, strlen(before), strlen(before),
                              &scratch, &q));
    (void)yew_comp_filter_run(&f.ed, &filter, &arena, &q, 0, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sa-top"));
    YEW_ASSERT_NULL(cd_find(&rows, "sb-inner"));

    Vec_CompItem_free(&rows);
    yew_comp_filter_free(&filter);
    yew_comp_listing_invalidate();
    arena_free_all(&arena);
    arena_free_all(&scratch);
    cd_rm(&f, "sub/sb-inner", false);
    cd_rm(&f, "sub", true);
    cd_rm(&f, "sa-top", false);
    cd_fix_drop(&f);
}

/* ------------------------------------------------------------------ */
/* Every consumer, through a full editor                               */
/* ------------------------------------------------------------------ */

typedef struct EdFix {
    SpecFix spec;
    char state[160];
    char *old_state;
    Ed ed;
} EdFix;

static void ed_path(const EdFix *f, const char *rel, char *out, size_t cap)
{
    SPEC_FMT(out, cap, "%s/%s", f->spec.root, rel);
}

static void ed_write(const EdFix *f, const char *rel, const char *text,
                     mode_t mode)
{
    char path[512];
    FILE *fp;

    ed_path(f, rel, path, sizeof(path));
    fp = fopen(path, "wb");
    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, strlen(text), fp), strlen(text));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_I64(chmod(path, mode), 0);
}

static void ed_mkdir(const EdFix *f, const char *rel)
{
    char path[512];

    ed_path(f, rel, path, sizeof(path));
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
}

/* The tree every test here shares:
 *   sa-top   sub/  sub/sb-inner  sub/run-me (x)  sub/inner/  run-top (x)
 */
static void ed_fix_init(EdFix *f)
{
    spec_fix_init(&f->spec);
    f->old_state = spec_env_copy("XDG_STATE_HOME");
    SPEC_FMT(f->state, sizeof(f->state), "%s/state", f->spec.root);
    YEW_ASSERT_EQ_I64(mkdir(f->state, 0700), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state, 1), 0);
    ed_mkdir(f, "sub");
    ed_mkdir(f, "sub/inner");
    ed_write(f, "sa-top", "", 0600);
    ed_write(f, "sub/sb-inner", "", 0600);
    ed_write(f, "sub/run-me", "#!/bin/sh\n", 0700);
    ed_write(f, "run-top", "#!/bin/sh\n", 0700);
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    yew_test_load_runtime(&f->ed);
    f->ed.ws.dir = f->spec.root;
    yew_compgen_test_set_timeout_ms(0);
}

static void ed_fix_drop(EdFix *f)
{
    if (f->ed.cmdline.active)
        yew_cmdline_close(&f->ed, false);
    f->ed.ws.dir = NULL;
    yew_ed_free(&f->ed);
    yew_comp_listing_invalidate();
    spec_env_restore("XDG_STATE_HOME", f->old_state);
    yew_compgen_test_set_timeout_ms(0);
    spec_fix_drop(&f->spec);
}

/* The SHELL rows for `line` (caret at its end) through a fresh filter,
 * as a Tab asks; `where` gets the filter's note. */
static void ed_rows(EdFix *f, const char *line, Arena *scratch, Arena *arena,
                    Vec_CompItem *rows, char *where, size_t cap)
{
    CompFilter filter;
    YewCompQuery q;

    rows->len = 0U;
    YEW_ASSERT(yew_comp_query(&f->ed, line, strlen(line), strlen(line),
                              scratch, &q));
    YEW_ASSERT_EQ_I64(q.kind, YEW_COMP_SHELL);
    yew_comp_filter_init(&filter);
    (void)yew_comp_filter_run(&f->ed, &filter, arena, &q, 0, rows);
    SPEC_FMT(where, cap, "%s", filter.where);
    yew_comp_filter_free(&filter);
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

static void pump_idle(Ed *ed)
{
    i64 start = yew_now_ms();

    while (yew_compgen_inflight() != 0U && yew_now_ms() - start < 10000)
        pump(ed, 10);
    YEW_ASSERT_EQ_U64(yew_compgen_inflight(), 0U);
}

/* §3's table, the path rows: the effective directory, the shape rules
 * intact, and the notes the pager shows. */
void test_cdaware_paths_follow_the_effective_directory(void)
{
    EdFix f;
    Arena scratch;
    Arena arena;
    Vec_CompItem rows = {0};
    char where[YEW_COMP_WHERE_MAX];

    ed_fix_init(&f);
    arena_init(&scratch);
    arena_init(&arena);

    ed_rows(&f, ":!cd sub && ls ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sb-inner"));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "inner/"));
    YEW_ASSERT_NULL(cd_find(&rows, "sa-top"));
    YEW_ASSERT_EQ_STR(where, "in sub/");

    /* The subshell's cd is gone at its close. */
    ed_rows(&f, ":!(cd sub); ls ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sa-top"));
    YEW_ASSERT_NULL(cd_find(&rows, "sb-inner"));
    YEW_ASSERT_EQ_STR(where, "");

    /* What the user typed stays relative to what they typed. */
    ed_rows(&f, ":!cd sub && ls ../s", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "../sa-top"));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "../sub/"));
    ed_rows(&f, ":!cd sub/inner && cd .. && ls s", &scratch, &arena, &rows,
            where, sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sb-inner"));
    ed_rows(&f, ":!cd sub/inner && ls ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_EQ_STR(where, "in sub/inner/");
    ed_rows(&f, ":!cd .. && ls ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_EQ_STR(where, "in ../");
    ed_rows(&f, ":!cd sub && cd .. && ls s", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sa-top"));
    YEW_ASSERT_EQ_STR(where, "");

    /* Unknown: nothing path-like, and the reason. */
    ed_rows(&f, ":!cd $FOO && ls ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    YEW_ASSERT_EQ_STR(where, "cd target unknown");
    ed_rows(&f, ":!cd sub || echo no; cat s", &scratch, &arena, &rows,
            where, sizeof(where));
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    /* ... but an absolute path names the same file from anywhere. */
    {
        char line[512];

        SPEC_FMT(line, sizeof(line), ":!cd $FOO && ls %s/sa", f.spec.root);
        ed_rows(&f, line, &scratch, &arena, &rows, where, sizeof(where));
        YEW_ASSERT_EQ_U64(rows.len, 1U);
    }

    /* Not created yet: nothing, though the directory is known. */
    ed_rows(&f, ":!mkdir new && cd new && ls ", &scratch, &arena, &rows,
            where, sizeof(where));
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    YEW_ASSERT_EQ_STR(where, "in new/");

    /* COMMAND position `./`: the executables of the effective dir. */
    ed_rows(&f, ":!cd sub && ./", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "./run-me"));
    YEW_ASSERT_NULL(cd_find(&rows, "./sb-inner"));
    YEW_ASSERT_NULL(cd_find(&rows, "./run-top"));
    ed_rows(&f, ":!./", &scratch, &arena, &rows, where, sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "./run-top"));
    YEW_ASSERT_NULL(cd_find(&rows, "./run-me"));

    /* ~ is the job environment's HOME (the fixture's). */
    ed_write(&f, "home/in-home", "", 0600);
    ed_rows(&f, ":!cd ~ && ls in", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "in-home"));

    /* tar -C moves extraction, not where -f is read. */
    ed_rows(&f, ":!tar -C sub -xf s", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sa-top"));
    YEW_ASSERT_NULL(cd_find(&rows, "sb-inner"));
    YEW_ASSERT_EQ_STR(where, "");

    Vec_CompItem_free(&rows);
    arena_free_all(&arena);
    arena_free_all(&scratch);
    ed_fix_drop(&f);
}

/* A Tab that finds nothing says why.  (`rmdir`: row 8, so no help
 * lookup makes the Tab wait.) */
void test_cdaware_empty_tab_names_the_reason(void)
{
    static const char line[] = "!cd $FOO && rmdir ";
    EdFix f;
    CmdCtx cx;

    ed_fix_init(&f);
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f.ed, (const u8 *)line, strlen(line));
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = &f.ed;
    (void)yew_cmdline_cmd_complete_next(&cx);
    YEW_ASSERT_NOT_NULL(strstr(f.ed.msg.text, "cd target unknown"));
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, 0U);
    yew_cmdline_close(&f.ed, false);
    ed_fix_drop(&f);
}

/* The open menu carries the note to the pager. */
void test_cdaware_menu_carries_the_note(void)
{
    static const char line[] = "!cd sub && ls s";
    EdFix f;
    CmdCtx cx;

    ed_fix_init(&f);
    ed_write(&f, "sub/sc-other", "", 0600);
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f.ed, (const u8 *)line, strlen(line));
    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = &f.ed;
    (void)yew_cmdline_cmd_complete_next(&cx);
    YEW_ASSERT_EQ_U64(f.ed.cmdline.menu.items.len, 2U);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.menu.where, "in sub/");
    yew_cmdline_close(&f.ed, false);
    YEW_ASSERT_EQ_STR(f.ed.cmdline.menu.where, "");
    ed_fix_drop(&f);
}

/* A spec generator that prints where it ran and how many arguments it
 * was given: `cwd-<basename>` and `args-<n>`. */
static const char gen_cwd_body[] =
    "#!/bin/sh\n"
    "echo \"cwd-$(basename \"$(pwd)\")\"\n"
    "echo \"args-$#\"\n";

static const char gen_cwd_spec[] =
    "# test 1\n{ completion: 1, command: \"fixcmd\",\n"
    "  generators: { g: { argv: [\"%s\"], pass_flags: [\"-C\", \"-X\"] } },\n"
    "  flags: [ { short: \"C\", arg: { kind: \"dir\" }, changes_dir: true },\n"
    "           { long: \"dir\", arg: { kind: \"dir\" }, changes_dir: true },\n"
    "           { short: \"X\", arg: { kind: \"none\" } } ],\n"
    "  args: [ { kind: \"generator\", generator: \"g\" } ] }\n";

/* The rows the generator gives for `line`, after it answers. */
static void gen_rows(EdFix *f, const char *line, Arena *scratch,
                     Arena *arena, Vec_CompItem *rows)
{
    char where[YEW_COMP_WHERE_MAX];

    ed_rows(f, line, scratch, arena, rows, where, sizeof(where));
    pump_idle(&f->ed);
    ed_rows(f, line, scratch, arena, rows, where, sizeof(where));
}

/* DoD 4/5: a generator runs in the effective directory -- after the
 * line's cd, after a changes_dir flag (chained, attached, `=` form) --
 * and is not handed the flag again. */
void test_cdaware_generator_runs_in_the_effective_directory(void)
{
    EdFix f;
    Arena scratch;
    Arena arena;
    Vec_CompItem rows = {0};
    char prog[512];
    char spec[2048];
    u32 spawns;

    ed_fix_init(&f);
    ed_mkdir(&f, "bin");
    ed_write(&f, "bin/fixgen", gen_cwd_body, 0700);
    ed_path(&f, "bin/fixgen", prog, sizeof(prog));
    SPEC_FMT(spec, sizeof(spec), gen_cwd_spec, prog);
    spec_fix_user(&f.spec, "fixcmd", spec);
    arena_init(&scratch);
    arena_init(&arena);

    gen_rows(&f, ":!cd sub && fixcmd ", &scratch, &arena, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "cwd-sub"));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "args-0"));
    gen_rows(&f, ":!fixcmd -C sub ", &scratch, &arena, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "cwd-sub"));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "args-0"));
    gen_rows(&f, ":!cd sub && fixcmd -C inner ", &scratch, &arena, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "cwd-inner"));
    gen_rows(&f, ":!fixcmd -C sub -Cinner ", &scratch, &arena, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "cwd-inner"));
    gen_rows(&f, ":!fixcmd --dir=sub ", &scratch, &arena, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "cwd-sub"));
    /* A pass flag that is not changes_dir still travels. */
    gen_rows(&f, ":!fixcmd -C sub -X v ", &scratch, &arena, &rows);
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "cwd-sub"));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "args-2"));
    /* Unknown: nothing runs, nothing is offered. */
    spawns = yew_compgen_test_spawns();
    gen_rows(&f, ":!cd $FOO && fixcmd ", &scratch, &arena, &rows);
    gen_rows(&f, ":!fixcmd -C $FOO ", &scratch, &arena, &rows);
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    YEW_ASSERT_EQ_U64(yew_compgen_test_spawns(), spawns);

    Vec_CompItem_free(&rows);
    arena_free_all(&arena);
    arena_free_all(&scratch);
    ed_fix_drop(&f);
}

/* DoD 5: `make -C sub` and `cd sub && make` read sub/Makefile; the
 * shipped git.fl moves its branch generator with -C and does not pass
 * -C to it again. */
void test_cdaware_make_and_git_follow_their_flag(void)
{
    EdFix f;
    Arena scratch;
    Arena arena;
    Vec_CompItem rows = {0};
    char where[YEW_COMP_WHERE_MAX];
    CompFilter filter;
    YewCompQuery q;
    char want[512];
    static const char git_line[] = ":!git -C sub checkout ";

    ed_fix_init(&f);
    ed_write(&f, "Makefile", "top-target:\n\ttrue\n", 0600);
    ed_write(&f, "sub/Makefile", "inner-target:\n\ttrue\n", 0600);
    arena_init(&scratch);
    arena_init(&arena);

    ed_rows(&f, ":!make ", &scratch, &arena, &rows, where, sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "top-target"));
    ed_rows(&f, ":!make -C sub ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "inner-target"));
    YEW_ASSERT_NULL(cd_find(&rows, "top-target"));
    YEW_ASSERT_EQ_STR(where, "in sub/");
    ed_rows(&f, ":!make --directory=sub ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "inner-target"));
    ed_rows(&f, ":!cd sub && make ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "inner-target"));
    ed_rows(&f, ":!cd $X && make ", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NULL(cd_find(&rows, "top-target"));
    /* After -C, a path operand is relative to it too. */
    ed_rows(&f, ":!make -C sub -f sb", &scratch, &arena, &rows, where,
            sizeof(where));
    YEW_ASSERT_NOT_NULL(cd_find(&rows, "sb-inner"));

    YEW_ASSERT(yew_comp_query(&f.ed, git_line, strlen(git_line),
                              strlen(git_line), &scratch, &q));
    yew_comp_filter_init(&filter);
    (void)yew_comp_filter_run(&f.ed, &filter, &arena, &q, 0, &rows);
    YEW_ASSERT_NOT_NULL(filter.gen_key);
    SPEC_FMT(want, sizeof(want), "\x1e%s/sub", f.spec.root);
    YEW_ASSERT_NOT_NULL(strstr(filter.gen_key, want));
    YEW_ASSERT_NULL(strstr(filter.gen_key, "\x1f-C"));
    yew_comp_filter_free(&filter);
    pump_idle(&f.ed);

    Vec_CompItem_free(&rows);
    arena_free_all(&arena);
    arena_free_all(&scratch);
    ed_fix_drop(&f);
}

/* §2: `changes_dir` is validated like the other keys -- it needs a `dir`
 * arg -- and resolution records every value it walks, in each spelling;
 * the shipped tar.fl never sets it. */
void test_cdaware_changes_dir_schema_and_walk(void)
{
    static const char *const bad[] = {
        "# t\n{ completion: 1, command: \"t\",\n"
        "  flags: [ { short: \"C\", changes_dir: true } ] }\n",
        "# t\n{ completion: 1, command: \"t\",\n"
        "  flags: [ { short: \"C\", arg: { kind: \"path\" },\n"
        "             changes_dir: true } ] }\n",
        "# t\n{ completion: 1, command: \"t\",\n"
        "  flags: [ { short: \"C\", arg: { kind: \"dir\" },\n"
        "             changes_dir: 1 } ] }\n"};
    static const char good[] =
        "# t\n{ completion: 1, command: \"t\",\n"
        "  flags: [ { short: \"C\", long: \"dir\", arg: { kind: \"dir\" },\n"
        "             changes_dir: true },\n"
        "           { short: \"v\" } ] }\n";
    static const char line[] = "t -C a -vCb --dir c --dir=d x";
    char err[512];
    YewCompSpec *spec;
    YewSpecPoint pt;
    YewShCtx ctx;
    Arena a;
    size_t i;
    SpecFix f;
    const YewCompSpec *tar;
    const YewSpecNode *root;

    for (i = 0U; i < YEW_ARRAY_LEN(bad); i++) {
        spec = yew_compspec_load_text("completions/t.fl", bad[i],
                                      strlen(bad[i]), err, sizeof(err));
        YEW_ASSERT_NULL(spec);
        YEW_ASSERT_NOT_NULL(strstr(err, "changes_dir"));
    }
    spec = yew_compspec_load_text("completions/t.fl", good, strlen(good),
                                  err, sizeof(err));
    YEW_ASSERT_NOT_NULL(spec);
    arena_init(&a);
    YEW_ASSERT(yew_shctx_at(line, strlen(line), strlen(line), &a, &ctx));
    YEW_ASSERT(yew_compspec_resolve(spec, &ctx, &pt));
    YEW_ASSERT_EQ_U64(pt.n_dirs, 4U);
    /* -C a: the next word. */
    YEW_ASSERT_EQ_U64(pt.dir_flag[0], 1U);
    YEW_ASSERT_EQ_U64(pt.dir_at[0], 2U);
    YEW_ASSERT_EQ_U64(pt.dir_off[0], 0U);
    /* -vCb: attached, inside a bundle. */
    YEW_ASSERT_EQ_U64(pt.dir_at[1], 3U);
    YEW_ASSERT_EQ_STR(ctx.argv[3] + pt.dir_off[1], "b");
    /* --dir c, --dir=d */
    YEW_ASSERT_EQ_U64(pt.dir_at[2], 5U);
    YEW_ASSERT_EQ_U64(pt.dir_flag[2], 4U);
    YEW_ASSERT_EQ_STR(ctx.argv[6] + pt.dir_off[3], "d");
    YEW_ASSERT(!pt.dirs_overflow);
    arena_free_all(&a);
    yew_compspec_free(spec);

    spec_fix_init(&f);
    tar = yew_compspec_get(NULL, "tar");
    YEW_ASSERT_NOT_NULL(tar);
    root = yew_compspec_root(tar);
    for (i = 0U; i < root->n_flags; i++)
        YEW_ASSERT(!root->flags[i].changes_dir);
    spec_fix_drop(&f);
}
