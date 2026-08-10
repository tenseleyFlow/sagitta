/*
 * Sprint 31 deliverable 6: the `fmt` module -- a bespoke formatter.
 *
 * A TEMPLATE IS DATA, NEVER A FORMAT.  fmt.f interprets the §6 directive
 * grammar itself; a `%n` inside a user template is a percent sign and an
 * `n`.  The only libc conversions in this file are the `f`/`e` paths,
 * and both build a format that is a LITERAL in this source with a
 * precision already clamped to 0..17.  scripts/bans.sh enforces that
 * shape for every call in src/fl/, and carries a seeded violation so the
 * rule is known to fire rather than merely present.
 *
 * TOTALITY.  fmt.f never reads outside its template, never loops without
 * consuming a byte, and ends in a string or a raise: a malformed
 * directive is kind "type" quoting the directive and its byte offset, a
 * missing argument is "index", and a result past 64 MiB is "limit".
 * Emitting a bad directive as literal text was rejected -- a statusline
 * reading `{tabwidth` is a bug report either way, and the raise names
 * the offset.
 *
 * THE repr LAW: fmt.repr's output re-reads through fl_parse_literal as a
 * value equal to its input, or fmt.repr raises.  It never returns text
 * that will not parse.  §12's grammar is narrower than §4's value space
 * in five places, and each one raises rather than emitting something
 * that looks fine until a state file fails to load:
 *
 *   - functions, natives and the reserved handles (a state file must not
 *     pretend to hold code),
 *   - cycles, named by path,
 *   - non-finite floats: §1.4 has no spelling for inf or nan,
 *   - bool map keys and NEGATIVE int map keys: pl_entry admits IDENT,
 *     STRING and INT only, and the sign is part of pl_value rather than
 *     of a key,
 *   - INT64_MIN: its magnitude is one past INT64_MAX and §1.4 makes an
 *     integer literal that does not fit a compile error, so the text
 *     `-9223372036854775808` does not lex.
 *
 * fmt.str does NOT raise -- display is best-effort by design, so a cycle
 * there elides to `[...]` and a function prints as `<fn name>`.  The
 * asymmetry is the point: one of these two is a serializer.
 *
 * THE PRETTY FORM IS THE SPRINT 25 WORKSPACE FORMAT, byte for byte at
 * indent 2: two spaces per level, one entry per line, `key: value`, a
 * trailing comma after every element, closers on their own line at the
 * parent's indent.  Sprint 36 swaps this in under s25's hand-written
 * emitter, so those are format rules and not style.  The one seam is the
 * document's final newline, which belongs to the writer rather than to
 * the value being rendered.
 */
#include "fl/std.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl/gc.h"
#include "fl/lex.h"
#include "unicode/grapheme.h"
#include "unicode/width.h"
#include "util/intern.h"

enum {
    /* Shared with str: the point where a runaway repeat stops being a
     * formatting request and starts being an out-of-memory. */
    FMT_MAX = 64U * 1024U * 1024U,
    /* A pad wider than this is a runaway, not an alignment. */
    FMT_MAX_WIDTH = 1U << 20,
    /* Cycles are caught by the open-container stack, so this is only the
     * backstop for a legitimately deep value. */
    FMT_MAX_DEPTH = 64,
    /* IEEE 754 double: 17 significant digits always round-trip. */
    FMT_MAX_PREC = 17
};

/*
 * Tabs measure as ONE cell here, not as a jump to the next tab stop.
 *
 * fmt.pad has no idea where in a line its result will land, so there is
 * no tab stop to jump to; assuming column zero would make the same call
 * align differently depending on text the formatter never saw.
 */
enum { FMT_TABW = 1U };

static const char HEXLO[] = "0123456789abcdef";

/* ---------------------------------------------------------------- */
/* Small emitters                                                   */
/* ---------------------------------------------------------------- */

static void put(Bytebuf *b, const char *s)
{
    bytebuf_append(b, s, strlen(s));
}

static void hex2(Bytebuf *b, u8 c)
{
    put(b, "\\x");
    bytebuf_push_u8(b, (u8)HEXLO[(c >> 4) & 0x0FU]);
    bytebuf_push_u8(b, (u8)HEXLO[c & 0x0FU]);
}

/*
 * The qualified name of the native running now.
 *
 * Range and limit messages name themselves through this rather than
 * through a literal, so fmt.hex delegating to fmt.int's body still
 * reports "fmt.hex: width must be ..." -- a message naming a function
 * the user did not call sends them to the wrong line.
 */
static const char *me(const FlVm *vm)
{
    const char *s = yew_intern_str(vm->in, vm->cur_native);

    return s == NULL ? "fmt" : s;
}

static FlValue take(FlVm *vm, Bytebuf *bb)
{
    FlValue v = FL_OBJ_V(FL_STR, fl_str_take(vm, bb));

    bytebuf_free(bb);
    return v;
}

/* True when the buffer has outgrown the cap; frees it and raises. */
static bool over(FlVm *vm, Bytebuf *bb, const char *who)
{
    if (bb->len <= (size_t)FMT_MAX)
        return false;
    bytebuf_free(bb);
    (void)fl_raise(vm, "limit", "%s: result exceeds 64 MiB", who);
    return true;
}

/*
 * Bespoke, because the radix set runs to 36 and no libc conversion
 * covers base 3.  The magnitude accumulates in u64 so INT64_MIN has one
 * -- negating it as i64 is undefined behaviour, and it is the value a
 * fuzzer reaches first.
 */
