/*
 * Sprint 32 §1: `sag fl` -- the Fletch entry point.
 *
 * Handled BEFORE the editor's own argument parser, because `sag fl`'s
 * options are not the editor's and threading them through one parser
 * would make `--list-natives` a top-level flag that means nothing
 * outside this subcommand.
 *
 * THE EXIT CODES ARE A CONTRACT (00-decisions.md), and `sag fl` NEVER
 * returns 2 -- that code belongs to Sprint 37's `--batch`:
 *
 *   0  ran to completion
 *   1  compile diagnostics, or an uncaught runtime error
 *   3  FILE unreadable, or a `capability`/`io` error reaching the top
 *   4  internal VM invariant break, through sag_bug
 *
 * A `capability` or `io` error is separated from every other uncaught
 * raise because those two mean the ENVIRONMENT refused, not that the
 * script was wrong -- a caller in a shell conditional wants to tell a
 * missing file from a syntax error without parsing our stderr.
 *
 * DIAGNOSTICS GO TO STDERR AND io.print GOES TO STDOUT, so a script's
 * data and its complaints never interleave in a pipe.
 *
 * `sag fl FILE` TAKES NO SCRIPT ARGUMENTS this campaign; Sprint 37's
 * `--batch` owns stdio and argv.  Saying so here is cheaper than
 * accepting them and quietly ignoring them.
 *
 * NO TERMINAL OWNERSHIP in any form below: no alternate screen, no raw
 * mode, no Tty.  Only the interactive REPL touches termios, and it
 * installs the s03 restore path before its first byte of output.
 *
 * THE ORIGIN CARRIES THE SCRIPT'S REALPATH.  Without it a relative
 * import would resolve against the XDG directory only, and `sag fl
 * ./x.fl` would fail to find the helper sitting next to it.  The CLI
 * gets all four capabilities: the user typed the command, so §13's
 * table grants what they already have a shell for.
 */
#define _POSIX_C_SOURCE 200809L

#include "flcli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "fl/compile.h"
#include "fl/opcodes.h"
#include "fl/parse.h"
#include "fl/repl.h"
#include "fl/trace.h"
#include "fl/std.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"

static const char fl_usage[] =
    "Usage:\n"
    "  sag fl                  Read a script from stdin (not a tty).\n"
    "  sag fl FILE             Evaluate FILE.\n"
    "  sag fl -e EXPR          Evaluate EXPR; print its value if non-nil.\n"
    "  sag fl -c FILE          Parse and compile only; do not run.\n"
    "  sag fl --dump-ast FILE  Print the AST.\n"
    "  sag fl --dump-bytecode FILE\n"
    "                          Disassemble every function.\n"
    "  sag fl --list-natives   List every builtin, one per line.\n"
    "\n"
    "Exit: 0 ran, 1 compile or runtime error, 3 unreadable file or a\n"
    "denied capability, 4 internal error.\n"
    "\n"
    "A script takes no arguments this campaign; batch mode with stdio\n"
    "and argv is Sprint 37.  A stepping debugger is not a 1.0 feature:\n"
    "the trace, --dump-bytecode and io.print are the debugging story.\n";

static int list_natives(void)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    Bytebuf out;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_vm_init(&vm, &arena, &in, &dc);
    fl_std_register(&vm);
    bytebuf_init(&out);
    /* REGISTRATION order, which is spec §11's listing order.  Sprint
     * 33's coverage ledger diffs against this, so a module added out of
     * order shows up as a ledger diff rather than a silent reshuffle. */
    (void)fl_std_list_natives(&vm, &out);
    (void)fwrite(out.data, 1U, out.len, stdout);
    bytebuf_free(&out);
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
    return SAG_EXIT_OK;
}

/* The whole file, or NULL with a message already printed. */
static char *slurp(const char *path, size_t *len, Arena *arena)
{
    FILE *fp = fopen(path, "rb");
    Bytebuf bb;
    char *copy;

    if (fp == NULL) {
        (void)fprintf(stderr, "sagitta: cannot read %s\n", path);
        return NULL;
    }
    bytebuf_init(&bb);
    for (;;) {
        char buf[65536];
        size_t n = fread(buf, 1U, sizeof(buf), fp);

        if (n != 0U)
            bytebuf_append(&bb, buf, n);
        if (n != sizeof(buf))
            break;
    }
    if (ferror(fp) != 0) {
        (void)fclose(fp);
        bytebuf_free(&bb);
        (void)fprintf(stderr, "sagitta: cannot read %s\n", path);
        return NULL;
    }
    (void)fclose(fp);
    /* Into the arena: DiagCtx keeps the source pointer so it can render
     * a caret line later, and a freed buffer would have the second
     * diagnostic read released memory. */
    copy = arena_alloc(arena, bb.len + 1U, 1U);
    if (bb.len != 0U)
        (void)memcpy(copy, bb.data, bb.len);
    copy[bb.len] = '\0';
    *len = bb.len;
    bytebuf_free(&bb);
    return copy;
}

