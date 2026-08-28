#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/bind.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/theme_cmds.h"
#include "fl/origin.h"
#include "search/regex.h"
#include "unit/syn_toy.h"
#include "term/grid.h"
#include "term/render.h"
#include "text/file.h"
#include "text/piece.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"

enum {
    ALLOC_WARMUP_FRAMES = 100,
    ALLOC_SESSION_FRAMES = 10000,
    ALLOC_INSERTS = 1000,
    ALLOC_INSERT_LIMIT = 40,
    ALLOC_OPEN_LIMIT = 2000,
    ALLOC_CLOSED_LIMIT = 64 * 1024,
    ALLOC_FILE_BYTES = 100 * 1024 * 1024
};

static void print_report(void)
{
    Bytebuf report;

    bytebuf_init(&report);
    yew_alloc_report(&report);
    if (report.len != 0U)
        (void)fwrite(report.data, 1U, report.len, stderr);
    bytebuf_free(&report);
}

static bool gate_calls(const char *name, u64 limit)
{
    u64 calls = yew_alloc_calls();

    (void)printf("alloc %-24s calls=%llu limit=%llu %s\n", name,
                 (unsigned long long)calls, (unsigned long long)limit,
                 calls <= limit ? "ok" : "FAIL");
    if (calls > limit)
        print_report();
    return calls <= limit;
}

static bool gate_live(const char *name, u64 limit)
{
    u64 live = yew_alloc_live_bytes();

    (void)printf("alloc %-24s live=%llu limit=%llu %s\n", name,
                 (unsigned long long)live, (unsigned long long)limit,
                 live <= limit ? "ok" : "FAIL");
    if (live > limit)
        print_report();
    return live <= limit;
}

static Cell ascii_cell(const Grid *grid, u8 byte)
{
    Cell cell = grid->blank;

    cell.utf8[0] = byte;
    return cell;
}

static bool check_render(void)
{
    Arena arena;
    Interner interner;
    Grid grid;
    Render render;
    TtyCaps caps;
    Bytebuf output;
    size_t frame;
    bool ok;

    arena_init(&arena);
    interner_init(&interner, &arena);
    if (!yew_grid_init(&grid, &interner, 24U, 80U)) {
        interner_free(&interner);
        arena_free_all(&arena);
        return false;
    }
    (void)memset(&caps, 0, sizeof(caps));
    yew_render_init(&render, &caps, NULL);
    bytebuf_init(&output);
    yew_grid_fill(&grid, 0U, 0U, grid.cols, ascii_cell(&grid, (u8)'a'));
    yew_grid_mark_all(&grid);
    (void)yew_render_frame(&render, &grid, &output);
    yew_grid_flip(&grid);

    yew_alloc_reset();
    for (frame = 0U; frame < 1000U; frame++) {
        u16 row = (u16)(frame % grid.rows);
        u8 byte = (frame & 1U) != 0U ? (u8)'a' : (u8)'b';

        yew_grid_fill(&grid, row, 0U, grid.cols, ascii_cell(&grid, byte));
        output.len = 0U;
        (void)yew_render_frame(&render, &grid, &output);
        yew_grid_flip(&grid);
    }
    ok = gate_calls("render-frame", 0U);
    bytebuf_free(&output);
    yew_grid_free(&grid);
    interner_free(&interner);
    arena_free_all(&arena);
    return ok;
}

static bool check_syntax(void)
{
    static const char line[] =
        "if (true) return 42; /* allocation-free syntax line */";
    SynToy toy;
    SynSpan spans[128];
    SynLineOut out;
    u32 entry;
    size_t i;
    bool ok;

    syn_toy_init(&toy);
    entry = syn_toy_state(&toy, SYN_TOY_MAIN);
    (void)syn_toy_line(&toy, entry, line, spans, YEW_ARRAY_LEN(spans), &out);
    yew_alloc_reset();
    for (i = 0U; i < 1000U; i++)
        (void)syn_toy_line(&toy, entry, line, spans,
                           YEW_ARRAY_LEN(spans), &out);
    ok = gate_calls("syntax-line", 0U);
    syn_toy_free(&toy);
    return ok;
}

