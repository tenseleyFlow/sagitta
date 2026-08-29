#define _POSIX_C_SOURCE 200809L

#include "edit/batch_test.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/loop.h"
#include "edit/select.h"
#include "fl/gc.h"
#include "fl/handle.h"
#include "fl/std.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "mod/lsp/lsp.h"
#include "text/piece.h"
#include "text/register.h"
#include "text/undo.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "util/intern.h"

/* Sprint 37 pins one VM and one test file per process. */
static YewBatchTestState *active_test;

typedef struct TestNativeDef {
    const char *name;
    FlNativeFn fn;
    u8 min_ar;
    u8 max_ar;
} TestNativeDef;

static YewBatchTestState *state_for(FlVm *vm)
{
    if (active_test == NULL || active_test->vm != vm)
        YEW_BUG("batch assertion has no installed state");
    return active_test;
}

static const char *native_name(const FlVm *vm)
{
    const char *name = vm == NULL || vm->in == NULL ? NULL :
                       yew_intern_str(vm->in, vm->cur_native);
    return name == NULL ? "t.?" : name;
}

static void escaped(Bytebuf *out, const char *s, size_t n)
{
    size_t i;
    for (i = 0U; i < n; i++) {
        switch ((u8)s[i]) {
        case '\\': bytebuf_append(out, "\\\\", 2U); break;
        case '\t': bytebuf_append(out, "\\t", 2U); break;
        case '\r': bytebuf_append(out, "\\r", 2U); break;
        case '\n': bytebuf_append(out, "\\n", 2U); break;
        default: bytebuf_push_u8(out, (u8)s[i]); break;
        }
    }
}

static void fail_n(FlVm *vm, const char *text, size_t len)
{
    YewBatchTestState *s = state_for(vm);
    const char *name = native_name(vm);
    s->failures++;
    bytebuf_append(&s->failure_records, "FAIL\t", 5U);
    escaped(&s->failure_records, name, strlen(name));
    bytebuf_push_u8(&s->failure_records, (u8)'\t');
    escaped(&s->failure_records, text, len);
    bytebuf_push_u8(&s->failure_records, (u8)'\n');
}

static void fail(FlVm *vm, const char *text) { fail_n(vm, text, strlen(text)); }
static void assertion(FlVm *vm) { state_for(vm)->assertions++; }

static bool repr(FlVm *vm, FlValue value, Bytebuf *out)
{
    FlValue saved = vm->err;
    if (fl_fmt_repr(vm, out, value)) return true;
    vm->err = saved;
    bytebuf_printf(out, "<%s>", fl_type_name((FlType)value.t));
    return false;
}

static void fail_values(FlVm *vm, FlValue want, FlValue got,
                        const FlStr *note)
{
    Bytebuf b;
    bytebuf_init(&b);
    if (note != NULL && note->len != 0U) {
        bytebuf_append(&b, note->b, note->len);
        bytebuf_append(&b, ": ", 2U);
    }
    bytebuf_append(&b, "want ", 5U); (void)repr(vm, want, &b);
    bytebuf_append(&b, "; got ", 6U); (void)repr(vm, got, &b);
    fail_n(vm, (const char *)b.data, b.len);
    bytebuf_free(&b);
}

static bool t_eq(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *note = NULL;
    assertion(vm);
    if (n == 3U && !fl_arg_str(vm, a, 2U, &note)) return false;
    if (!fl_equal(a[0], a[1])) fail_values(vm, a[1], a[0], note);
    *out = FL_NIL_V; return true;
}

static bool t_ne(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    (void)n; assertion(vm);
    if (fl_equal(a[0], a[1])) fail_values(vm, a[1], a[0], NULL);
    *out = FL_NIL_V; return true;
}

