#define _POSIX_C_SOURCE 200809L

/*
 * Sprint 57.26 §2: the fish oracle.
 *
 * Every test but one runs against a STUB: YEW_TEST_FISH names a shell
 * script that records its argv (one file per element, so a test can
 * compare an element byte for byte) and prints canned rows for the
 * command it was asked about -- or nothing, which is the gate closed.
 * The suite's pass/fail never depends on fish being installed
 * (Amendment S57.26-A1).
 *
 * The one real-fish test skips, loudly, when fish is not on PATH; when
 * it is, it re-verifies the measured facts §2's design rests on and says
 * which one a later fish changed.
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
#include <sys/wait.h>
#include <unistd.h>

#include "compspec_fix.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/option.h"
#include "ui/cmdcomp.h"
#include "ui/cmdline.h"
#include "ui/compfish.h"
#include "ui/compgen.h"
#include "ui/comphelp.h"
#include "ui/compspec.h"
#include "util/buf.h"

#ifndef YEW_TEST_HELPFIX
#define YEW_TEST_HELPFIX "build/help_fixture"
#endif

/* ------------------------------------------------------------------ */
/* The stub and its fixture                                            */
/* ------------------------------------------------------------------ */

/*
 * $1..$7 are `-N --private -c QUERY -- LINE CMD`.  Rows come from
 * `<cmd>.rows` (or `<cmd>.flags` for a `-` query stem); no file, no
 * output -- exactly what the real script prints when fish has no rules.
 * `<cmd>.fail` exits 3 with nothing printed; `<cmd>.hold` waits.
 */
static const char stub_script[] =
    "#!/bin/sh\n"
    "PATH=/usr/bin:/bin\n"
    "d=$(dirname \"$0\")\n"
    "n=0\n"
    "for a in \"$@\"; do n=$((n+1)); printf '%s' \"$a\" > \"$d/arg.$n\"; "
    "done\n"
    "echo \"$n\" > \"$d/argc\"\n"
    "echo call >> \"$d/calls\"\n"
    "line=$6\n"
    "cmd=$7\n"
    "i=0\n"
    "while [ -e \"$d/$cmd.hold\" ] && [ $i -lt 3000 ]; do "
    "sleep 0.02; i=$((i+1)); done\n"
    "if [ -e \"$d/$cmd.fail\" ]; then exit 3; fi\n"
    "case \"$line\" in\n"
    "  *' -') f=\"$d/$cmd.flags\" ;;\n"
    "  *) f=\"$d/$cmd.rows\" ;;\n"
    "esac\n"
    "if [ -e \"$f\" ]; then cat \"$f\"; fi\n"
    "exit 0\n";

typedef struct FishFix {
    SpecFix spec;
    char stubdir[160];
    char stub[192];
    char bin[160];
    char *old_fish;
    char *old_path;
    Ed ed;
} FishFix;

static void fix_write(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    size_t n = strlen(text);

    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(write(fd, text, n), (i64)n);
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static void fish_fix_init(FishFix *f)
{
    spec_fix_init(&f->spec);
    SPEC_FMT(f->stubdir, sizeof(f->stubdir), "%s/stub", f->spec.root);
    SPEC_FMT(f->stub, sizeof(f->stub), "%s/fish", f->stubdir);
    SPEC_FMT(f->bin, sizeof(f->bin), "%s/bin", f->spec.root);
    YEW_ASSERT_EQ_I64(mkdir(f->stubdir, 0700), 0);
    YEW_ASSERT_EQ_I64(mkdir(f->bin, 0700), 0);
    fix_write(f->stub, stub_script, 0755);
    f->old_fish = spec_env_copy("YEW_TEST_FISH");
    f->old_path = spec_env_copy("PATH");
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_FISH", f->stub, 1), 0);
    /* No tool the help layer could run, unless a test installs one. */
    YEW_ASSERT_EQ_I64(setenv("PATH", f->bin, 1), 0);
    yew_compfish_reset();
    yew_comphelp_reset();
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_scratch(&f->ed));
    yew_test_load_runtime(&f->ed);
    f->ed.ws.dir = f->spec.root;
}

static void fish_fix_drop(FishFix *f)
{
    if (f->ed.cmdline.active)
        yew_cmdline_close(&f->ed, false);
    f->ed.ws.dir = NULL;
    yew_ed_free(&f->ed);
    yew_compfish_reset();
    yew_comphelp_reset();
    spec_env_restore("YEW_TEST_FISH", f->old_fish);
    spec_env_restore("PATH", f->old_path);
    spec_fix_drop(&f->spec);
}

/* `<cmd>.<what>` beside the stub. */
static void stub_file(const FishFix *f, const char *cmd, const char *what,
                      const char *text)
{
    char path[256];

    SPEC_FMT(path, sizeof(path), "%s/%s.%s", f->stubdir, cmd, what);
    fix_write(path, text, 0644);
}

