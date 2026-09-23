#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.26 §3: history suggestions for `:!`.
 *
 * Fixtures (tests/unit/fixtures/history/):
 *   zsh_history   WRITTEN BY zsh 5.9 (`print -s …; fc -W` under
 *                 EXTENDED_HISTORY), so its timestamps, its `\`-newline
 *                 continuation and its metafication are zsh's own.  `é`
 *                 (C3 A9) is outside zsh's meta range and is stored as
 *                 is; `ă` (C4 83) and `ę` (C4 99) are metafied to
 *                 C4 83 A3 and C4 83 B9 -- the bytes the unmetafy test
 *                 is about;
 *   fish_history  fish's `- cmd:` format, hand-written;
 *   bash_history  with HISTTIMEFORMAT's `#<seconds>` lines.
 * Every command in them is made up for the test.
 *
 * `shell.suggest_history` defaults to `all`, so the unit harness gives
 * every test an EMPTY home of its own (harness.c); the sentinel tests at
 * the end prove a real home's history never reaches a ghost.
 */

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compspec_fix.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "ui/cmdhist.h"
#include "ui/cmdline.h"
#include "ui/compfish.h"
#include "util/buf.h"

#define HIST_FIXTURES "tests/unit/fixtures/history/"

static char *read_fixture(const char *name, size_t *len)
{
    char path[256];
    Bytebuf b;
    char *out;

    SPEC_FMT(path, sizeof(path), HIST_FIXTURES "%s", name);
    bytebuf_init(&b);
    spec_read_file(path, &b);
    YEW_ASSERT(b.len != 0U);
    out = yew_xmalloc(b.len + 1U);
    (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    *len = b.len;
    bytebuf_free(&b);
    return out;
}

static void copy_fixture(const char *name, const char *to)
{
    size_t len;
    char *data = read_fixture(name, &len);
    int fd = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, data, len), (i64)len);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    yew_xfree(data);
}

static void write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    size_t n = strlen(text);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, text, n), (i64)n);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void mkdirs(const char *root, const char *rel)
{
    char path[512];
    const char *p = rel;

    for (;;) {
        const char *slash = strchr(p, '/');

        SPEC_FMT(path, sizeof(path), "%s/%.*s", root,
                 (int)(slash == NULL ? strlen(rel) : (size_t)(slash - rel)),
                 rel);
        if (mkdir(path, 0700) != 0)
            YEW_ASSERT_EQ_I64(errno, EEXIST);
        if (slash == NULL)
            return;
        p = slash + 1;
    }
}

/* All three shells' histories in `home`'s default places. */
static void plant_home(const char *home)
{
    char path[512];

    mkdirs(home, ".local/share/fish");
    SPEC_FMT(path, sizeof(path), "%s/.local/share/fish/fish_history", home);
    copy_fixture("fish_history", path);
    SPEC_FMT(path, sizeof(path), "%s/.zsh_history", home);
    copy_fixture("zsh_history", path);
    SPEC_FMT(path, sizeof(path), "%s/.bash_history", home);
    copy_fixture("bash_history", path);
}

static const char *snap_at(const YewHistSuggest *s, u32 i)
{
    YEW_ASSERT(i < s->n);
    return yew_hist_suggest_at(s, i);
}

/* ------------------------------------------------------------------ */
/* The readers                                                         */
/* ------------------------------------------------------------------ */

void test_histsuggest_fish_reader(void)
{
    YewHistSuggest s;
    size_t len;
    char *data = read_fixture("fish_history", &len);

    yew_hist_suggest_init(&s);
    yew_hist_parse_fish(&s, data, len);
    /* Newest first; the multi-line and the secret-bearing are refused;
     * `\\` is one backslash. */
    YEW_ASSERT_EQ_U64(s.n, 4U);
    YEW_ASSERT_EQ_STR(snap_at(&s, 0U), "fish-only --flag");
    YEW_ASSERT_EQ_STR(snap_at(&s, 1U), "git status");
    YEW_ASSERT_EQ_STR(snap_at(&s, 2U), "grep -r \"a\\b\" src");
    YEW_ASSERT_EQ_STR(snap_at(&s, 3U), "git status --short");
    yew_hist_suggest_free(&s);
    yew_xfree(data);
}