static void text_append(Bytebuf *out, const TextBuf *tb, Span span)
{
    TextIter it;
    u64 at = span.lo;
    if (span.lo == span.hi) return;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        YEW_BUG("batch assertion cannot start text iterator");
    do {
        const u8 *chunk;
        u64 avail;
        u64 take;
        if (!yew_textiter_chunk(&it, tb, &chunk, &avail))
            YEW_BUG("batch assertion text iterator truncated");
        take = avail < span.hi - at ? avail : span.hi - at;
        bytebuf_append(out, chunk, (size_t)take);
        at += take;
    } while (at < span.hi && yew_textiter_advance(&it, tb));
    if (at != span.hi) YEW_BUG("batch assertion text ended early");
}

static size_t first_diff(const u8 *a, size_t an, const u8 *b, size_t bn)
{
    size_t n = an < bn ? an : bn;
    size_t i;
    for (i = 0U; i < n; i++) if (a[i] != b[i]) return i;
    return n;
}

static void fail_bytes(FlVm *vm, const FlStr *want, const Bytebuf *got)
{
    Bytebuf b;
    FlValue actual;
    size_t diff = first_diff((const u8 *)want->b, want->len,
                             got->data, got->len);
    bytebuf_init(&b);
    bytebuf_append(&b, "want ", 5U); (void)repr(vm, FL_OBJ_V(FL_STR, want), &b);
    bytebuf_append(&b, "; got ", 6U);
    actual = FL_OBJ_V(FL_STR, fl_str_new(vm, (const char *)got->data,
                                        (u32)got->len));
    fl_gc_protect(vm, actual); (void)repr(vm, actual, &b); fl_gc_release(vm, 1U);
    bytebuf_printf(&b, "; first difference at byte %zu", diff);
    fail_n(vm, (const char *)b.data, b.len);
    bytebuf_free(&b);
}

static bool assert_text(FlVm *vm, Buffer *buf, Span span,
                        const FlStr *want, FlValue *out)
{
    Bytebuf got;
    if (buf->tb == NULL) {
        fail(vm, "buffer is not resident"); *out = FL_NIL_V; return true;
    }
    bytebuf_init(&got); text_append(&got, buf->tb, span);
    if (got.len != want->len ||
        (got.len != 0U && memcmp(got.data, want->b, got.len) != 0))
        fail_bytes(vm, want, &got);
    bytebuf_free(&got); *out = FL_NIL_V; return true;
}

static bool t_text(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *buf; const FlStr *want = NULL; (void)n; assertion(vm);
    buf = fl_h_buf(vm, a[0]);
    if (buf == NULL || !fl_arg_str(vm, a, 1U, &want)) return false;
    return assert_text(vm, buf, (Span){0U, yew_buf_len(buf)}, want, out);
}

static bool t_line(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *buf; const FlStr *want = NULL; i64 line = 0;
    Span span; Bytebuf tail;
    (void)n; assertion(vm); buf = fl_h_buf(vm, a[0]);
    if (buf == NULL || !fl_arg_int(vm, a, 1U, &line) ||
        !fl_arg_str(vm, a, 2U, &want)) return false;
    if (buf->tb == NULL || line < 1 || (u64)line > yew_buf_line_count(buf)) {
        fail(vm, "requested line is outside the buffer");
        *out = FL_NIL_V; return true;
    }
    span = yew_buf_line_span(buf, LINENO((u64)line - 1U));
    bytebuf_init(&tail);
    if (span.hi > span.lo) {
        text_append(&tail, buf->tb, (Span){span.hi - 1U, span.hi});
        if (tail.len == 1U && tail.data[0] == (u8)'\n') {
            span.hi--; tail.len = 0U;
            if (span.hi > span.lo) {
                text_append(&tail, buf->tb, (Span){span.hi - 1U, span.hi});
                if (tail.len == 1U && tail.data[0] == (u8)'\r') span.hi--;
            }
        }
    }
    bytebuf_free(&tail); return assert_text(vm, buf, span, want, out);
}