static void stub_unlink(const FishFix *f, const char *cmd, const char *what)
{
    char path[256];

    SPEC_FMT(path, sizeof(path), "%s/%s.%s", f->stubdir, cmd, what);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
}

/* The stub's record of its last invocation: element `i` (1-based), or
 * NULL when it never ran.  Heap-owned. */
static char *stub_arg(const FishFix *f, u32 i)
{
    char path[256];
    Bytebuf b;
    char *out;

    SPEC_FMT(path, sizeof(path), "%s/arg.%u", f->stubdir, (unsigned)i);
    if (access(path, F_OK) != 0)
        return NULL;
    bytebuf_init(&b);
    spec_read_file(path, &b);
    out = yew_xmalloc(b.len + 1U);
    if (b.len != 0U)
        (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

/* How many times the stub ran. */
static u32 stub_calls(const FishFix *f)
{
    char path[256];
    Bytebuf b;
    u32 n = 0U;
    size_t i;

    SPEC_FMT(path, sizeof(path), "%s/calls", f->stubdir);
    if (access(path, F_OK) != 0)
        return 0U;
    bytebuf_init(&b);
    spec_read_file(path, &b);
    for (i = 0U; i < b.len; i++) {
        if (b.data[i] == '\n')
            n++;
    }
    bytebuf_free(&b);
    return n;
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

/* Idle turns and job pumps until nothing is queued or in flight. */
static void settle(Ed *ed)
{
    i64 start = yew_now_ms();

    while ((yew_compfish_inflight() != 0U || yew_compfish_idle_ready() ||
            yew_comphelp_inflight() != 0U || yew_comphelp_idle_ready() ||
            yew_cmdline_comp_idle_pending(ed)) &&
           yew_now_ms() - start < 10000) {
        if (ed->cmdline.active)
            (void)yew_cmdline_comp_idle(ed);
        else if (yew_compfish_idle(ed) == 0U)
            (void)yew_comphelp_idle(ed);
        pump(ed, 10);
    }
    YEW_ASSERT_EQ_U64(yew_compfish_inflight(), 0U);
    YEW_ASSERT_EQ_U64(yew_comphelp_inflight(), 0U);
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

static void tab(Ed *ed)
{
    CmdCtx cx;

    (void)memset(&cx, 0, sizeof(cx));
    cx.ed = ed;
    (void)yew_cmdline_cmd_complete_next(&cx);
}

/* Open `:`, type `line`, Tab, let the oracle answer. */
static void ask(FishFix *f, const char *line)
{
    if (f->ed.cmdline.active)
        yew_cmdline_close(&f->ed, false);
    yew_cmdline_open(&f->ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f->ed, (const u8 *)line, strlen(line));
    tab(&f->ed);
    settle(&f->ed);
}

/* ask(), then -- when the first Tab only waited for the answer -- the
 * Tab that completes from it. */
static void complete_line(FishFix *f, const char *line)
{
    char *text;

    ask(f, line);
    text = prompt_text(&f->ed);
    if (strcmp(text, line) == 0)
        tab(&f->ed);
    yew_xfree(text);
}

static void assert_prompt(Ed *ed, const char *want)
{
    char *text = prompt_text(ed);

    YEW_ASSERT_EQ_STR(text, want);
    yew_xfree(text);
}

/* The menu row whose text is `text`, or NULL. */
static const CompItem *menu_row(const Ed *ed, const char *text)
{
    size_t i;

    for (i = 0U; i < ed->cmdline.menu.items.len; i++) {
        if (strcmp(ed->cmdline.menu.items.data[i].text, text) == 0)
            return &ed->cmdline.menu.items.data[i];
    }
    return NULL;
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

/* ------------------------------------------------------------------ */
/* The line fish sees                                                  */
/* ------------------------------------------------------------------ */

static char *escaped(const char *word)
{
    Bytebuf b;
    char *out;

    bytebuf_init(&b);
    yew_compfish_escape(&b, word);
    out = yew_xmalloc(b.len + 1U);
    if (b.len != 0U)
        (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

void test_compfish_escapes_words_for_fish(void)
{
    static const struct {
        const char *in;
        const char *out;
    } rows[] = {
        {"plain-word_1.txt", "plain-word_1.txt"},
        {"a b", "a\\ b"},
        {"$HOME", "\\$HOME"},
        {"*?~#", "\\*\\?\\~\\#"},
        {"(){}[]", "\\(\\)\\{\\}\\[\\]"},
        {"<>&|;", "\\<\\>\\&\\|\\;"},
        {"\"'\\", "\\\"\\'\\\\"},
        {"tab\there", "tab\\there"},
        {"new\nline", "new\\nline"},
        {"bell\x07", "bell\\x07"},
        {"caf\xC3\xA9", "caf\xC3\xA9"}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        char *got = escaped(rows[i].in);

        YEW_ASSERT_EQ_STR(got, rows[i].out);
        yew_xfree(got);
    }
}

void test_compfish_query_stem_is_empty_dash_or_the_flag(void)
{
    static const struct {
        const char *stem;
        const char *q;
    } rows[] = {
        {"", ""},       {"bu", ""},        {"-", "-"},
        {"-v", "-"},    {"--del", "-"},    {"--out=", "--out="},
        {"--out=x", "--out="}, {"-o=x", "-"}, {".g", "."},
        {"..", "."}
    };
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        char *got = yew_compfish_query_stem(rows[i].stem);

        YEW_ASSERT_EQ_STR(got, rows[i].q);
        yew_xfree(got);
    }
}

void test_compfish_parse_rows(void)
{
    static const char out[] =
        "a b.txt\n"
        "d$x\n"
        "sub/\n"
        "--del\tAn alias for --delete-during\n"
        "--odd\tbell\x07here\n"
        "\n"
        "a b.txt\n"
        "bad\x01name\tdropped\n"
        "--bare\t\n";
    YewFishRow *rows;
    u32 n = 0U;

    rows = yew_compfish_parse(out, sizeof(out) - 1U, &n);
    YEW_ASSERT_EQ_U64(n, 6U);
    YEW_ASSERT_EQ_STR(rows[0].text, "a b.txt");
    YEW_ASSERT_NULL(rows[0].desc);
    YEW_ASSERT_EQ_STR(rows[1].text, "d$x");
    YEW_ASSERT_EQ_STR(rows[2].text, "sub");
    YEW_ASSERT(rows[2].is_dir);
    YEW_ASSERT(!rows[1].is_dir);
    YEW_ASSERT_EQ_STR(rows[3].text, "--del");
    YEW_ASSERT_EQ_STR(rows[3].desc, "An alias for --delete-during");
    /* A control byte in a description is drawn as `·`. */
    YEW_ASSERT_EQ_STR(rows[4].desc, "bell\xC2\xB7here");
    YEW_ASSERT_EQ_STR(rows[5].text, "--bare");
    YEW_ASSERT_NULL(rows[5].desc);
    yew_compfish_rows_free(rows, n);
}

void test_compfish_parse_caps_at_5000_lines(void)
{
    Bytebuf b;
    YewFishRow *rows;
    u32 n = 0U;
    u32 i;

    bytebuf_init(&b);
    for (i = 0U; i < 6000U; i++)
        bytebuf_printf(&b, "row%u\n", (unsigned)i);
    rows = yew_compfish_parse((const char *)b.data, b.len, &n);
    YEW_ASSERT_EQ_U64(n, YEW_COMPFISH_MAX_LINES);
    yew_compfish_rows_free(rows, n);
    bytebuf_free(&b);
}

/* ------------------------------------------------------------------ */
/* The oracle through the prompt                                       */
/* ------------------------------------------------------------------ */

/* Gate open: fish's rows, with descriptions, as GEN rows; the second
 * Tab completes from them. */
void test_compfish_gate_open_rows_answer(void)
{
    FishFix f;
    const CompItem *row;

    fish_fix_init(&f);
    stub_file(&f, "tool", "rows",
              "build\tcompile it\nbench\trun the benchmarks\n");
    ask(&f, "!tool b");
    assert_prompt(&f.ed, "!tool b");
    YEW_ASSERT(!f.ed.cmdline.filter.gen_pending);
    row = menu_row(&f.ed, "build");
    YEW_ASSERT_NOT_NULL(row);
    YEW_ASSERT_EQ_U64(row->kind, (u64)YEW_COMP_GEN);
    YEW_ASSERT_EQ_STR(row->detail, "compile it");
    YEW_ASSERT_NOT_NULL(menu_row(&f.ed, "bench"));
    yew_cmdline_paste(&f.ed, (const u8 *)"u", 1U);
    tab(&f.ed);
    assert_prompt(&f.ed, "!tool build ");
    YEW_ASSERT_EQ_U64(stub_calls(&f), 1U);
    fish_fix_drop(&f);
}

/* Gate closed: the stub prints nothing, so the help rung answers. */
void test_compfish_gate_closed_falls_through_to_help(void)
{
    static const char help[] =
        "Usage: helped <COMMAND>\n\n"
        "Commands:\n"
        "  deploy  Ship it\n"
        "  doctor  Check it\n";
    FishFix f;
    char path[256];
    Bytebuf b;

    fish_fix_init(&f);
    bytebuf_init(&b);
    spec_read_file(YEW_TEST_HELPFIX, &b);
    YEW_ASSERT(b.len > 4U);
    SPEC_FMT(path, sizeof(path), "%s/helped", f.bin);
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);

        YEW_ASSERT(fd >= 0);
        YEW_ASSERT_EQ_I64(write(fd, b.data, b.len), (i64)b.len);
        YEW_ASSERT_EQ_I64(close(fd), 0);
    }
    bytebuf_free(&b);
    SPEC_FMT(path, sizeof(path), "%s/helped.help", f.bin);
    fix_write(path, help, 0644);
    ask(&f, "!helped d");
    YEW_ASSERT_EQ_U64(stub_calls(&f), 1U);
    YEW_ASSERT_EQ_U64(yew_comphelp_test_spawns(), 1U);
    YEW_ASSERT(!f.ed.cmdline.filter.gen_pending);
    YEW_ASSERT_NOT_NULL(menu_row(&f.ed, "deploy"));
    YEW_ASSERT_EQ_U64(menu_row(&f.ed, "deploy")->kind,
                      (u64)YEW_COMP_SPEC);
    fish_fix_drop(&f);
}

/* A directory keeps its `/` and takes no space; `a b.txt` and `d$x` are
 * re-quoted on insert -- fish printed them raw. */
void test_compfish_directories_and_requoting(void)
{
    FishFix f;
    const CompItem *row;

    fish_fix_init(&f);
    stub_file(&f, "tool", "rows", "a b.txt\nd$x\nsubdir/\n");
    ask(&f, "!tool su");
    row = menu_row(&f.ed, "subdir/");
    YEW_ASSERT_NOT_NULL(row);
    YEW_ASSERT(row->is_dir);
    tab(&f.ed);
    assert_prompt(&f.ed, "!tool subdir/");
    complete_line(&f, "!tool a");
    assert_prompt(&f.ed, "!tool a\\ b.txt ");
    complete_line(&f, "!tool d");
    assert_prompt(&f.ed, "!tool d\\$x ");
    fish_fix_drop(&f);
}

/*
 * §2's pitfall: a `:!` line is arbitrary text.  The hostile word arrives
 * inside ONE argv element -- the rebuilt, escaped line -- and the script
 * fish evaluates is the fixed text, byte for byte.
 */
void test_compfish_hostile_line_is_one_argv_element(void)
{
    FishFix f;
    char *argc_text;
    char *a;

    fish_fix_init(&f);
    ask(&f, "!tool \"x'; rm -rf ~; '\" ");
    a = stub_arg(&f, 6U);
    YEW_ASSERT_NOT_NULL(a);
    YEW_ASSERT_EQ_STR(a, "tool x\\'\\;\\ rm\\ -rf\\ \\~\\;\\ \\' ");
    yew_xfree(a);
    a = stub_arg(&f, 4U);
    YEW_ASSERT_EQ_STR(a, YEW_COMPFISH_QUERY);
    yew_xfree(a);
    a = stub_arg(&f, 7U);
    YEW_ASSERT_EQ_STR(a, "tool");
    yew_xfree(a);
    a = stub_arg(&f, 1U);
    YEW_ASSERT_EQ_STR(a, "-N");
    yew_xfree(a);
    a = stub_arg(&f, 2U);
    YEW_ASSERT_EQ_STR(a, "--private");
    yew_xfree(a);
    a = stub_arg(&f, 5U);
    YEW_ASSERT_EQ_STR(a, "--");
    yew_xfree(a);
    argc_text = stub_arg(&f, 0U);
    YEW_ASSERT_NULL(argc_text);
    {
        char path[256];
        Bytebuf b;

        SPEC_FMT(path, sizeof(path), "%s/argc", f.stubdir);
        bytebuf_init(&b);
        spec_read_file(path, &b);
        YEW_ASSERT(b.len >= 1U);
        YEW_ASSERT_EQ_MEM(b.data, "7\n", 2U);
        bytebuf_free(&b);
    }
    /* The script never carries a byte of the line. */
    YEW_ASSERT_NULL(strstr(YEW_COMPFISH_QUERY, "rm"));
    fish_fix_drop(&f);
}

/* The rebuilt words, not the raw text: `$(…)` reaches fish as a literal
 * word, and fish grammar never sees sh's. */
void test_compfish_passes_rebuilt_words_not_the_raw_line(void)
{
    FishFix f;
    char *a;

    fish_fix_init(&f);
    ask(&f, "!tool $(pwd)   'two  words' x");
    a = stub_arg(&f, 6U);
    YEW_ASSERT_NOT_NULL(a);
    /* The expansion's word is unknown until the shell runs it: fish
     * gets an empty word in its place, keeping the positions, and none
     * of its text. */
    YEW_ASSERT_EQ_STR(a, "tool '' two\\ \\ words ");
    yew_xfree(a);
    fish_fix_drop(&f);
}

/* `""` serves every stem in a slot; `-` asks for flags. */
void test_compfish_query_stem_reaches_fish(void)
{
    FishFix f;
    char *a;

    fish_fix_init(&f);
    stub_file(&f, "tool", "flags", "--verbose\tsay more\n-q\tquiet\n");
    ask(&f, "!tool --ve");
    a = stub_arg(&f, 6U);
    YEW_ASSERT_EQ_STR(a, "tool -");
    yew_xfree(a);
    YEW_ASSERT_NOT_NULL(menu_row(&f.ed, "--verbose"));
    tab(&f.ed);
    assert_prompt(&f.ed, "!tool --verbose ");
    ask(&f, "!tool zz");
    a = stub_arg(&f, 6U);
    YEW_ASSERT_EQ_STR(a, "tool ");
    yew_xfree(a);
    ask(&f, "!tool --out=a");
    a = stub_arg(&f, 6U);
    YEW_ASSERT_EQ_STR(a, "tool --out=");
    yew_xfree(a);
    fish_fix_drop(&f);
}

/* Fresh for 5000 ms: the same slot asks once; an aged answer is served
 * and refreshed. */
void test_compfish_cache_hit_within_five_seconds(void)
{
    FishFix f;

    fish_fix_init(&f);
    stub_file(&f, "tool", "rows", "build\tcompile it\nbench\tmeasure\n");
    ask(&f, "!tool b");
    YEW_ASSERT_EQ_U64(stub_calls(&f), 1U);
    /* Stems that complete nothing, so the caret stays in this slot. */
    ask(&f, "!tool q");
    ask(&f, "!tool zz");
    YEW_ASSERT_EQ_U64(stub_calls(&f), 1U);
    YEW_ASSERT_EQ_U64(yew_compfish_test_spawns(), 1U);
    yew_compfish_test_age(YEW_COMPFISH_FRESH_MS + 1);
    yew_cmdline_close(&f.ed, false);
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f.ed, (const u8 *)"!tool b", 7U);
    /* Served stale at once (no marker), refreshed behind it. */
    YEW_ASSERT_NOT_NULL(menu_row(&f.ed, "build"));
    YEW_ASSERT(!f.ed.cmdline.filter.gen_pending);
    settle(&f.ed);
    YEW_ASSERT_NOT_NULL(menu_row(&f.ed, "build"));
    YEW_ASSERT_EQ_U64(stub_calls(&f), 2U);
    fish_fix_drop(&f);
}

/* A missing fish (spawn/exec failure) disables the oracle once, with one
 * log line; the help rung answers from then on. */
void test_compfish_spawn_failure_disables_once(void)
{
    FishFix f;
    char missing[256];

    fish_fix_init(&f);
    SPEC_FMT(missing, sizeof(missing), "%s/no-such-fish", f.stubdir);
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_FISH", missing, 1), 0);
    yew_compfish_reset();
    yew_test_capture_log();
    ask(&f, "!tool b");
    YEW_ASSERT(yew_compfish_test_disabled());
    YEW_ASSERT(!f.ed.cmdline.filter.gen_pending);
    YEW_ASSERT(!yew_compfish_enabled(&f.ed));
    ask(&f, "!tool c");
    ask(&f, "!other d");
    YEW_ASSERT_EQ_U64(yew_compfish_test_spawns(), 1U);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_INFO,
                                     "fish oracle disabled"));
    fish_fix_drop(&f);
}

/* Non-zero exit with nothing printed is a failure too. */
void test_compfish_failed_exit_disables_once(void)
{
    FishFix f;

    fish_fix_init(&f);
    stub_file(&f, "tool", "fail", "");
    yew_test_capture_log();
    ask(&f, "!tool b");
    YEW_ASSERT(yew_compfish_test_disabled());
    ask(&f, "!tool c");
    YEW_ASSERT_EQ_U64(stub_calls(&f), 1U);
    YEW_ASSERT_EQ_U64(yew_test_log_count(), 1U);
    fish_fix_drop(&f);
}

/* A spec'd command never asks fish: the stub never runs. */
void test_compfish_spec_precedence_never_queries(void)
{
    FishFix f;

    fish_fix_init(&f);
    stub_file(&f, "wolf", "rows", "bogus\tfrom fish\n");
    ask(&f, "!wolf bu");
    ask(&f, "!git sta");
    ask(&f, "!echo x");
    ask(&f, "!cd s");
    YEW_ASSERT_EQ_U64(stub_calls(&f), 0U);
    YEW_ASSERT(access(f.stubdir, F_OK) == 0);
    {
        char path[256];

        SPEC_FMT(path, sizeof(path), "%s/argc", f.stubdir);
        YEW_ASSERT(access(path, F_OK) != 0);
    }
    fish_fix_drop(&f);
}

/* Shape rows win: a path, `$VAR` or `~user` stem never reaches fish. */
void test_compfish_shape_rows_never_query(void)
{
    FishFix f;

    fish_fix_init(&f);
    /* Stems that complete to nothing: a completed word would move the
     * caret into a fresh slot, which fish may answer. */
    ask(&f, "!tool ./zz");
    ask(&f, "!tool $ZZ_NO_SUCH_VAR");
    ask(&f, "!tool ~zz-no-such-user");
    ask(&f, "!tool zz/zz");
    YEW_ASSERT_EQ_U64(stub_calls(&f), 0U);
    fish_fix_drop(&f);
}

/* `off` asks nothing. */
void test_compfish_off_never_queries(void)
{
    FishFix f;

    fish_fix_init(&f);
    set_option(&f.ed, "shell.complete_fish", "off");
    stub_file(&f, "tool", "rows", "build\tcompile it\n");
    ask(&f, "!tool b");
    YEW_ASSERT_EQ_U64(stub_calls(&f), 0U);
    YEW_ASSERT_NULL(menu_row(&f.ed, "build"));
    fish_fix_drop(&f);
}

/* The default is auto; an empty YEW_TEST_FISH is "no fish". */
void test_compfish_default_auto_and_empty_seam(void)
{
    FishFix f;
    OptVal v;

    fish_fix_init(&f);
    YEW_ASSERT(yew_opt_get(&f.ed, NULL, NULL, "shell.complete_fish", 19U,
                           &v));
    YEW_ASSERT_EQ_U64(v.as.str.len, 4U);
    YEW_ASSERT_EQ_MEM(v.as.str.s, "auto", 4U);
    YEW_ASSERT(yew_compfish_enabled(&f.ed));
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_FISH", "", 1), 0);
    yew_compfish_reset();
    YEW_ASSERT_NULL(yew_compfish_path());
    YEW_ASSERT(!yew_compfish_enabled(&f.ed));
    fish_fix_drop(&f);
}

/*
 * Nothing spawns on the keystroke path, Tab included: the typing and the
 * Tab only QUEUE; the idle turn spawns.  While pending the `…` shows and
 * Tab inserts nothing; the arrival refills the menu and never edits the
 * line.  One fish job in flight at a time.
 */
void test_compfish_spawns_only_on_the_idle_turn(void)
{
    FishFix f;

    fish_fix_init(&f);
    stub_file(&f, "tool", "rows", "build\tcompile it\n");
    stub_file(&f, "tool", "hold", "");
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f.ed, (const u8 *)"!tool bu", 8U);
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);
    tab(&f.ed);
    YEW_ASSERT_EQ_U64(f.ed.jobs.len, 0U);
    YEW_ASSERT_EQ_U64(yew_compfish_test_spawns(), 0U);
    YEW_ASSERT(f.ed.cmdline.filter.gen_pending);
    assert_prompt(&f.ed, "!tool bu");
    YEW_ASSERT(yew_cmdline_comp_idle_pending(&f.ed));
    YEW_ASSERT_EQ_I64(yew_loop_deadline(&f.ed, yew_now_ms()), 0);
    YEW_ASSERT_EQ_U64(yew_cmdline_comp_idle(&f.ed), 1U);
    YEW_ASSERT_EQ_U64(yew_compfish_inflight(), 1U);
    /* A second slot's request waits for the first. */
    yew_cmdline_paste(&f.ed, (const u8 *)" x ", 3U);
    YEW_ASSERT_EQ_U64(yew_cmdline_comp_idle(&f.ed), 0U);
    YEW_ASSERT_EQ_U64(yew_compfish_inflight(), 1U);
    YEW_ASSERT(yew_compfish_queued() >= 1U);
    stub_unlink(&f, "tool", "hold");
    settle(&f.ed);
    YEW_ASSERT_EQ_U64(yew_compfish_test_spawns(), 2U);
    yew_cmdline_close(&f.ed, false);
    fish_fix_drop(&f);
}