void test_histsuggest_unmetafy(void)
{
    char bytes[] = "cat \xC4\x83\xA3\xC4\x83\xB9.txt \x83\xA3";
    size_t n = yew_hist_unmetafy(bytes, sizeof(bytes) - 1U);

    YEW_ASSERT_EQ_U64(n, 14U);
    YEW_ASSERT_EQ_MEM(bytes, "cat \xC4\x83\xC4\x99.txt \x83", n);
}

void test_histsuggest_zsh_reader_unmetafies_first(void)
{
    YewHistSuggest s;
    size_t len;
    char *data = read_fixture("zsh_history", &len);
    u32 i;

    /* The fixture really is metafied: the raw file does NOT contain
     * `ăę` as UTF-8. */
    YEW_ASSERT_NULL(strstr(data, "\xC4\x83\xC4\x99"));
    yew_hist_suggest_init(&s);
    yew_hist_parse_zsh(&s, data, len);
    /* Timestamp prefixes gone; the continuation line is one (refused)
     * multi-line entry; the secret refused. */
    YEW_ASSERT_EQ_U64(s.n, 4U);
    YEW_ASSERT_EQ_STR(snap_at(&s, 0U), "zsh-only run");
    YEW_ASSERT_EQ_STR(snap_at(&s, 1U), "git stash list");
    YEW_ASSERT_EQ_STR(snap_at(&s, 2U), "cat \xC4\x83\xC4\x99.txt");
    YEW_ASSERT_EQ_STR(snap_at(&s, 3U), "ls caf\xC3\xA9");
    for (i = 0U; i < s.n; i++) {
        YEW_ASSERT_NULL(strchr(yew_hist_suggest_at(&s, i), '\n'));
        YEW_ASSERT_NULL(strstr(yew_hist_suggest_at(&s, i), ": 17"));
    }
    yew_hist_suggest_free(&s);
    yew_xfree(data);
}

void test_histsuggest_bash_reader_skips_timestamps(void)
{
    YewHistSuggest s;
    size_t len;
    char *data = read_fixture("bash_history", &len);

    yew_hist_suggest_init(&s);
    yew_hist_parse_bash(&s, data, len);
    YEW_ASSERT_EQ_U64(s.n, 3U);
    YEW_ASSERT_EQ_STR(snap_at(&s, 0U), "ls -la");
    YEW_ASSERT_EQ_STR(snap_at(&s, 1U), "git status --porcelain");
    YEW_ASSERT_EQ_STR(snap_at(&s, 2U), "bash-only thing");
    /* A `#` line that is not all digits is a command (a comment one). */
    yew_hist_suggest_free(&s);
    yew_hist_suggest_init(&s);
    yew_hist_parse_bash(&s, "#12\n# note\n", 11U);
    YEW_ASSERT_EQ_U64(s.n, 1U);
    YEW_ASSERT_EQ_STR(snap_at(&s, 0U), "# note");
    yew_hist_suggest_free(&s);
    yew_xfree(data);
}

void test_histsuggest_refuses_secrets_multiline_and_controls(void)
{
    static const char *const refused[] = {
        "export API_TOKEN=abc", "AWS_SECRET_ACCESS_KEY=x deploy",
        "a=1;DB_PASSWORD=hunter2 run", "curl --token=abc host",
        "env GITHUB_TOKEN=x gh", "echo one\ntwo", "tab\there",
        "bell\x07"};
    static const char *const kept[] = {
        "export PATH=/usr/bin", "make CC=clang", "echo TOKEN",
        "git log --format=%H", "ls caf\xC3\xA9"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(refused); i++)
        YEW_ASSERT(yew_hist_suggest_refused(refused[i],
                                            strlen(refused[i])));
    for (i = 0U; i < YEW_ARRAY_LEN(kept); i++)
        YEW_ASSERT(!yew_hist_suggest_refused(kept[i], strlen(kept[i])));
}

void test_histsuggest_dedupes_newest_first_and_caps(void)
{
    YewHistSuggest s;
    char line[64];
    u32 i;

    yew_hist_suggest_init(&s);
    YEW_ASSERT(yew_hist_suggest_add(&s, "make test", 9U));
    YEW_ASSERT(yew_hist_suggest_add(&s, "  make", 6U));
    YEW_ASSERT(!yew_hist_suggest_add(&s, "make test", 9U));
    YEW_ASSERT(!yew_hist_suggest_add(&s, " make test", 10U));
    YEW_ASSERT(!yew_hist_suggest_add(&s, "   ", 3U));
    YEW_ASSERT_EQ_U64(s.n, 2U);
    YEW_ASSERT_EQ_STR(snap_at(&s, 1U), "make");
    for (i = 0U; i < YEW_HIST_SUGGEST_MAX + 100U; i++) {
        (void)snprintf(line, sizeof(line), "cmd-%u", (unsigned)i);
        (void)yew_hist_suggest_add(&s, line, strlen(line));
    }
    YEW_ASSERT_EQ_U64(s.n, YEW_HIST_SUGGEST_MAX);
    yew_hist_suggest_free(&s);
}

