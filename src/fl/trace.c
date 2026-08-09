/* Sprint 32 §6: stack traces.  The contract is in trace.h. */
#include "fl/trace.h"

#include <stdio.h>
#include <string.h>

#include "fl/gc.h"
#include "util/intern.h"

/*
 * The frame-naming table, closed and asserted row by row by the tests.
 *
 * A name is not derivable from the chunk alone: a module's top-level
 * chunk and a script's are both anonymous with a path, and only the
 * caller that compiled them knows which is which -- which is why
 * FlFn carries `fnkind`.
 */
static void frame_name(const FlVm *vm, const FlFn *fn, Bytebuf *out)
{
    const char *nm = fn->name_id == 0U
                         ? NULL
                         : sag_intern_str(vm->in, fn->name_id);

    switch ((FlFnKind)fn->fnkind) {
    case FL_FN_MACRO:
        bytebuf_append(out, "macro ", 6U);
        bytebuf_append(out, nm == NULL ? "?" : nm,
                       nm == NULL ? 1U : strlen(nm));
        return;
    case FL_FN_MODULE:
        bytebuf_append(out, "<module init>", 13U);
        return;
    case FL_FN_SCRIPT:
        bytebuf_append(out, "<script>", 8U);
        return;
    case FL_FN_REPL:
        bytebuf_append(out, "<repl>", 6U);
        return;
    default:
        if (nm == NULL)
            bytebuf_append(out, "<fn>", 4U);
        else
            bytebuf_append(out, nm, strlen(nm));
        return;
    }
}

/* The line-run table is keyed by INSTRUCTION START, so the greatest run
 * at or below `pc` is the instruction containing it. */
static void site_at(const FlChunk *ch, u32 pc, u32 *line, u32 *col)
{
    u32 lo = 0U;
    u32 hi = ch->nlines;

    *line = 0U;
    *col = 0U;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;

        if (ch->lines[mid].pc <= pc)
            lo = mid + 1U;
        else
            hi = mid;
    }
    if (lo != 0U) {
        *line = ch->lines[lo - 1U].line;
        *col = ch->lines[lo - 1U].col;
    }
}

static const char *file_of(const FlVm *vm, u32 file_id)
{
    if (vm->dc == NULL || file_id >= vm->dc->nfiles)
        return NULL;
    return vm->dc->files[file_id].path;
}

/* `name (file:line:col)` for one frame. */
static void frame_line(const FlVm *vm, u32 i, Bytebuf *out)
{
    const FlFrame *f = &vm->frames[i];
    const FlFn *fn = f->cl->fn;
    const char *path = file_of(vm, fn->ch.file_id);
    u32 pc;
    u32 line = 0U;
    u32 col = 0U;

    frame_name(vm, fn, out);
    if (i + 1U == vm->nframes) {
        /*
         * The innermost frame: the RAISING instruction.  `ip` was saved
         * pointing just past it, so any byte before that is inside it,
         * and the run covering that byte is the one to report.
         */
        u32 off = (u32)(f->ip - fn->ch.code);

        pc = off == 0U ? 0U : off - 1U;
    } else if (vm->frames[i + 1U].via_native != 0U) {
        /*
         * The frame above was entered from C, through a native, so it
         * recorded no call_pc -- there was no CALL in a chunk to
         * record.  This frame's own saved ip is where it called that
         * native from: the CALL case saves it before dispatching,
         * precisely so a native that raises can be located.
         */
        u32 off = (u32)(f->ip - fn->ch.code);

        pc = off == 0U ? 0U : off - 1U;
    } else {
        /* Every other frame is sitting at the CALL that pushed the one
         * above it, whose own pc that frame recorded. */
        pc = vm->frames[i + 1U].call_pc;
    }
    site_at(&fn->ch, pc, &line, &col);
    bytebuf_printf(out, " (%s:%u:%u)", path == NULL ? "?" : path,
                   (unsigned)line, (unsigned)col);
}

/*
 * The buffer as a string, PUSHED under protection.
 *
 * fl_list_push allocates when the array grows, and gc.h rule 2 says a
 * value reachable only from a C local does not survive an allocation --
 * so the fresh string is protected across the push rather than handed
 * over and hoped for.
 */
