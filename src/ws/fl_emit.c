/*
 * Sprint 25 §4: the emitter.
 *
 * Every layout rule here is part of the FORMAT, not a style choice —
 * Sprint 36 reimplements this against the same corpus and must produce
 * identical bytes.  So: two-space indent per depth, one key or element
 * per line, `key: value` with no space before the colon, closers on
 * their own line at the parent's indent, and a trailing comma after
 * EVERY element (which makes a diff between two sessions a line diff
 * rather than a line diff plus comma churn).
 */
#include "ws/fllit.h"

#include <stdio.h>
#include <string.h>

#include "util/log.h"

void sag_fl_emit_init(FlEmit *e, Bytebuf *out)
{
    if (e == NULL)
        return;
    e->out = out;
    e->depth = 0U;
    e->opened = 0U;
}

void sag_fl_emit_done(const FlEmit *e)
{
    if (e == NULL)
        return;
    /*
     * Unbalanced open/close is a BUG, not a warning.  The document
     * would still be bytes; it would just parse into a different shape
     * than the one the caller described, and the first thing to notice
     * would be a restore putting a pane's cursor somewhere else.
     */
    if (e->depth != 0U)
        SAG_BUG("fletch emitter: %u container(s) left open",
                (unsigned)e->depth);
}

static void emit_indent(FlEmit *e)
{
    u32 i;

    for (i = 0U; i < e->depth; i++)
        bytebuf_append(e->out, (const u8 *)"  ", 2U);
}

/* `key: ` at the current indent, or just the indent for a list
 * element. */
static void emit_key(FlEmit *e, const char *key)
{
    emit_indent(e);
    if (key == NULL)
        return;
    bytebuf_append(e->out, (const u8 *)key, strlen(key));
    bytebuf_append(e->out, (const u8 *)": ", 2U);
}

void sag_fl_map_open(FlEmit *e, const char *key)
{
    emit_key(e, key);
    bytebuf_append(e->out, (const u8 *)"{\n", 2U);
    e->depth++;
}

/*
 * The ROOT container closes without a trailing comma.
 *
 * The comma belongs to an element of something; the root is an element
 * of nothing, and emitting one there makes the document fail its own
 * parser with "trailing bytes" — which is exactly how this was found.
 */
static void emit_close(FlEmit *e, const char *closer)
{
    if (e->depth == 0U)
        SAG_BUG("fletch emitter: close without open");
    e->depth--;
    emit_indent(e);
    bytebuf_append(e->out, (const u8 *)closer, 1U);
    if (e->depth > 0U)
        bytebuf_push_u8(e->out, (u8)',');
    bytebuf_push_u8(e->out, (u8)'\n');
}

void sag_fl_map_close(FlEmit *e)
{
    emit_close(e, "}");
}

void sag_fl_list_open(FlEmit *e, const char *key)
{
    emit_key(e, key);
    bytebuf_append(e->out, (const u8 *)"[\n", 2U);
    e->depth++;
}

void sag_fl_list_close(FlEmit *e)
{
    emit_close(e, "]");
}

/*
 * The escape table, pinned exhaustively: \" \\ \n \t \r \0 \xNN.
 *
 * The emitter escapes EXACTLY `"`, `\`, bytes < 0x20, and 0x7F.  Every
 * other byte rides through verbatim, so UTF-8 survives unmangled and
 * invalid bytes round-trip losslessly.  Paths are bytes, not text
 * (invariant 2) — a path that is not valid UTF-8 is still a path, and
 * escaping it into \xNN soup or replacing it with U+FFFD would lose the
 * file.
 */
static void emit_escaped(FlEmit *e, const char *s, u64 n)
{
    static const char hex[] = "0123456789abcdef";
    u64 i;

    bytebuf_push_u8(e->out, (u8)'"');
    for (i = 0U; i < n; i++) {
        u8 c = (u8)s[i];

        switch (c) {
        case (u8)'"':
            bytebuf_append(e->out, (const u8 *)"\\\"", 2U);
            break;
        case (u8)'\\':
            bytebuf_append(e->out, (const u8 *)"\\\\", 2U);
            break;
        case (u8)'\n':
            bytebuf_append(e->out, (const u8 *)"\\n", 2U);
            break;
        case (u8)'\t':
            bytebuf_append(e->out, (const u8 *)"\\t", 2U);
            break;
        case (u8)'\r':
            bytebuf_append(e->out, (const u8 *)"\\r", 2U);
            break;
        case 0U:
            bytebuf_append(e->out, (const u8 *)"\\0", 2U);
            break;
        default:
            if (c < 0x20U || c == 0x7FU) {
                bytebuf_append(e->out, (const u8 *)"\\x", 2U);
                bytebuf_push_u8(e->out, (u8)hex[(c >> 4) & 0x0FU]);
                bytebuf_push_u8(e->out, (u8)hex[c & 0x0FU]);
            } else {
                bytebuf_push_u8(e->out, c);
            }
            break;
        }
    }
    bytebuf_push_u8(e->out, (u8)'"');
}

void sag_fl_str(FlEmit *e, const char *key, const char *s, u64 n)
{
    emit_key(e, key);
    if (s == NULL) {
        /* A NULL string is nil, not "" — the schema distinguishes an
         * untitled tab from one named the empty string. */
        bytebuf_append(e->out, (const u8 *)"nil,\n", 5U);
        return;
    }
    emit_escaped(e, s, n);
    bytebuf_append(e->out, (const u8 *)",\n", 2U);
}

void sag_fl_int(FlEmit *e, const char *key, i64 v)
{
    char buf[32];
    int n;

    emit_key(e, key);
    /* %lld, never a locale-aware formatter: the whole reason there are
     * no floats here is that locale must not reach the format. */
    n = snprintf(buf, sizeof(buf), "%lld,\n", (long long)v);
    if (n > 0)
        bytebuf_append(e->out, (const u8 *)buf, (size_t)n);
}

void sag_fl_bool(FlEmit *e, const char *key, bool v)
{
    emit_key(e, key);
    if (v)
        bytebuf_append(e->out, (const u8 *)"true,\n", 6U);
    else
        bytebuf_append(e->out, (const u8 *)"false,\n", 7U);
}

void sag_fl_nil(FlEmit *e, const char *key)
{
    emit_key(e, key);
    bytebuf_append(e->out, (const u8 *)"nil,\n", 5U);
}

void sag_fl_emit_lit(FlEmit *e, const char *key, const FlLit *v)
{
    u32 i;

    if (v == NULL) {
        sag_fl_nil(e, key);
        return;
    }
    switch (v->kind) {
    case FL_NIL:
        sag_fl_nil(e, key);
        break;
    case FL_BOOL:
        sag_fl_bool(e, key, v->i != 0);
        break;
    case FL_INT:
        sag_fl_int(e, key, v->i);
        break;
    case FL_STR:
        sag_fl_str(e, key, v->s, v->slen);
        break;
    case FL_LIST:
        sag_fl_list_open(e, key);
        for (i = 0U; i < v->len; i++)
            sag_fl_emit_lit(e, NULL, v->items[i]);
        sag_fl_list_close(e);
        break;
    case FL_MAP:
    default:
        sag_fl_map_open(e, key);
        /* INSERTION ORDER, which is the order the parser saw and the
         * order the schema tables define.  Re-emitting in any other
         * order would make save->restore->save stop being a fixpoint
         * and break every golden. */
        for (i = 0U; i < v->len; i++)
            sag_fl_emit_lit(e, v->keys[i], v->items[i]);
        sag_fl_map_close(e);
        break;
    }
}
