/* Sprint 32 §2-§5: the interactive `sag fl` prompt. */
#include "fl/repl.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/opcodes.h"
#include "fl/parse.h"
#include "fl/suggest.h"
#include "fl/trace.h"
#include "util/intern.h"
#include "fl/std.h"
#include "util/base.h"
#include "term/input.h"
#include "term/tty.h"
#include "text/cursor.h"
#include "text/edit.h"
#include "text/piece.h"
#include "ui/cmdhist.h"
#include "unicode/width.h"

/* Hand-counting a literal's length is a bug waiting for the next edit
 * to the literal; this file counts nothing. */
static void put(Bytebuf *b, const char *text)
{
    bytebuf_append(b, text, strlen(text));
}

/*
 * The helpers below are deliberately NOT named cmd_* : that prefix
 * belongs to Sprint 19's command registry, and scripts/check-cmd-
 * dispatch.sh requires every cmd_ symbol to appear exactly twice -- a
 * definition and one registration.  A prompt command is not a registry
 * command and must not look like one.
 *
 * The eight names, in the order `:help` prints them.  One table, so the
 * help text, the completion list and the did-you-mean candidate set
 * cannot drift apart -- three copies of eight strings is three chances
 * to forget one.
 */
static const struct {
    const char *name;
    const char *arg;
    const char *doc;
} REPL_CMDS[] = {
    {"help",    "[NAME]", "this table, or what NAME is"},
    {"quit",    "",       "leave; :q is the same"},
    {"load",    "PATH",   "evaluate PATH here, keeping its globals"},
    {"reload",  "",       "evaluate the last :load again"},
    {"globals", "",       "the names you have bound, in order"},
    {"disasm",  "NAME",   "the bytecode of a function"},
    {"caps",    "",       "this prompt's origin and its grants"},
    {"q",       "",       "leave"}
};

void sag_fl_print_result(FlVm *vm, FlValue v, Bytebuf *out)
{
    Bytebuf bb;

    if (v.t == (u8)FL_NIL)
        return;
    bytebuf_init(&bb);
    if (!fl_fmt_repl(vm, &bb, v, (u32)FL_REPL_MAX_DEPTH)) {
        /* fl_fmt_repl elides rather than refusing, so the only way here
         * is a limit the renderer itself hit; say so instead of
         * printing half a value. */
        bytebuf_free(&bb);
        put(out, "<unprintable>\n");
        return;
    }
    if (bb.len > (size_t)FL_REPL_MAX_BYTES) {
        bytebuf_append(out, bb.data, (size_t)FL_REPL_MAX_BYTES);
        bytebuf_printf(out, "\xe2\x80\xa6 (%zu more bytes)",
                       bb.len - (size_t)FL_REPL_MAX_BYTES);
    } else {
        bytebuf_append(out, bb.data, bb.len);
    }
    bytebuf_push_u8(out, (u8)'\n');
    bytebuf_free(&bb);
}

/* Counts and discards: the trial parse must be silent. */
static void quiet_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    u32 *n = ctx;

    (void)sp;
    (void)msg;
    (void)rendered;
    if (level == FL_DIAG_ERROR)
        (*n)++;
}

FlReplVerdict sag_fl_repl_classify(Arena *arena, Interner *in,
                                   const char *text, size_t len)
{
    DiagCtx dc;
    FlProgram p;
    u32 nerr = 0U;

    /*
     * A THROWAWAY context, not the caller's with its sink swapped:
     * fl_diag_add_file appends, and a prompt that classified forty
     * lines would fill the caller's file table and trip a SAG_BUG about
     * source files -- a very confusing way to learn that someone typed
     * a lot.
     */
    fl_diag_init(&dc, arena);
    fl_diag_set_sink(&dc, quiet_sink, &nerr);
    (void)fl_diag_add_file(&dc, "<repl>", text, len);
    p = fl_parse(arena, &dc, in, text, len, 0U);
    /* had_error FIRST: an error inside an open bracket sets both, and
     * reading `incomplete` first waits forever for a fixed `)`. */
    if (p.had_error || nerr != 0U)
        return FL_REPL_ERROR;
    if (p.incomplete)
        return FL_REPL_CONTINUE;
    return FL_REPL_RUN;
}