static void emit_radix(Bytebuf *out, i64 v, u32 base, bool upper)
{
    static const char LO[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    static const char UP[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const char *dig = upper ? UP : LO;
    char tmp[64];
    u32 n = 0U;
    u64 m = v < 0 ? (u64)0 - (u64)v : (u64)v;

    do {
        tmp[n++] = dig[m % (u64)base];
        m /= (u64)base;
    } while (m != 0U && n < (u32)sizeof(tmp));
    if (v < 0)
        bytebuf_push_u8(out, (u8)'-');
    while (n-- > 0U)
        bytebuf_push_u8(out, (u8)tmp[n]);
}

/*
 * THE LIBC CONVERSION SITES.  Both formats are literals; `prec` is an
 * int already clamped into 0..17 by every caller and clamped again here,
 * because the clamp is the reason this is safe and it must not depend on
 * a caller remembering.
 *
 * The buffer is sized for the worst case a double can produce: %.17f of
 * DBL_MAX is 309 integer digits, a point and 17 fraction digits, so 512
 * leaves room to spare and the truncation branch is unreachable.
 */
static void emit_conv(Bytebuf *out, double v, int prec, bool sci)
{
    char buf[512];
    int n;

    if (prec < 0)
        prec = 0;
    if (prec > FMT_MAX_PREC)
        prec = FMT_MAX_PREC;
    if (sci)
        n = snprintf(buf, sizeof(buf), "%.*e", prec, v);
    else
        n = snprintf(buf, sizeof(buf), "%.*f", prec, v);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    bytebuf_append(out, buf, (size_t)n);
}

/*
 * Shortest text that reads back as the same double.
 *
 * 15 digits then 16 then 17, taking the first that round-trips, which is
 * the standard result: every double is recovered by 17 significant
 * digits, and most need fewer.  strtod is locale-sensitive in principle
 * and deterministic here in fact -- the process never leaves the C
 * locale, which is why scripts/bans.sh refuses the call that would
 * change it.
 */
static void emit_shortest(Bytebuf *out, double v)
{
    char buf[64];
    const char *e;
    int p;
    int n = 0;

    for (p = 15; p <= FMT_MAX_PREC; p++) {
        n = snprintf(buf, sizeof(buf), "%.*g", p, v);
        if (n < 0 || (size_t)n >= sizeof(buf)) {
            n = 0;
            break;
        }
        if (strtod(buf, NULL) == v)
            break;
    }
    if (n <= 0) {
        put(out, "0.0");
        return;
    }
    /*
     * §1.4's FLOAT is `digits "." digits [exp]` -- the point is not
     * optional, and %g drops it whenever the value is integral.  Putting
     * it back is what makes 3.0 print as `3.0` rather than as `3`, which
     * would read back an int and change the value's type.
     */
    e = memchr(buf, 'e', (size_t)n);
    if (e == NULL) {
        bytebuf_append(out, buf, (size_t)n);
        if (memchr(buf, '.', (size_t)n) == NULL)
            put(out, ".0");
        return;
    }
    bytebuf_append(out, buf, (size_t)(e - buf));
    if (memchr(buf, '.', (size_t)(e - buf)) == NULL)
        put(out, ".0");
    bytebuf_append(out, e, (size_t)n - (size_t)(e - buf));
}

/* ---------------------------------------------------------------- */
/* str / repr                                                       */
/* ---------------------------------------------------------------- */

/*
 * The escape table, matching src/ws/fl_emit.c exactly: `"` `\` `\n` `\t`
 * `\r` `\0`, bytes below 0x20 and 0x7F as `\xNN` in lowercase hex, and
 * EVERY other byte verbatim.
 *
 * Byte-oriented on purpose.  Valid UTF-8 rides through unmangled, and so
 * does an invalid byte -- the Fletch lexer copies a non-escape byte into
 * the string verbatim, so a path that is not UTF-8 survives the round
 * trip as itself rather than as \xNN soup or a U+FFFD.  That is
 * invariant 2 at the serializer.
 */
static void emit_quoted(Bytebuf *out, const char *s, u32 n)
{
    u32 i;

    bytebuf_push_u8(out, (u8)'"');
    for (i = 0U; i < n; i++) {
        u8 c = (u8)s[i];

        switch (c) {
        case (u8)'"':  put(out, "\\\""); break;
        case (u8)'\\': put(out, "\\\\"); break;
        case (u8)'\n': put(out, "\\n"); break;
        case (u8)'\t': put(out, "\\t"); break;
        case (u8)'\r': put(out, "\\r"); break;
        case 0U:       put(out, "\\0"); break;
        default:
            if (c < 0x20U || c == 0x7FU)
                hex2(out, c);
            else
                bytebuf_push_u8(out, c);
            break;
        }
    }
    bytebuf_push_u8(out, (u8)'"');
}

/* A map key may be written bare only when it lexes back as an IDENT.
 * `{nil: 1}` is not the map it looks like; it is a parse error. */
static bool bare_key(const char *s, u32 n)
{
    u32 i;

    if (n == 0U)
        return false;
    for (i = 0U; i < n; i++) {
        char c = s[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     c == '_';

        if (!alpha && !(i > 0U && c >= '0' && c <= '9'))
            return false;
    }
    return !fl_kw_reserved(s, (size_t)n);
}

typedef struct Rw {
    FlVm *vm;
    Bytebuf *out;
    /*
     * QUOTING and REFUSING are separate axes, because the REPL wants
     * one of each: repr's quoting, so a result can be pasted back in,
     * and display's elision, so a cyclic value prints rather than
     * raising at the prompt.  Folding them into one `strict` flag made
     * that combination unreachable.
     */
    bool quote;
    bool refuse;
    /* Depth past which a container prints as an ellipsis.  fmt's own
     * entry points pass the backstop; the REPL passes 8. */
    u32 max_depth;
    u32 indent;              /* spaces per level; 0 = single line       */
    Bytebuf path;            /* ".tabs[0].parent", for the cycle raise  */
    /* The containers between the root and here.  Membership IS the
     * cycle test: a value reachable from itself appears twice on the
     * way down, and nothing else does. */
    const FlObj *open[FMT_MAX_DEPTH];
    u32 nopen;
} Rw;

static bool rw_value(Rw *w, FlValue v);

static void rw_newline(Rw *w, u32 depth)
{
    u32 i;

    if (w->indent == 0U)
        return;
    bytebuf_push_u8(w->out, (u8)'\n');
    for (i = 0U; i < depth * w->indent; i++)
        bytebuf_push_u8(w->out, (u8)' ');
}

static bool rw_cycle(Rw *w)
{
    return fl_raise(w->vm, "type", "fmt.repr: cycle at %.*s",
                    (int)w->path.len,
                    w->path.len == 0U ? "" : (const char *)w->path.data);
}

/* Pushes `o` and reports whether it was already on the way down. */
static bool rw_enter(Rw *w, const FlObj *o, bool *dup)
{
    u32 i;

    *dup = false;
    for (i = 0U; i < w->nopen; i++) {
        if (w->open[i] == o) {
            *dup = true;
            return true;
        }
    }
    if (w->nopen >= w->max_depth) {
        if (w->refuse)
            return fl_raise(w->vm, "limit",
                            "fmt.repr: nested deeper than %u",
                            (unsigned)w->max_depth);
        *dup = true;      /* display gives up quietly rather than lying */
        return true;
    }
    w->open[w->nopen++] = o;
    return true;
}

static bool rw_list(Rw *w, FlList *l)
{
    u32 depth;
    u32 i;
    bool dup;

    if (!rw_enter(w, &l->h, &dup))
        return false;
    if (dup) {
        if (w->refuse)
            return rw_cycle(w);
        put(w->out, "[...]");
        return true;
    }
    depth = w->nopen;
    bytebuf_push_u8(w->out, (u8)'[');
    for (i = 0U; i < l->n; i++) {
        size_t mark = w->path.len;
        char ix[24];
        int k = snprintf(ix, sizeof(ix), "[%u]", (unsigned)i);

        if (k > 0)
            bytebuf_append(&w->path, ix, (size_t)k);
        rw_newline(w, depth);
        if (!rw_value(w, l->v[i]))
            return false;
        /* A comma after EVERY element in the pretty form, so a diff
         * between two sessions is a line diff and not a line diff plus
         * comma churn.  The single-line form drops the last one. */
        if (w->indent != 0U || i + 1U < l->n)
            bytebuf_push_u8(w->out, (u8)',');
        if (w->indent == 0U && i + 1U < l->n)
            bytebuf_push_u8(w->out, (u8)' ');
        w->path.len = mark;
    }
    rw_newline(w, depth - 1U);
    bytebuf_push_u8(w->out, (u8)']');
    w->nopen--;
    return true;
}

static bool rw_key(Rw *w, FlValue k)
{
    switch ((FlType)k.t) {
    case FL_STR: {
        const FlStr *s = (const FlStr *)k.as.o;

        if (bare_key(s->b, s->len))
            bytebuf_append(w->out, s->b, (size_t)s->len);
        else
            emit_quoted(w->out, s->b, s->len);
        return true;
    }
    case FL_INT:
        /*
         * §12's pl_entry is `( IDENT | STRING | INT ) ":"`, with no sign
         * -- the minus belongs to pl_value.  So a negative key has no
         * spelling, and emitting one would produce a file that fails to
         * load rather than a file that loads wrong.
         */
        if (w->refuse && k.as.i < 0)
            return fl_raise(w->vm, "type",
                            "fmt.repr: map key %lld is negative, which "
                            "pure-literal syntax cannot spell",
                            (long long)k.as.i);
        emit_radix(w->out, k.as.i, 10U, false);
        return true;
    case FL_BOOL:
        if (w->refuse)
            return fl_raise(w->vm, "type",
                            "fmt.repr: map key %s is a bool, which "
                            "pure-literal syntax cannot spell",
                            k.as.b ? "true" : "false");
        put(w->out, k.as.b ? "true" : "false");
        return true;
    default:
        /* fl_hashable already closed the key set to these three. */
        put(w->out, "?");
        return true;
    }
}

static bool rw_map(Rw *w, FlMap *m)
{
    u32 depth;
    u32 cursor = 0U;
    u32 seen = 0U;
    u32 live;
    FlValue k;
    FlValue v;
    bool dup;

    if (!rw_enter(w, &m->h, &dup))
        return false;
    if (dup) {
        if (w->refuse)
            return rw_cycle(w);
        put(w->out, "{...}");
        return true;
    }
    depth = w->nopen;
    live = fl_map_count(m);
    bytebuf_push_u8(w->out, (u8)'{');
    while (fl_map_iter(m, &cursor, &k, &v)) {
        size_t mark = w->path.len;

        if (k.t == (u8)FL_STR) {
            const FlStr *s = (const FlStr *)k.as.o;

            bytebuf_push_u8(&w->path, (u8)'.');
            bytebuf_append(&w->path, s->b, (size_t)s->len);
        } else {
            char ix[32];
            int n = k.t == (u8)FL_INT
                        ? snprintf(ix, sizeof(ix), "[%lld]",
                                   (long long)k.as.i)
                        : snprintf(ix, sizeof(ix), "[%s]",
                                   k.as.b ? "true" : "false");

            if (n > 0)
                bytebuf_append(&w->path, ix, (size_t)n);
        }
        rw_newline(w, depth);
        if (!rw_key(w, k))
            return false;
        put(w->out, ": ");
        if (!rw_value(w, v))
            return false;
        seen++;
        if (w->indent != 0U || seen < live)
            bytebuf_push_u8(w->out, (u8)',');
        if (w->indent == 0U && seen < live)
            bytebuf_push_u8(w->out, (u8)' ');
        w->path.len = mark;
    }
    rw_newline(w, depth - 1U);
    bytebuf_push_u8(w->out, (u8)'}');
    w->nopen--;
    return true;
}

static bool rw_uncodeable(Rw *w, FlValue v)
{
    const char *nm = fl_type_name((FlType)v.t);

    if (w->refuse)
        return fl_raise(w->vm, "type",
                        "fmt.repr: a %s has no pure-literal form", nm);
    bytebuf_push_u8(w->out, (u8)'<');
    put(w->out, nm);
    if (v.t == (u8)FL_CLOSURE || v.t == (u8)FL_NATIVE) {
        u32 id = v.t == (u8)FL_NATIVE
                     ? ((const FlNative *)v.as.o)->name_id
                     : ((const FlClosure *)v.as.o)->fn->name_id;
        const char *s = id == 0U ? NULL : yew_intern_str(w->vm->in, id);

        if (s != NULL) {
            bytebuf_push_u8(w->out, (u8)' ');
            put(w->out, s);
        }
    }
    bytebuf_push_u8(w->out, (u8)'>');
    return true;
}

static bool rw_value(Rw *w, FlValue v)
{
    switch ((FlType)v.t) {
    case FL_NIL:
        put(w->out, "nil");
        return true;
    case FL_BOOL:
        put(w->out, v.as.b ? "true" : "false");
        return true;
    case FL_INT:
        /*
         * INT64_MIN has no literal: §1.4 caps an integer literal at
         * INT64_MAX and folds the sign afterwards, so the magnitude
         * 9223372036854775808 is a compile error wherever it appears.
         */
        if (w->refuse && v.as.i == (-9223372036854775807LL - 1LL))
            return fl_raise(w->vm, "type",
                            "fmt.repr: %lld has no integer literal; its "
                            "magnitude is one past the i64 maximum",
                            (long long)v.as.i);
        emit_radix(w->out, v.as.i, 10U, false);
        return true;
    case FL_FLOAT:
        /* inf and nan are values §1.4 cannot spell.  Display prints what
         * the platform calls them; repr refuses. */
        if (v.as.f != v.as.f || v.as.f > 1.7976931348623157e308 ||
            v.as.f < -1.7976931348623157e308) {
            if (w->refuse)
                return fl_raise(w->vm, "type",
                                "fmt.repr: %s has no float literal",
                                v.as.f != v.as.f
                                    ? "nan"
                                    : (v.as.f > 0.0 ? "inf" : "-inf"));
            put(w->out, v.as.f != v.as.f
                            ? "nan"
                            : (v.as.f > 0.0 ? "inf" : "-inf"));
            return true;
        }
        emit_shortest(w->out, v.as.f);
        return true;
    case FL_STR: {
        const FlStr *s = (const FlStr *)v.as.o;

        /*
         * Bare only at the ROOT of a display.  Inside a container the
         * quotes are what keep `["a, b"]` distinguishable from
         * `["a", "b"]`, and a display nobody can read back is worth
         * less than two quote characters.
         */
        if (!w->quote && w->nopen == 0U)
            bytebuf_append(w->out, s->b, (size_t)s->len);
        else
            emit_quoted(w->out, s->b, s->len);
        return true;
    }
    case FL_LIST:
        return rw_list(w, (FlList *)v.as.o);
    case FL_MAP:
        return rw_map(w, (FlMap *)v.as.o);
    default:
        return rw_uncodeable(w, v);
    }
}

/* Renders `v` into `out`.  Frees nothing it did not allocate; `out`
 * belongs to the caller either way. */
static bool render_at(FlVm *vm, Bytebuf *out, FlValue v, bool quote,
                      bool refuse, u32 indent, u32 max_depth)
{
    Rw w;
    bool ok;

    w.vm = vm;
    w.out = out;
    w.quote = quote;
    w.refuse = refuse;
    w.max_depth = max_depth;
    w.indent = indent;
    w.nopen = 0U;
    bytebuf_init(&w.path);
    ok = rw_value(&w, v);
    bytebuf_free(&w.path);
    return ok;
}

static bool render(FlVm *vm, Bytebuf *out, FlValue v, bool strict, u32 indent)
{
    return render_at(vm, out, v, strict, strict, indent, (u32)FMT_MAX_DEPTH);
}

bool fl_fmt_display(FlVm *vm, Bytebuf *out, FlValue v)
{
    return render(vm, out, v, false, 0U);
}

bool fl_fmt_repr(FlVm *vm, Bytebuf *out, FlValue v)
{
    return render(vm, out, v, true, 0U);
}

bool fl_fmt_repl(FlVm *vm, Bytebuf *out, FlValue v, u32 max_depth)
{
    /* repr's QUOTING with display's ELISION: at a prompt the difference
     * between the string "a\nb" and a two-line result is information
     * the user needs, and a cyclic value must print rather than raise
     * -- `let l = []` then `list.push(l, l)` is all it takes. */
    return render_at(vm, out, v, true, false, 0U,
                     max_depth == 0U ? (u32)FMT_MAX_DEPTH : max_depth);
}

/* ---------------------------------------------------------------- */
/* Padding                                                          */
/* ---------------------------------------------------------------- */

/*
 * Widths are DISPLAY CELLS, so `fmt.f("{:>10}", "漢字")` lines up in a
 * terminal where a byte or cluster count would leave it four columns
 * short.
 */
static int cells(const Bytebuf *b)
{
    return yew_str_width(b->data, b->len, FMT_TABW);
}

/*
 * `fill` is one grapheme cluster and may itself be wide.  Whole fills go
 * in first and the remainder is spaces: a two-cell fill against an odd
 * gap cannot tile it, and overshooting the requested width would break
 * the alignment the caller asked for.
 */
static void pad_with(Bytebuf *out, const char *fill, u32 filln, int gap)
{
    int fw = filln == 0U ? 1 : yew_cluster_width((const u8 *)fill, filln);
    int i;

    if (fw < 1)
        fw = 1;
    while (gap >= fw) {
        if (filln == 0U)
            bytebuf_push_u8(out, (u8)' ');
        else
            bytebuf_append(out, fill, (size_t)filln);
        gap -= fw;
    }
    for (i = 0; i < gap; i++)
        bytebuf_push_u8(out, (u8)' ');
}

static void align_into(Bytebuf *out, const Bytebuf *body, u32 width,
                       char align, const char *fill, u32 filln)
{
    int gap = (int)width - cells(body);

    if (gap <= 0) {
        bytebuf_append(out, body->data, body->len);
        return;
    }
    switch (align) {
    case '>':
        pad_with(out, fill, filln, gap);
        bytebuf_append(out, body->data, body->len);
        break;
    case '^':
        /* The odd cell goes on the RIGHT, so centring a growing string
         * moves it one way only. */
        pad_with(out, fill, filln, gap / 2);
        bytebuf_append(out, body->data, body->len);
        pad_with(out, fill, filln, gap - gap / 2);
        break;
    default:
        bytebuf_append(out, body->data, body->len);
        pad_with(out, fill, filln, gap);
        break;
    }
}

/* ---------------------------------------------------------------- */
/* The directive grammar (§6)                                       */
/* ---------------------------------------------------------------- */

typedef struct Spec {
    const char *fill;
    u32 filln;
    char align;       /* 0 = by type: numbers right, everything left    */
    char sign;        /* 0 | '+' | ' '                                  */
    bool zero;
    bool has_width;
    u32 width;
    bool has_prec;
    u32 prec;
    char type;        /* 0 = 's'                                        */
} Spec;

static bool bad_directive(FlVm *vm, const char *d, size_t n, size_t at)
{
    if (n > 24U)
        n = 24U;
    return fl_raise(vm, "type", "fmt.f: bad directive \"%.*s\" at byte %zu",
                    (int)n, d, at);
}

static bool is_align(char c)
{
    return c == '<' || c == '>' || c == '^';
}

static bool is_type(char c)
{
    return c == 's' || c == 'd' || c == 'x' || c == 'X' || c == 'o' ||
           c == 'b' || c == 'f' || c == 'e' || c == '%' || c == '?';
}

/* Reads an unsigned decimal, refusing anything that could overflow the
 * caps rather than wrapping into a small number. */
static bool read_num(const char *s, size_t n, size_t *i, u32 *out)
{
    u64 v = 0U;
    size_t start = *i;

    while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
        v = v * 10U + (u64)(s[*i] - '0');
        if (v > (u64)FMT_MAX_WIDTH)
            v = (u64)FMT_MAX_WIDTH + 1U;
        (*i)++;
    }
    if (*i == start)
        return false;
    *out = (u32)v;
    return true;
}

/*
 * spec = [ [fill] align ] [ sign ] [ "0" ] [ width ] [ "." prec ] [ type ]
 *
 * The fill/align pair is read by looking PAST the first cluster: `{:^^5}`
 * is a caret filling a centred field, `{:^5}` is a centred field with the
 * default fill, and only the second character can tell them apart.
 */
static bool parse_spec(FlVm *vm, const char *s, size_t n, Spec *sp,
                       const char *dir, size_t dirn, size_t at)
{
    size_t i = 0U;

    memset(sp, 0, sizeof(*sp));
    if (n > 0U) {
        size_t c1 = yew_gb_next_bytes((const u8 *)s, n, 0U);

        if (c1 < n && is_align(s[c1])) {
            sp->fill = s;
            sp->filln = (u32)c1;
            sp->align = s[c1];
            i = c1 + 1U;
        } else if (is_align(s[0])) {
            sp->align = s[0];
            i = 1U;
        }
    }
    if (i < n && (s[i] == '+' || s[i] == ' '))
        sp->sign = s[i++];
    if (i < n && s[i] == '0') {
        sp->zero = true;
        i++;
    }
    if (i < n && s[i] >= '0' && s[i] <= '9') {
        if (!read_num(s, n, &i, &sp->width))
            return bad_directive(vm, dir, dirn, at);
        sp->has_width = true;
        if (sp->width > (u32)FMT_MAX_WIDTH)
            return fl_raise(vm, "limit",
                            "fmt.f: field width at byte %zu exceeds %d cells",
                            at, FMT_MAX_WIDTH);
    }
    if (i < n && s[i] == '.') {
        i++;
        if (!read_num(s, n, &i, &sp->prec))
            return bad_directive(vm, dir, dirn, at);
        sp->has_prec = true;
        if (sp->prec > (u32)FMT_MAX_PREC)
            return bad_directive(vm, dir, dirn, at);
    }
    if (i < n && is_type(s[i]))
        sp->type = s[i++];
    if (i != n)
        return bad_directive(vm, dir, dirn, at);
    return true;
}

static bool numeric_type(char t)
{
    return t == 'd' || t == 'x' || t == 'X' || t == 'o' || t == 'b' ||
           t == 'f' || t == 'e' || t == '%';
}

static bool want_int(FlVm *vm, FlValue v, char t, size_t at, i64 *out)
{
    if (v.t == (u8)FL_INT) {
        *out = v.as.i;
        return true;
    }
    /* §6: `d` takes a float only when it is integral -- rounding here
     * would move a line number by one and never say so. */
    if (t == 'd' && v.t == (u8)FL_FLOAT) {
        double f = v.as.f;

        if (f == (double)(i64)f && f >= -9.2233720368547758e18 &&
            f <= 9.2233720368547758e18) {
            *out = (i64)f;
            return true;
        }
        return fl_raise(vm, "type",
                        "fmt.f: {:d} at byte %zu needs an integral value",
                        at);
    }
    return fl_raise(vm, "type",
                    "fmt.f: {:%c} at byte %zu needs an int, found %s", t, at,
                    fl_type_name((FlType)v.t));
}

/* Renders one value under `sp` into `out`. */
static bool fmt_one(FlVm *vm, Bytebuf *out, FlValue v, const Spec *sp,
                    size_t at)
{
    Bytebuf body;
    char type = sp->type == 0 ? 's' : sp->type;
    char align = sp->align;
    /*
     * "Numeric" is about the VALUE, not only the directive: `{:5}` on an
     * int right-aligns and `{:+}` on an int signs it, because a column
     * of numbers that aligns only when the writer remembered `:d` is a
     * column that silently misaligns.
     */
    bool num = numeric_type(type) ||
               (sp->type == 0 &&
                (v.t == (u8)FL_INT || v.t == (u8)FL_FLOAT));
    bool ok = true;
    bool neg;

    bytebuf_init(&body);
    switch (type) {
    case 's':
        ok = render(vm, &body, v, false, 0U);
        break;
    case '?':
        ok = render(vm, &body, v, true, 0U);
        break;
    case 'd': case 'x': case 'X': case 'o': case 'b': {
        i64 iv = 0;
        u32 base = type == 'd' ? 10U
                 : type == 'o' ? 8U
                 : type == 'b' ? 2U
                               : 16U;

        ok = want_int(vm, v, type, at, &iv);
        if (ok)
            emit_radix(&body, iv, base, type == 'X');
        break;
    }
    case 'f': case 'e': case '%': {
        double d = 0.0;

        if (v.t == (u8)FL_INT) {
            d = (double)v.as.i;
        } else if (v.t == (u8)FL_FLOAT) {
            d = v.as.f;
        } else {
            ok = fl_raise(vm, "type",
                          "fmt.f: {:%c} at byte %zu needs a number, found %s",
                          type, at, fl_type_name((FlType)v.t));
        }
        if (ok) {
            int prec = sp->has_prec ? (int)sp->prec : 6;

            if (type == '%')
                d *= 100.0;
            emit_conv(&body, d, prec, type == 'e');
            if (type == '%')
                bytebuf_push_u8(&body, (u8)'%');
        }
        break;
    }
    default:
        ok = false;
        break;
    }
    if (!ok) {
        bytebuf_free(&body);
        return false;
    }
    /* `+`/` ` apply to the numeric conversions only, and only when the
     * value did not bring its own sign. */
    neg = body.len > 0U && body.data[0] == (u8)'-';
    if (sp->sign != 0 && num && !neg) {
        Bytebuf signed_body;

        bytebuf_init(&signed_body);
        bytebuf_push_u8(&signed_body, (u8)sp->sign);
        bytebuf_append(&signed_body, body.data, body.len);
        bytebuf_free(&body);
        body = signed_body;
    }
    if (align == 0)
        align = num ? '>' : '<';
    if (!sp->has_width) {
        bytebuf_append(out, body.data, body.len);
        bytebuf_free(&body);
        return true;
    }
    /*
     * The zero flag pads AFTER the sign -- `-0042`, never `00-42` -- and
     * an explicit align wins over it, because a caller who wrote both
     * said what they wanted second.
     */
    if (sp->zero && sp->align == 0 && num) {
        int gap = (int)sp->width - cells(&body);
        size_t skip = (body.len > 0U && (body.data[0] == (u8)'-' ||
                                         body.data[0] == (u8)'+' ||
                                         body.data[0] == (u8)' '))
                          ? 1U
                          : 0U;

        if (gap > 0) {
            bytebuf_append(out, body.data, skip);
            pad_with(out, "0", 1U, gap);
            bytebuf_append(out, body.data + skip, body.len - skip);
        } else {
            bytebuf_append(out, body.data, body.len);
        }
    } else {
        align_into(out, &body, sp->width, align, sp->fill, sp->filln);
    }
    bytebuf_free(&body);
    return true;
}

/* ---------------------------------------------------------------- */
/* fmt.f                                                            */
/* ---------------------------------------------------------------- */

static bool resolve(FlVm *vm, FlValue *a, u32 argc, const char *nm,
                    size_t nmn, u32 *autoix, size_t at, FlValue *out)
{
    u32 ix;
    size_t i;
    bool digits = nmn > 0U;

    for (i = 0U; i < nmn; i++) {
        if (nm[i] < '0' || nm[i] > '9') {
            digits = false;
            break;
        }
    }
    if (nmn == 0U || digits) {
        if (nmn == 0U) {
            ix = (*autoix)++;
        } else {
            u32 v = 0U;
            size_t k = 0U;

            if (!read_num(nm, nmn, &k, &v) || k != nmn)
                return bad_directive(vm, nm, nmn, at);
            ix = v;
        }
        /* argv[0] is the template, so argument 0 is argv[1]. */
        if (ix + 1U >= argc)
            return fl_raise(vm, "index",
                            "fmt.f: directive at byte %zu wants argument %u, "
                            "%u given", at, (unsigned)ix, (unsigned)argc - 1U);
        *out = a[ix + 1U];
        return true;
    }
    /*
     * A named directive reads argument 0, which must be a map.  One map
     * rather than a keyword mechanism because Fletch has no keyword
     * arguments (§8) and inventing them for the formatter alone would
     * put a second calling convention in the language.
     */
    if (argc < 2U)
        return fl_raise(vm, "index",
                        "fmt.f: directive at byte %zu names \"%.*s\" but no "
                        "map was given", at, (int)nmn, nm);
    if (a[1].t != (u8)FL_MAP)
        return fl_raise(vm, "type",
                        "fmt.f: directive at byte %zu names \"%.*s\", so "
                        "argument 1 must be map, found %s", at, (int)nmn, nm,
                        fl_type_name((FlType)a[1].t));
    {
        FlMap *m = (FlMap *)a[1].as.o;
        FlStr *key = fl_str_new(vm, nm, (u32)nmn);

        if (!fl_map_get(m, FL_OBJ_V(FL_STR, key), out))
            return fl_raise(vm, "index",
                            "fmt.f: no key \"%.*s\" in argument 1", (int)nmn,
                            nm);
    }
    return true;
}

static bool f_f(FlVm *vm, FlValue *a, u32 argc, FlValue *out)
{
    const FlStr *t;
    Bytebuf o;
    size_t i = 0U;
    size_t n;
    u32 autoix = 0U;

    if (!fl_arg_str(vm, a, 0U, &t))
        return false;
    n = (size_t)t->len;
    bytebuf_init(&o);
    while (i < n) {
        char c = t->b[i];
        size_t start = i;
        size_t close;
        size_t colon;
        FlValue v;
        Spec sp;

        if (c != '{' && c != '}') {
            bytebuf_push_u8(&o, (u8)c);
            i++;
            if (over(vm, &o, "fmt.f"))
                return false;
            continue;
        }
        if (i + 1U < n && t->b[i + 1U] == c) {
            bytebuf_push_u8(&o, (u8)c);
            i += 2U;
            continue;
        }
        if (c == '}') {
            bytebuf_free(&o);
            return bad_directive(vm, t->b + i, 1U, i);
        }
        close = i + 1U;
        while (close < n && t->b[close] != '}')
            close++;
        if (close == n) {
            bytebuf_free(&o);
            return bad_directive(vm, t->b + start, n - start, start);
        }
        colon = i + 1U;
        while (colon < close && t->b[colon] != ':')
            colon++;
        if (!resolve(vm, a, argc, t->b + i + 1U, colon - (i + 1U), &autoix,
                     start, &v)) {
            bytebuf_free(&o);
            return false;
        }
        if (!parse_spec(vm, t->b + colon + (colon < close ? 1U : 0U),
                        colon < close ? close - colon - 1U : 0U, &sp,
                        t->b + start, close - start + 1U, start)) {
            bytebuf_free(&o);
            return false;
        }
        if (!fmt_one(vm, &o, v, &sp, start)) {
            bytebuf_free(&o);
            return false;
        }
        if (over(vm, &o, "fmt.f"))
            return false;
        i = close + 1U;
    }
    *out = take(vm, &o);
    return true;
}

/* ---------------------------------------------------------------- */
/* The scalar entry points                                          */
/* ---------------------------------------------------------------- */

static bool f_str(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Bytebuf b;

    (void)n;
    bytebuf_init(&b);
    if (!render(vm, &b, a[0], false, 0U)) {
        bytebuf_free(&b);
        return false;
    }
    if (over(vm, &b, "fmt.str"))
        return false;
    *out = take(vm, &b);
    return true;
}

static bool f_repr(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Bytebuf b;
    i64 indent = 0;

    if (n >= 2U) {
        if (!fl_arg_int(vm, a, 1U, &indent))
            return false;
        if (indent < 0 || indent > 16)
            return fl_raise(vm, "type",
                            "fmt.repr: indent must be 0..16, found %lld",
                            (long long)indent);
    }
    bytebuf_init(&b);
    if (!render(vm, &b, a[0], true, (u32)indent)) {
        bytebuf_free(&b);
        return false;
    }
    if (over(vm, &b, "fmt.repr"))
        return false;
    *out = take(vm, &b);
    return true;
}

static bool f_int(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Bytebuf b;
    i64 v;
    i64 base = 10;
    i64 width = 0;

    if (!fl_arg_int(vm, a, 0U, &v))
        return false;
    if (n >= 2U && !fl_arg_int(vm, a, 1U, &base))
        return false;
    if (n >= 3U && !fl_arg_int(vm, a, 2U, &width))
        return false;
    if (base < 2 || base > 36)
        return fl_raise(vm, "type", "%s: base must be 2..36, found %lld",
                        me(vm), (long long)base);
    if (width < 0 || width > (i64)FMT_MAX_WIDTH)
        return fl_raise(vm, "limit", "%s: width must be 0..%d, found %lld",
                        me(vm), FMT_MAX_WIDTH, (long long)width);
    bytebuf_init(&b);
    emit_radix(&b, v, (u32)base, false);
    if ((i64)b.len < width) {
        Bytebuf pad;
        size_t skip = b.data[0] == (u8)'-' ? 1U : 0U;

        bytebuf_init(&pad);
        bytebuf_append(&pad, b.data, skip);
        pad_with(&pad, "0", 1U, (int)(width - (i64)b.len));
        bytebuf_append(&pad, b.data + skip, b.len - skip);
        bytebuf_free(&b);
        b = pad;
    }
    *out = take(vm, &b);
    return true;
}

static bool f_hex(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    FlValue args[3];
    i64 width = 0;

    if (n >= 2U && !fl_arg_int(vm, a, 1U, &width))
        return false;
    args[0] = a[0];
    args[1] = FL_INT_V(16);
    args[2] = FL_INT_V(width);
    /* Routed through fmt.int rather than reimplemented, so the two can
     * never disagree about where the zeros go around a minus sign. */
    return f_int(vm, args, 3U, out);
}

static bool f_float(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Bytebuf b;
    double d;
    i64 prec = 0;

    if (!fl_arg_num(vm, a, 0U, &d))
        return false;
    if (n >= 2U) {
        if (!fl_arg_int(vm, a, 1U, &prec))
            return false;
        if (prec < 0 || prec > (i64)FMT_MAX_PREC)
            return fl_raise(vm, "type",
                            "fmt.float: prec must be 0..%d, found %lld",
                            FMT_MAX_PREC, (long long)prec);
    }
    bytebuf_init(&b);
    if (n >= 2U)
        emit_conv(&b, d, (int)prec, false);
    else
        emit_shortest(&b, d);
    *out = take(vm, &b);
    return true;
}

static bool f_pad(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    const FlStr *s;
    const FlStr *fill = NULL;
    const FlStr *al = NULL;
    Bytebuf body;
    Bytebuf o;
    i64 width;
    char align = '<';

    if (!fl_arg_str(vm, a, 0U, &s) || !fl_arg_int(vm, a, 1U, &width))
        return false;
    if (n >= 3U && !fl_arg_str(vm, a, 2U, &al))
        return false;
    if (n >= 4U && !fl_arg_str(vm, a, 3U, &fill))
        return false;
    if (width < 0 || width > (i64)FMT_MAX_WIDTH)
        return fl_raise(vm, "limit", "fmt.pad: width must be 0..%d, found %lld",
                        FMT_MAX_WIDTH, (long long)width);
    if (al != NULL) {
        if (al->len != 1U || !is_align(al->b[0]))
            return fl_raise(vm, "type",
                            "fmt.pad: align must be \"<\", \">\" or \"^\"");
        align = al->b[0];
    }
    /* A fill of more than one cluster has no single width to tile with,
     * so it is refused rather than silently truncated. */
    if (fill != NULL && fill->len > 0U &&
        yew_gb_next_bytes((const u8 *)fill->b, fill->len, 0U) !=
            (size_t)fill->len)
        return fl_raise(vm, "type",
                        "fmt.pad: fill must be one grapheme cluster");
    bytebuf_init(&body);
    bytebuf_append(&body, s->b, (size_t)s->len);
    bytebuf_init(&o);
    align_into(&o, &body, (u32)width, align,
               fill == NULL ? NULL : fill->b,
               fill == NULL ? 0U : fill->len);
    bytebuf_free(&body);
    if (over(vm, &o, "fmt.pad"))
        return false;
    *out = take(vm, &o);
    return true;
}

/* ---------------------------------------------------------------- */

static const FlNativeDef FMT_DEFS[] = {
    {"f",     f_f,     1U, FL_VARIADIC, 0U, "(template, ...) -> str"},
    {"str",   f_str,   1U, 1U, 0U, "(v) -> str"},
    {"repr",  f_repr,  1U, 2U, 0U, "(v, [indent]) -> str"},
    {"int",   f_int,   1U, 3U, 0U, "(i, [base], [width]) -> str"},
    {"float", f_float, 1U, 2U, 0U, "(f, [prec]) -> str"},
    {"hex",   f_hex,   1U, 2U, 0U, "(i, [width]) -> str"},
    {"pad",   f_pad,   2U, 4U, 0U, "(s, width, [align], [fill]) -> str"}
};

const FlModuleDef fl_mod_fmt = {
    "fmt", FMT_DEFS, (u32)YEW_ARRAY_LEN(FMT_DEFS), NULL, 0U
};