void test_histsuggest_match_newest_longer_byte_exact(void)
{
    YewHistSuggest s;
    const char *rest;
    size_t n = 0U;

    yew_hist_suggest_init(&s);
    (void)yew_hist_suggest_add(&s, "git status", 10U);         /* newest */
    (void)yew_hist_suggest_add(&s, "git status --short", 18U);
    (void)yew_hist_suggest_add(&s, "Git Stash", 9U);
    rest = yew_hist_suggest_match(&s, "git st", 6U, &n);
    YEW_ASSERT_EQ_U64(n, 4U);
    YEW_ASSERT_EQ_MEM(rest, "atus", 4U);
    /* Equal is not longer: the next entry answers. */
    rest = yew_hist_suggest_match(&s, "git status", 10U, &n);
    YEW_ASSERT_EQ_U64(n, 8U);
    YEW_ASSERT_EQ_MEM(rest, " --short", 8U);
    /* Case-sensitive. */
    rest = yew_hist_suggest_match(&s, "Git S", 5U, &n);
    YEW_ASSERT_EQ_MEM(rest, "tash", 4U);
    YEW_ASSERT_NULL(yew_hist_suggest_match(&s, "GIT", 3U, &n));
    YEW_ASSERT_EQ_U64(n, 0U);
    /* Leading blanks are the shell's, not the command's. */
    rest = yew_hist_suggest_match(&s, "  git st", 8U, &n);
    YEW_ASSERT_EQ_MEM(rest, "atus", 4U);
    YEW_ASSERT_NULL(yew_hist_suggest_match(&s, "", 0U, &n));
    YEW_ASSERT_NULL(yew_hist_suggest_match(&s, "  ", 2U, &n));
    yew_hist_suggest_free(&s);
}

void test_histsuggest_reads_shell_files_from_the_environment(void)
{
    SpecFix f;
    YewHistSuggest s;
    char path[512];
    char *old_data = spec_env_copy("XDG_DATA_HOME");
    char *old_hist = spec_env_copy("HISTFILE");

    spec_fix_init(&f);
    plant_home(f.home);
    YEW_ASSERT_EQ_I64(unsetenv("XDG_DATA_HOME"), 0);
    YEW_ASSERT_EQ_I64(unsetenv("HISTFILE"), 0);
    yew_hist_test_reset_shell_opens();
    yew_hist_suggest_init(&s);
    yew_hist_suggest_read_shells(&s);
    YEW_ASSERT_EQ_U64(yew_hist_test_shell_opens(), 3U);
    /* fish, then zsh, then bash; each newest first. */
    YEW_ASSERT_EQ_U64(s.n, 11U);
    YEW_ASSERT_EQ_STR(snap_at(&s, 0U), "fish-only --flag");
    YEW_ASSERT_EQ_STR(snap_at(&s, 4U), "zsh-only run");
    YEW_ASSERT_EQ_STR(snap_at(&s, 8U), "ls -la");
    yew_hist_suggest_free(&s);

    /* XDG_DATA_HOME moves fish's; a zsh-named HISTFILE replaces
     * ~/.zsh_history; otherwise HISTFILE is bash's. */
    mkdirs(f.root, "data/fish");
    SPEC_FMT(path, sizeof(path), "%s/data/fish/fish_history", f.root);
    write_text(path, "- cmd: from-xdg-data\n");
    SPEC_FMT(path, sizeof(path), "%s/data", f.root);
    YEW_ASSERT_EQ_I64(setenv("XDG_DATA_HOME", path, 1), 0);
    SPEC_FMT(path, sizeof(path), "%s/my_zsh_hist", f.root);
    write_text(path, ": 1:0;from-histfile-zsh\n");
    YEW_ASSERT_EQ_I64(setenv("HISTFILE", path, 1), 0);
    yew_hist_suggest_init(&s);
    yew_hist_suggest_read_shells(&s);
    YEW_ASSERT_EQ_STR(snap_at(&s, 0U), "from-xdg-data");
    YEW_ASSERT_EQ_STR(snap_at(&s, 1U), "from-histfile-zsh");
    YEW_ASSERT_EQ_STR(snap_at(&s, 2U), "ls -la");
    yew_hist_suggest_free(&s);
    SPEC_FMT(path, sizeof(path), "%s/bashlog", f.root);
    write_text(path, "from-histfile-bash\n");
    YEW_ASSERT_EQ_I64(setenv("HISTFILE", path, 1), 0);
    yew_hist_suggest_init(&s);
    yew_hist_suggest_read_shells(&s);
    YEW_ASSERT_EQ_STR(snap_at(&s, s.n - 1U), "from-histfile-bash");
    yew_hist_suggest_free(&s);

    /* No HOME, no XDG_DATA_HOME, no HISTFILE: nothing is read -- never
     * the password database's idea of home. */
    YEW_ASSERT_EQ_I64(unsetenv("HOME"), 0);
    YEW_ASSERT_EQ_I64(unsetenv("XDG_DATA_HOME"), 0);
    YEW_ASSERT_EQ_I64(unsetenv("HISTFILE"), 0);
    yew_hist_test_reset_shell_opens();
    yew_hist_suggest_init(&s);
    yew_hist_suggest_read_shells(&s);
    YEW_ASSERT_EQ_U64(s.n, 0U);
    YEW_ASSERT_EQ_U64(yew_hist_test_shell_opens(), 0U);
    yew_hist_suggest_free(&s);
    spec_env_restore("XDG_DATA_HOME", old_data);
    spec_env_restore("HISTFILE", old_hist);
    spec_fix_drop(&f);
}

