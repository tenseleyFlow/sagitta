#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compspec_fix.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "term/input.h"
#include "ui/cmdcomp.h"
#include "ui/cmdline.h"
#include "ui/compgen.h"

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static void write_file(const char *path, const char *text, mode_t mode)
{
    FILE *fp = fopen(path, "wb");

    YEW_ASSERT_NOT_NULL(fp);
    YEW_ASSERT_EQ_U64(fwrite(text, 1U, strlen(text), fp), strlen(text));
    YEW_ASSERT_EQ_I64(fclose(fp), 0);
    YEW_ASSERT_EQ_I64(chmod(path, mode), 0);
}

/* A script `name` in the fixture's bin directory; `path` gets its
 * absolute path. */
static void script(const SpecFix *f, const char *name, const char *body,
                   char *path, size_t cap)
{
    char dir[256];

    SPEC_FMT(dir, sizeof(dir), "%s/bin", f->root);
    (void)mkdir(dir, 0700);
    SPEC_FMT(path, cap, "%s/%s", dir, name);
    write_file(path, body, 0700);
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

/* Pump until no generator is in flight (bounded). */
static void pump_idle(Ed *ed)
{
    i64 start = yew_now_ms();

    while (yew_compgen_inflight() != 0U && yew_now_ms() - start < 10000)
        pump(ed, 10);
    YEW_ASSERT_EQ_U64(yew_compgen_inflight(), 0U);
}

typedef struct GenFix {
    SpecFix spec;
    Ed ed;
} GenFix;

static void gen_fix_init(GenFix *g)
{
    spec_fix_init(&g->spec);
    (void)memset(&g->ed, 0, sizeof(g->ed));
    arena_init(&g->ed.arena);
    g->ed.ws.dir = g->spec.root;
    yew_jobs_init(&g->ed.jobs);
    yew_compgen_test_set_timeout_ms(0);
}

static void gen_fix_drop(GenFix *g)
{
    yew_jobs_free(&g->ed);
    yew_compgen_test_set_timeout_ms(0);
    arena_free_all(&g->ed.arena);
    spec_fix_drop(&g->spec);
}

static YewCompGenKey key_for(const char *name, const char *const *argv,
                             const char *cwd)
{
    YewCompGenKey k;

    (void)memset(&k, 0, sizeof(k));
    k.name = name;
    k.argv = argv;
    k.cwd = cwd;
    k.cache_ms = YEW_COMPGEN_DEFAULT_CACHE_MS;
    return k;
}

static const CompItem *row_named(const Vec_CompItem *v, const char *text)
{
    size_t i;

    for (i = 0U; i < v->len; i++) {
        if (strcmp(v->data[i].text, text) == 0)
            return &v->data[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Built-ins                                                           */
/* ------------------------------------------------------------------ */

/* DoD 7 and §5's hosts pitfall: $HOME is read at CALL time and pointed
 * at a fixture; patterns are skipped, hashed known_hosts lines too. */
void test_compgen_hosts_reads_the_fixture_home_only(void)
{
    SpecFix f;
    char path[256];
    Arena a;
    Vec_CompItem rows = {0};

    spec_fix_init(&f);
    SPEC_FMT(path, sizeof(path), "%s/.ssh", f.home);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);
    SPEC_FMT(path, sizeof(path), "%s/.ssh/config", f.home);
    write_file(path,
               "# comment\n"
               "Host a b\n"
               "  User me\n"
               "Host *.corp\n"
               "Host !x\n"
               "host=lower\n",
               0600);
    SPEC_FMT(path, sizeof(path), "%s/.ssh/known_hosts", f.home);
    write_file(path,
               "kh1,10.0.0.1 ssh-ed25519 AAAA\n"
               "|1|HASHED= ssh-ed25519 AAAA\n"
               "[kh2]:2222 ssh-rsa AAAA\n"
               "@cert-authority *.example ssh-rsa AAAA\n"
               "a ssh-ed25519 AAAA\n",
               0600);
    arena_init(&a);
    YEW_ASSERT(yew_compgen_builtin("hosts", NULL, NULL, &a, &rows));
    YEW_ASSERT_EQ_U64(rows.len, 6U);
    YEW_ASSERT_EQ_STR(rows.data[0].text, "a");
    YEW_ASSERT_EQ_STR(rows.data[0].detail, "ssh config");
    YEW_ASSERT_EQ_STR(rows.data[1].text, "b");
    YEW_ASSERT_EQ_STR(rows.data[2].text, "lower");
    YEW_ASSERT_EQ_STR(rows.data[3].text, "kh1");
    YEW_ASSERT_EQ_STR(rows.data[3].detail, "known host");
    YEW_ASSERT_EQ_STR(rows.data[4].text, "10.0.0.1");
    YEW_ASSERT_EQ_STR(rows.data[5].text, "kh2");
    YEW_ASSERT_NULL(row_named(&rows, "*.corp"));
    YEW_ASSERT_NULL(row_named(&rows, "!x"));
    /* The spec's `Host a b, Host *.corp, Host !x` case, exactly: a and b
     * only from the config. */
    {
        size_t i;
        u32 from_config = 0U;

        for (i = 0U; i < rows.len; i++)
            if (strcmp(rows.data[i].detail, "ssh config") == 0 &&
                strcmp(rows.data[i].text, "lower") != 0)
                from_config++;
        YEW_ASSERT_EQ_U64(from_config, 2U);
    }
    /* No HOME, no hosts -- and certainly not the real ones. */
    YEW_ASSERT_EQ_I64(unsetenv("HOME"), 0);
    rows.len = 0U;
    YEW_ASSERT(yew_compgen_builtin("hosts", NULL, NULL, &a, &rows));
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
    spec_fix_drop(&f);
}

/*
 * DoD 6: make_targets PARSES the Makefile and never runs make.  A
 * `$(shell touch sentinel)` would create the sentinel if anything
 * evaluated the file; the targets are listed and the sentinel is not.
 */
void test_compgen_make_targets_never_executes_the_makefile(void)
{
    SpecFix f;
    char path[256];
    char sentinel[256];
    Arena a;
    Vec_CompItem rows = {0};
    static const char *const want[] = {"all", "build", "test", "a", "b",
                                       "install", "dist/yew.tar",
                                       "check-x"};
    size_t i;

    spec_fix_init(&f);
    SPEC_FMT(sentinel, sizeof(sentinel), "%s/sentinel", f.root);
    SPEC_FMT(path, sizeof(path), "%s/Makefile", f.root);
    write_file(path,
               "X := $(shell touch sentinel)\n"
               "Y ::= y\n"
               "Z = z\n"
               "W ?= w\n"
               ".PHONY: all build\n"
               "all: build\n"
               "build test: dep\n"
               "\t$(CC) -o $@ $^\n"
               "a b : \\\n"
               "\tnot-a-target: x\n"
               "install:\n"
               "%.o: %.c\n"
               "$(OUT): in\n"
               "dist/yew.tar: all\n"
               "define RECIPE\n"
               "hidden: x\n"
               "endef\n"
               "check-x:: y\n"
               "all: again\n",
               0600);
    arena_init(&a);
    YEW_ASSERT(yew_compgen_builtin("make_targets", f.root, NULL, &a, &rows));
    YEW_ASSERT_EQ_U64(rows.len, YEW_ARRAY_LEN(want));
    for (i = 0U; i < YEW_ARRAY_LEN(want); i++) {
        YEW_ASSERT_EQ_STR(rows.data[i].text, want[i]);
        YEW_ASSERT_EQ_STR(rows.data[i].detail, "make target");
    }
    YEW_ASSERT(access(sentinel, F_OK) != 0);
    YEW_ASSERT_EQ_I64(errno, ENOENT);
    /* make's own order: GNUmakefile wins over Makefile. */
    SPEC_FMT(path, sizeof(path), "%s/GNUmakefile", f.root);
    write_file(path, "gnu-only:\n", 0600);
    rows.len = 0U;
    YEW_ASSERT(yew_compgen_builtin("make_targets", f.root, NULL, &a, &rows));
    YEW_ASSERT_EQ_U64(rows.len, 1U);
    YEW_ASSERT_EQ_STR(rows.data[0].text, "gnu-only");
    /* `make -f other.mk`: that file. */
    SPEC_FMT(path, sizeof(path), "%s/other.mk", f.root);
    write_file(path, "from-other:\n", 0600);
    rows.len = 0U;
    YEW_ASSERT(yew_compgen_builtin("make_targets", f.root, "other.mk", &a,
                                   &rows));
    YEW_ASSERT_EQ_U64(rows.len, 1U);
    YEW_ASSERT_EQ_STR(rows.data[0].text, "from-other");
    YEW_ASSERT(access(sentinel, F_OK) != 0);
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
    spec_fix_drop(&f);
}

void test_compgen_signals_with_and_without_sig(void)
{
    Arena a;
    Vec_CompItem rows = {0};

    arena_init(&a);
    YEW_ASSERT(yew_compgen_builtin("signals", NULL, NULL, &a, &rows));
    YEW_ASSERT_EQ_U64(rows.len, 22U);
    YEW_ASSERT_NOT_NULL(row_named(&rows, "KILL"));
    YEW_ASSERT_NOT_NULL(row_named(&rows, "SIGWINCH"));
    YEW_ASSERT_EQ_STR(row_named(&rows, "TERM")->detail, "terminate");
    YEW_ASSERT(!yew_compgen_builtin("nope", NULL, NULL, &a, &rows));
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
}

/* ------------------------------------------------------------------ */
/* Subprocess generators (§5 async contract)                           */
/* ------------------------------------------------------------------ */

/* Output rules: `candidate<TAB>description`, trailing blanks trimmed,
 * control bytes drawn as `·`, blank lines skipped, order kept. */
void test_compgen_lines_become_rows(void)
{
    GenFix g;
    char prog[256];
    const char *argv[2];
    YewCompGenKey k;
    Arena a;
    Vec_CompItem rows = {0};
    bool pending = false;

    gen_fix_init(&g);
    script(&g.spec, "lines",
           "#!/bin/sh\n"
           "printf 'zeta\\tlast letter  \\n\\n'\n"
           "printf 'alpha  \\n'\n"
           "printf 'b\\033[2Jx\\tde\\001sc\\n'\n"
           "printf '   \\n'\n"
           "printf 'NO_COLOR=%s TERM=%s PAGER=%s\\n' \"$NO_COLOR\" \"$TERM\" "
           "\"$PAGER\"\n"
           "printf 'cwd=%s\\n' \"$(pwd)\"\n",
           prog, sizeof(prog));
    argv[0] = prog;
    argv[1] = NULL;
    k = key_for("lines", argv, g.spec.root);
    arena_init(&a);
    /* First ask: nothing cached, a job in flight, never a wait. */
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    YEW_ASSERT(pending);
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    YEW_ASSERT_EQ_U64(yew_compgen_inflight(), 1U);
    {
        char *ks = yew_compgen_key_string(&k);

        YEW_ASSERT(yew_compgen_awaiting(ks));
        pump_idle(&g.ed);
        YEW_ASSERT(!yew_compgen_awaiting(ks));
        yew_xfree(ks);
    }
    /* Internal: never a user-visible job. */
    YEW_ASSERT_EQ_U64(yew_job_running_count(&g.ed), 0U);
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    YEW_ASSERT(!pending);
    YEW_ASSERT_EQ_U64(rows.len, 5U);
    YEW_ASSERT_EQ_STR(rows.data[0].text, "zeta");
    YEW_ASSERT_EQ_STR(rows.data[0].detail, "last letter");
    YEW_ASSERT_EQ_STR(rows.data[1].text, "alpha");
    YEW_ASSERT_NULL(rows.data[1].detail);
    YEW_ASSERT_EQ_STR(rows.data[2].text, "b\xC2\xB7[2Jx");
    YEW_ASSERT_EQ_STR(rows.data[2].detail, "de\xC2\xB7sc");
    YEW_ASSERT_EQ_STR(rows.data[3].text, "NO_COLOR=1 TERM=dumb PAGER=cat");
    YEW_ASSERT_EQ_U64(rows.data[4].kind, YEW_COMP_GEN);
    YEW_ASSERT_NOT_NULL(strstr(rows.data[4].text, "yew-compspec-"));
    /* Fresh: served from the cache, nothing spawned. */
    {
        u32 spawns = yew_compgen_test_spawns();

        rows.len = 0U;
        YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                    &pending));
        YEW_ASSERT_EQ_U64(yew_compgen_test_spawns(), spawns);
        YEW_ASSERT_EQ_U64(rows.len, 5U);
        /* Stale: served AND refreshed. */
        rows.len = 0U;
        YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms() + 60000, &a,
                                    &rows, &pending));
        YEW_ASSERT_EQ_U64(rows.len, 5U);
        YEW_ASSERT(pending);
        YEW_ASSERT_EQ_U64(yew_compgen_test_spawns(), spawns + 1U);
        pump_idle(&g.ed);
    }
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
    gen_fix_drop(&g);
}