static bool check_regex(void)
{
    static const u8 input_bytes[] = "alpha beta_42 gamma";
    static const char pattern[] = "[[:alpha:]_][[:alnum:]_]*";
    Arena arena;
    YewReErr error = {0U, NULL};
    YewReWorkspace workspace;
    YewReInput input;
    YewReMatch match;
    YewRe *regex;
    size_t i;
    bool ok;

    arena_init(&arena);
    regex = yew_re_compile(&arena, pattern, sizeof(pattern) - 1U,
                           YEW_RE_NOCAPTURE, &error);
    if (regex == NULL) {
        (void)fprintf(stderr, "perf_alloc: regex compile: %s\n",
                      error.msg == NULL ? "unknown error" : error.msg);
        arena_free_all(&arena);
        return false;
    }
    yew_re_workspace_init(&workspace);
    input = yew_re_input_bytes(input_bytes, sizeof(input_bytes) - 1U);
    (void)yew_re_match_at_ws(&workspace, regex, &input, BYTEOFF(0U), &match);
    yew_alloc_reset();
    for (i = 0U; i < 1000U; i++)
        (void)yew_re_match_at_ws(&workspace, regex, &input, BYTEOFF(0U),
                                 &match);
    ok = gate_calls("regex-exec", 0U);
    yew_re_workspace_free(&workspace);
    arena_free_all(&arena);
    return ok;
}

static bool check_inserts(void)
{
    TextBuf *text = yew_textbuf_new();
    static const u8 byte = (u8)'x';
    size_t i;
    bool ok;

    yew_alloc_reset();
    for (i = 0U; i < ALLOC_INSERTS; i++)
        yew_textbuf_insert(text, BYTEOFF(yew_textbuf_len(text)), &byte, 1U);
    ok = gate_calls("1000-byte-inserts", ALLOC_INSERT_LIMIT);
    yew_textbuf_free(text);
    return ok;
}

static bool make_sparse_file(char *path, size_t cap)
{
    const char *tmp = getenv("TMPDIR");
    int fd;
    int n;

    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    n = snprintf(path, cap, "%s/yew-perf-alloc-XXXXXX", tmp);
    if (n < 0 || (size_t)n >= cap) {
        (void)fprintf(stderr, "perf_alloc: temporary path too long\n");
        return false;
    }
    fd = mkstemp(path);
    if (fd < 0) {
        (void)fprintf(stderr, "perf_alloc: mkstemp: %s\n", strerror(errno));
        return false;
    }
    if (ftruncate(fd, ALLOC_FILE_BYTES) != 0) {
        (void)fprintf(stderr, "perf_alloc: ftruncate: %s\n", strerror(errno));
        (void)close(fd);
        (void)unlink(path);
        return false;
    }
    if (close(fd) != 0) {
        (void)fprintf(stderr, "perf_alloc: close: %s\n", strerror(errno));
        (void)unlink(path);
        return false;
    }
    return true;
}

static bool check_open(void)
{
    char path[PATH_MAX];
    FileMeta meta;
    TextBuf *text = NULL;
    YewLoadErr error;
    bool calls_ok;
    bool closed_ok;

    if (!make_sparse_file(path, sizeof(path)))
        return false;
    yew_filemeta_init(&meta);
    yew_alloc_reset();
    error = yew_file_load(path, &text, &meta);
    calls_ok = error == YEW_LOAD_OK && gate_calls("open-100mb", ALLOC_OPEN_LIMIT);
    if (error != YEW_LOAD_OK)
        (void)fprintf(stderr, "perf_alloc: 100 MB load failed (%d)\n",
                      (int)error);
    yew_textbuf_free(text);
    yew_filemeta_dispose(&meta);
    closed_ok = gate_live("open-100mb-closed", ALLOC_CLOSED_LIMIT);
    if (unlink(path) != 0) {
        (void)fprintf(stderr, "perf_alloc: unlink: %s\n", strerror(errno));
        return false;
    }
    return calls_ok && closed_ok;
}