static void cursor_pos(const Win *w, const Cursor *c, u64 *line, u64 *col)
{
    LineNo ln = yew_buf_line_of(w->buf, c->pos);
    Span s = yew_buf_line_span(w->buf, ln);
    *line = ln.v + 1U;
    *col = yew_off_to_gcol(w->buf->tb, s, c->pos).v;
}

static bool t_cursor(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Win *w; i64 wl = 0; i64 wc = 0; u64 gl, gc; char msg[128];
    (void)n; assertion(vm); w = fl_h_win(vm, a[0]);
    if (w == NULL || !fl_arg_int(vm, a, 1U, &wl) ||
        !fl_arg_int(vm, a, 2U, &wc)) return false;
    cursor_pos(w, &w->cs.curs.data[w->cs.primary], &gl, &gc);
    if (wl < 1 || wc < 0 || gl != (u64)wl || gc != (u64)wc) {
        (void)snprintf(msg, sizeof(msg), "want [%lld,%lld]; got [%llu,%llu]",
                       (long long)wl, (long long)wc,
                       (unsigned long long)gl, (unsigned long long)gc);
        fail(vm, msg);
    }
    *out = FL_NIL_V; return true;
}

static bool pair(const FlValue *v, i64 *line, i64 *col)
{
    const FlList *l;
    if (v->t != (u8)FL_LIST) return false;
    l = (const FlList *)v->as.o;
    if (l->n != 2U || l->v[0].t != (u8)FL_INT ||
        l->v[1].t != (u8)FL_INT) return false;
    *line = l->v[0].as.i; *col = l->v[1].as.i; return true;
}

static bool t_cursors(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Win *w; FlList *want = NULL; u32 i; bool same = true; char msg[112];
    (void)n; assertion(vm); w = fl_h_win(vm, a[0]);
    if (w == NULL || !fl_arg_list(vm, a, 1U, &want)) return false;
    if ((size_t)want->n != w->cs.curs.len) same = false;
    for (i = 0U; same && i < want->n; i++) {
        i64 wl, wc; u64 gl, gc;
        if (!pair(&want->v[i], &wl, &wc)) { same = false; break; }
        cursor_pos(w, &w->cs.curs.data[i], &gl, &gc);
        if (wl < 1 || wc < 0 || gl != (u64)wl || gc != (u64)wc) same = false;
    }
    if (!same) {
        (void)snprintf(msg, sizeof(msg),
                       "cursor set differs (want %u cursors; got %zu)",
                       (unsigned)want->n, w->cs.curs.len); fail(vm, msg);
    }
    *out = FL_NIL_V; return true;
}

static bool t_sel(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Win *w; const FlStr *want = NULL; const Cursor *c; Bytebuf got; (void)n;
    assertion(vm); w = fl_h_win(vm, a[0]);
    if (w == NULL || !fl_arg_str(vm, a, 1U, &want)) return false;
    c = &w->cs.curs.data[w->cs.primary]; bytebuf_init(&got);
    if (c->pos.v != c->anchor.v) {
        if (w->h.kind == YEW_SEL_RECT) {
            YewSelSpanVec spans = {0}; size_t i;
            yew_sel_rect_spans(w, c, &spans);
            for (i = 0U; i < spans.len; i++)
                text_append(&got, w->buf->tb, spans.data[i]);
            YewSelSpanVec_free(&spans);
        } else {
            text_append(&got, w->buf->tb, yew_sel_span(w, c));
        }
    }
    if (got.len != want->len ||
        (got.len != 0U && memcmp(got.data, want->b, got.len) != 0))
        fail_bytes(vm, want, &got);
    bytebuf_free(&got); *out = FL_NIL_V; return true;
}