/* A generator that does not answer in time is KILLED, its empty answer
 * cached, and the next keystroke does not respawn it. */
void test_compgen_timeout_kills_and_caches_empty(void)
{
    GenFix g;
    char prog[256];
    const char *argv[2];
    YewCompGenKey k;
    Arena a;
    Vec_CompItem rows = {0};
    bool pending = false;
    pid_t pid;
    u32 spawns;
    i64 start;

    gen_fix_init(&g);
    yew_compgen_test_set_timeout_ms(200);
    script(&g.spec, "sleeper", "#!/bin/sh\nexec sleep 5\n", prog,
           sizeof(prog));
    argv[0] = prog;
    argv[1] = NULL;
    k = key_for("sleeper", argv, g.spec.root);
    arena_init(&a);
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    YEW_ASSERT(pending);
    YEW_ASSERT_EQ_U64(g.ed.jobs.len, 1U);
    pid = g.ed.jobs.v[0].pid;
    YEW_ASSERT(pid > 0);
    start = yew_now_ms();
    pump_idle(&g.ed);
    /* Well under the script's 5 s: it was killed, not waited for. */
    YEW_ASSERT(yew_now_ms() - start < 4000);
    YEW_ASSERT_EQ_U64(g.ed.jobs.len, 0U);
    YEW_ASSERT(kill(pid, 0) != 0);
    YEW_ASSERT_EQ_I64(errno, ESRCH);
    spawns = yew_compgen_test_spawns();
    rows.len = 0U;
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    YEW_ASSERT(!pending);
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    YEW_ASSERT_EQ_U64(yew_compgen_test_spawns(), spawns);
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
    gen_fix_drop(&g);
}