/* An in-flight fish job holds one of the four completion job slots. */
void test_compfish_counts_toward_the_four_jobs(void)
{
    FishFix f;

    fish_fix_init(&f);
    stub_file(&f, "tool", "hold", "");
    yew_cmdline_open(&f.ed, YEW_PROMPT_CMD, NULL);
    yew_cmdline_paste(&f.ed, (const u8 *)"!tool ", 6U);
    YEW_ASSERT_EQ_U64(yew_cmdline_comp_idle(&f.ed), 1U);
    YEW_ASSERT_EQ_U64(yew_compfish_inflight(), 1U);
    YEW_ASSERT_EQ_U64(yew_compgen_inflight() + yew_comphelp_inflight() +
                          yew_compfish_inflight(),
                      1U);
    YEW_ASSERT(!yew_compfish_idle_ready());
    stub_unlink(&f, "tool", "hold");
    settle(&f.ed);
    yew_cmdline_close(&f.ed, false);
    fish_fix_drop(&f);
}

/* ------------------------------------------------------------------ */
/* Real fish (conditional)                                             */
/* ------------------------------------------------------------------ */

static char *real_fish_path(void)
{
    const char *env = getenv("PATH");
    const char *p = env;

    if (env == NULL)
        return NULL;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t n = colon == NULL ? strlen(p) : (size_t)(colon - p);

        if (n != 0U && p[0] == '/') {
            char path[PATH_MAX];

            if ((size_t)snprintf(path, sizeof(path), "%.*s/fish", (int)n,
                                 p) < sizeof(path) &&
                access(path, X_OK) == 0)
                return yew_xstrdup(path);
        }
        if (colon == NULL)
            return NULL;
        p = colon + 1;
    }
}