static bool reg_type_is(FlValue v, u8 actual)
{
    const FlStr *s;
    if (v.t == (u8)FL_INT) return v.as.i == (i64)actual;
    if (v.t != (u8)FL_STR) return false;
    s = (const FlStr *)v.as.o;
    if (actual == YEW_REG_CHARWISE)
        return (s->len == 4U && memcmp(s->b, "char", 4U) == 0) ||
               (s->len == 8U && memcmp(s->b, "charwise", 8U) == 0);
    if (actual == YEW_REG_LINEWISE)
        return (s->len == 4U && memcmp(s->b, "line", 4U) == 0) ||
               (s->len == 8U && memcmp(s->b, "linewise", 8U) == 0);
    return (s->len == 5U && memcmp(s->b, "block", 5U) == 0) ||
           (s->len == 9U && memcmp(s->b, "blockwise", 9U) == 0);
}

static bool t_reg(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *name = NULL;
    const FlStr *want = NULL;
    RegVal *reg;
    Bytebuf got;
    assertion(vm);
    if (!fl_arg_str(vm, a, 0U, &name) ||
        !fl_arg_str(vm, a, 1U, &want)) return false;
    if (name->len != 1U || vm->ed == NULL ||
        (reg = yew_reg_get(&vm->ed->regs, (u8)name->b[0])) == NULL) {
        fail(vm, "register name is not readable"); *out = FL_NIL_V; return true;
    }
    bytebuf_init(&got); bytebuf_append(&got, reg->bytes.data, reg->bytes.len);
    if (got.len != want->len ||
        (got.len != 0U && memcmp(got.data, want->b, got.len) != 0))
        fail_bytes(vm, want, &got);
    bytebuf_free(&got);
    if (n == 3U && !reg_type_is(a[2], reg->type))
        fail(vm, "register type differs");
    *out = FL_NIL_V; return true;
}

static bool t_undo(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *buf; i64 want = 0; u64 live = 0U; size_t i; char msg[96];
    (void)n; assertion(vm); buf = fl_h_buf(vm, a[0]);
    if (buf == NULL || !fl_arg_int(vm, a, 1U, &want)) return false;
    if (buf->undo != NULL)
        for (i = 0U; i < buf->undo->nodes.len; i++)
            if ((buf->undo->nodes.data[i].flags & YEW_TXN_DEAD) == 0U) live++;
    /*
     * UndoTree exposes no count helper.  This is the same count
     * yew_undo_dump prints: every non-DEAD node, including the root.
     */
    if (want < 0 || live != (u64)want) {
        (void)snprintf(msg, sizeof(msg), "want %lld undo nodes; got %llu",
                       (long long)want, (unsigned long long)live); fail(vm, msg);
    }
    *out = FL_NIL_V; return true;
}

static bool has_nul(const FlStr *s)
{
    return s->len != 0U && memchr(s->b, '\0', s->len) != NULL;
}

static bool read_path(const char *path, Bytebuf *out)
{
    u8 block[8192];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));
        if (n > 0) bytebuf_append(out, block, (size_t)n);
        else if (n == 0) break;
        else if (errno != EINTR) { int e = errno; (void)close(fd); errno = e; return false; }
    }
    return close(fd) == 0;
}

static bool t_file(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *path = NULL; const FlStr *want = NULL;
    Bytebuf got; char *owned;
    (void)n; assertion(vm);
    if (!fl_arg_str(vm, a, 0U, &path) ||
        !fl_arg_str(vm, a, 1U, &want)) return false;
    if (has_nul(path)) { fail(vm, "file path contains NUL"); *out = FL_NIL_V; return true; }
    owned = yew_xmalloc((size_t)path->len + 1U);
    (void)memcpy(owned, path->b, path->len); owned[path->len] = '\0';
    bytebuf_init(&got);
    if (!read_path(owned, &got)) {
        Bytebuf b; int e = errno; bytebuf_init(&b);
        bytebuf_printf(&b, "cannot read %s: %s", owned, strerror(e));
        fail_n(vm, (const char *)b.data, b.len); bytebuf_free(&b);
    } else if (got.len != want->len ||
               (got.len != 0U && memcmp(got.data, want->b, got.len) != 0)) {
        fail_bytes(vm, want, &got);
    }
    bytebuf_free(&got); yew_xfree(owned); *out = FL_NIL_V; return true;
}

