/*
 * Sprint 31: `sag fl` -- the headless Fletch entry point.
 *
 * Handled BEFORE the editor's own argument parser, because `sag fl`'s
 * options are not the editor's and threading them through one parser
 * would make `--list-natives` a top-level flag that means nothing
 * outside this subcommand.
 *
 * `sag fl FILE` TAKES NO SCRIPT ARGUMENTS this campaign; Sprint 37's
 * `--batch` owns stdio and argv.  Saying so here is cheaper than
 * accepting them and quietly ignoring them.
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

#include "fl/compile.h"
#include "fl/parse.h"
#include "fl/std.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"

static const char fl_usage[] =
    "Usage:\n"
    "  sag fl FILE           Run a Fletch script.\n"
    "  sag fl --list-natives List every builtin, one per line.\n"
    "\n"
    "A script takes no arguments this campaign; batch mode with stdio\n"
    "and argv is Sprint 37.\n";

/* Renders a diagnostic the way the editor does not: to stderr, once. */
static void cli_diag(void *ctx, FlDiagLevel level, FlSpan sp, const char *msg,
                     const char *rendered)
{
    (void)ctx;
    (void)level;
    (void)sp;
    if (rendered != NULL)
        (void)fputs(rendered, stderr);
    else
        (void)fprintf(stderr, "sagitta: %s\n", msg == NULL ? "error" : msg);
}

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

static int run_script(const char *path)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    FlProgram p;
    FlFn *fn;
    FlOrigin origin;
    FlValue out = FL_NIL_V;
    char *src;
    char *real;
    size_t len = 0U;
    u32 file_id;
    int rc = SAG_EXIT_OK;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_diag_set_sink(&dc, cli_diag, NULL);
    fl_vm_init(&vm, &arena, &in, &dc);
    fl_std_register(&vm);

    src = slurp(path, &len, &arena);
    if (src == NULL) {
        rc = SAG_EXIT_IO;
        goto done;
    }
    real = realpath(path, NULL);
    origin.kind = (u8)FL_ORIGIN_CLI;
    origin.path_id = real == NULL
                         ? 0U
                         : sag_intern(&in, real, strlen(real));
    origin.caps = (u32)FL_CAP_FS_READ | (u32)FL_CAP_FS_WRITE |
                  (u32)FL_CAP_SHELL | (u32)FL_CAP_NET;
    free(real);
    vm.root_origin = origin;

    file_id = fl_diag_add_file(&dc, path, src, len);
    p = fl_parse(&arena, &dc, &in, src, len, file_id);
    if (p.had_error) {
        rc = SAG_EXIT_ERR;
        goto done;
    }
    fn = fl_compile(&vm, &dc, &p, file_id, origin);
    if (fn == NULL) {
        rc = SAG_EXIT_ERR;
        goto done;
    }
    if (!fl_vm_run(&vm, fn, &out)) {
        char kind[64];
        char msg[2048];

        /* An uncaught raise is an ERROR EXIT, not a silent zero: a
         * script run from a shell must be usable in a conditional. */
        err_field(&vm, out, "kind", kind, sizeof(kind));
        err_field(&vm, out, "msg", msg, sizeof(msg));
        (void)fprintf(stderr, "sagitta: %s: %s\n", kind, msg);
        rc = SAG_EXIT_ERR;
    }
done:
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
    return rc;
}

int sag_fl_main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--list-natives") == 0)
        return list_natives();
    if (argc == 2 && argv[1][0] != '-')
        return run_script(argv[1]);
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        (void)fputs(fl_usage, stdout);
        return SAG_EXIT_OK;
    }
    (void)fputs(fl_usage, stderr);
    return SAG_EXIT_ERR;
}