/* ------------------------------------------------------------------ */
/* The ghost                                                           */
/* ------------------------------------------------------------------ */

typedef struct GhostFix {
    SpecFix spec;
    char *old_data;
    Ed ed;
} GhostFix;

static void ghost_fix_init(GhostFix *g, bool plant)
{
    spec_fix_init(&g->spec);
    /* fish's history is found through HOME here, as on most machines. */
    g->old_data = spec_env_copy("XDG_DATA_HOME");
    YEW_ASSERT_EQ_I64(unsetenv("XDG_DATA_HOME"), 0);
    if (plant)
        plant_home(g->spec.home);
    yew_hist_test_reset_shell_opens();
    yew_ed_init(&g->ed);
    YEW_ASSERT(yew_ed_open_scratch(&g->ed));
    yew_test_load_runtime(&g->ed);
    /* A session-lived history: nothing is written under a state dir. */
    g->ed.clean = true;
    g->ed.ws.dir = g->spec.root;
}

static void ghost_fix_drop(GhostFix *g)
{
    if (g->ed.cmdline.active)
        yew_cmdline_close(&g->ed, false);
    g->ed.ws.dir = NULL;
    yew_ed_free(&g->ed);
    spec_env_restore("XDG_DATA_HOME", g->old_data);
    spec_fix_drop(&g->spec);
}

static void set_option(Ed *ed, const char *name, const char *value)
{
    OptVal v;
    const char *err = NULL;

    (void)memset(&v, 0, sizeof(v));
    v.type = (u8)YEW_OPT_ENUM;
    v.as.str.s = value;
    v.as.str.len = (u32)strlen(value);
    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_SCOPE_DECLARED, name,
                           (u32)strlen(name), &v, &err));
}

/* Open `:`, record `own` E-mode lines (oldest first) in its history,
 * type `line`. */
static void prompt(GhostFix *g, const char *const *own, size_t n_own,
                   const char *line)
{
    size_t i;

    if (g->ed.cmdline.active)
        yew_cmdline_close(&g->ed, false);
    yew_cmdline_open(&g->ed, YEW_PROMPT_CMD, NULL);
    for (i = 0U; i < n_own; i++)
        yew_hist_add(g->ed.cmdline.history, own[i]);
    yew_cmdline_paste(&g->ed, (const u8 *)line, strlen(line));
}

static char *text_of(Ed *ed)
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

static void assert_ghost(Ed *ed, const char *want)
{
    size_t n = 0U;
    const char *ghost = yew_cmdline_ghost(ed, &n);

    if (want == NULL) {
        YEW_ASSERT(ghost == NULL || n == 0U);
        return;
    }
    YEW_ASSERT_NOT_NULL(ghost);
    YEW_ASSERT_EQ_U64(n, strlen(want));
    YEW_ASSERT_EQ_MEM(ghost, want, n);
}