/* Everything the four file-reading modes share. */
typedef struct FlRun {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    FlOrigin origin;
    u32 nerr;
} FlRun;

static void cli_count(void *ctx, FlDiagLevel level, FlSpan sp,
                      const char *msg, const char *rendered)
{
    FlRun *r = ctx;

    (void)sp;
    if (level == FL_DIAG_ERROR)
        r->nerr++;
    /* Diagnostics on STDERR, so a script's data and its complaints do
     * not interleave in a pipe. */
    if (rendered != NULL)
        (void)fputs(rendered, stderr);
    else
        (void)fprintf(stderr, "sagitta: %s\n", msg == NULL ? "error" : msg);
}

static void run_open(FlRun *r)
{
    (void)memset(r, 0, sizeof(*r));
    arena_init(&r->arena);
    interner_init(&r->in, &r->arena);
    fl_diag_init(&r->dc, &r->arena);
    fl_diag_set_sink(&r->dc, cli_count, r);
    fl_vm_init(&r->vm, &r->arena, &r->in, &r->dc);
    fl_std_register(&r->vm);
    r->origin.kind = (u8)FL_ORIGIN_CLI;
    r->origin.path_id = 0U;
    /* §13: all four.  The user typed the command, so this grants what
     * they already have a shell for. */
    r->origin.caps = (u32)FL_CAP_FS_READ | (u32)FL_CAP_FS_WRITE |
                     (u32)FL_CAP_SHELL | (u32)FL_CAP_NET;
}

static void run_close(FlRun *r)
{
    fl_vm_free(&r->vm);
    interner_free(&r->in);
    arena_free_all(&r->arena);
}

/* The script's realpath, so a relative import resolves beside it. */
static void run_anchor(FlRun *r, const char *path)
{
    char *real = realpath(path, NULL);

    if (real != NULL) {
        r->origin.path_id = sag_intern(&r->in, real, strlen(real));
        free(real);
    }
    r->vm.root_origin = r->origin;
}

/* One field of the {kind, msg} error map. */
static void err_field(FlVm *vm, FlValue err, const char *key, char *out,
                      size_t cap)
{
    FlValue got = FL_NIL_V;
    FlStr *k = fl_str_new(vm, key, (u32)strlen(key));

    out[0] = '\0';
    if (err.t != (u8)FL_MAP)
        return;
    if (!fl_map_get((FlMap *)err.as.o, FL_OBJ_V(FL_STR, k), &got))
        return;
    if (got.t != (u8)FL_STR)
        return;
    (void)snprintf(out, cap, "%.*s", (int)((const FlStr *)got.as.o)->len,
                   ((const FlStr *)got.as.o)->b);
}

/*
 * An uncaught raise, reported and turned into an exit code.
 *
 * `capability` and `io` mean the ENVIRONMENT refused rather than that
 * the script was wrong, so they exit 3: a caller in a shell conditional
 * can tell a missing file from a syntax error without parsing stderr.
 */
static int report_raise(FlVm *vm, FlValue err)
{
    char kind[64];
    Bytebuf bb;

    err_field(vm, err, "kind", kind, sizeof(kind));
    bytebuf_init(&bb);
    /* The §6 block: the error line, the frames, and the caret. */
    fl_trace_render(vm, err, &bb);
    (void)fwrite(bb.data, 1U, bb.len, stderr);
    bytebuf_free(&bb);
    if (strcmp(kind, "capability") == 0 || strcmp(kind, "io") == 0)
        return SAG_EXIT_IO;
    return SAG_EXIT_ERR;
}

/*
 * Parses `src`.  Returns false having reported.
 *
 * `incomplete` IS AN ERROR HERE.  s29 sets it when the input ends with
 * a construct still open, which for the REPL means "keep reading" and
 * for a file means the file is truncated -- `fn f( {` at the end of a
 * script is not a program.  Only the prompt may treat it as an
 * invitation; every entry point in this file must not, or a half-typed
 * script runs its first half and exits 0.
 */
static bool cli_parse(FlRun *r, const char *name, const char *src, size_t len,
                      FlProgram *out)
{
    u32 file_id = fl_diag_add_file(&r->dc, name, src, len);

    *out = fl_parse(&r->arena, &r->dc, &r->in, src, len, file_id);
    if (out->had_error)
        return false;
    if (out->incomplete) {
        (void)fprintf(stderr,
                      "%s: error: unexpected end of input; a bracket or "
                      "block is still open\n", name);
        r->nerr++;
        return false;
    }
    return true;
}

