/* Sprint 31's shared Fletch test fixture; the contract is in flfix.h. */
#define _POSIX_C_SOURCE 200809L

#include "flfix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/parse.h"
#include "fl/trace.h"

static void flfix_sink(void *ctx, FlDiagLevel level, FlSpan sp,
                       const char *msg, const char *rendered)
{
    FlFix *f = ctx;

    (void)level;
    (void)sp;
    (void)rendered;
    if (f->ndiag == 0U)
        (void)snprintf(f->first, sizeof(f->first), "%s", msg);
    f->ndiag++;
}

void flfix_open(FlFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    arena_init(&f->arena);
    interner_init(&f->in, &f->arena);
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, flfix_sink, f);
    fl_vm_init(&f->vm, &f->arena, &f->in, &f->dc);
    fl_std_register(&f->vm);
    f->origin.kind = (u8)FL_ORIGIN_CLI;
    f->origin.path_id = 0U;
    f->origin.caps = 0U;
}

static void flfix_rm_rf(const char *path)
{
    char cmd[512];
    int removed;

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    /* Assigned rather than cast away: glibc marks system() as
     * warn_unused_result under _FORTIFY_SOURCE, which some
     * distributions enable by default and others do not -- the cast
     * built here and failed CI once already. */
    removed = system(cmd);
    (void)removed;
}

void flfix_close(FlFix *f)
{
    fl_vm_free(&f->vm);
    interner_free(&f->in);
    arena_free_all(&f->arena);
    if (f->has_tmp)
        flfix_rm_rf(f->tmp);
    f->has_tmp = false;
}

const char *flfix_tmpdir(FlFix *f)
{
    char init[512];

    if (f->has_tmp)
        return f->tmp;
    (void)snprintf(f->tmp, sizeof(f->tmp), "/tmp/sag-flfix-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->tmp));
    f->has_tmp = true;
    /*
     * The origin's path is what a relative import and io.glob resolve
     * against, so pointing it at a file INSIDE the directory is what
     * makes those two behave as they would for a real config.  The file
     * need not exist: only its directory is ever read.
     */
    (void)snprintf(init, sizeof(init), "%s/init.fl", f->tmp);
    f->origin.path_id = sag_intern(&f->in, init, strlen(init));
    return f->tmp;
}

void flfix_write(FlFix *f, const char *rel, const char *body)
{
    char path[1024];
    FILE *fp;

    (void)snprintf(path, sizeof(path), "%s/%s", flfix_tmpdir(f), rel);
    fp = fopen(path, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fwrite(body, 1U, strlen(body), fp);
    SAG_ASSERT_EQ_I64(fclose(fp), 0);
}

void flfix_mkdir(FlFix *f, const char *rel)
{
    char path[1024];

    (void)snprintf(path, sizeof(path), "%s/%s", flfix_tmpdir(f), rel);
    SAG_ASSERT_EQ_I64(mkdir(path, 0777), 0);
}

void flfix_as(FlFix *f, u8 kind, u32 caps)
{
    f->origin.kind = kind;
    f->origin.caps = caps;
}

/* One field of the error map, as text. */
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

void flfix_run(FlFix *f, const char *src, char *out, size_t cap)
{
    FlProgram p;
    FlFn *fn;
    FlValue res = FL_NIL_V;
    size_t n = strlen(src);

    /*
     * The diagnostic context is rebuilt per program.  A fixture that
     * runs forty programs otherwise fills the file table and the
     * forty-first trips a SAG_BUG about source files, which is a very
     * confusing way to learn that a test file grew.
     */
    f->ndiag = 0U;
    f->first[0] = '\0';
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, flfix_sink, f);
    (void)fl_diag_add_file(&f->dc, "t.fl", src, n);
    p = fl_parse(&f->arena, &f->dc, &f->in, src, n, 0U);
    if (p.had_error) {
        (void)snprintf(out, cap, "!parse");
        return;
    }
    fn = fl_compile(&f->vm, &f->dc, &p, 0U, f->origin);
    if (fn == NULL) {
        (void)snprintf(out, cap, "!compile");
        return;
    }
    if (!fl_vm_run(&f->vm, fn, &res)) {
        char kind[64];
        char msg[2048];

        err_field(&f->vm, res, "kind", kind, sizeof(kind));
        err_field(&f->vm, res, "msg", msg, sizeof(msg));
        (void)snprintf(out, cap, "!%s: %s", kind, msg);
        return;
    }
    switch ((FlType)res.t) {
    case FL_NIL:
        (void)snprintf(out, cap, "nil");
        return;
    case FL_BOOL:
        (void)snprintf(out, cap, "%s", res.as.b ? "true" : "false");
        return;
    case FL_INT:
        (void)snprintf(out, cap, "%lld", (long long)res.as.i);
        return;
    case FL_STR: {
        const FlStr *s = (const FlStr *)res.as.o;
        size_t k = (size_t)s->len < cap - 1U ? (size_t)s->len : cap - 1U;

        (void)memcpy(out, s->b, k);
        out[k] = '\0';
        return;
    }
    default:
        /* Floats included: a float's spelling is fmt's business, and a
         * test that wants one asks fmt for it. */
        (void)snprintf(out, cap, "<%s>", fl_type_name((FlType)res.t));
        return;
    }
}

void flfix_run_trace(FlFix *f, const char *src, u8 kind, char *out,
                     size_t cap)
{
    FlProgram p;
    FlFn *fn;
    FlValue res = FL_NIL_V;
    size_t n = strlen(src);
    Bytebuf bb;
    size_t k;

    out[0] = '\0';
    f->ndiag = 0U;
    fl_diag_init(&f->dc, &f->arena);
    fl_diag_set_sink(&f->dc, flfix_sink, f);
    /* The source is BORROWED by the diag context, and the caret quotes
     * it -- so it has to be the same bytes the compiler read. */
    (void)fl_diag_add_file(&f->dc, "t.fl", src, n);
    p = fl_parse(&f->arena, &f->dc, &f->in, src, n, 0U);
    if (p.had_error)
        return;
    fn = fl_compile(&f->vm, &f->dc, &p, 0U, f->origin);
    if (fn == NULL)
        return;
    fn->fnkind = kind;
    if (fl_vm_run(&f->vm, fn, &res))
        return;
    bytebuf_init(&bb);
    fl_trace_render(&f->vm, res, &bb);
    k = bb.len < cap - 1U ? bb.len : cap - 1U;
    if (k != 0U)
        (void)memcpy(out, bb.data, k);
    out[k] = '\0';
    bytebuf_free(&bb);
}