/* Run `fish -N --private -c script -- a1 a2` in `cwd`; stdout. */
static char *run_fish(const char *fish, const char *cwd, const char *script,
                      const char *a1, const char *a2)
{
    int fds[2];
    pid_t pid;
    Bytebuf b;
    char chunk[4096];
    char *out;
    int status = 0;

    YEW_ASSERT_EQ_I64(pipe(fds), 0);
    pid = fork();
    YEW_ASSERT(pid >= 0);
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);

        (void)dup2(fds[1], 1);
        if (devnull >= 0) {
            (void)dup2(devnull, 0);
            (void)dup2(devnull, 2);
        }
        (void)close(fds[0]);
        (void)close(fds[1]);
        if (chdir(cwd) != 0)
            _exit(126);
        execl(fish, fish, "-N", "--private", "-c", script, "--", a1, a2,
              (char *)NULL);
        _exit(127);
    }
    (void)close(fds[1]);
    bytebuf_init(&b);
    for (;;) {
        ssize_t n = read(fds[0], chunk, sizeof(chunk));

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        bytebuf_append(&b, chunk, (size_t)n);
    }
    (void)close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    out = yew_xmalloc(b.len + 1U);
    if (b.len != 0U)
        (void)memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    bytebuf_free(&b);
    return out;
}