static FlFn *cli_compile(FlRun *r, const char *name, const char *src,
                         size_t len)
{
    FlProgram p;
    FlFn *fn;

    if (!cli_parse(r, name, src, len, &p))
        return NULL;
    fn = fl_compile(&r->vm, &r->dc, &p, 0U, r->origin);
    if (fn != NULL)
        fn->fnkind = (u8)FL_FN_SCRIPT;
    return fn;
}

/* ---------------------------------------------------------------- */
/* The modes                                                        */
/* ---------------------------------------------------------------- */

static int mode_eval(FlRun *r, const char *name, const char *src, size_t len,
                     bool print_value)
{
    FlFn *fn = cli_compile(r, name, src, len);
    FlValue out = FL_NIL_V;

    if (fn == NULL)
        return SAG_EXIT_ERR;
    if (!fl_vm_run(&r->vm, fn, &out))
        return report_raise(&r->vm, out);
    /* §5: nil prints NOTHING -- otherwise every let, every io.print and
     * every void call spams a line. */
    if (print_value) {
        Bytebuf bb;

        bytebuf_init(&bb);
        sag_fl_print_result(&r->vm, out, &bb);
        (void)fwrite(bb.data, 1U, bb.len, stdout);
        bytebuf_free(&bb);
    }
    return SAG_EXIT_OK;
}

static int mode_compile_only(FlRun *r, const char *name, const char *src,
                             size_t len)
{
    return cli_compile(r, name, src, len) == NULL ? SAG_EXIT_ERR
                                                  : SAG_EXIT_OK;
}

static int mode_dump_ast(FlRun *r, const char *name, const char *src,
                         size_t len)
{
    FlProgram p;
    Bytebuf bb;

    if (!cli_parse(r, name, src, len, &p))
        return SAG_EXIT_ERR;
    bytebuf_init(&bb);
    fl_ast_dump(&bb, &p, &r->in);
    (void)fwrite(bb.data, 1U, bb.len, stdout);
    bytebuf_free(&bb);
    return SAG_EXIT_OK;
}

/*
 * EVERY function, not just the top-level chunk: a nested `fn` lives in
 * its parent's constant pool, and a disassembly that stopped at the top
 * would omit the code most worth reading.
 */
static void disasm_fn(Bytebuf *bb, const FlFn *fn, const Interner *in,
                      u32 depth)
{
    const char *nm = fn->name_id == 0U ? NULL
                                       : sag_intern_str(in, fn->name_id);
    u32 i;

    if (depth > 32U)
        return;                    /* the compiler cannot nest this far */
    bytebuf_printf(bb, "-- %s/%u\n", nm == NULL ? "<fn>" : nm,
                   (unsigned)fn->arity);
    fl_disasm_chunk(bb, &fn->ch, in);
    bytebuf_push_u8(bb, (u8)'\n');
    for (i = 0U; i < fn->ch.nconsts; i++) {
        if (fn->ch.consts[i].t == (u8)FL_FN)
            disasm_fn(bb, (const FlFn *)fn->ch.consts[i].as.o, in, depth + 1U);
    }
}

static int mode_dump_bytecode(FlRun *r, const char *name, const char *src,
                              size_t len)
{
    FlFn *fn = cli_compile(r, name, src, len);
    Bytebuf bb;

    if (fn == NULL)
        return SAG_EXIT_ERR;
    bytebuf_init(&bb);
    disasm_fn(&bb, fn, &r->in, 0U);
    (void)fwrite(bb.data, 1U, bb.len, stdout);
    bytebuf_free(&bb);
    return SAG_EXIT_OK;
}

/* ---------------------------------------------------------------- */

typedef int (*FlMode)(FlRun *r, const char *name, const char *src, size_t len);

/* Loads `path` and hands it to `mode`.  An unreadable file is exit 3. */
static int with_file(const char *path, FlMode mode, bool anchor)
{
    FlRun r;
    char *src;
    size_t len = 0U;
    int rc;

    run_open(&r);
    src = slurp(path, &len, &r.arena);
    if (src == NULL) {
        run_close(&r);
        return SAG_EXIT_IO;
    }
    if (anchor)
        run_anchor(&r, path);
    rc = mode(&r, path, src, len);
    run_close(&r);
    return rc;
}

static int mode_eval_file(FlRun *r, const char *n, const char *s, size_t l)
{
    /* A FILE's value is not printed: a script communicates through
     * io.print, and echoing its last expression would surprise anyone
     * piping it. */
    return mode_eval(r, n, s, l, false);
}