static void repl_help(Bytebuf *out)
{
    size_t i;

    for (i = 0U; i < SAG_ARRAY_LEN(REPL_CMDS); i++) {
        if (strcmp(REPL_CMDS[i].name, "q") == 0)
            continue;                 /* listed as an alias of :quit */
        bytebuf_printf(out, "  :%-8s %-7s %s\n", REPL_CMDS[i].name,
                       REPL_CMDS[i].arg, REPL_CMDS[i].doc);
    }
    put(out,
        "\n"
        "  Enter runs the line; an open bracket or a trailing operator\n"
        "  continues it.  Ctrl-C discards what you are typing, Ctrl-D on\n"
        "  an empty line leaves, Ctrl-L clears the screen.\n"
        "\n"
        "  There is no stepping debugger and no breakpoints in 1.0: the\n"
        "  trace, :disasm and io.print are the debugging story.  The\n"
        "  language is .docs/fletch-spec.md.\n");
}

static void repl_help_name(Bytebuf *out, const char *name)
{
    const char *sig = fl_std_sig(name);

    if (sig != NULL) {
        bytebuf_printf(out, "  %s %s\n", name, sig);
        return;
    }
    bytebuf_printf(out, "  no builtin called '%s'\n", name);
}

/* Every global the user bound, excluding the builtin module maps an
 * `import` put there -- `:globals` answers "what have I got", and the
 * seven modules are not an answer to that. */
static void repl_globals(FlRepl *r, Bytebuf *out)
{
    u32 cursor = 0U;
    FlValue k;
    FlValue v;
    u32 shown = 0U;

    while (fl_map_iter(r->vm->globals, &cursor, &k, &v)) {
        const char *nm;
        u32 bc = 0U;
        FlValue bk;
        FlValue bv;
        bool builtin = false;

        if (k.t != (u8)FL_INT)
            continue;
        while (fl_map_iter(r->vm->builtins, &bc, &bk, &bv)) {
            if (bv.t == v.t && bv.as.o == v.as.o)
                builtin = true;
        }
        if (builtin)
            continue;
        nm = sag_intern_str(r->in, (u32)k.as.i);
        if (nm == NULL)
            continue;
        bytebuf_printf(out, "  %s = ", nm);
        sag_fl_print_result(r->vm, v, out);
        if (v.t == (u8)FL_NIL)
            put(out, "nil\n");
        shown++;
    }
    if (shown == 0U)
        put(out, "  (none yet)\n");
}

static void repl_caps(FlRepl *r, Bytebuf *out)
{
    FlOrigin o = fl_cap_origin(r->vm);
    static const u32 BITS[] = {
        (u32)FL_CAP_FS_READ, (u32)FL_CAP_FS_WRITE,
        (u32)FL_CAP_SHELL, (u32)FL_CAP_NET
    };
    size_t i;

    bytebuf_printf(out, "  origin: %s\n", fl_origin_name(r->vm, &o));
    put(out, "  grants:");
    for (i = 0U; i < SAG_ARRAY_LEN(BITS); i++) {
        if ((o.caps & BITS[i]) != 0U)
            bytebuf_printf(out, " %s", fl_cap_name(BITS[i]));
    }
    if (o.caps == 0U)
        put(out, " (none)");
    bytebuf_push_u8(out, (u8)'\n');
}

static void repl_disasm(FlRepl *r, Bytebuf *out, const char *name)
{
    FlValue got = FL_NIL_V;
    FlValue key = FL_INT_V((i64)sag_intern(r->in, name, strlen(name)));

    if (!fl_map_get(r->vm->globals, key, &got) ||
        got.t != (u8)FL_CLOSURE) {
        bytebuf_printf(out, "  no function called '%s'\n", name);
        return;
    }
    fl_disasm_chunk(out, &((const FlClosure *)got.as.o)->fn->ch, r->in);
}