#define FACT(cond, what)                                                     \
    do {                                                                     \
        yew_test_count_assertion();                                          \
        if (!(cond))                                                         \
            yew_test_fail(__FILE__, __LINE__,                                \
                          "fish changed a measured fact: " what);            \
    } while (0)

/*
 * DoD 3: with a fixture XDG_CONFIG_HOME holding completions/demo.fish,
 * whose rules call a helper in functions/, `demo <Tab>` returns the
 * fixture's rows (both prepends work) and `nosuchtool ` is gated closed.
 * First, §2's measured facts, each named if a later fish breaks it.
 */
void test_compfish_real_fish_demo_and_gate(void)
{
    static const char no_prepend[] =
        "set -l r (complete -C -- $argv[1]); printf '%s\\n' $r";
    static const char rules_count[] =
        "set -p fish_complete_path $__fish_config_dir/completions; "
        "set -p fish_function_path $__fish_config_dir/functions; "
        "complete -C -- $argv[1] >/dev/null; complete -c $argv[2] | count";
    char *fish = real_fish_path();
    FishFix f;
    char dir[256];
    char path[256];
    char *out;

    if (fish == NULL)
        yew_test_skip("real-fish: fish not on PATH");
    fish_fix_init(&f);
    YEW_ASSERT_EQ_I64(setenv("YEW_TEST_FISH", fish, 1), 0);
    yew_compfish_reset();
    SPEC_FMT(dir, sizeof(dir), "%s/fish", f.spec.config);
    YEW_ASSERT_EQ_I64(mkdir(dir, 0700), 0);
    SPEC_FMT(dir, sizeof(dir), "%s/fish/completions", f.spec.config);
    YEW_ASSERT_EQ_I64(mkdir(dir, 0700), 0);
    SPEC_FMT(dir, sizeof(dir), "%s/fish/functions", f.spec.config);
    YEW_ASSERT_EQ_I64(mkdir(dir, 0700), 0);
    SPEC_FMT(path, sizeof(path), "%s/fish/completions/demo.fish",
             f.spec.config);
    fix_write(path,
              "complete -c demo -f -a '(__demo_items)'\n"
              "complete -c demo -l verbose -d 'Say more'\n",
              0644);
    SPEC_FMT(path, sizeof(path), "%s/fish/functions/__demo_items.fish",
             f.spec.config);
    fix_write(path,
              "function __demo_items\n"
              "    printf '%s\\t%s\\n' alpha 'first item' beta "
              "'second item'\n"
              "end\n",
              0644);
    SPEC_FMT(path, sizeof(path), "%s/a b.txt", f.spec.root);
    fix_write(path, "x", 0644);
    SPEC_FMT(path, sizeof(path), "%s/d$x", f.spec.root);
    fix_write(path, "x", 0644);
    SPEC_FMT(path, sizeof(path), "%s/somedir", f.spec.root);
    YEW_ASSERT_EQ_I64(mkdir(path, 0700), 0);

    /* Fact 0 (measured while writing this test, fish 4.8.1): fish
     * autoloads a completion file only for a command that resolves, so
     * the fixture's `demo` must be on PATH before its rules answer. */
    out = run_fish(fish, f.spec.root, YEW_COMPFISH_QUERY, "demo ", "demo");
    FACT(out[0] == '\0',
         "a completion file now loads for a command not on PATH");
    yew_xfree(out);
    SPEC_FMT(path, sizeof(path), "%s/demo", f.bin);
    fix_write(path, "#!/bin/sh\nexit 0\n", 0755);
    /* Fact 1: -N does not load the user's completions. */
    out = run_fish(fish, f.spec.root, no_prepend, "demo ", "demo");
    FACT(strstr(out, "alpha") == NULL,
         "fish -N now loads ~/.config/fish/completions");
    yew_xfree(out);
    /* Fact 2: with the prepends, the user's rules (and their helper)
     * answer, descriptions after a TAB. */
    out = run_fish(fish, f.spec.root, YEW_COMPFISH_QUERY, "demo ", "demo");
    FACT(strstr(out, "alpha\tfirst item\n") != NULL &&
             strstr(out, "beta\tsecond item\n") != NULL,
         "the prepended completion/function dirs no longer answer");
    yew_xfree(out);
    /* Fact 4: for an unknown command, complete -C lists the cwd's files,
     * RAW (`a b.txt`, `d$x`), directories ending in `/`. */
    out = run_fish(fish, f.spec.root, no_prepend, "nosuchtool ",
                   "nosuchtool");
    FACT(strstr(out, "a b.txt\n") != NULL && strstr(out, "d$x\n") != NULL,
         "unknown commands no longer list the cwd's files raw");
    FACT(strstr(out, "somedir/\n") != NULL,
         "directories no longer end in /");
    yew_xfree(out);
    /* Fact 6: the gate distinguishes known from unknown. */
    out = run_fish(fish, f.spec.root, rules_count, "demo ", "demo");
    FACT(atoi(out) > 0, "complete -c demo lists no rules after autoload");
    yew_xfree(out);
    out = run_fish(fish, f.spec.root, rules_count, "nosuchtool ",
                   "nosuchtool");
    FACT(atoi(out) == 0, "complete -c lists rules for an unknown command");
    yew_xfree(out);
    /* ... so the full script prints nothing for it. */
    out = run_fish(fish, f.spec.root, YEW_COMPFISH_QUERY, "nosuchtool ",
                   "nosuchtool");
    FACT(out[0] == '\0', "the gate no longer closes for an unknown command");
    yew_xfree(out);

    /* Through the editor: the fixture's rows, then the gate closed. */
    ask(&f, "!demo ");
    YEW_ASSERT_NOT_NULL(menu_row(&f.ed, "alpha"));
    YEW_ASSERT_EQ_STR(menu_row(&f.ed, "alpha")->detail, "first item");
    YEW_ASSERT_NOT_NULL(menu_row(&f.ed, "beta"));
    ask(&f, "!demo --verb");
    YEW_ASSERT_NOT_NULL(menu_row(&f.ed, "--verbose"));
    YEW_ASSERT_EQ_STR(menu_row(&f.ed, "--verbose")->detail, "Say more");
    {
        YewShCtx ctx;
        YewFishLookup fl;
        Arena a;

        arena_init(&a);
        YEW_ASSERT(yew_shctx_at("nosuchtool ", 11U, 11U, &a, &ctx));
        (void)yew_compfish_lookup(&f.ed, &ctx, &fl);
        settle(&f.ed);
        YEW_ASSERT_EQ_U64(yew_compfish_lookup(&f.ed, &ctx, &fl),
                          YEW_FISH_CLOSED);
        arena_free_all(&a);
    }
    /* The hostile line against the REAL fish: nothing runs. */
    ask(&f, "!demo \"x'; touch PWNED; '\" '$(touch PWNED2)' `touch PWNED3` ");
    SPEC_FMT(path, sizeof(path), "%s/PWNED", f.spec.root);
    YEW_ASSERT(access(path, F_OK) != 0);
    SPEC_FMT(path, sizeof(path), "%s/PWNED2", f.spec.root);
    YEW_ASSERT(access(path, F_OK) != 0);
    SPEC_FMT(path, sizeof(path), "%s/PWNED3", f.spec.root);
    YEW_ASSERT(access(path, F_OK) != 0);
    yew_xfree(fish);
    fish_fix_drop(&f);
}
