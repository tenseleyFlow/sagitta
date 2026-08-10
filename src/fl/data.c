#include "fl/data.h"

#include <stdio.h>
#include <string.h>

#include "fl/ast.h"
#include "fl/gc.h"
#include "fl/parse.h"
#include "util/intern.h"
#include "util/log.h"

typedef struct DataRead {
    FlVm *vm;
    DiagCtx *dc;
    u32 nodes;
    bool failed;
} DataRead;

static void data_error(DataRead *r, const FlNode *n, const char *msg)
{
    if (r->failed)
        return;
    r->failed = true;
    fl_diag_emit(r->dc, FL_DIAG_ERROR,
                 n == NULL ? (FlSpan){0U, 1U, 1U, 1U} : n->sp,
                 "pure-literal data: %s", msg);
}

static FlValue data_from_node(DataRead *r, const FlNode *n, u32 depth)
{
    u32 i;

    if (n == NULL || r->failed)
        return FL_NIL_V;
    if (++r->nodes > (u32)FL_DATA_MAX_NODES) {
        data_error(r, n, "node cap exceeded");
        return FL_NIL_V;
    }
    if (depth > (u32)FL_DATA_MAX_DEPTH) {
        FlSpan sp = n->sp;

        /* Match the frozen state reader's position: after the map key
         * and separator that led to the over-deep value. */
        sp.col += 3U;
        r->failed = true;
        fl_diag_emit(r->dc, FL_DIAG_ERROR, sp,
                     "pure-literal data: nesting deeper than 32");
        return FL_NIL_V;
    }
    switch ((FlAstKind)n->kind) {
    case FL_A_LIT:
        switch ((FlLitKind)n->as.lit.lit) {
        case FL_L_NIL:
            return FL_NIL_V;
        case FL_L_BOOL:
            return FL_BOOL_V(n->as.lit.v.b);
        case FL_L_INT:
            return FL_INT_V(n->as.lit.v.i);
        case FL_L_FLOAT:
            return FL_FLOAT_V(n->as.lit.v.f);
        case FL_L_STR: {
            const char *s = yew_intern_str(r->vm->in, n->as.lit.v.str_id);
            size_t len = yew_intern_len(r->vm->in, n->as.lit.v.str_id);

            if (len > (size_t)FL_DATA_MAX_STRING) {
                data_error(r, n, "string exceeds the 4096-byte cap");
                return FL_NIL_V;
            }
            return FL_OBJ_V(FL_STR, fl_str_new(r->vm, s, (u32)len));
        }
        default:
            break;
        }
        break;
    case FL_A_LIST: {
        FlList *list = fl_list_new(r->vm);

        for (i = 0U; i < n->as.list.n && !r->failed; i++)
            (void)fl_list_push(r->vm, list,
                               data_from_node(r, n->as.list.items[i],
                                              depth + 1U));
        return FL_OBJ_V(FL_LIST, list);
    }
    case FL_A_MAP: {
        FlMap *map = fl_map_new(r->vm);

        for (i = 0U; i < n->as.map.n && !r->failed; i++) {
            FlValue k = data_from_node(r, n->as.map.keys[i], depth + 1U);
            FlValue v = data_from_node(r, n->as.map.vals[i], depth + 1U);

            if (!fl_map_set(r->vm, map, k, v)) {
                data_error(r, n->as.map.keys[i], "unhashable map key");
                break;
            }
        }
        return FL_OBJ_V(FL_MAP, map);
    }
    default:
        break;
    }
    data_error(r, n, "non-data AST node");
    return FL_NIL_V;
}