static void push_line(FlVm *vm, FlList *l, Bytebuf *bb)
{
    FlValue v = FL_OBJ_V(FL_STR, fl_str_new(vm, (const char *)bb->data,
                                            (u32)bb->len));

    fl_gc_protect(vm, v);
    (void)fl_list_push(vm, l, v);
    fl_gc_release(vm, 1U);
    bb->len = 0U;
}

/*
 * The source line and a caret under it, for the INNERMOST frame only.
 *
 * Written here rather than through fl_diag_render because that renders
 * a full diagnostic, header and all, and the location is already on the
 * `at` line above -- repeating it is noise at exactly the moment the
 * reader is scanning.
 *
 * Skipped silently when the source is unavailable.  Pointing a caret at
 * the wrong line is worse than no caret.
 */
static void render_caret(FlVm *vm, Bytebuf *out)
{
    const FlFrame *f;
    const FlFn *fn;
    const FlDiagFile *file;
    u32 pc;
    u32 line = 0U;
    u32 col = 0U;
    size_t at = 0U;
    size_t eol;
    u32 seen = 1U;

    if (vm->nframes == 0U || vm->dc == NULL)
        return;
    f = &vm->frames[vm->nframes - 1U];
    fn = f->cl->fn;
    if (fn->ch.file_id >= vm->dc->nfiles)
        return;
    file = &vm->dc->files[fn->ch.file_id];
    if (file->src == NULL || file->len == 0U)
        return;
    {
        u32 off = (u32)(f->ip - fn->ch.code);

        pc = off == 0U ? 0U : off - 1U;
    }
    site_at(&fn->ch, pc, &line, &col);
    if (line == 0U)
        return;
    while (at < file->len && seen < line) {
        if (file->src[at] == '\n')
            seen++;
        at++;
    }
    if (seen != line)
        return;
    eol = at;
    while (eol < file->len && file->src[eol] != '\n')
        eol++;
    {
        char gutter[16];
        int pad = snprintf(gutter, sizeof(gutter), "%u", (unsigned)line);
        u32 i;

        if (pad < 0)
            return;
        bytebuf_printf(out, "\n  %s | %.*s\n", gutter, (int)(eol - at),
                       file->src + at);
        /* The caret row's gutter is BLANK but the same width, or the
         * caret lands under the wrong column. */
        bytebuf_append(out, "  ", 2U);
        for (i = 0U; i < (u32)pad; i++)
            bytebuf_push_u8(out, (u8)' ');
        bytebuf_append(out, " | ", 3U);
        /* A tab in the source advances the caret row by a tab too, so
         * the two stay aligned whatever the terminal's tab stops are. */
        for (i = 1U; i < col && at + i - 1U < eol; i++)
            bytebuf_push_u8(out, file->src[at + i - 1U] == '\t'
                                     ? (u8)'\t' : (u8)' ');
        bytebuf_push_u8(out, (u8)'^');
        bytebuf_push_u8(out, (u8)'\n');
    }
}