/* Non-zero exit and a program that cannot run: empty answer, cached,
 * logged -- never a footer message. */
void test_compgen_failure_caches_an_empty_answer(void)
{
    GenFix g;
    char prog[256];
    char missing[256];
    const char *argv[2];
    YewCompGenKey k;
    Arena a;
    Vec_CompItem rows = {0};
    bool pending = false;
    u32 spawns;

    gen_fix_init(&g);
    yew_test_capture_log();
    script(&g.spec, "fails", "#!/bin/sh\necho partial\nexit 3\n", prog,
           sizeof(prog));
    argv[0] = prog;
    argv[1] = NULL;
    k = key_for("fails", argv, g.spec.root);
    arena_init(&a);
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    pump_idle(&g.ed);
    spawns = yew_compgen_test_spawns();
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    YEW_ASSERT_EQ_U64(yew_compgen_test_spawns(), spawns);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_INFO,
                                     "completion generator fails"));
    YEW_ASSERT(!g.ed.msg.active);
    SPEC_FMT(missing, sizeof(missing), "%s/no-such-program",
                   g.spec.root);
    argv[0] = missing;
    k = key_for("missing", argv, g.spec.root);
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    pump_idle(&g.ed);
    spawns = yew_compgen_test_spawns();
    rows.len = 0U;
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    YEW_ASSERT(!pending);
    YEW_ASSERT_EQ_U64(yew_compgen_test_spawns(), spawns);
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
    gen_fix_drop(&g);
}

