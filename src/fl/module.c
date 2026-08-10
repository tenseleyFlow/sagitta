/*
 * Sprint 31 deliverable 9: `import`.
 *
 * The reasoning for the (realpath, origin.kind) cache key and for
 * resolving through realpath(3) is in module.h, next to the struct it
 * shapes.  What lives here is the mechanism.
 *
 * A MODULE BODY GETS ITS OWN GLOBALS.  Spec §7 makes a top-level
 * binding a global, and §11 makes the non-underscore ones its exports;
 * with one shared map, module B defining `helper` would overwrite
 * module A's `helper` the moment A imported B.  So the body runs
 * against a fresh globals map, the importer's is held on the GC's temp
 * roots for the duration, and the exports are copied out of the fresh
 * one under STRING keys -- globals are keyed by interned id, and `m.x`
 * reads a map by name.
 *
 * NOTHING HERE GUESSES.  The quoted path is used verbatim: no extension
 * added, no `~` expanded, no cwd consulted.  A config that resolves
 * differently depending on the directory sagitta was launched from is
 * unshippable, and "explicit beats magic" is cheaper to explain than
 * any amount of convenience.
 */
#define _POSIX_C_SOURCE 200809L

#include "fl/module.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fl/compile.h"
#include "fl/gc.h"
#include "fl/parse.h"
#include "fl/std.h"
#include "fl/vm.h"
#include "util/intern.h"
#include "util/xdg.h"

enum { MOD_MAX_BYTES = 8U * 1024U * 1024U };

/* ---------------------------------------------------------------- */
/* The table                                                        */
/* ---------------------------------------------------------------- */

void fl_mod_free(FlVm *vm)
{
    free(vm->mods.v);
    vm->mods.v = NULL;
    vm->mods.n = 0U;
    vm->mods.cap = 0U;
}

static u32 mod_find(const FlVm *vm, u32 path_id, u8 kind)
{
    u32 i;

    for (i = 0U; i < vm->mods.n; i++) {
        if (vm->mods.v[i].path_id == path_id &&
            vm->mods.v[i].origin.kind == kind)
            return i;
    }
    return (u32)-1;
}

static u32 mod_add(FlVm *vm, u32 path_id, FlOrigin origin, u32 importer)
{
    FlModule *m;

    if (vm->mods.n == vm->mods.cap) {
        vm->mods.cap = vm->mods.cap == 0U ? 8U : vm->mods.cap * 2U;
        vm->mods.v = sag_xreallocarray(vm->mods.v, vm->mods.cap,
                                       sizeof(*vm->mods.v));
    }
    m = &vm->mods.v[vm->mods.n];
    m->path_id = path_id;
    m->origin = origin;
    m->exports = NULL;
    m->state = (u8)FL_MOD_LOADING;
    m->importer = importer;
    return vm->mods.n++;
}

/* Which module the frame stack is currently inside, or -1 at the top
 * level.  Read from the innermost non-builtin frame, which is the
 * module whose body is executing the import. */
static u32 mod_current(const FlVm *vm)
{
    u32 i;

    for (i = vm->nframes; i-- > 0U; ) {
        FlOrigin o = vm->frames[i].cl->fn->origin;

        if (o.kind == (u8)FL_ORIGIN_BUILTIN || o.path_id == 0U)
            continue;
        {
            u32 k = mod_find(vm, o.path_id, o.kind);

            if (k != (u32)-1)
                return k;
        }
    }
    return (u32)-1;
}

/* ---------------------------------------------------------------- */
/* Errors                                                           */
/* ---------------------------------------------------------------- */

/*
 * The whole chain, in load order.
 *
 * "cyclic import of b.fl" is the version nobody can act on: in a tree
 * of any size the question is which file reached it, and the answer is
 * the chain.  Walked backwards through `importer` and then reversed, so
 * the reader sees the order the loads happened in.
 */
static void chain_line(Bytebuf *bb, const FlVm *vm, u32 path_id, bool first)
{
    const char *p = sag_intern_str(vm->in, path_id);

    bytebuf_append(bb, "\n  ", 3U);
    if (!first)
        bytebuf_append(bb, "-> ", 3U);
    bytebuf_append(bb, p == NULL ? "?" : p, p == NULL ? 1U : strlen(p));
}