static void assert_text(Ed *ed, const char *want)
{
    char *t = text_of(ed);

    YEW_ASSERT_EQ_STR(t, want);
    yew_xfree(t);
}

static CmdStatus invoke(Ed *ed, CmdStatus (*fn)(CmdCtx *))
{
    CmdCtx cx;

    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    cx.win = yew_cmdline_target(ed);
    cx.count = 1U;
    return fn(&cx);
}

static void move(Ed *ed, const char *command)
{
    CmdCtx cx;

    (void)memset(&cx, 0, sizeof(cx));
    cx.win = yew_cmdline_target(ed);
    cx.count = 1U;
    YEW_ASSERT_EQ_I64(yew_ed_invoke(ed, yew_cmd_lookup(command,
                                                       (u32)strlen(command)),
                                    &cx),
                      YEW_CMD_OK);
}

void test_histsuggest_default_is_all(void)
{
    GhostFix g;
    OptVal v;

    ghost_fix_init(&g, false);
    YEW_ASSERT(yew_opt_get(&g.ed, NULL, NULL, "shell.suggest_history", 21U,
                           &v));
    YEW_ASSERT_EQ_U64(v.as.str.len, 3U);
    YEW_ASSERT_EQ_MEM(v.as.str.s, "all", 3U);
    ghost_fix_drop(&g);
}

/* The ghost is the remainder of the newest match, and it is never in the
 * prompt's text. */
void test_histsuggest_ghost_is_the_remainder(void)
{
    GhostFix g;

    ghost_fix_init(&g, true);
    prompt(&g, NULL, 0U, "!git st");
    assert_ghost(&g.ed, "atus");
    assert_text(&g.ed, "!git st");
    /* Newest wins; an equal entry is not a suggestion. */
    prompt(&g, NULL, 0U, "!git status");
    assert_ghost(&g.ed, " --short");
    prompt(&g, NULL, 0U, "!git stash");
    assert_ghost(&g.ed, " list");
    /* zsh's unmetafied bytes, exactly. */
    prompt(&g, NULL, 0U, "!ls caf");
    assert_ghost(&g.ed, "\xC3\xA9");
    prompt(&g, NULL, 0U, "!cat \xC4\x83");
    assert_ghost(&g.ed, "\xC4\x99.txt");
    /* Refused entries never show. */
    prompt(&g, NULL, 0U, "!env AWS");
    assert_ghost(&g.ed, NULL);
    prompt(&g, NULL, 0U, "!export API");
    assert_ghost(&g.ed, NULL);
    prompt(&g, NULL, 0U, "!echo one");
    assert_ghost(&g.ed, NULL);
    /* Only a bang body, only at the end, only non-empty. */
    prompt(&g, NULL, 0U, "git st");
    assert_ghost(&g.ed, NULL);
    prompt(&g, NULL, 0U, "!");
    assert_ghost(&g.ed, NULL);
    prompt(&g, NULL, 0U, "!git st");
    move(&g.ed, "ed.move.char.prev");
    assert_ghost(&g.ed, NULL);
    ghost_fix_drop(&g);
}

/* yew's own E-mode bang entries, matched by BODY whichever prefix form
 * ran them; they come before the shells'. */
void test_histsuggest_own_history_bodies(void)
{
    static const char *const own[] = {
        "w", "%!sort -u", "r !date +%s", "!!htop -d 5", "!git stage -p"};
    GhostFix g;

    ghost_fix_init(&g, true);
    prompt(&g, own, YEW_ARRAY_LEN(own), "!git st");
    assert_ghost(&g.ed, "age -p");
    prompt(&g, own, YEW_ARRAY_LEN(own), "!so");
    assert_ghost(&g.ed, "rt -u");
    prompt(&g, own, YEW_ARRAY_LEN(own), "r !da");
    assert_ghost(&g.ed, "te +%s");
    prompt(&g, own, YEW_ARRAY_LEN(own), "%!ht");
    assert_ghost(&g.ed, "op -d 5");
    ghost_fix_drop(&g);
}