static bool data_complete(const char *src, size_t len, u32 *line, u32 *col)
{
    char stack[FL_DATA_MAX_DEPTH + 2U];
    u32 depth = 0U;
    u32 ln = 1U;
    u32 cl = 1U;
    size_t i;
    bool string = false;
    bool escape = false;
    bool comment = false;

    for (i = 0U; i < len; i++) {
        char c = src[i];

        if (comment) {
            if (c == '\n')
                comment = false;
        } else if (string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                string = false;
            }
        } else if (c == '#') {
            comment = true;
        } else if (c == '"') {
            string = true;
        } else if (c == '{' || c == '[') {
            if (depth < YEW_ARRAY_LEN(stack))
                stack[depth] = c;
            depth++;
        } else if (c == '}' || c == ']') {
            char want = c == '}' ? '{' : '[';

            if (depth == 0U || depth > YEW_ARRAY_LEN(stack) ||
                stack[depth - 1U] != want)
                return true; /* the parser diagnoses mismatched closers */
            depth--;
        }
        if (c == '\n') {
            ln++;
            cl = 1U;
        } else {
            cl++;
        }
    }
    *line = ln;
    *col = cl;
    return depth == 0U && !string && !escape;
}

FlValue fl_data_read(FlVm *vm, const char *src, size_t len, DiagCtx *dc)
{
    DataRead r;
    FlNode *root;
    u32 file_id;
    u32 end_line;
    u32 end_col;

    if (vm == NULL || dc == NULL)
        return FL_NIL_V;
    if (src == NULL)
        src = "";
    if (len > (size_t)FL_DATA_MAX_BYTES) {
        fl_diag_emit(dc, FL_DIAG_ERROR, (FlSpan){0U, 1U, 1U, 1U},
                     "pure-literal data: document exceeds 8 MiB");
        return FL_NIL_V;
    }
    file_id = fl_diag_add_file(dc, "<data>", src, len);
    root = fl_parse_literal(vm->arena, dc, vm->in, src, len, file_id);
    if (root == NULL)
        return FL_NIL_V;
    if (!data_complete(src, len, &end_line, &end_col)) {
        fl_diag_emit(dc, FL_DIAG_ERROR,
                     (FlSpan){file_id, end_line, end_col, 1U},
                     "pure-literal data: unterminated container");
        return FL_NIL_V;
    }
    (void)memset(&r, 0, sizeof(r));
    r.vm = vm;
    r.dc = dc;
    {
        FlValue result = data_from_node(&r, root, 1U);

        return r.failed ? FL_NIL_V : result;
    }
}

static void data_indent(Bytebuf *out, u32 depth)
{
    u32 i;

    for (i = 0U; i < depth; i++)
        bytebuf_append(out, (const u8 *)"  ", 2U);
}

static void data_string(Bytebuf *out, const char *s, u32 n)
{
    static const char hex[] = "0123456789abcdef";
    u32 i;

    bytebuf_push_u8(out, (u8)'"');
    for (i = 0U; i < n; i++) {
        u8 c = (u8)s[i];

        switch (c) {
        case (u8)'"': bytebuf_append(out, (const u8 *)"\\\"", 2U); break;
        case (u8)'\\': bytebuf_append(out, (const u8 *)"\\\\", 2U); break;
        case (u8)'\n': bytebuf_append(out, (const u8 *)"\\n", 2U); break;
        case (u8)'\t': bytebuf_append(out, (const u8 *)"\\t", 2U); break;
        case (u8)'\r': bytebuf_append(out, (const u8 *)"\\r", 2U); break;
        case 0U: bytebuf_append(out, (const u8 *)"\\0", 2U); break;
        default:
            if (c < 0x20U || c == 0x7fU) {
                bytebuf_append(out, (const u8 *)"\\x", 2U);
                bytebuf_push_u8(out, (u8)hex[c >> 4]);
                bytebuf_push_u8(out, (u8)hex[c & 0x0fU]);
            } else {
                bytebuf_push_u8(out, c);
            }
            break;
        }
    }
    bytebuf_push_u8(out, (u8)'"');
}