static bool key_from_token(const char *token, Key *key)
{
    static const struct {
        const char *name;
        u32 code;
    } named[] = {
        {"esc", YEW_KEY_ESCAPE}, {"down", YEW_KEY_DOWN},
        {"up", YEW_KEY_UP}, {"right", YEW_KEY_RIGHT},
        {"left", YEW_KEY_LEFT}, {"home", YEW_KEY_HOME},
        {"end", YEW_KEY_END}, {"pagedown", YEW_KEY_PAGE_DOWN},
        {"pageup", YEW_KEY_PAGE_UP}
    };
    size_t i;

    (void)memset(key, 0, sizeof(*key));
    key->kind = YEW_EV_KEY;
    key->ev = YEW_KEY_PRESS;
    if (strcmp(token, "space") == 0) {
        key->code = (u32)' ';
        key->text[0] = (u8)' ';
        key->ntext = 1U;
        return true;
    }
    if (token[0] != '\0' && token[1] == '\0') {
        key->code = (u8)token[0];
        key->text[0] = (u8)token[0];
        key->ntext = 1U;
        return true;
    }
    for (i = 0U; i < YEW_ARRAY_LEN(named); i++) {
        if (strcmp(token, named[i].name) == 0) {
            key->code = named[i].code;
            return true;
        }
    }
    return false;
}

static bool add_session_binding(Ed *ed, Mode mode, const char *sequence,
                                const char *command, const char *sarg)
{
    CmdId id = yew_cmd_lookup(command, (u32)strlen(command));

    if (id.v == 0U ||
        yew_bind_add(ed, FL_ORIGIN_ID_CONFIG, mode, sequence, id, 0,
                     sarg, FL_NIL_V) == 0U) {
        (void)fprintf(stderr, "perf_alloc: bind %s: %s\n", sequence,
                      id.v == 0U ? "unknown command" : yew_bind_error(ed));
        return false;
    }
    return true;
}

static bool install_session_bindings(Ed *ed)
{
    static const struct {
        Mode mode;
        const char *sequence;
        const char *command;
        const char *sarg;
    } bindings[] = {
        {YEW_MODE_L, "i", "ed.mode.enter", "I"},
        {YEW_MODE_L, "<left>", "ed.move.unit.home", NULL},
        {YEW_MODE_L, "<right>", "ed.move.unit.end", NULL},
        {YEW_MODE_L, "<up>", "ed.move.unit.prev", NULL},
        {YEW_MODE_L, "<down>", "ed.move.unit.next", NULL},
        {YEW_MODE_L, "<home>", "ed.move.unit.home", NULL},
        {YEW_MODE_L, "<end>", "ed.move.unit.end", NULL},
        {YEW_MODE_L, "<pgup>", "ed.view.page_up", NULL},
        {YEW_MODE_L, "<pgdn>", "ed.view.page_down", NULL},
        {YEW_MODE_I, "<esc>", "ed.shadow.dismiss", NULL},
        {YEW_MODE_I, "<left>", "ed.move.unit.prev", NULL},
        {YEW_MODE_I, "<right>", "ed.move.unit.next", NULL},
        {YEW_MODE_I, "<up>", "ed.move.line.up", NULL},
        {YEW_MODE_I, "<down>", "ed.move.line.down", NULL},
        {YEW_MODE_I, "<home>", "ed.move.line.home", NULL},
        {YEW_MODE_I, "<end>", "ed.move.line.end", NULL}
    };
    size_t i;
    bool ok = true;

    yew_bind_batch_begin(ed);
    for (i = 0U; i < YEW_ARRAY_LEN(bindings); i++)
        ok = add_session_binding(ed, bindings[i].mode,
                                 bindings[i].sequence, bindings[i].command,
                                 bindings[i].sarg) && ok;
    yew_bind_batch_end(ed);
    return ok;
}

static bool editor_open(Ed *ed, const u8 *bytes, size_t len, int output_fd)
{
    TtyCaps caps;

    yew_ed_init(ed);
    if (!install_session_bindings(ed) ||
        !yew_ed_open_memory(ed, bytes, len, "alloc-session") ||
        !yew_grid_init(&ed->grid, &ed->interner, 24U, 80U)) {
        yew_ed_free(ed);
        return false;
    }
    ed->grid_ready = true;
    (void)memset(&caps, 0, sizeof(caps));
    yew_render_init(&ed->render, &caps, NULL);
    ed->render_ready = true;
    ed->tty.wfd = output_fd;
    yew_theme_sync_surfaces(ed);
    yew_ed_layout(ed);
    yew_ed_render(ed);
    return !ed->quit;
}