/* `yew` reads no shell file at all. */
void test_histsuggest_yew_reads_no_shell_file(void)
{
    static const char *const own[] = {"!git stage -p"};
    GhostFix g;

    ghost_fix_init(&g, true);
    set_option(&g.ed, "shell.suggest_history", "yew");
    prompt(&g, own, 1U, "!git st");
    assert_ghost(&g.ed, "age -p");
    prompt(&g, own, 1U, "!fish-only");
    assert_ghost(&g.ed, NULL);
    prompt(&g, own, 1U, "!zsh-o");
    assert_ghost(&g.ed, NULL);
    YEW_ASSERT_EQ_U64(yew_hist_test_shell_opens(), 0U);
    set_option(&g.ed, "shell.suggest_history", "all");
    prompt(&g, own, 1U, "!zsh-o");
    assert_ghost(&g.ed, "nly run");
    YEW_ASSERT_EQ_U64(yew_hist_test_shell_opens(), 3U);
    ghost_fix_drop(&g);
}

/* The snapshot is taken once per prompt: a history file written after
 * it cannot change a frame (invariant 5). */
void test_histsuggest_snapshot_is_fixed_per_prompt(void)
{
    GhostFix g;
    char path[512];

    ghost_fix_init(&g, true);
    prompt(&g, NULL, 0U, "!git st");
    assert_ghost(&g.ed, "atus");
    SPEC_FMT(path, sizeof(path), "%s/.bash_history", g.spec.home);
    write_text(path, "git stable-newest\n");
    SPEC_FMT(path, sizeof(path), "%s/.local/share/fish/fish_history",
             g.spec.home);
    write_text(path, "- cmd: git stable-newest\n");
    yew_cmdline_paste(&g.ed, (const u8 *)"a", 1U);
    assert_ghost(&g.ed, "tus");
    YEW_ASSERT_EQ_U64(yew_hist_test_shell_opens(), 3U);
    /* The next prompt takes a new snapshot. */
    prompt(&g, NULL, 0U, "!git st");
    assert_ghost(&g.ed, "able-newest");
    ghost_fix_drop(&g);
}

/* The snapshot is taken on the first idle turn after the prompt opens,
 * so no keystroke pays for reading the files; typing a bang body before
 * that turn loads it on demand. */
void test_histsuggest_snapshot_taken_on_the_idle_turn(void)
{
    GhostFix g;

    ghost_fix_init(&g, true);
    yew_cmdline_open(&g.ed, YEW_PROMPT_CMD, NULL);
    YEW_ASSERT(!g.ed.cmdline.suggest_loaded);
    YEW_ASSERT(yew_cmdline_comp_idle_pending(&g.ed));
    YEW_ASSERT_EQ_U64(yew_hist_test_shell_opens(), 0U);
    (void)yew_cmdline_comp_idle(&g.ed);
    YEW_ASSERT(g.ed.cmdline.suggest_loaded);
    YEW_ASSERT_EQ_U64(yew_hist_test_shell_opens(), 3U);
    YEW_ASSERT(!yew_cmdline_comp_idle_pending(&g.ed));
    yew_cmdline_paste(&g.ed, (const u8 *)"!git st", 7U);
    assert_ghost(&g.ed, "atus");
    YEW_ASSERT_EQ_U64(yew_hist_test_shell_opens(), 3U);
    ghost_fix_drop(&g);
}

/* `<right>` takes it all; A-f one word at a time, blanks and quoting
 * respected; no ghost is a motion. */