static bool data_bare_key(const FlStr *s)
{
    u32 i;

    if (s == NULL || s->len == 0U ||
        !((s->b[0] >= 'a' && s->b[0] <= 'z') || s->b[0] == '_'))
        return false;
    for (i = 1U; i < s->len; i++) {
        char c = s->b[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_'))
            return false;
    }
    return true;
}

static void data_value(Bytebuf *out, FlValue v, u32 depth, bool comma);

static void data_key(Bytebuf *out, FlValue key)
{
    char num[32];
    int n;

    switch ((FlType)key.t) {
    case FL_STR: {
        const FlStr *s = (const FlStr *)key.as.o;

        if (data_bare_key(s))
            bytebuf_append(out, (const u8 *)s->b, s->len);
        else
            data_string(out, s->b, s->len);
        break;
    }
    case FL_INT:
        n = snprintf(num, sizeof(num), "%lld", (long long)key.as.i);
        if (n > 0)
            bytebuf_append(out, (const u8 *)num, (size_t)n);
        break;
    case FL_BOOL:
        bytebuf_append(out, (const u8 *)(key.as.b ? "true" : "false"),
                       key.as.b ? 4U : 5U);
        break;
    default:
        YEW_BUG("fletch data: unhashable map key reached writer");
    }
}

static void data_float(Bytebuf *out, double value)
{
    char num[64];
    int n = snprintf(num, sizeof(num), "%.17g", value);
    int i;
    bool fractional = false;

    if (n <= 0)
        return;
    for (i = 0; i < n; i++) {
        if (num[i] == ',')
            num[i] = '.';
        if (num[i] == '.' || num[i] == 'e' || num[i] == 'E')
            fractional = true;
    }
    bytebuf_append(out, (const u8 *)num, (size_t)n);
    if (!fractional)
        bytebuf_append(out, (const u8 *)".0", 2U);
}

static void data_value(Bytebuf *out, FlValue v, u32 depth, bool comma)
{
    char num[32];
    int n;
    u32 i;

    switch ((FlType)v.t) {
    case FL_NIL:
        bytebuf_append(out, (const u8 *)"nil", 3U);
        break;
    case FL_BOOL:
        bytebuf_append(out, (const u8 *)(v.as.b ? "true" : "false"),
                       v.as.b ? 4U : 5U);
        break;
    case FL_INT:
        n = snprintf(num, sizeof(num), "%lld", (long long)v.as.i);
        if (n > 0)
            bytebuf_append(out, (const u8 *)num, (size_t)n);
        break;
    case FL_FLOAT:
        data_float(out, v.as.f);
        break;
    case FL_STR: {
        const FlStr *s = (const FlStr *)v.as.o;

        data_string(out, s->b, s->len);
        break;
    }
    case FL_LIST: {
        const FlList *list = (const FlList *)v.as.o;

        bytebuf_append(out, (const u8 *)"[\n", 2U);
        for (i = 0U; i < list->n; i++) {
            data_indent(out, depth + 1U);
            data_value(out, list->v[i], depth + 1U, true);
        }
        data_indent(out, depth);
        bytebuf_push_u8(out, (u8)']');
        break;
    }
    case FL_MAP: {
        const FlMap *map = (const FlMap *)v.as.o;
        u32 cur = 0U;
        FlValue key;
        FlValue val;

        bytebuf_append(out, (const u8 *)"{\n", 2U);
        while (fl_map_iter(map, &cur, &key, &val)) {
            data_indent(out, depth + 1U);
            data_key(out, key);
            bytebuf_append(out, (const u8 *)": ", 2U);
            data_value(out, val, depth + 1U, true);
        }
        data_indent(out, depth);
        bytebuf_push_u8(out, (u8)'}');
        break;
    }
    default:
        YEW_BUG("fletch data: non-data value '%s'", fl_type_name(v.t));
    }
    if (comma)
        bytebuf_push_u8(out, (u8)',');
    bytebuf_push_u8(out, (u8)'\n');
}

void fl_data_write(Bytebuf *out, FlValue v, u32 indent)
{
    if (out == NULL)
        return;
    data_indent(out, indent);
    data_value(out, v, indent, false);
}