/* :load and :reload share everything but where the path comes from. */
static void repl_load(FlRepl *r, Bytebuf *out, const char *path)
{
    FILE *fp = fopen(path, "rb");
    Bytebuf src;
    char *copy;
    FlProgram p;
    FlFn *fn;
    FlValue res = FL_NIL_V;
    u32 file_id;

    if (fp == NULL) {
        bytebuf_printf(out, "  cannot read %s\n", path);
        return;
    }
    bytebuf_init(&src);
    for (;;) {
        char buf[65536];
        size_t n = fread(buf, 1U, sizeof(buf), fp);

        if (n != 0U)
            bytebuf_append(&src, buf, n);
        if (n != sizeof(buf))
            break;
    }
    (void)fclose(fp);
    /* Into the arena: the diag context borrows it for caret rendering
     * and outlives this call. */
    copy = arena_alloc(r->arena, src.len + 1U, 1U);
    if (src.len != 0U)
        (void)memcpy(copy, src.data, src.len);
    copy[src.len] = '\0';
    file_id = fl_diag_add_file(r->dc, path, copy, src.len);
    p = fl_parse(r->arena, r->dc, r->in, copy, src.len, file_id);
    bytebuf_free(&src);
    if (p.had_error || p.incomplete) {
        bytebuf_printf(out, "  %s did not parse\n", path);
        return;
    }
    fn = fl_compile(r->vm, r->dc, &p, file_id, fl_cap_origin(r->vm));
    if (fn == NULL) {
        bytebuf_printf(out, "  %s did not compile\n", path);
        return;
    }
    /*
     * FL_FN_SCRIPT, and run against the PROMPT'S OWN globals -- which
     * fl_vm_run does by construction, since it builds the closure with
     * vm->globals.  That is the whole point of :load: you edit a config
     * and re-run it against the session you already have.
     */
    fn->fnkind = (u8)FL_FN_SCRIPT;
    if (!fl_vm_run(r->vm, fn, &res)) {
        fl_trace_render(r->vm, res, out);
        return;
    }
    (void)snprintf(r->last_load, sizeof(r->last_load), "%s", path);
    r->has_load = true;
    bytebuf_printf(out, "  loaded %s\n", path);
}