/* At most 5000 lines are taken. */
void test_compgen_output_is_capped(void)
{
    GenFix g;
    char prog[256];
    const char *argv[2];
    YewCompGenKey k;
    Arena a;
    Vec_CompItem rows = {0};
    bool pending = false;

    gen_fix_init(&g);
    script(&g.spec, "big",
           "#!/bin/sh\ni=0\nwhile [ $i -lt 6000 ]; do echo row$i; "
           "i=$((i+1)); done\n",
           prog, sizeof(prog));
    argv[0] = prog;
    argv[1] = NULL;
    k = key_for("big", argv, g.spec.root);
    arena_init(&a);
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    pump_idle(&g.ed);
    YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                &pending));
    YEW_ASSERT_EQ_U64(rows.len, (u64)YEW_COMPGEN_MAX_LINES);
    YEW_ASSERT_EQ_STR(rows.data[0].text, "row0");
    YEW_ASSERT_EQ_STR(rows.data[4999].text, "row4999");
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
    gen_fix_drop(&g);
}

/*
 * DoD 4: 50 keystrokes against one key, then against six: at most one
 * job per key and four in all, whatever the typing does.
 */
void test_compgen_fifty_keystrokes_stay_within_the_caps(void)
{
    GenFix g;
    char prog[256];
    char arg[6][16];
    const char *argv[6][3];
    YewCompGenKey k[6];
    Arena a;
    Vec_CompItem rows = {0};
    bool pending = false;
    u32 i;

    gen_fix_init(&g);
    script(&g.spec, "slow", "#!/bin/sh\nsleep 0.3\necho \"$1\"\n", prog,
           sizeof(prog));
    for (i = 0U; i < 6U; i++) {
        SPEC_FMT(arg[i], sizeof(arg[i]), "key%u", (unsigned)i);
        argv[i][0] = prog;
        argv[i][1] = arg[i];
        argv[i][2] = NULL;
        k[i] = key_for("slow", argv[i], g.spec.root);
    }
    arena_init(&a);
    for (i = 0U; i < 50U; i++) {
        rows.len = 0U;
        YEW_ASSERT(yew_compgen_rows(&g.ed, &k[0], yew_now_ms(), &a, &rows,
                                    &pending));
        YEW_ASSERT(yew_compgen_inflight() <= 1U);
        YEW_ASSERT(g.ed.jobs.len <= 1U);
        YEW_ASSERT_EQ_U64(yew_job_running_count(&g.ed), 0U);
    }
    YEW_ASSERT_EQ_U64(yew_compgen_test_spawns(), 1U);
    for (i = 0U; i < 50U; i++) {
        rows.len = 0U;
        YEW_ASSERT(yew_compgen_rows(&g.ed, &k[i % 6U], yew_now_ms(), &a,
                                    &rows, &pending));
        YEW_ASSERT(yew_compgen_inflight() <= (u32)YEW_COMPGEN_MAX_INFLIGHT);
        YEW_ASSERT(g.ed.jobs.len <= (u32)YEW_COMPGEN_MAX_INFLIGHT);
        if (i % 7U == 0U)
            pump(&g.ed, 0);
    }
    pump_idle(&g.ed);
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
    gen_fix_drop(&g);
}

