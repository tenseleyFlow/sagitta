/* Sprint 32 §2-§5: the interactive `sag fl` prompt. */
#include "fl/repl.h"

#include <stdio.h>
#include <string.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/opcodes.h"
#include "fl/parse.h"
#include "fl/suggest.h"
#include "fl/trace.h"
#include "util/intern.h"
#include "fl/std.h"
#include "util/base.h"

/* Hand-counting a literal's length is a bug waiting for the next edit
 * to the literal; this file counts nothing. */
static void put(Bytebuf *b, const char *text)
{
    bytebuf_append(b, text, strlen(text));
}

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

int sag_fl_repl(void)
{
    /*
     * No silent stub: the prompt is Sprint 32 §2-§4 and is not built in
     * this tree yet.  Naming the section rather than returning a usage
     * message keeps `sag fl` on a tty from looking like a typo.
     */
    (void)fprintf(stderr,
                  "sagitta: the interactive REPL lands in Sprint 32 §2;\n"
                  "         `sag fl FILE`, `-e`, and stdin all work today.\n");
    return SAG_EXIT_ERR;
}

/* ---------------------------------------------------------------- */
/* §4: the `:`-commands                                             */
/* ---------------------------------------------------------------- */

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