bool sag_fl_repl_command(FlRepl *r, const char *line, size_t len,
                         Bytebuf *out, bool *quit)
{
    char name[64];
    char arg[1024];
    size_t at = 0U;
    size_t n = 0U;
    size_t i;

    *quit = false;
    while (at < len && (line[at] == ' ' || line[at] == '\t'))
        at++;
    if (at >= len || line[at] != ':')
        return false;
    at++;
    while (at < len && line[at] != ' ' && line[at] != '\t' &&
           line[at] != '\n' && n + 1U < sizeof(name))
        name[n++] = line[at++];
    name[n] = '\0';
    while (at < len && (line[at] == ' ' || line[at] == '\t'))
        at++;
    n = 0U;
    while (at < len && line[at] != '\n' && n + 1U < sizeof(arg))
        arg[n++] = line[at++];
    while (n != 0U && (arg[n - 1U] == ' ' || arg[n - 1U] == '\t'))
        n--;
    arg[n] = '\0';

    if (strcmp(name, "quit") == 0 || strcmp(name, "q") == 0) {
        *quit = true;
        return true;
    }
    if (strcmp(name, "help") == 0) {
        if (arg[0] == '\0')
            repl_help(out);
        else
            repl_help_name(out, arg);
        return true;
    }
    if (strcmp(name, "globals") == 0) {
        repl_globals(r, out);
        return true;
    }
    if (strcmp(name, "caps") == 0) {
        repl_caps(r, out);
        return true;
    }
    if (strcmp(name, "disasm") == 0) {
        if (arg[0] == '\0')
            put(out, "  :disasm needs a name\n");
        else
            repl_disasm(r, out, arg);
        return true;
    }
    if (strcmp(name, "load") == 0) {
        if (arg[0] == '\0')
            put(out, "  :load needs a path\n");
        else
            repl_load(r, out, arg);
        return true;
    }
    if (strcmp(name, "reload") == 0) {
        if (!r->has_load)
            put(out, "  nothing has been :load'ed yet\n");
        else
            repl_load(r, out, r->last_load);
        return true;
    }
    /*
     * Unknown: name it, and suggest over the eight.  §7's machinery
     * with a candidate set of exactly the commands -- which is the
     * fourth site in its table.
     */
    {
        FlSuggest sg;

        bytebuf_printf(out, "  unknown command ':%s'", name);
        fl_suggest_reset(&sg);
        for (i = 0U; i < SAG_ARRAY_LEN(REPL_CMDS); i++)
            fl_suggest_add(&sg, REPL_CMDS[i].name,
                           (u32)strlen(REPL_CMDS[i].name), FL_SCOPE_LOCAL);
        put(out, "; ");
        if (fl_suggest_render(&sg, name, (u32)strlen(name), out) == 0U)
            out->len -= 2U;
        bytebuf_push_u8(out, (u8)'\n');
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* §2: the line editor, over the ONE text editor                    */
/* ---------------------------------------------------------------- */

/*
 * WHAT IS SHARED, AND WHAT IS NOT.
 *
 * §2's law is that there is exactly one text editor in this program,
 * and it holds here at the layer that means it: the prompt's text is a
 * TextBuf, its caret is a Cursor, its edits go through sag_edit_insert
 * and sag_edit_delete, and its motion is s02's grapheme-aware
 * sag_cursor_*.  There is no second buffer type and no char array
 * pretending to be a line.
 *
 * What is NOT shared is COMMAND DISPATCH.  §2 says the only obstacle is
 * that sag_cmdline_key takes an `Ed *`; in fact the prompt inserts text
 * by calling sag_ed_invoke(ed, "ed.edit.insert.text", ...) -- the
 * Sprint 19 registry -- and every editing key is a SAG_MODE_E binding
 * dispatched the same way.  Reuse needs the registry and the registry
 * needs an Ed, which `sag fl` does not have and should not grow one
 * for.  So the key switch below is this prompt's own, and the editing
 * underneath it is the editor's.
 *
 * The EditCtx carries no undo tree and no journal.  edit.c already
 * treats both as optional, so this is composition rather than a
 * special case: a prompt line has nothing to journal and nothing to
 * undo past its own lifetime.
 */
typedef struct FlLine {
    TextBuf *tb;
    Cursor cur;
} FlLine;

static EditCtx line_ctx(FlLine *l)
{
    EditCtx ec;

    (void)memset(&ec, 0, sizeof(ec));
    ec.tb = l->tb;
    return ec;
}

static void line_open(FlLine *l)
{
    l->tb = sag_textbuf_new();
    (void)memset(&l->cur, 0, sizeof(l->cur));
}

static void line_close(FlLine *l)
{
    sag_textbuf_free(l->tb);
    l->tb = NULL;
}

/* The line's bytes, NUL-terminated, caller frees. */
static char *line_text(const FlLine *l)
{
    TextIter it;
    u64 total = sag_textbuf_len(l->tb);
    u64 copied = 0U;
    char *text = sag_xmalloc((size_t)total + 1U);

    if (total != 0U && sag_textiter_begin(&it, l->tb, BYTEOFF(0U))) {
        while (copied < total) {
            const u8 *bytes = NULL;
            size_t n = 0U;

            if (!sag_textiter_chunk(&it, l->tb, &bytes, &n) || n == 0U)
                break;
            if ((u64)n > total - copied)
                n = (size_t)(total - copied);
            (void)memcpy(text + copied, bytes, n);
            copied += (u64)n;
            if (!sag_textiter_advance(&it, l->tb))
                break;
        }
    }
    text[copied] = '\0';
    return text;
}

static void line_insert(FlLine *l, const u8 *bytes, size_t n)
{
    EditCtx ec = line_ctx(l);

    if (n == 0U || !sag_edit_insert(&ec, l->cur.pos, bytes, (u64)n))
        return;
    l->cur.pos = BYTEOFF(l->cur.pos.v + (u64)n);
    l->cur.anchor = l->cur.pos;
}

static void line_delete(FlLine *l, u64 lo, u64 hi)
{
    EditCtx ec = line_ctx(l);

    if (hi <= lo || !sag_edit_delete(&ec, (Span){lo, hi}))
        return;
    l->cur.pos = BYTEOFF(lo);
    l->cur.anchor = l->cur.pos;
}

/* Backspace removes a whole CLUSTER, which is what the user sees --
 * s02's motion decides where that starts, so an emoji goes in one. */
static void line_backspace(FlLine *l)
{
    Cursor probe = l->cur;

    if (l->cur.pos.v == 0U)
        return;
    sag_cursor_left(l->tb, &probe);
    line_delete(l, probe.pos.v, l->cur.pos.v);
}

static void line_delete_fwd(FlLine *l)
{
    Cursor probe = l->cur;

    if (l->cur.pos.v >= sag_textbuf_len(l->tb))
        return;
    sag_cursor_right(l->tb, &probe);
    line_delete(l, l->cur.pos.v, probe.pos.v);
}

static void line_set(FlLine *l, const char *text)
{
    line_delete(l, 0U, sag_textbuf_len(l->tb));
    line_insert(l, (const u8 *)text, strlen(text));
}

/* Word motion, for Ctrl-Left/Right and Ctrl-W. */
static u64 word_left_of(FlLine *l, u64 from)
{
    char *text = line_text(l);
    u64 at = from;

    while (at > 0U && (text[at - 1U] == ' ' || text[at - 1U] == '\t'))
        at--;
    while (at > 0U && text[at - 1U] != ' ' && text[at - 1U] != '\t')
        at--;
    free(text);
    return at;
}

/* ---------------------------------------------------------------- */
/* §2: the loop                                                     */
/* ---------------------------------------------------------------- */

/*
 * ONE LINE, REDRAWN IN PLACE.  No alternate screen and no grid: the
 * prompt shares the terminal with whatever came before it, so a
 * session scrolls like any other program's output and a user can
 * select and copy it.  Invariant 6 still applies -- the guard restores
 * the terminal on every exit path, including sag_bug's.
 */
/*
 * Raw mode turns OFF ONLCR, so a bare "\n" drops a row without
 * returning to column 0 and every line after the first is indented by
 * the length of the one above it.  Everything the prompt prints goes
 * through here.
 */
static void write_out(const Bytebuf *out)
{
    size_t at = 0U;

    while (at < out->len) {
        size_t run = at;

        while (run < out->len && out->data[run] != (u8)'\n')
            run++;
        if (run > at)
            (void)fwrite(out->data + at, 1U, run - at, stdout);
        if (run < out->len)
            (void)fputs("\r\n", stdout);
        at = run + 1U;
    }
    (void)fflush(stdout);
}

static void redraw(const FlLine *l, const char *prompt)
{
    char *text = line_text(l);
    int cells;

    /* CR, the prompt, the line, then clear to the end: the erase has to
     * come AFTER the text or a line that just got shorter keeps its
     * tail. */
    (void)fprintf(stdout, "\r%s%s\x1b[K", prompt, text);
    /* Put the caret where the cursor is, measured in CELLS -- a CJK
     * name is two columns per cluster and a byte count would land the
     * caret inside it. */
    cells = sag_str_width((const u8 *)text, (size_t)l->cur.pos.v, 1U);
    (void)fprintf(stdout, "\r\x1b[%dC",
                  (int)strlen(prompt) + (cells < 0 ? 0 : cells));
    (void)fflush(stdout);
    free(text);
}

/*
 * The prompt's diagnostics go into the SAME Bytebuf as its results,
 * not to stderr.  A prompt redraws the line it is sitting on, so a
 * complaint that arrived on a different stream would land in the
 * middle of the caret run and leave the terminal looking corrupt.
 */
static void repl_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                      const char *msg, const char *rendered)
{
    Bytebuf *out = ctx;

    (void)level;
    (void)sp;
    if (rendered != NULL)
        put(out, rendered);
    else if (msg != NULL)
        bytebuf_printf(out, "sagitta: %s\n", msg);
}

/*
 * ONE COMPLETED ENTRY.  `pending` holds everything typed since the last
 * prompt, newline-terminated per line; it is CLEARED when the entry is
 * disposed of and LEFT INTACT when more input is wanted, which is the
 * whole of the continuation protocol.
 */
static void handle_entry(FlRepl *r, Bytebuf *pending, Bytebuf *out,
                         bool *quit)
{
    FlProgram p;
    FlFn *fn;
    FlValue result;
    const char *src = (const char *)pending->data;
    size_t len = pending->len;

    if (len == 0U)
        return;
    /* A `:`-command only when nothing is pending before it: inside a
     * continuation the bytes are source, and source may legitimately
     * start a line with `:`. */
    if (sag_fl_repl_command(r, src, len, out, quit)) {
        pending->len = 0U;
        return;
    }
    switch (sag_fl_repl_classify(r->arena, r->in, src, len)) {
    case FL_REPL_CONTINUE:
        return;                       /* keep pending; show `... ` */
    case FL_REPL_ERROR:
        break;                        /* fall through and re-parse loud */
    case FL_REPL_RUN:
    default:
        break;
    }
    /*
     * Re-parsed with the REAL sink now that the entry is known to be
     * disposable -- classify ran silent precisely so this is the only
     * place a diagnostic can come from.
     */
    /* A fresh DiagCtx per entry: the arena outlives the prompt, so
     * reusing one would accumulate every line's diagnostics for the
     * length of the session. */
    fl_diag_init(r->dc, r->arena);
    fl_diag_set_sink(r->dc, repl_sink, out);
    (void)fl_diag_add_file(r->dc, "<repl>", src, len);
    p = fl_parse(r->arena, r->dc, r->in, src, len, 0U);
    if (p.had_error) {
        pending->len = 0U;
        return;
    }
    fn = fl_compile_repl(r->vm, r->dc, &p, 0U, r->vm->root_origin);
    if (fn == NULL) {
        pending->len = 0U;
        return;
    }
    result = FL_NIL_V;
    if (fl_vm_run(r->vm, fn, &result)) {
        sag_fl_print_result(r->vm, result, out);
    } else {
        /* §6: the raise renders with its trace, exactly as a script's
         * uncaught error does -- a prompt that hid the frames would be
         * the one place the trace was most wanted. */
        fl_trace_render(r->vm, r->vm->err, out);
    }
    pending->len = 0U;
}

static int repl_main(bool selftest_bug)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    FlRepl r;
    FlLine line;
    Tty tty;
    TtyGuard guard;
    In input;
    CmdHist *hist;
    HistCur hcur;
    Bytebuf pending;
    Bytebuf out;
    bool quit = false;
    int rc = SAG_EXIT_OK;

    (void)memset(&tty, 0, sizeof(tty));
    if (!sag_tty_open(&tty))
        return SAG_EXIT_IO;
    (void)memset(&guard, 0, sizeof(guard));
    /* The restore path is installed BEFORE the first byte of output:
     * invariant 6 applies to `sag fl` exactly as it does to the
     * editor, and a prompt that died owing a cooked terminal is a
     * shell nobody can type into. */
    if (!sag_tty_guard_start(&guard)) {
        sag_tty_close(&tty);
        return SAG_EXIT_IO;
    }
    /* Raw, but NO alternate screen: a REPL session belongs in the
     * scrollback with the shell history around it. */
    if (!sag_tty_raw(&tty)) {
        (void)sag_tty_guard_finish(&guard);
        sag_tty_close(&tty);
        return SAG_EXIT_IO;
    }
    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_vm_init(&vm, &arena, &in, &dc);
    fl_std_register(&vm);
    (void)memset(&r, 0, sizeof(r));
    r.vm = &vm;
    r.arena = &arena;
    r.in = &in;
    r.dc = &dc;
    /* §13: the REPL's origin is FL_ORIGIN_REPL with all four grants --
     * the user is sitting at it. */
    vm.root_origin.kind = (u8)FL_ORIGIN_REPL;
    vm.root_origin.path_id = 0U;
    vm.root_origin.caps = (u32)FL_CAP_FS_READ | (u32)FL_CAP_FS_WRITE |
                          (u32)FL_CAP_SHELL | (u32)FL_CAP_NET;
    sag_input_init(&input, &tty.caps);
    /* A fourth history file beside the editor's three. */
    hist = sag_hist_open("fl");
    (void)memset(&hcur, 0, sizeof(hcur));
    sag_hist_cur_reset(&hcur, "");
    line_open(&line);
    bytebuf_init(&pending);
    bytebuf_init(&out);

    (void)fprintf(stdout,
                  "sagitta %s -- :help for help, :quit to leave\r\n",
                  SAG_VERSION);
    if (selftest_bug) {
        /*
         * Raw mode is on and sag_bug's prehook is installed; break a
         * chunk and let §9's report prove invariant 6 holds through
         * it.  sag_bug exits, so nothing below this runs.
         *
         * It waits for a keypress first so the pty golden can pin the
         * PROMPT -- which is stable -- while the report, which carries
         * a vm.c line number, is checked as bytes instead of being
         * frozen into a grid that would rot on the next vm.c edit.
         */
        static const char *const src = "return 1\n";
        FlProgram sp;
        FlFn *bad;
        FlValue v = FL_NIL_V;

        u8 discard[64];

        redraw(&line, FL_REPL_PROMPT);
        while (read(tty.rfd, discard, sizeof(discard)) <= 0) {
            struct pollfd wait_fd;

            wait_fd.fd = tty.rfd;
            wait_fd.events = POLLIN;
            wait_fd.revents = 0;
            if (poll(&wait_fd, 1U, -1) < 0 && errno != EINTR)
                goto done;
        }
        (void)fprintf(stdout, "\r\n");
        (void)fflush(stdout);
        (void)fl_diag_add_file(&dc, "<selftest>", src, strlen(src));
        sp = fl_parse(&arena, &dc, &in, src, strlen(src), 0U);
        bad = fl_compile_repl(&vm, &dc, &sp, 0U, vm.root_origin);
        if (bad != NULL) {
            bad->ch.code[0] = 0xFEU;         /* no such opcode */
            (void)fl_vm_run(&vm, bad, &v);
        }
        rc = SAG_EXIT_ERR;
        goto done;
    }
    redraw(&line, FL_REPL_PROMPT);
    for (;;) {
        u8 buf[1024];
        struct pollfd pfd;
        ssize_t got;
        int ready;
        Key key;

        /*
         * POLL, THEN READ.  Raw mode sets VMIN=0, so a bare read()
         * returns 0 the instant the input queue is empty -- reading
         * without waiting first would take that for end-of-input and
         * quit before the user typed a single byte.
         */
        pfd.fd = tty.rfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1U, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;
        got = read(tty.rfd, buf, sizeof(buf));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;                     /* EIO: the terminal hung up */
        }
        if (got == 0)
            break;                     /* readable and empty: hangup */
        sag_input_feed(&input, buf, (size_t)got);
        while (sag_input_next(&input, 0, &key)) {
            bool redraw_now = true;
            bool ctrl = (key.mods & SAG_MOD_CTRL) != 0U;

            if (key.ev == SAG_KEY_RELEASE)
                continue;
            out.len = 0U;
            if (key.code == SAG_KEY_ENTER) {
                char *text = line_text(&line);

                (void)fprintf(stdout, "\r\n");
                if (pending.len == 0U && text[0] != '\0')
                    sag_hist_add(hist, text);
                bytebuf_append(&pending, text, strlen(text));
                bytebuf_push_u8(&pending, (u8)'\n');
                free(text);
                line_set(&line, "");
                handle_entry(&r, &pending, &out, &quit);
                if (out.len != 0U)
                    write_out(&out);
                if (quit)
                    goto done;
                redraw(&line, pending.len == 0U ? FL_REPL_PROMPT
                                                : FL_REPL_CONT);
                continue;
            }
            switch (key.code) {
            case SAG_KEY_BACKSPACE:
                line_backspace(&line);
                break;
            case SAG_KEY_DELETE:
                line_delete_fwd(&line);
                break;
            case SAG_KEY_LEFT:
                if ((key.mods & SAG_MOD_CTRL) != 0U)
                    line.cur.pos = BYTEOFF(word_left_of(&line,
                                                        line.cur.pos.v));
                else
                    sag_cursor_left(line.tb, &line.cur);
                line.cur.anchor = line.cur.pos;
                break;
            case SAG_KEY_RIGHT:
                sag_cursor_right(line.tb, &line.cur);
                line.cur.anchor = line.cur.pos;
                break;
            case SAG_KEY_HOME:
                sag_cursor_line_home(line.tb, &line.cur);
                line.cur.anchor = line.cur.pos;
                break;
            case SAG_KEY_END:
                sag_cursor_line_end(line.tb, &line.cur);
                line.cur.anchor = line.cur.pos;
                break;
            case SAG_KEY_UP:
            case SAG_KEY_DOWN: {
                const char *found = key.code == SAG_KEY_UP
                                        ? sag_hist_prev(hist, &hcur)
                                        : sag_hist_next(hist, &hcur);

                if (found != NULL)
                    line_set(&line, found);
                break;
            }
            default:
                /* s04 decodes control bytes as the LETTER plus a CTRL
                 * modifier -- Ctrl-C is 'c'+CTRL, never the raw 0x03 --
                 * so testing the byte would silently match nothing. */
                if (key.code == (u32)'c' && ctrl) {
                    /*
                     * Ctrl-C discards what is being typed and returns
                     * to a fresh prompt.  It NEVER exits: a REPL that
                     * quit on Ctrl-C loses the session every time
                     * someone reflexively cancels a line.
                     */
                    pending.len = 0U;
                    line_set(&line, "");
                    (void)fprintf(stdout, "\r\n");
                } else if (key.code == (u32)'d' && ctrl) {
                    char *text = line_text(&line);
                    bool empty = text[0] == '\0' && pending.len == 0U;

                    free(text);
                    if (empty) {
                        (void)fprintf(stdout, "\r\n");
                        goto done;
                    }
                    /* With something pending, Ctrl-D discards it like
                     * Ctrl-C rather than leaving. */
                    pending.len = 0U;
                    line_set(&line, "");
                    (void)fprintf(stdout, "\r\n");
                } else if (key.code == (u32)'l' && ctrl) {
                    (void)fprintf(stdout, "\x1b[2J\x1b[H");
                } else if (key.code < SAG_KEY_BASE && key.ntext != 0U &&
                           (key.mods & (SAG_MOD_ALT | SAG_MOD_CTRL |
                                        SAG_MOD_SUPER)) == 0U) {
                    line_insert(&line, key.text, key.ntext);
                } else {
                    redraw_now = false;
                }
                break;
            }
            if (redraw_now)
                redraw(&line, pending.len == 0U ? FL_REPL_PROMPT
                                                : FL_REPL_CONT);
        }
    }
done:
    bytebuf_free(&out);
    bytebuf_free(&pending);
    line_close(&line);
    sag_hist_flush(hist);
    sag_input_free(&input);
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
    (void)sag_tty_guard_finish(&guard);
    sag_tty_close(&tty);
    return rc;
}


/* ---------------------------------------------------------------- */
/* §4: the `:`-commands                                             */
/* ---------------------------------------------------------------- */


int sag_fl_repl(void)
{
    return repl_main(false);
}

int sag_fl_repl_selftest_bug(void)
{
    return repl_main(true);
}