static const FlStr *error_kind(FlVm *vm, FlValue error)
{
    FlValue got = FL_NIL_V;
    FlValue key;
    if (error.t != (u8)FL_MAP) return NULL;
    key = FL_OBJ_V(FL_STR, fl_str_new(vm, "kind", 4U));
    if (!fl_map_get((FlMap *)error.as.o, key, &got) || got.t != (u8)FL_STR)
        return NULL;
    return (const FlStr *)got.as.o;
}

static bool t_raises(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *want = NULL; FlValue fn = FL_NIL_V;
    FlValue ignored = FL_NIL_V;
    FlValue saved = vm->err; const char *saved_caret = vm->err_caret;
    u32 saved_native = vm->cur_native; bool raised; const FlStr *got = NULL;
    (void)n; assertion(vm);
    if (!fl_arg_str(vm, a, 0U, &want) || !fl_arg_fn(vm, a, 1U, &fn))
        return false;
    raised = !fl_call(vm, fn, NULL, 0U, &ignored);
    if (raised) got = error_kind(vm, vm->err);
    vm->err = saved; vm->err_caret = saved_caret; vm->cur_native = saved_native;
    if (!raised) {
        fail(vm, "expected function to raise; it returned");
    } else if (got == NULL || got->len != want->len ||
               (got->len != 0U && memcmp(got->b, want->b, got->len) != 0)) {
        Bytebuf b; bytebuf_init(&b);
        bytebuf_printf(&b, "want error kind %.*s; got %.*s",
                       (int)want->len, want->b,
                       got == NULL ? 1 : (int)got->len,
                       got == NULL ? "?" : got->b);
        fail_n(vm, (const char *)b.data, b.len); bytebuf_free(&b);
    }
    *out = FL_NIL_V; return true;
}

static bool parse_level(const FlStr *s, YewLogLevel *out)
{
    static const char *const names[] = {"debug", "info", "warn", "error"};
    u32 i;
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        size_t n = strlen(names[i]);
        if (s->len == n && memcmp(s->b, names[i], n) == 0) {
            *out = (YewLogLevel)i; return true;
        }
    }
    return false;
}

static bool contains(const char *haystack, const FlStr *needle)
{
    size_t hn = strlen(haystack);
    size_t i;
    if (needle->len == 0U) return true;
    if ((size_t)needle->len > hn) return false;
    for (i = 0U; i <= hn - needle->len; i++)
        if (memcmp(haystack + i, needle->b, needle->len) == 0) return true;
    return false;
}

static bool t_log(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    YewBatchTestState *s = state_for(vm);
    const FlStr *level = NULL; const FlStr *sub = NULL;
    YewLogLevel wanted; u32 i; bool found = false; (void)n; assertion(vm);
    if (!fl_arg_str(vm, a, 0U, &level) ||
        !fl_arg_str(vm, a, 1U, &sub)) return false;
    if (!parse_level(level, &wanted)) fail(vm, "unknown log level");
    else {
        for (i = 0U; i < s->nlogs; i++)
            if (s->logs[i].level == (u8)wanted &&
                contains(s->logs[i].message, sub)) { found = true; break; }
        if (!found) fail(vm, "matching log line was not emitted");
    }
    *out = FL_NIL_V; return true;
}