static bool cycle_err(FlVm *vm, u32 hit)
{
    u32 chain[FL_MOD_MAX_DEPTH + 2U];
    u32 n = 0U;
    u32 total;
    u32 i = mod_current(vm);
    Bytebuf bb;
    bool ok;

    /* Walked BACKWARDS through `importer`, from the module doing the
     * import to the one it re-entered. */
    while (i != (u32)-1 && n < (u32)FL_MOD_MAX_DEPTH + 1U) {
        chain[n++] = i;
        if (i == hit)
            break;
        i = vm->mods.v[i].importer == 0U ? (u32)-1
                                         : vm->mods.v[i].importer - 1U;
    }
    total = n;
    bytebuf_init(&bb);
    bytebuf_append(&bb, "import cycle:", 13U);
    /* ...and printed FORWARDS, because that is the order the loads
     * happened in and the only order a reader can follow.  The module
     * that was re-entered appears twice, at both ends: that repetition
     * is what makes the chain visibly a cycle. */
    while (n-- > 0U)
        chain_line(&bb, vm, vm->mods.v[chain[n]].path_id, n + 1U == total);
    chain_line(&bb, vm, vm->mods.v[hit].path_id, false);
    ok = fl_raise(vm, "import", "%.*s", (int)bb.len, (const char *)bb.data);
    bytebuf_free(&bb);
    return ok;
}

/* ---------------------------------------------------------------- */
/* Resolution                                                       */
/* ---------------------------------------------------------------- */

static void join(Bytebuf *bb, const char *dir, const char *rel, size_t rn)
{
    size_t dn = strlen(dir);

    bb->len = 0U;
    bytebuf_append(bb, dir, dn);
    if (dn != 0U && dir[dn - 1U] != '/')
        bytebuf_push_u8(bb, (u8)'/');
    bytebuf_append(bb, rel, rn);
    bytebuf_push_u8(bb, 0U);
    bb->len--;
}

/* The importing file's directory, or NULL when there is no importing
 * file (the CLI and the REPL). */
static char *importer_dir(FlVm *vm, const FlOrigin *o)
{
    const char *p = o->path_id == 0U ? NULL : sag_intern_str(vm->in, o->path_id);
    const char *slash;
    char *dir;
    size_t n;

    if (p == NULL)
        return NULL;
    slash = strrchr(p, '/');
    if (slash == NULL)
        return NULL;
    n = slash == p ? 1U : (size_t)(slash - p);
    dir = sag_xmalloc(n + 1U);
    (void)memcpy(dir, p, n);
    dir[n] = '\0';
    return dir;
}

/*
 * Tries the two places §11 names, in order, and records each attempt so
 * a miss can list them.  Returns a heap realpath, or NULL.
 */
static char *resolve(FlVm *vm, const FlOrigin *o, const char *rel, size_t rn,
                     Bytebuf *tried)
{
    Bytebuf cand;
    char *dir;
    char *real = NULL;
    char *cfg;

    bytebuf_init(&cand);
    dir = importer_dir(vm, o);
    if (dir != NULL) {
        join(&cand, dir, rel, rn);
        bytebuf_append(tried, "\n  ", 3U);
        bytebuf_append(tried, cand.data, cand.len);
        real = realpath((const char *)cand.data, NULL);
        free(dir);
    }
    if (real == NULL) {
        cfg = sag_xdg_config_dir();
        if (cfg != NULL) {
            Bytebuf under;

            bytebuf_init(&under);
            bytebuf_append(&under, cfg, strlen(cfg));
            bytebuf_append(&under, "/fl", 3U);
            bytebuf_push_u8(&under, 0U);
            join(&cand, (const char *)under.data, rel, rn);
            bytebuf_append(tried, "\n  ", 3U);
            bytebuf_append(tried, cand.data, cand.len);
            real = realpath((const char *)cand.data, NULL);
            bytebuf_free(&under);
            free(cfg);
        }
    }
    bytebuf_free(&cand);
    return real;
}

/* ---------------------------------------------------------------- */
/* Execution                                                        */
/* ---------------------------------------------------------------- */