/*
 * §5.2's reason, proven: YEW_JOB_MAX fails rather than queues, so with
 * the table full of the user's own jobs plus four generators, the
 * user's next `:!` command still starts -- it takes a generator's slot.
 */
void test_compgen_never_costs_the_user_a_job_slot(void)
{
    GenFix g;
    char prog[256];
    char arg[4][16];
    const char *argv[4][3];
    static const char *const sleeper[] = {"/bin/sleep", "5", NULL};
    YewCompGenKey k;
    Arena a;
    Vec_CompItem rows = {0};
    bool pending = false;
    char err[256];
    u32 i;
    u32 user_jobs;

    gen_fix_init(&g);
    script(&g.spec, "slowgen", "#!/bin/sh\nsleep 5\n", prog, sizeof(prog));
    arena_init(&a);
    for (i = 0U; i < 4U; i++) {
        SPEC_FMT(arg[i], sizeof(arg[i]), "k%u", (unsigned)i);
        argv[i][0] = prog;
        argv[i][1] = arg[i];
        argv[i][2] = NULL;
        k = key_for("slowgen", argv[i], g.spec.root);
        YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                    &pending));
    }
    YEW_ASSERT_EQ_U64(yew_compgen_inflight(), 4U);
    user_jobs = (u32)YEW_JOB_MAX - 4U;
    for (i = 0U; i < user_jobs; i++) {
        YewJobSpec spec;

        (void)memset(&spec, 0, sizeof(spec));
        spec.argv = (char **)sleeper;
        spec.sink = YEW_SINK_DISCARD;
        YEW_ASSERT(yew_job_spawn(&g.ed, &spec, err, sizeof(err)) != 0U);
    }
    YEW_ASSERT_EQ_U64(g.ed.jobs.len, (u64)YEW_JOB_MAX);
    /* A generator never evicts anything: a fifth key is simply not run. */
    {
        const char *more[] = {prog, "k9", NULL};

        k = key_for("slowgen", more, g.spec.root);
        YEW_ASSERT(yew_compgen_rows(&g.ed, &k, yew_now_ms(), &a, &rows,
                                    &pending));
        YEW_ASSERT_EQ_U64(g.ed.jobs.len, (u64)YEW_JOB_MAX);
    }
    /* The user's own command starts. */
    {
        YewJobSpec spec;

        (void)memset(&spec, 0, sizeof(spec));
        spec.argv = (char **)sleeper;
        spec.sink = YEW_SINK_DISCARD;
        YEW_ASSERT(yew_job_spawn(&g.ed, &spec, err, sizeof(err)) != 0U);
    }
    YEW_ASSERT_EQ_U64(g.ed.jobs.len, (u64)YEW_JOB_MAX);
    YEW_ASSERT_EQ_U64(yew_compgen_inflight(), 3U);
    YEW_ASSERT_EQ_U64(yew_job_running_count(&g.ed), (u64)user_jobs + 1U);
    Vec_CompItem_free(&rows);
    arena_free_all(&a);
    gen_fix_drop(&g);
}