static bool write_all(int fd, const u8 *data, size_t len)
{
    while (len != 0U) {
        ssize_t n = write(fd, data, len);
        if (n > 0) { data += (size_t)n; len -= (size_t)n; }
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

static bool copy_path(const char *source, const char *dest)
{
    Bytebuf b; int fd; bool ok;
    bytebuf_init(&b);
    if (!read_path(source, &b)) { bytebuf_free(&b); return false; }
    fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { bytebuf_free(&b); return false; }
    ok = write_all(fd, b.data, b.len);
    if (close(fd) != 0) ok = false;
    bytebuf_free(&b); return ok;
}

static bool fixture_name_ok(const FlStr *s)
{
    if (s->len == 0U || has_nul(s) || memchr(s->b, '/', s->len) != NULL)
        return false;
    return !(s->len == 1U && s->b[0] == '.') &&
           !(s->len == 2U && memcmp(s->b, "..", 2U) == 0);
}

static bool t_fixture(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *name = NULL; char *cwd; char *source; char *dest;
    size_t sn, dn; bool ok; (void)n;
    if (!fl_arg_str(vm, a, 0U, &name)) return false;
    if (!fixture_name_ok(name))
        return fl_raise(vm, "io", "t.fixture: name must be one path component");
    cwd = yew_xgetcwd();
    if (cwd == NULL)
        return fl_raise(vm, "io", "t.fixture: cannot read cwd: %s", strerror(errno));
    sn = strlen(cwd) + 11U + name->len; dn = strlen(cwd) + 2U + name->len;
    source = yew_xmalloc(sn); dest = yew_xmalloc(dn);
    (void)snprintf(source, sn, "%s/fixtures/%.*s", cwd, (int)name->len, name->b);
    (void)snprintf(dest, dn, "%s/%.*s", cwd, (int)name->len, name->b);
    ok = copy_path(source, dest); yew_xfree(source); yew_xfree(cwd);
    if (!ok) { int e = errno; yew_xfree(dest);
        return fl_raise(vm, "io", "t.fixture: copy failed: %s", strerror(e)); }
    *out = FL_OBJ_V(FL_STR, fl_str_new(vm, dest, (u32)strlen(dest)));
    yew_xfree(dest); return true;
}

static bool t_tmpdir(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const char *path = getenv("YEW_SCRIPT_TMPDIR"); (void)a; (void)n;
    if (path == NULL || path[0] == '\0') path = getenv("TMPDIR");
    if (path == NULL || path[0] == '\0') path = "/tmp";
    *out = FL_OBJ_V(FL_STR, fl_str_new(vm, path, (u32)strlen(path))); return true;
}

/*
 * A batch script normally runs to completion without entering loop.c.  LSP
 * integration tests still need the same process and pipe mechanics as an
 * interactive turn, so this test-only native drives precisely the job/LSP
 * portion of that loop.  The caller supplies the wall-clock budget; both the
 * per-call cap and the monotonic deadline keep a broken fixture bounded.
 */
static bool t_pump(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    enum {
        PUMP_MAX_MS = 5000,
        PUMP_SLICE_MS = 10,
        PUMP_INSTRUMENTED_SCALE = 8
    };
    Ed *ed = vm->ed;
    i64 duration = 0;
    i64 scale;
    i64 start;
    i64 deadline;
    bool first = true;

    (void)n;
    if (!fl_arg_int(vm, a, 0U, &duration))
        return false;
    if (duration < 0 || duration > PUMP_MAX_MS)
        return fl_raise(vm, "range", "t.pump: milliseconds must be 0..%u",
                        (unsigned)PUMP_MAX_MS);
    if (ed == NULL)
        return fl_raise(vm, "handle", "t.pump: no editor is attached");
    start = yew_now_ms();
    if (start < 0)
        return fl_raise(vm, "io", "t.pump: monotonic clock failed");
    scale = getenv("YEW_TEST_INSTRUMENTED") == NULL ? 1 :
            PUMP_INSTRUMENTED_SCALE;
    deadline = start + duration * scale;
    for (;;) {
        struct pollfd pfd[YEW_JOB_MAX * 4U];
        u32 nfds = 0U;
        i64 now = yew_now_ms();
        int polled;

        if (!first && now >= deadline)
            break;
        first = false;
        yew_job_collect_fds(ed, pfd, &nfds);
        for (;;) {
            i64 remain;
            i64 job_wait;
            int timeout;

            now = yew_now_ms();
            remain = deadline - now;
            timeout = remain <= 0 ? 0 :
                      remain < PUMP_SLICE_MS ? (int)remain : PUMP_SLICE_MS;
            job_wait = yew_job_deadline(ed, now);
            if (job_wait >= 0 && job_wait < timeout)
                timeout = (int)job_wait;
            polled = poll(pfd, (nfds_t)nfds, timeout);
            if (polled >= 0 || errno != EINTR)
                break;
        }
        if (polled < 0)
            return fl_raise(vm, "io", "t.pump: poll failed: %s",
                            strerror(errno));
        now = yew_now_ms();
        ed->now_ms = now;
        yew_job_pump(ed, pfd, nfds);
        yew_job_reap(ed);
        yew_job_tick(ed, now);
        yew_job_settle(ed);
        yew_lsp_pump(ed);
    }
    *out = FL_NIL_V;
    return true;
}

/* Exact, generic observation of subprocess stderr for integration scripts. */
static bool t_job_stderr(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *want = NULL;
    Bytebuf got;
    u32 i;

    (void)n;
    assertion(vm);
    if (!fl_arg_str(vm, a, 0U, &want))
        return false;
    bytebuf_init(&got);
    if (vm->ed != NULL) {
        for (i = 0U; i < vm->ed->jobs.len; i++) {
            const YewJob *job = &vm->ed->jobs.v[i];

            bytebuf_append(&got, job->framed_err.data, job->framed_err.len);
        }
    }
    if (got.len != want->len ||
        (got.len != 0U && memcmp(got.data, want->b, got.len) != 0))
        fail_bytes(vm, want, &got);
    bytebuf_free(&got);
    *out = FL_NIL_V;
    return true;
}

static bool t_skip(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *reason; (void)n;
    if (!fl_arg_str(vm, a, 0U, &reason)) return false;
    (void)reason; state_for(vm)->skipped = true; *out = FL_NIL_V; return true;
}

static const TestNativeDef TEST_NATIVES[] = {
    {"eq", t_eq, 2U, 3U}, {"ne", t_ne, 2U, 2U},
    {"text", t_text, 2U, 2U}, {"line", t_line, 3U, 3U},
    {"cursor", t_cursor, 3U, 3U}, {"cursors", t_cursors, 2U, 2U},
    {"sel", t_sel, 2U, 2U}, {"reg", t_reg, 2U, 3U},
    {"undo", t_undo, 2U, 2U}, {"file", t_file, 2U, 2U},
    {"raises", t_raises, 2U, 2U}, {"log", t_log, 2U, 2U},
    {"fixture", t_fixture, 1U, 1U}, {"tmpdir", t_tmpdir, 0U, 0U},
    {"pump", t_pump, 1U, 1U}, {"job_stderr", t_job_stderr, 1U, 1U},
    {"skip", t_skip, 1U, 1U}
};

void yew_batch_test_init(YewBatchTestState *s)
{
    if (s == NULL) YEW_BUG("batch test init: NULL state");
    (void)memset(s, 0, sizeof(*s)); bytebuf_init(&s->failure_records);
}

bool yew_batch_test_install(YewBatchTestState *s, FlVm *vm)
{
    FlMap *module; u32 i;
    if (s == NULL || vm == NULL || s->installed ||
        (active_test != NULL && active_test != s)) return false;
    s->vm = vm; active_test = s; module = fl_map_new(vm);
    fl_gc_protect(vm, FL_OBJ_V(FL_MAP, module));
    for (i = 0U; i < YEW_ARRAY_LEN(TEST_NATIVES); i++) {
        const TestNativeDef *d = &TEST_NATIVES[i];
        FlNative *nat = fl_gc_alloc(vm, sizeof(*nat), FL_NATIVE);
        char qualified[48]; int qn = snprintf(qualified, sizeof(qualified),
                                               "t.%s", d->name);
        nat->fn = d->fn;
        nat->name_id = yew_intern(vm->in, qualified, (size_t)(qn < 0 ? 0 : qn));
        nat->min_ar = d->min_ar; nat->max_ar = d->max_ar;
        nat->has_recv = 0U; nat->rsv = 0U; nat->caps = 0U; nat->recv = FL_NIL_V;
        (void)fl_map_set(vm, module,
                         FL_OBJ_V(FL_STR, fl_str_new(vm, d->name,
                                                    (u32)strlen(d->name))),
                         FL_OBJ_V(FL_NATIVE, nat));
    }
    module->h.oflags |= (u16)FL_OF_FROZEN;
    (void)fl_map_set(vm, vm->globals,
                     FL_INT_V((i64)yew_intern(vm->in, "t", 1U)),
                     FL_OBJ_V(FL_MAP, module));
    fl_gc_release(vm, 1U); s->installed = true; return true;
}

void yew_batch_test_note_log(YewBatchTestState *s, YewLogLevel level,
                             const char *message)
{
    YewBatchTestLog *entry; size_t len;
    if (s == NULL || message == NULL || s->finished) return;
    if (s->nlogs == s->caplogs) {
        u32 cap = s->caplogs == 0U ? 16U : s->caplogs * 2U;
        s->logs = yew_xreallocarray(s->logs, cap, sizeof(*s->logs));
        s->caplogs = cap;
    }
    entry = &s->logs[s->nlogs++]; len = strlen(message);
    entry->message = yew_xmalloc(len + 1U);
    (void)memcpy(entry->message, message, len + 1U); entry->level = (u8)level;
}

static int result_fd(int fd)
{
    const char *v; char *end; long parsed;
    if (fd >= 0) return fd;
    v = getenv("YEW_SCRIPT_RESULT_FD");
    if (v == NULL || v[0] == '\0') return STDERR_FILENO;
    errno = 0; end = NULL; parsed = strtol(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0' || parsed < 0 ||
        parsed > INT_MAX) return STDERR_FILENO;
    return (int)parsed;
}

bool yew_batch_test_finish(YewBatchTestState *s, int fd)
{
    Bytebuf summary; u64 failures; bool io_ok = true;
    bool close_result = fd < 0;
    if (s == NULL || !s->installed) return false;
    if (!s->finished && !s->skipped && s->assertions == 0U) {
        static const char none[] = "FAIL\tt.?\tno assertions\n";
        s->failures++; bytebuf_append(&s->failure_records, none, sizeof(none) - 1U);
    }
    s->finished = true; failures = s->skipped ? 0U : s->failures;
    fd = result_fd(fd);
    if (!s->skipped)
        io_ok = write_all(fd, s->failure_records.data, s->failure_records.len);
    bytebuf_init(&summary);
    bytebuf_printf(&summary, "YEWTEST\t%llu\t%llu\t%u\n",
                   (unsigned long long)s->assertions,
                   (unsigned long long)failures, s->skipped ? 1U : 0U);
    if (!write_all(fd, summary.data, summary.len)) io_ok = false;
    bytebuf_free(&summary);
    if (close_result && fd > STDERR_FILENO && close(fd) != 0)
        io_ok = false;
    return io_ok && (s->skipped || failures == 0U);
}

void yew_batch_test_free(YewBatchTestState *s)
{
    u32 i;
    if (s == NULL) return;
    if (active_test == s) active_test = NULL;
    for (i = 0U; i < s->nlogs; i++) yew_xfree(s->logs[i].message);
    yew_xfree(s->logs); bytebuf_free(&s->failure_records);
    (void)memset(s, 0, sizeof(*s));
}