static int mode_ast(FlRun *r, const char *n, const char *s, size_t l)
{
    return mode_dump_ast(r, n, s, l);
}

static int mode_bc(FlRun *r, const char *n, const char *s, size_t l)
{
    return mode_dump_bytecode(r, n, s, l);
}

static int mode_conly(FlRun *r, const char *n, const char *s, size_t l)
{
    return mode_compile_only(r, n, s, l);
}

/* `sag fl` with stdin not a tty: the whole of stdin as one script.
 * No raw mode, no prompt, no history -- a REPL that emitted escape
 * sequences into a pipe is how CI logs become unreadable. */
static int run_stdin(void)
{
    FlRun r;
    Bytebuf bb;
    char *src;
    int rc;

    run_open(&r);
    bytebuf_init(&bb);
    for (;;) {
        char buf[65536];
        size_t n = fread(buf, 1U, sizeof(buf), stdin);

        if (n != 0U)
            bytebuf_append(&bb, buf, n);
        if (n != sizeof(buf))
            break;
    }
    if (ferror(stdin) != 0) {
        bytebuf_free(&bb);
        run_close(&r);
        (void)fprintf(stderr, "sagitta: cannot read stdin\n");
        return SAG_EXIT_IO;
    }
    src = arena_alloc(&r.arena, bb.len + 1U, 1U);
    if (bb.len != 0U)
        (void)memcpy(src, bb.data, bb.len);
    src[bb.len] = '\0';
    rc = mode_eval(&r, "<stdin>", src, bb.len, false);
    bytebuf_free(&bb);
    run_close(&r);
    return rc;
}

static int run_expr(const char *expr)
{
    FlRun r;
    int rc;

    run_open(&r);
    /* -e PRINTS its value: the point of it is to ask a question from a
     * shell, and a question with no answer is not one. */
    rc = mode_eval(&r, "<expr>", expr, strlen(expr), true);
    run_close(&r);
    return rc;
}

/*
 * Sprint 32 §9's exit-4 row, driven for real rather than described.
 *
 * A chunk is corrupted with an opcode the table does not have and then
 * run, so the report comes out of the VM's own invariant path instead
 * of a mock -- the thing being tested is that a broken VM produces a
 * structured report and exit 4, and a mock would test the mock.
 *
 * Hidden, and named for what it is: nothing but the smoke script and a
 * developer chasing the report format should ever call it.
 */
static int selftest_bug(void)
{
    FlRun r;
    FlFn *fn;
    FlValue out = FL_NIL_V;
    static const char *const src = "return 1\n";

    run_open(&r);
    fn = cli_compile(&r, "<selftest>", src, strlen(src));
    if (fn == NULL) {
        run_close(&r);
        return SAG_EXIT_ERR;
    }
    fn->ch.code[0] = 0xFEU;          /* no such opcode */
    (void)fl_vm_run(&r.vm, fn, &out);
    /* Unreachable: the VM exits 4 through sag_bug. */
    run_close(&r);
    return SAG_EXIT_ERR;
}

int sag_fl_main(int argc, char **argv)
{
    if (argc == 1)
        return isatty(0) ? sag_fl_repl() : run_stdin();
    if (argc == 2 && strcmp(argv[1], "--selftest-fl-bug") == 0) {
        /*
         * On a TTY the selftest runs THROUGH THE PROMPT, because that
         * is the only configuration where invariant 6 has anything to
         * prove: sag_bug's prehook is installed by sag_tty_raw, so a
         * headless run would restore a terminal it never took.
         */
        if (isatty(0))
            return sag_fl_repl_selftest_bug();
        return selftest_bug();
    }
    if (argc == 2 && strcmp(argv[1], "--list-natives") == 0)
        return list_natives();
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        (void)fputs(fl_usage, stdout);
        return SAG_EXIT_OK;
    }
    if (argc == 3 && strcmp(argv[1], "-e") == 0)
        return run_expr(argv[2]);
    if (argc == 3 && strcmp(argv[1], "-c") == 0)
        return with_file(argv[2], mode_conly, true);
    if (argc == 3 && strcmp(argv[1], "--dump-ast") == 0)
        return with_file(argv[2], mode_ast, true);
    if (argc == 3 && strcmp(argv[1], "--dump-bytecode") == 0)
        return with_file(argv[2], mode_bc, true);
    if (argc == 2 && argv[1][0] != '-')
        return with_file(argv[1], mode_eval_file, true);
    (void)fputs(fl_usage, stderr);
    return SAG_EXIT_ERR;
}