static bool read_source(FlVm *vm, const char *path, char **out, size_t *len)
{
    struct stat st;
    Bytebuf bb;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return fl_raise(vm, "import", "cannot read %s", path);
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        (void)close(fd);
        return fl_raise(vm, "import", "cannot read %s", path);
    }
    bytebuf_init(&bb);
    for (;;) {
        char buf[65536];
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            bytebuf_free(&bb);
            return fl_raise(vm, "import", "cannot read %s", path);
        }
        if (n == 0)
            break;
        bytebuf_append(&bb, buf, (size_t)n);
        if (bb.len > (size_t)MOD_MAX_BYTES) {
            (void)close(fd);
            bytebuf_free(&bb);
            return fl_raise(vm, "limit", "%s exceeds 8 MiB", path);
        }
    }
    (void)close(fd);
    /*
     * Copied into the VM arena, not handed over as a Bytebuf: DiagCtx
     * keeps the source pointer so it can render a caret line later, and
     * a freed buffer would make the second diagnostic about this file
     * read from released memory.
     */
    *out = arena_alloc(vm->arena, bb.len + 1U, 1U);
    if (bb.len != 0U)
        (void)memcpy(*out, bb.data, bb.len);
    (*out)[bb.len] = '\0';
    *len = bb.len;
    bytebuf_free(&bb);
    return true;
}

/* Everything the module's body bound, minus the `_`-prefixed names,
 * re-keyed from interner ids to strings and frozen. */
static FlMap *collect_exports(FlVm *vm, FlMap *globals)
{
    FlMap *ex = fl_map_new(vm);
    u32 cursor = 0U;
    FlValue k;
    FlValue v;

    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, ex));
    while (fl_map_iter(globals, &cursor, &k, &v)) {
        const char *nm;

        if (k.t != (u8)FL_INT)
            continue;
        nm = sag_intern_str(vm->in, (u32)k.as.i);
        if (nm == NULL || nm[0] == '_')
            continue;
        (void)fl_map_set(vm, ex,
                         FL_OBJ_V(FL_STR,
                                  fl_str_new(vm, nm, (u32)strlen(nm))), v);
    }
    /* Frozen: a module is a value other code holds, and `m.x = 1` from
     * outside would rewrite it for every holder. */
    ex->h.oflags |= (u16)FL_OF_FROZEN;
    fl_gc_release(vm, 1U);
    return ex;
}

static bool run_body(FlVm *vm, u32 idx, const char *path, const char *src,
                     size_t len)
{
    FlProgram p;
    FlFn *fn;
    FlMap *fresh;
    FlValue ignored = FL_NIL_V;
    FlClosure *cl;
    u32 file_id = fl_diag_add_file(vm->dc, path, src, len);
    bool ok;

    p = fl_parse(vm->arena, vm->dc, vm->in, src, len, file_id);
    if (p.had_error)
        return fl_raise(vm, "import", "%s did not parse", path);
    fn = fl_compile(vm, vm->dc, &p, file_id, vm->mods.v[idx].origin);
    if (fn == NULL)
        return fl_raise(vm, "import", "%s did not compile", path);
    /* §6's frame-naming table: a module's top-level chunk prints as
     * `<module init>`, which nothing but its compiler can know. */
    fn->fnkind = (u8)FL_FN_MODULE;
    /*
     * The body's own globals, carried on ITS closure.
     *
     * Nothing swaps vm->globals: a closure reads the map it was made
     * with, so the module's functions keep seeing the module's
     * bindings however far from home they are eventually called.  The
     * map is protected until the closure holds it, and the closure is
     * on the VM stack for the whole call after that.
     */
    fresh = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, fresh));
    cl = fl_gc_alloc(vm, sizeof(*cl), FL_CLOSURE);
    cl->fn = fn;
    cl->up = NULL;
    cl->nup = 0U;
    cl->globals = fresh;
    fl_gc_release(vm, 1U);
    ok = fl_call(vm, FL_OBJ_V(FL_CLOSURE, cl), NULL, 0U, &ignored);
    if (ok) {
        FlMap *ex = collect_exports(vm, fresh);

        vm->mods.v[idx].exports = ex;
        vm->mods.v[idx].state = (u8)FL_MOD_READY;
        (void)fl_map_set(vm, vm->modules, FL_INT_V((i64)idx),
                         FL_OBJ_V(FL_MAP, ex));
    }
    return ok;
}