void test_histsuggest_accept_whole_and_word(void)
{
    static const char *const own[] = {
        "!rsync -av \"my dir/\" host:backup/ --delete"};
    GhostFix g;

    ghost_fix_init(&g, false);
    prompt(&g, own, 1U, "!rsync -a");
    assert_ghost(&g.ed, "v \"my dir/\" host:backup/ --delete");
    YEW_ASSERT_EQ_I64(invoke(&g.ed, yew_cmdline_cmd_ghost_accept_word),
                      YEW_CMD_OK);
    assert_text(&g.ed, "!rsync -av ");
    YEW_ASSERT_EQ_I64(invoke(&g.ed, yew_cmdline_cmd_ghost_accept_word),
                      YEW_CMD_OK);
    assert_text(&g.ed, "!rsync -av \"my dir/\" ");
    YEW_ASSERT_EQ_I64(invoke(&g.ed, yew_cmdline_cmd_ghost_accept),
                      YEW_CMD_OK);
    assert_text(&g.ed, "!rsync -av \"my dir/\" host:backup/ --delete");
    assert_ghost(&g.ed, NULL);
    /* Inside an open quote, the blank is quoted. */
    prompt(&g, own, 1U, "!rsync -av \"my");
    assert_ghost(&g.ed, " dir/\" host:backup/ --delete");
    YEW_ASSERT_EQ_I64(invoke(&g.ed, yew_cmdline_cmd_ghost_accept_word),
                      YEW_CMD_OK);
    assert_text(&g.ed, "!rsync -av \"my dir/\" ");
    /* A ghost that starts with a blank takes it and the next word. */
    prompt(&g, own, 1U, "!rsync");
    YEW_ASSERT_EQ_I64(invoke(&g.ed, yew_cmdline_cmd_ghost_accept_word),
                      YEW_CMD_OK);
    assert_text(&g.ed, "!rsync -av ");
    /* No ghost: <right> moves one grapheme, A-f one word. */
    prompt(&g, NULL, 0U, "!zzz qq");
    move(&g.ed, "ed.move.line.home");
    move(&g.ed, "ed.move.char.next");
    YEW_ASSERT_EQ_U64(g.ed.cmdline.cur.pos.v, 1U);
    YEW_ASSERT_EQ_I64(invoke(&g.ed, yew_cmdline_cmd_ghost_accept),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(g.ed.cmdline.cur.pos.v, 2U);
    YEW_ASSERT_EQ_I64(invoke(&g.ed, yew_cmdline_cmd_ghost_accept_word),
                      YEW_CMD_OK);
    YEW_ASSERT(g.ed.cmdline.cur.pos.v > 2U);
    assert_text(&g.ed, "!zzz qq");
    ghost_fix_drop(&g);
}

/*
 * Precedence: without an explicit selection the history ghost wins over
 * the top row's rest; WITH one, the selected row's rest (the user is
 * navigating the menu); no history match, the top row's rest.
 */
void test_histsuggest_precedence_against_the_token_ghost(void)
{
    static const char *const own[] = {"!git stxyz-from-history"};
    GhostFix g;
    const CompItem *sel;
    size_t n = 0U;
    const char *ghost;

    ghost_fix_init(&g, false);
    prompt(&g, own, 1U, "!git st");
    YEW_ASSERT(g.ed.cmdline.menu.items.len != 0U);
    assert_ghost(&g.ed, "xyz-from-history");
    YEW_ASSERT_EQ_I64(invoke(&g.ed, yew_cmdline_cmd_menu_next), YEW_CMD_OK);
    sel = yew_menu_selected(&g.ed.cmdline.menu);
    YEW_ASSERT_NOT_NULL(sel);
    ghost = yew_cmdline_ghost(&g.ed, &n);
    YEW_ASSERT_NOT_NULL(ghost);
    YEW_ASSERT_EQ_STR(ghost, sel->text + 2);
    /* No history match: the top row's rest. */
    prompt(&g, own, 1U, "!git chec");
    YEW_ASSERT(g.ed.cmdline.menu.items.len != 0U);
    YEW_ASSERT_NULL(yew_menu_selected(&g.ed.cmdline.menu));
    ghost = yew_cmdline_ghost(&g.ed, &n);
    YEW_ASSERT_NOT_NULL(ghost);
    YEW_ASSERT_EQ_STR(ghost, g.ed.cmdline.menu.items.data[0].text + 4);
    ghost_fix_drop(&g);
}

/* ------------------------------------------------------------------ */
/* Isolation: a real home's history never reaches a ghost              */
/* ------------------------------------------------------------------ */

#define SENTINEL "yew-sentinel-7f3a LEAKED-FROM-REAL-HOME"

/* A home that looks like a developer's: all three histories carry the
 * sentinel, in their default places and via XDG_DATA_HOME / HISTFILE. */
static void plant_sentinel(const char *home)
{
    char path[512];

    mkdirs(home, ".local/share/fish");
    SPEC_FMT(path, sizeof(path), "%s/.local/share/fish/fish_history", home);
    write_text(path, "- cmd: echo " SENTINEL "\n");
    SPEC_FMT(path, sizeof(path), "%s/.zsh_history", home);
    write_text(path, ": 1700000000:0;echo " SENTINEL "\n");
    SPEC_FMT(path, sizeof(path), "%s/.bash_history", home);
    write_text(path, "echo " SENTINEL "\n");
}

/* The harness's default: HOME and XDG_DATA_HOME are the run's own,
 * HISTFILE is unset, and no test reaches a real fish. */
void test_histsuggest_harness_isolates_by_default(void)
{
    const char *home = getenv("HOME");
    const char *data = getenv("XDG_DATA_HOME");
    const char *fish = getenv("YEW_TEST_FISH");

    YEW_ASSERT_NOT_NULL(home);
    YEW_ASSERT_NOT_NULL(data);
    YEW_ASSERT_NOT_NULL(strstr(home, "/yew-unit-cache-"));
    YEW_ASSERT_NOT_NULL(strstr(data, "/yew-unit-cache-"));
    YEW_ASSERT_NULL(getenv("HISTFILE"));
    YEW_ASSERT_NOT_NULL(fish);
    YEW_ASSERT_EQ_STR(fish, "");
    yew_compfish_reset();
    YEW_ASSERT_NULL(yew_compfish_path());
}

/* The probe the leak test runs in a CHILD whose environment points at a
 * sentinel home.  In the ordinary run it passes trivially. */
void test_histsuggest_sentinel_probe(void)
{
    Ed ed;
    size_t n = 0U;
    const char *ghost;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_test_load_runtime(&ed);
    ed.clean = true;
    yew_cmdline_open(&ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&ed, (const u8 *)"!echo yew-sentinel", 18U);
    ghost = yew_cmdline_ghost(&ed, &n);
    YEW_ASSERT(ghost == NULL || strstr(ghost, "LEAKED") == NULL);
    yew_cmdline_close(&ed, false);
    yew_ed_free(&ed);
}

/*
 * The proof.  A fake "real" home carries the sentinel in every history
 * file a shell would use.  In-process, pointed at it, the ghost DOES show
 * the sentinel -- the probe can see a leak.  Then the unit binary runs
 * the probe as a child with HOME, XDG_DATA_HOME and HISTFILE all aimed at
 * that home, exactly as a developer's shell would launch it: the
 * harness's default isolation must keep the sentinel out.
 */
void test_histsuggest_sentinel_never_leaks(void)
{
    SpecFix f;
    char real_home[256];
    char data[512];
    char histfile[512];
    char out_path[512];
    Ed ed;
    size_t n = 0U;
    const char *ghost;
    pid_t pid;
    int status = 0;

    spec_fix_init(&f);
    SPEC_FMT(real_home, sizeof(real_home), "%s/real-home", f.root);
    YEW_ASSERT_EQ_I64(mkdir(real_home, 0700), 0);
    plant_sentinel(real_home);
    SPEC_FMT(data, sizeof(data), "%s/.local/share", real_home);
    SPEC_FMT(histfile, sizeof(histfile), "%s/.zsh_history", real_home);

    /* Positive control: this home, unisolated, leaks. */
    YEW_ASSERT_EQ_I64(setenv("HOME", real_home, 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    yew_test_load_runtime(&ed);
    ed.clean = true;
    yew_cmdline_open(&ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&ed, (const u8 *)"!echo yew-sentinel", 18U);
    ghost = yew_cmdline_ghost(&ed, &n);
    YEW_ASSERT_NOT_NULL(ghost);
    YEW_ASSERT_NOT_NULL(strstr(ghost, "LEAKED"));
    yew_cmdline_close(&ed, false);
    yew_ed_free(&ed);

    /* The child: a fresh unit run of the probe alone. */
    SPEC_FMT(out_path, sizeof(out_path), "%s/child.out", f.root);
    pid = fork();
    YEW_ASSERT(pid >= 0);
    if (pid == 0) {
        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (fd < 0)
            _exit(125);
        (void)dup2(fd, 1);
        (void)dup2(fd, 2);
        (void)setenv("HOME", real_home, 1);
        (void)setenv("XDG_DATA_HOME", data, 1);
        (void)setenv("HISTFILE", histfile, 1);
        (void)unsetenv("YEW_TEST_FISH");
        execl(yew_test_program_path(), yew_test_program_path(), "--filter",
              "histsuggest_sentinel_probe", (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    {
        Bytebuf b;

        bytebuf_init(&b);
        spec_read_file(out_path, &b);
        bytebuf_push_u8(&b, 0U);
        YEW_ASSERT_NOT_NULL(strstr((const char *)b.data,
                                   "PASS histsuggest_sentinel_probe"));
        YEW_ASSERT_NULL(strstr((const char *)b.data, "LEAKED"));
        bytebuf_free(&b);
    }
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
    spec_fix_drop(&f);
}