/* ------------------------------------------------------------------ */
/* Arrival races (§5.4): through the real prompt                       */
/* ------------------------------------------------------------------ */

typedef struct PromptFix {
    GenFix g;
    char state[128];
    char *old_state;
    char prog[256];
} PromptFix;

static void prompt_fix_init(PromptFix *p, const char *gen_body,
                            const char *spec_text)
{
    char spec[1024];

    spec_fix_init(&p->g.spec);
    p->old_state = spec_env_copy("XDG_STATE_HOME");
    SPEC_FMT(p->state, sizeof(p->state), "%s/state", p->g.spec.root);
    YEW_ASSERT_EQ_I64(mkdir(p->state, 0700), 0);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", p->state, 1), 0);
    yew_ed_init(&p->g.ed);
    YEW_ASSERT(yew_ed_open_scratch(&p->g.ed));
    yew_test_load_runtime(&p->g.ed);
    script(&p->g.spec, "fixgen", gen_body, p->prog, sizeof(p->prog));
    SPEC_FMT(spec, sizeof(spec), spec_text, p->prog);
    spec_fix_user(&p->g.spec, "fixcmd", spec);
    yew_compgen_test_set_timeout_ms(0);
}

static void prompt_fix_drop(PromptFix *p)
{
    yew_ed_free(&p->g.ed);
    spec_env_restore("XDG_STATE_HOME", p->old_state);
    yew_compgen_test_set_timeout_ms(0);
    spec_fix_drop(&p->g.spec);
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

static void press_tab(Ed *ed)
{
    CmdCtx cx;

    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    (void)yew_cmdline_cmd_complete_next(&cx);
}

static const char gen_spec_fmt[] =
    "# test 1\n{ completion: 1, command: \"fixcmd\",\n"
    "  generators: { g: { argv: [\"%s\"] } },\n"
    "  subcommands: [ { name: \"stat-one\" }, { name: \"stat-two\" } ],\n"
    "  args: [ { kind: \"generator\", generator: \"g\" } ] }\n";

/* The answer arrives with the prompt still open and the same context:
 * the menu refills, the pending marker goes, and the TEXT is untouched
 * -- byte for byte, even when the answer is a sole survivor. */
void test_compgen_arrival_refilters_but_never_edits(void)
{
    PromptFix p;
    char *before;
    char *after;

    prompt_fix_init(&p, "#!/bin/sh\nsleep 0.2\necho only-branch\n",
                    gen_spec_fmt);
    yew_cmdline_open(&p.g.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&p.g.ed, (const u8 *)"!fixcmd ", 8U);
    /* An empty word completes nothing live; Tab asks. */
    YEW_ASSERT_EQ_U64(yew_compgen_inflight(), 0U);
    press_tab(&p.g.ed);
    /* Static rows show while the generator is pending, with the marker,
     * and Tab inserted nothing from the incomplete set. */
    YEW_ASSERT(p.g.ed.cmdline.filter.gen_pending);
    YEW_ASSERT(p.g.ed.cmdline.menu.pending);
    YEW_ASSERT_EQ_U64(p.g.ed.cmdline.menu.items.len, 2U);
    before = prompt_text(&p.g.ed);
    YEW_ASSERT_EQ_STR(before, "!fixcmd ");
    pump_idle(&p.g.ed);
    after = prompt_text(&p.g.ed);
    YEW_ASSERT_EQ_STR(after, before);
    YEW_ASSERT(!p.g.ed.cmdline.menu.pending);
    YEW_ASSERT_EQ_U64(p.g.ed.cmdline.menu.items.len, 3U);
    yew_xfree(after);
    /* Narrow to the generator's one row and ask again: the arrival path
     * had every chance to insert it and did not; Tab does. */
    yew_cmdline_paste(&p.g.ed, (const u8 *)"only", 4U);
    after = prompt_text(&p.g.ed);
    YEW_ASSERT_EQ_STR(after, "!fixcmd only");
    yew_xfree(after);
    yew_xfree(before);
    yew_cmdline_close(&p.g.ed, false);
    prompt_fix_drop(&p);
}

/* The prompt closes before the answer: nothing repaints, nothing leaks
 * (the ASan lane runs this), and the answer only lands in the cache. */
void test_compgen_arrival_after_prompt_close(void)
{
    PromptFix p;

    prompt_fix_init(&p, "#!/bin/sh\nsleep 0.2\necho late\n", gen_spec_fmt);
    yew_cmdline_open(&p.g.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&p.g.ed, (const u8 *)"!fixcmd ", 8U);
    press_tab(&p.g.ed);
    YEW_ASSERT_EQ_U64(yew_compgen_inflight(), 1U);
    yew_cmdline_close(&p.g.ed, false);
    YEW_ASSERT(!p.g.ed.cmdline.active);
    pump_idle(&p.g.ed);
    YEW_ASSERT(!p.g.ed.cmdline.active);
    YEW_ASSERT_EQ_U64(p.g.ed.cmdline.menu.items.len, 0U);
    prompt_fix_drop(&p);
}

/* The context moved on before the answer: it is cached, not shown. */
void test_compgen_arrival_for_a_stale_context_is_only_cached(void)
{
    PromptFix p;
    char *text;
    size_t i;
    u32 spawns;

    prompt_fix_init(&p, "#!/bin/sh\nsleep 0.2\necho gen-row\n",
                    gen_spec_fmt);
    yew_cmdline_open(&p.g.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&p.g.ed, (const u8 *)"!fixcmd ", 8U);
    press_tab(&p.g.ed);
    YEW_ASSERT(p.g.ed.cmdline.filter.gen_key != NULL);
    /* Move to a subcommand: a different node, no generator. */
    yew_cmdline_paste(&p.g.ed, (const u8 *)"stat-one ", 9U);
    YEW_ASSERT_EQ_U64(p.g.ed.cmdline.menu.items.len, 0U);
    pump_idle(&p.g.ed);
    YEW_ASSERT_EQ_U64(p.g.ed.cmdline.menu.items.len, 0U);
    for (i = 0U; i < p.g.ed.cmdline.menu.items.len; i++)
        YEW_ASSERT(strcmp(p.g.ed.cmdline.menu.items.data[i].text,
                          "gen-row") != 0);
    text = prompt_text(&p.g.ed);
    YEW_ASSERT_EQ_STR(text, "!fixcmd stat-one ");
    yew_xfree(text);
    /* Cached: going back serves it without a new job. */
    spawns = yew_compgen_test_spawns();
    /* Delete back to `!fixcmd `. */
    for (i = 0U; i < 9U; i++) {
        Key key;

        (void)memset(&key, 0, sizeof(key));
        key.code = YEW_KEY_BACKSPACE;
        key.kind = YEW_EV_KEY;
        key.ev = YEW_KEY_PRESS;
        yew_ed_handle_key(&p.g.ed, key, 1);
    }
    text = prompt_text(&p.g.ed);
    YEW_ASSERT_EQ_STR(text, "!fixcmd ");
    yew_xfree(text);
    press_tab(&p.g.ed);
    YEW_ASSERT_EQ_U64(yew_compgen_test_spawns(), spawns);
    YEW_ASSERT(!p.g.ed.cmdline.menu.pending);
    {
        bool found = false;

        for (i = 0U; i < p.g.ed.cmdline.menu.items.len; i++)
            if (strcmp(p.g.ed.cmdline.menu.items.data[i].text, "gen-row") ==
                0)
                found = true;
        YEW_ASSERT(found);
    }
    yew_cmdline_close(&p.g.ed, false);
    prompt_fix_drop(&p);
}