void fl_trace_attach(FlVm *vm)
{
    FlList *l;
    Bytebuf bb;
    u32 i;

    if (vm->err.t != (u8)FL_MAP || vm->nframes == 0U)
        return;
    /*
     * ONCE PER ERROR, and the FIRST attach is the true one.
     *
     * An error raised inside fl_call propagates by the native
     * returning false, and fl_call drops the callee's frames before the
     * outer dispatch loop reaches its own `raised:` -- which would
     * attach again, over the deep trace, with only the frames that
     * happen to be left.  fl_raise builds a fresh map for every raise,
     * so a map that already carries a trace is one that has been here.
     */
    {
        FlValue seen = FL_NIL_V;

        if (fl_map_get((FlMap *)vm->err.as.o,
                       FL_OBJ_V(FL_STR, fl_str_new(vm, "trace", 5U)), &seen))
            return;
    }
    /* Protected for the whole build: every push allocates, and until
     * the list is on the error map nothing else points at it. */
    fl_gc_protect(vm, vm->err);
    l = fl_list_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, l));
    bytebuf_init(&bb);
    /* Innermost first, which is the order a reader scans. */
    for (i = vm->nframes; i-- > 0U; ) {
        u32 shown = vm->nframes - 1U - i;

        if (vm->nframes > (u32)FL_TRACE_MAX_FRAMES &&
            shown == (u32)FL_TRACE_HEAD) {
            bytebuf_printf(&bb, "... %u frames elided ...",
                           (unsigned)(vm->nframes - (u32)FL_TRACE_HEAD -
                                      (u32)FL_TRACE_TAIL));
            push_line(vm, l, &bb);
        }
        if (vm->nframes > (u32)FL_TRACE_MAX_FRAMES &&
            shown >= (u32)FL_TRACE_HEAD &&
            shown < vm->nframes - (u32)FL_TRACE_TAIL)
            continue;
        frame_line(vm, i, &bb);
        push_line(vm, l, &bb);
        /* A native between two Fletch frames: `list.map(f, ...)` is a
         * real link in the chain and a trace that skipped it would
         * leave an unexplained jump. */
        if (vm->frames[i].via_native != 0U) {
            const char *nm = sag_intern_str(vm->in, vm->frames[i].via_native);

            bytebuf_printf(&bb, "<native %s>", nm == NULL ? "?" : nm);
            push_line(vm, l, &bb);
        }
    }
    (void)fl_map_set(vm, (FlMap *)vm->err.as.o,
                     FL_OBJ_V(FL_STR, fl_str_new(vm, "trace", 5U)),
                     FL_OBJ_V(FL_LIST, l));
    fl_gc_release(vm, 2U);
    /* Captured HERE: the frames are gone by the time anyone renders. */
    bb.len = 0U;
    render_caret(vm, &bb);
    vm->err_caret = NULL;
    if (bb.len != 0U) {
        char *copy = arena_alloc(vm->arena, bb.len + 1U, 1U);

        (void)memcpy(copy, bb.data, bb.len);
        copy[bb.len] = '\0';
        vm->err_caret = copy;
    }
    bytebuf_free(&bb);
}

/* One string field of the error map, or NULL. */
static const FlStr *err_str(FlVm *vm, FlValue err, const char *key)
{
    FlValue got = FL_NIL_V;
    FlStr *k = fl_str_new(vm, key, (u32)strlen(key));

    if (err.t != (u8)FL_MAP)
        return NULL;
    if (!fl_map_get((FlMap *)err.as.o, FL_OBJ_V(FL_STR, k), &got))
        return NULL;
    return got.t == (u8)FL_STR ? (const FlStr *)got.as.o : NULL;
}

void fl_trace_render(FlVm *vm, FlValue err, Bytebuf *out)
{
    const FlStr *kind = err_str(vm, err, "kind");
    const FlStr *msg = err_str(vm, err, "msg");
    FlValue tv = FL_NIL_V;
    FlStr *tk = fl_str_new(vm, "trace", 5U);
    u32 i;

    bytebuf_printf(out, "error: %.*s: %.*s\n",
                   kind == NULL ? 1 : (int)kind->len,
                   kind == NULL ? "?" : kind->b,
                   msg == NULL ? 1 : (int)msg->len,
                   msg == NULL ? "?" : msg->b);
    if (err.t != (u8)FL_MAP ||
        !fl_map_get((FlMap *)err.as.o, FL_OBJ_V(FL_STR, tk), &tv) ||
        tv.t != (u8)FL_LIST)
        return;
    {
        const FlList *l = (const FlList *)tv.as.o;

        for (i = 0U; i < l->n; i++) {
            const FlStr *s;

            if (l->v[i].t != (u8)FL_STR)
                continue;
            s = (const FlStr *)l->v[i].as.o;
            /* The elision line is not a frame and takes no `at`. */
            if (s->len > 4U && memcmp(s->b, "... ", 4U) == 0)
                bytebuf_printf(out, "  %.*s\n", (int)s->len, s->b);
            else
                bytebuf_printf(out, "  at %.*s\n", (int)s->len, s->b);
        }
    }
    if (vm->err_caret != NULL)
        bytebuf_append(out, vm->err_caret, strlen(vm->err_caret));
}