static bool check_session(const char *name, const char *path,
                          const u8 *bytes, size_t len, int output_fd)
{
    Ed ed;
    FILE *file;
    char token[64];
    size_t frames = 0U;
    bool ok = true;

    if (!editor_open(&ed, bytes, len, output_fd))
        return false;
    file = fopen(path, "r");
    if (file == NULL) {
        (void)fprintf(stderr, "perf_alloc: cannot open %s: %s\n", path,
                      strerror(errno));
        yew_ed_free(&ed);
        return false;
    }
    while (fgets(token, sizeof(token), file) != NULL) {
        size_t token_len = strlen(token);
        Key key;

        while (token_len != 0U &&
               (token[token_len - 1U] == '\n' ||
                token[token_len - 1U] == '\r'))
            token[--token_len] = '\0';
        if (!key_from_token(token, &key)) {
            (void)fprintf(stderr, "perf_alloc: unknown key token: %s\n",
                          token);
            ok = false;
            break;
        }
        yew_ed_handle_key(&ed, key, (i64)frames + 1);
        yew_ed_render(&ed);
        frames++;
        if (frames == ALLOC_WARMUP_FRAMES)
            yew_alloc_reset();
        if (ed.quit) {
            (void)fprintf(stderr, "perf_alloc: %s quit at frame %zu\n",
                          name, frames);
            ok = false;
            break;
        }
    }
    if (ferror(file)) {
        (void)fprintf(stderr, "perf_alloc: read failed for %s\n", path);
        ok = false;
    }
    if (fclose(file) != 0)
        ok = false;
    if (frames != ALLOC_SESSION_FRAMES) {
        (void)fprintf(stderr, "perf_alloc: %s has %zu frames, expected %d\n",
                      name, frames, ALLOC_SESSION_FRAMES);
        ok = false;
    }
    if (ok)
        ok = gate_calls(name, 0U);
    else
        print_report();
    yew_ed_free(&ed);
    return ok;
}

static void make_navigation_text(Bytebuf *text)
{
    static const char line[] =
        "the quick brown fox jumps over the lazy dog for allocation gates\n";
    size_t i;

    bytebuf_init(text);
    for (i = 0U; i < 1024U; i++)
        bytebuf_append(text, line, sizeof(line) - 1U);
}

static bool check_closed_lifecycle(int output_fd)
{
    static const u8 first[] = "configured editor\n";
    static const u8 second[] = "opened after config\nsecond line\n";
    Ed ed;
    Key key;
    size_t i;
    bool ok;

    if (!editor_open(&ed, first, sizeof(first) - 1U, output_fd))
        return false;
    yew_alloc_reset();
    if (!yew_ed_open_memory(&ed, second, sizeof(second) - 1U,
                            "post-config")) {
        yew_ed_free(&ed);
        return false;
    }
    (void)key_from_token("down", &key);
    for (i = 0U; i < 100U; i++) {
        yew_ed_handle_key(&ed, key, (i64)i + 1);
        yew_ed_render(&ed);
    }
    yew_ed_free(&ed);
    ok = gate_live("closed-minus-config", ALLOC_CLOSED_LIMIT);
    return ok;
}

int main(void)
{
#if !YEW_ALLOC_DEBUG
    (void)fprintf(stderr,
                  "perf_alloc: requires make ALLOCDBG=1 BUILD=build-adbg\n");
    return 2;
#else
    Bytebuf navigation;
    int output_fd;
    bool ok = true;

    output_fd = open("/dev/null", O_WRONLY);
    if (output_fd < 0) {
        (void)fprintf(stderr, "perf_alloc: open /dev/null: %s\n",
                      strerror(errno));
        return 2;
    }
    yew_cmd_init();
    ok = check_render() && ok;
    ok = check_syntax() && ok;
    ok = check_regex() && ok;
    ok = check_inserts() && ok;
    ok = check_open() && ok;
    ok = check_session("typing-frames-100-10000",
                       "tests/perf/sessions/typing.keys",
                       (const u8 *)"", 0U, output_fd) && ok;
    make_navigation_text(&navigation);
    ok = check_session("navigate-after-warmup",
                       "tests/perf/sessions/navigate.keys",
                       navigation.data, navigation.len, output_fd) && ok;
    bytebuf_free(&navigation);
    ok = check_closed_lifecycle(output_fd) && ok;
    yew_cmd_shutdown();
    if (close(output_fd) != 0) {
        (void)fprintf(stderr, "perf_alloc: close /dev/null: %s\n",
                      strerror(errno));
        return 2;
    }
    return ok ? 0 : 1;
#endif
}