/* ---------------------------------------------------------------- */

bool fl_import(FlVm *vm, u32 id, bool is_path, FlValue *out)
{
    FlOrigin o = fl_cap_origin(vm);
    const char *text = sag_intern_str(vm->in, id);
    size_t textlen = sag_intern_len(vm->in, id);
    Bytebuf tried;
    char *real;
    u32 rid;
    u32 idx;
    char *src = NULL;
    size_t srclen = 0U;
    bool ok;

    if (text == NULL)
        return fl_raise(vm, "import", "the import names nothing");
    if (!is_path) {
        /*
         * A bare IDENT is a builtin and nothing else, ever.  Resolving
         * `import str` against the filesystem would let a file named
         * str.fl in the wrong directory shadow the standard library.
         */
        FlValue m;
        FlStr *k = fl_str_new(vm, text, (u32)textlen);

        if (fl_map_get(vm->builtins, FL_OBJ_V(FL_STR, k), &m)) {
            *out = m;
            return true;
        }
        {
            /*
             * GENERATED from vm->builtins, not spelled out.  The list was
             * a literal, and Sprint 34's `buf` made it wrong the moment
             * it registered -- the message named seven modules while the
             * map held eight, so a user was told `buf` did not exist
             * while `import buf` in the very next line worked.  A message
             * that has to be kept in step by hand is one that drifts.
             */
            Bytebuf names;
            u32 cursor = 0U;
            FlValue mk;
            FlValue mv;
            bool first = true;
            bool raised;

            bytebuf_init(&names);
            while (fl_map_iter(vm->builtins, &cursor, &mk, &mv)) {
                const FlStr *nm;

                if (mk.t != (u8)FL_STR)
                    continue;
                nm = (const FlStr *)mk.as.o;
                if (!first)
                    bytebuf_append(&names, ", ", 2U);
                bytebuf_append(&names, nm->b, (size_t)nm->len);
                first = false;
            }
            raised = fl_raise(vm, "import",
                              "there is no builtin module '%s'; they are "
                              "%.*s", text, (int)names.len,
                              names.data == NULL ? "" :
                              (const char *)names.data);
            bytebuf_free(&names);
            return raised;
        }
    }
    if (vm->mods.n >= (u32)FL_MOD_MAX_DEPTH * 64U)
        return fl_raise(vm, "limit", "too many modules loaded");
    bytebuf_init(&tried);
    real = resolve(vm, &o, text, textlen, &tried);
    if (real == NULL) {
        ok = fl_raise(vm, "import", "cannot find '%s'; tried:%.*s", text,
                      (int)tried.len,
                      tried.len == 0U ? "" : (const char *)tried.data);
        bytebuf_free(&tried);
        return ok;
    }
    bytebuf_free(&tried);
    rid = sag_intern(vm->in, real, strlen(real));
    idx = mod_find(vm, rid, o.kind);
    if (idx != (u32)-1) {
        if (vm->mods.v[idx].state == (u8)FL_MOD_LOADING) {
            free(real);
            return cycle_err(vm, idx);
        }
        /* The SAME map object every time, which is what makes two
         * imports of one file compare equal by reference. */
        *out = FL_OBJ_V(FL_MAP, vm->mods.v[idx].exports);
        free(real);
        return true;
    }
    if (!read_source(vm, real, &src, &srclen)) {
        free(real);
        return false;
    }
    free(real);
    {
        FlOrigin sub;
        u32 cur = mod_current(vm);

        /* The importer's KIND and CAPS, this file's path: a helper
         * loaded by a plugin runs as a plugin, which is the whole point
         * of the cache key. */
        sub.kind = o.kind;
        sub.path_id = rid;
        sub.caps = o.caps;
        idx = mod_add(vm, rid, sub, cur == (u32)-1 ? 0U : cur + 1U);
    }
    {
        const char *path = sag_intern_str(vm->in, rid);

        if (!run_body(vm, idx, path, src, srclen))
            return false;
    }
    *out = FL_OBJ_V(FL_MAP, vm->mods.v[idx].exports);
    return true;
}
