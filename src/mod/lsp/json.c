#include "json.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/utf8.h"
#include "util/log.h"

#define JSON_CHUNK 32u

typedef struct JsonPtrChunk JsonPtrChunk;
typedef struct JsonMemChunk JsonMemChunk;

struct JsonPtrChunk {
    JsonPtrChunk *next;
    u32 n;
    JsonValue *v[JSON_CHUNK];
};

struct JsonMemChunk {
    JsonMemChunk *next;
    u32 n;
    JsonMember m[JSON_CHUNK];
};

typedef struct {
    Arena *a;
    const u8 *s;
    u64 len;
    u64 pos;
    u32 nodes;
    JsonErr *err;
} JsonParser;

static void json_err_at(JsonParser *p, u64 off, const char *msg)
{
    u64 i;
    u32 line = 1u;
    u32 col = 1u;

    if (p->err == NULL)
        return;
    if (off > p->len)
        off = p->len;
    for (i = 0u; i < off; i++) {
        if (p->s[i] == (u8)'\n') {
            line++;
            col = 1u;
        } else {
            col++;
        }
    }
    p->err->line = line;
    p->err->col = col;
    p->err->off = off;
    (void)snprintf(p->err->msg, sizeof(p->err->msg), "%s", msg);
}

static void json_err(JsonParser *p, const char *msg)
{
    json_err_at(p, p->pos, msg);
}

static void json_skip_ws(JsonParser *p)
{
    while (p->pos < p->len) {
        u8 c = p->s[p->pos];

        if (c != (u8)' ' && c != (u8)'\t' && c != (u8)'\r' &&
            c != (u8)'\n')
            break;
        p->pos++;
    }
}

static JsonValue *json_new(JsonParser *p, JsonKind kind)
{
    JsonValue *v;

    if (p->nodes >= YEW_JSON_MAX_NODES) {
        json_err(p, "too many JSON nodes");
        return NULL;
    }
    p->nodes++;
    v = arena_alloc(p->a, sizeof(*v), _Alignof(JsonValue));
    memset(v, 0, sizeof(*v));
    v->kind = (u8)kind;
    return v;
}

static bool json_hex4(JsonParser *p, u32 *out)
{
    u32 value = 0u;
    u32 i;

    if (p->len - p->pos < 4u) {
        json_err(p, "short Unicode escape");
        return false;
    }
    for (i = 0u; i < 4u; i++) {
        u8 c = p->s[p->pos + i];
        u32 digit;

        if (c >= (u8)'0' && c <= (u8)'9')
            digit = (u32)(c - (u8)'0');
        else if (c >= (u8)'a' && c <= (u8)'f')
            digit = (u32)(c - (u8)'a') + 10u;
        else if (c >= (u8)'A' && c <= (u8)'F')
            digit = (u32)(c - (u8)'A') + 10u;
        else {
            json_err_at(p, p->pos + i, "non-hex digit in Unicode escape");
            return false;
        }
        value = (value << 4u) | digit;
    }
    p->pos += 4u;
    *out = value;
    return true;
}

static bool json_string(JsonParser *p, const u8 **out, u32 *out_len,
                        u8 *out_flags)
{
    u64 content;
    u64 scan;
    u8 *dst;
    u32 n = 0u;
    u8 flags = 0u;

    if (p->pos >= p->len || p->s[p->pos] != (u8)'"') {
        json_err(p, "expected string");
        return false;
    }
    content = ++p->pos;
    scan = content;
    while (scan < p->len) {
        u8 c = p->s[scan++];

        if (c == (u8)'"')
            break;
        if (c < 0x20u) {
            json_err_at(p, scan - 1u, "control byte in string");
            return false;
        }
        if (c == (u8)'\\') {
            if (scan >= p->len) {
                json_err_at(p, scan - 1u, "unterminated escape");
                return false;
            }
            scan++;
        }
    }
    if (scan == p->len && (scan == content || p->s[scan - 1u] != (u8)'"')) {
        json_err_at(p, content - 1u, "unterminated string");
        return false;
    }
    if (scan - content - 1u > UINT32_MAX) {
        json_err_at(p, content, "string is too long");
        return false;
    }
    dst = arena_alloc(p->a, (size_t)(scan - content - 1u), 1u);
    while (p->pos < scan - 1u) {
        u8 c = p->s[p->pos++];

        if (c != (u8)'\\') {
            dst[n++] = c;
            continue;
        }
        c = p->s[p->pos++];
        switch (c) {
        case (u8)'"': dst[n++] = (u8)'"'; break;
        case (u8)'\\': dst[n++] = (u8)'\\'; break;
        case (u8)'/': dst[n++] = (u8)'/'; break;
        case (u8)'b': dst[n++] = (u8)'\b'; break;
        case (u8)'f': dst[n++] = (u8)'\f'; break;
        case (u8)'n': dst[n++] = (u8)'\n'; break;
        case (u8)'r': dst[n++] = (u8)'\r'; break;
        case (u8)'t': dst[n++] = (u8)'\t'; break;
        case (u8)'u': {
            u32 cp;
            u8 encoded[YEW_UTF8_MAX];
            size_t encoded_len;

            if (!json_hex4(p, &cp))
                return false;
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                u32 low;
                u32 high = cp;

                if (p->len - p->pos < 6u || p->s[p->pos] != (u8)'\\' ||
                    p->s[p->pos + 1u] != (u8)'u') {
                    char msg[96];

                    (void)snprintf(msg, sizeof(msg),
                                   "lone high surrogate U+%04X", high);
                    json_err_at(p, p->pos - 4u, msg);
                    return false;
                }
                p->pos += 2u;
                if (!json_hex4(p, &low))
                    return false;
                if (low < 0xDC00u || low > 0xDFFFu) {
                    char msg[96];

                    (void)snprintf(msg, sizeof(msg),
                                   "lone high surrogate U+%04X", high);
                    json_err_at(p, p->pos - 10u, msg);
                    return false;
                }
                cp = 0x10000u + ((high - 0xD800u) << 10u) +
                     (low - 0xDC00u);
            } else if (cp >= 0xDC80u && cp <= 0xDCFFu) {
                dst[n++] = (u8)(cp - 0xDC00u);
                flags |= YEW_JSF_RAW_BYTE;
                break;
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                char msg[96];

                (void)snprintf(msg, sizeof(msg),
                               "lone low surrogate U+%04X", cp);
                json_err_at(p, p->pos - 4u, msg);
                return false;
            }
            encoded_len = yew_utf8_encode(cp, encoded);
            if (encoded_len == 0u) {
                json_err(p, "invalid Unicode scalar");
                return false;
            }
            memcpy(dst + n, encoded, encoded_len);
            n += (u32)encoded_len;
            break;
        }
        default: {
            char msg[96];

            (void)snprintf(msg, sizeof(msg), "unknown escape '\\%c'", c);
            json_err_at(p, p->pos - 1u, msg);
            return false;
        }
        }
    }
    p->pos = scan;
    *out = dst;
    *out_len = n;
    *out_flags = flags;
    return true;
}

static JsonValue *json_value(JsonParser *p, u32 depth);

static bool json_ptr_push(JsonParser *p, JsonPtrChunk **head,
                          JsonPtrChunk **tail, JsonValue *value, u32 *n)
{
    JsonPtrChunk *chunk = *tail;

    if (*n == UINT32_MAX) {
        json_err(p, "array is too long");
        return false;
    }
    if (chunk == NULL || chunk->n == JSON_CHUNK) {
        chunk = arena_alloc(p->a, sizeof(*chunk), _Alignof(JsonPtrChunk));
        chunk->next = NULL;
        chunk->n = 0u;
        if (*tail != NULL)
            (*tail)->next = chunk;
        else
            *head = chunk;
        *tail = chunk;
    }
    chunk->v[chunk->n++] = value;
    (*n)++;
    return true;
}

static JsonValue *json_array(JsonParser *p, u32 depth)
{
    JsonValue *arr;
    JsonPtrChunk *head = NULL;
    JsonPtrChunk *tail = NULL;
    JsonPtrChunk *chunk;
    u32 n = 0u;
    u32 at = 0u;

    if (depth + 1u >= YEW_JSON_MAX_DEPTH) {
        json_err(p, "nesting deeper than 128");
        return NULL;
    }
    arr = json_new(p, YEW_JS_ARR);
    if (arr == NULL)
        return NULL;
    p->pos++;
    json_skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == (u8)']') {
        p->pos++;
        return arr;
    }
    for (;;) {
        JsonValue *value = json_value(p, depth + 1u);

        if (value == NULL || !json_ptr_push(p, &head, &tail, value, &n))
            return NULL;
        json_skip_ws(p);
        if (p->pos >= p->len) {
            json_err(p, "unterminated array");
            return NULL;
        }
        if (p->s[p->pos] == (u8)']') {
            p->pos++;
            break;
        }
        if (p->s[p->pos] != (u8)',') {
            json_err(p, "expected ',' or ']'");
            return NULL;
        }
        p->pos++;
        json_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == (u8)']') {
            json_err(p, "trailing comma in array");
            return NULL;
        }
    }
    arr->arr.v = arena_alloc(p->a, (size_t)n * sizeof(*arr->arr.v),
                             _Alignof(JsonValue *));
    arr->arr.n = n;
    for (chunk = head; chunk != NULL; chunk = chunk->next) {
        memcpy(arr->arr.v + at, chunk->v,
               (size_t)chunk->n * sizeof(*chunk->v));
        at += chunk->n;
    }
    return arr;
}

static JsonMember *json_member_find(JsonMemChunk *head, const u8 *key,
                                    u32 klen)
{
    JsonMemChunk *chunk;

    for (chunk = head; chunk != NULL; chunk = chunk->next) {
        u32 i;

        for (i = 0u; i < chunk->n; i++) {
            if (chunk->m[i].klen == klen &&
                memcmp(chunk->m[i].key, key, klen) == 0)
                return &chunk->m[i];
        }
    }
    return NULL;
}

static bool json_member_push(JsonParser *p, JsonMemChunk **head,
                             JsonMemChunk **tail, JsonMember member, u32 *n)
{
    JsonMemChunk *chunk = *tail;

    if (*n == UINT32_MAX) {
        json_err(p, "object is too large");
        return false;
    }
    if (chunk == NULL || chunk->n == JSON_CHUNK) {
        chunk = arena_alloc(p->a, sizeof(*chunk), _Alignof(JsonMemChunk));
        chunk->next = NULL;
        chunk->n = 0u;
        if (*tail != NULL)
            (*tail)->next = chunk;
        else
            *head = chunk;
        *tail = chunk;
    }
    chunk->m[chunk->n++] = member;
    (*n)++;
    return true;
}

static JsonValue *json_object(JsonParser *p, u32 depth)
{
    JsonValue *obj;
    JsonMemChunk *head = NULL;
    JsonMemChunk *tail = NULL;
    JsonMemChunk *chunk;
    u32 n = 0u;
    u32 at = 0u;

    if (depth + 1u >= YEW_JSON_MAX_DEPTH) {
        json_err(p, "nesting deeper than 128");
        return NULL;
    }
    obj = json_new(p, YEW_JS_OBJ);
    if (obj == NULL)
        return NULL;
    p->pos++;
    json_skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == (u8)'}') {
        p->pos++;
        return obj;
    }
    for (;;) {
        const u8 *key;
        u32 klen;
        u8 key_flags;
        JsonValue *value;
        JsonMember *old;
        JsonMember member;

        if (!json_string(p, &key, &klen, &key_flags))
            return NULL;
        (void)key_flags;
        json_skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != (u8)':') {
            json_err(p, "expected ':' after object key");
            return NULL;
        }
        p->pos++;
        value = json_value(p, depth + 1u);
        if (value == NULL)
            return NULL;
        old = json_member_find(head, key, klen);
        if (old != NULL) {
            old->val = value;
            obj->flags |= YEW_JSF_DUP_KEY;
            yew_log(YEW_LOG_WARN, "duplicate JSON object key");
        } else {
            member.key = key;
            member.klen = klen;
            member.val = value;
            if (!json_member_push(p, &head, &tail, member, &n))
                return NULL;
        }
        json_skip_ws(p);
        if (p->pos >= p->len) {
            json_err(p, "unterminated object");
            return NULL;
        }
        if (p->s[p->pos] == (u8)'}') {
            p->pos++;
            break;
        }
        if (p->s[p->pos] != (u8)',') {
            json_err(p, "expected ',' or '}'");
            return NULL;
        }
        p->pos++;
        json_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == (u8)'}') {
            json_err(p, "trailing comma in object");
            return NULL;
        }
    }
    obj->obj.m = arena_alloc(p->a, (size_t)n * sizeof(*obj->obj.m),
                             _Alignof(JsonMember));
    obj->obj.n = n;
    for (chunk = head; chunk != NULL; chunk = chunk->next) {
        memcpy(obj->obj.m + at, chunk->m,
               (size_t)chunk->n * sizeof(*chunk->m));
        at += chunk->n;
    }
    return obj;
}

static JsonValue *json_number(JsonParser *p)
{
    u64 start = p->pos;
    u64 digits;
    bool negative = false;
    bool real = false;
    bool overflow = false;
    u64 magnitude = 0u;
    u64 limit;
    JsonValue *v;

    if (p->s[p->pos] == (u8)'-') {
        negative = true;
        p->pos++;
    }
    if (p->pos >= p->len) {
        json_err_at(p, start, "malformed number");
        return NULL;
    }
    digits = p->pos;
    if (p->s[p->pos] == (u8)'0') {
        p->pos++;
        if (p->pos < p->len && p->s[p->pos] >= (u8)'0' &&
            p->s[p->pos] <= (u8)'9') {
            json_err_at(p, start, "malformed number");
            return NULL;
        }
    } else if (p->s[p->pos] >= (u8)'1' && p->s[p->pos] <= (u8)'9') {
        while (p->pos < p->len && p->s[p->pos] >= (u8)'0' &&
               p->s[p->pos] <= (u8)'9')
            p->pos++;
    } else {
        json_err_at(p, start, "malformed number");
        return NULL;
    }
    if (p->pos < p->len && p->s[p->pos] == (u8)'.') {
        real = true;
        p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < (u8)'0' ||
            p->s[p->pos] > (u8)'9') {
            json_err_at(p, start, "malformed number");
            return NULL;
        }
        while (p->pos < p->len && p->s[p->pos] >= (u8)'0' &&
               p->s[p->pos] <= (u8)'9')
            p->pos++;
    }
    if (p->pos < p->len &&
        (p->s[p->pos] == (u8)'e' || p->s[p->pos] == (u8)'E')) {
        real = true;
        p->pos++;
        if (p->pos < p->len &&
            (p->s[p->pos] == (u8)'+' || p->s[p->pos] == (u8)'-'))
            p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < (u8)'0' ||
            p->s[p->pos] > (u8)'9') {
            json_err_at(p, start, "malformed number");
            return NULL;
        }
        while (p->pos < p->len && p->s[p->pos] >= (u8)'0' &&
               p->s[p->pos] <= (u8)'9')
            p->pos++;
    }
    limit = negative ? (u64)INT64_MAX + 1u : (u64)INT64_MAX;
    if (!real) {
        u64 i;

        for (i = digits; i < p->pos; i++) {
            u64 digit = (u64)(p->s[i] - (u8)'0');

            if (magnitude > (limit - digit) / 10u) {
                overflow = true;
                break;
            }
            magnitude = magnitude * 10u + digit;
        }
        if (!overflow) {
            v = json_new(p, YEW_JS_INT);
            if (v == NULL)
                return NULL;
            if (negative && magnitude == (u64)INT64_MAX + 1u)
                v->i = INT64_MIN;
            else
                v->i = negative ? -(i64)magnitude : (i64)magnitude;
            return v;
        }
    }
    {
        size_t n = (size_t)(p->pos - start);
        char *lexeme = arena_alloc(p->a, n + 1u, 1u);
        char *end;
        double d;

        memcpy(lexeme, p->s + start, n);
        lexeme[n] = '\0';
        errno = 0;
        /* Repository policy leaves LC_NUMERIC in the C locale forever. */
        d = strtod(lexeme, &end);
        if ((size_t)(end - lexeme) != n || isinf(d) ||
            (errno == ERANGE && (d == HUGE_VAL || d == -HUGE_VAL))) {
            json_err_at(p, start, "number out of range");
            return NULL;
        }
        v = json_new(p, YEW_JS_REAL);
        if (v == NULL)
            return NULL;
        v->d = d;
        if (overflow || (errno == ERANGE && d == 0.0))
            v->flags |= YEW_JSF_LOSSY_NUM;
        return v;
    }
}

static bool json_literal(JsonParser *p, const char *word)
{
    size_t n = strlen(word);

    if (p->len - p->pos < n || memcmp(p->s + p->pos, word, n) != 0)
        return false;
    p->pos += n;
    return true;
}

static JsonValue *json_value(JsonParser *p, u32 depth)
{
    JsonValue *v;

    json_skip_ws(p);
    if (p->pos >= p->len) {
        json_err(p, "expected JSON value");
        return NULL;
    }
    switch (p->s[p->pos]) {
    case (u8)'{': return json_object(p, depth);
    case (u8)'[': return json_array(p, depth);
    case (u8)'"':
        v = json_new(p, YEW_JS_STR);
        if (v == NULL)
            return NULL;
        if (!json_string(p, &v->s.p, &v->s.len, &v->flags))
            return NULL;
        return v;
    case (u8)'n':
        if (!json_literal(p, "null"))
            break;
        return json_new(p, YEW_JS_NULL);
    case (u8)'t':
        if (!json_literal(p, "true"))
            break;
        v = json_new(p, YEW_JS_BOOL);
        if (v != NULL)
            v->b = true;
        return v;
    case (u8)'f':
        if (!json_literal(p, "false"))
            break;
        v = json_new(p, YEW_JS_BOOL);
        if (v != NULL)
            v->b = false;
        return v;
    default:
        if ((p->len - p->pos >= 8u &&
             memcmp(p->s + p->pos, "Infinity", 8u) == 0) ||
            (p->len - p->pos >= 3u &&
             memcmp(p->s + p->pos, "NaN", 3u) == 0)) {
            json_err(p, "number out of range");
            return NULL;
        }
        if (p->s[p->pos] == (u8)'-' || p->s[p->pos] == (u8)'+' ||
            p->s[p->pos] == (u8)'.' ||
            (p->s[p->pos] >= (u8)'0' && p->s[p->pos] <= (u8)'9'))
            return json_number(p);
        break;
    }
    json_err(p, "expected JSON value");
    return NULL;
}

JsonValue *yew_json_parse(Arena *a, const u8 *s, u64 len, JsonErr *err)
{
    JsonParser p;
    JsonValue *root;

    if (err != NULL)
        memset(err, 0, sizeof(*err));
    if (a == NULL || (s == NULL && len != 0u)) {
        if (err != NULL)
            (void)snprintf(err->msg, sizeof(err->msg), "invalid parser input");
        return NULL;
    }
    p.a = a;
    p.s = s;
    p.len = len;
    p.pos = 0u;
    p.nodes = 0u;
    p.err = err;
    if (len > YEW_JSON_MAX_BYTES) {
        json_err(&p, "document exceeds 64 MiB");
        return NULL;
    }
    json_skip_ws(&p);
    if (p.pos == p.len) {
        json_err(&p, "empty document");
        return NULL;
    }
    root = json_value(&p, 0u);
    if (root == NULL)
        return NULL;
    json_skip_ws(&p);
    if (p.pos != p.len) {
        json_err(&p, "trailing bytes after JSON value");
        return NULL;
    }
    return root;
}

const JsonValue *yew_json_get(const JsonValue *o, const char *key)
{
    size_t n;
    u32 i;

    if (o == NULL || o->kind != YEW_JS_OBJ || key == NULL)
        return NULL;
    n = strlen(key);
    if (n > UINT32_MAX)
        return NULL;
    for (i = 0u; i < o->obj.n; i++) {
        if (o->obj.m[i].klen == (u32)n &&
            memcmp(o->obj.m[i].key, key, n) == 0)
            return o->obj.m[i].val;
    }
    return NULL;
}

const JsonValue *yew_json_at(const JsonValue *a, u32 i)
{
    if (a == NULL || a->kind != YEW_JS_ARR || i >= a->arr.n)
        return NULL;
    return a->arr.v[i];
}

u32 yew_json_len(const JsonValue *v)
{
    if (v == NULL)
        return 0u;
    if (v->kind == YEW_JS_STR)
        return v->s.len;
    if (v->kind == YEW_JS_ARR)
        return v->arr.n;
    if (v->kind == YEW_JS_OBJ)
        return v->obj.n;
    return 0u;
}

i64 yew_json_int(const JsonValue *v, i64 dflt)
{
    return v != NULL && v->kind == YEW_JS_INT ? v->i : dflt;
}

double yew_json_real(const JsonValue *v, double dflt)
{
    return v != NULL && v->kind == YEW_JS_REAL ? v->d : dflt;
}

bool yew_json_bool(const JsonValue *v, bool dflt)
{
    return v != NULL && v->kind == YEW_JS_BOOL ? v->b : dflt;
}

const u8 *yew_json_str(const JsonValue *v, u32 *len)
{
    if (len != NULL)
        *len = 0u;
    if (v == NULL || v->kind != YEW_JS_STR)
        return NULL;
    if (len != NULL)
        *len = v->s.len;
    return v->s.p;
}

bool yew_json_streq(const JsonValue *v, const char *s)
{
    size_t n;

    if (v == NULL || v->kind != YEW_JS_STR || s == NULL)
        return false;
    n = strlen(s);
    return n == v->s.len && memcmp(v->s.p, s, n) == 0;
}

const JsonValue *yew_json_path(const JsonValue *root, const char *path)
{
    const JsonValue *value = root;
    const char *part = path;

    if (path == NULL || *path == '\0')
        return value;
    while (*part != '\0') {
        const char *dot = strchr(part, '.');
        size_t n = dot != NULL ? (size_t)(dot - part) : strlen(part);
        u32 i;

        if (value == NULL || value->kind != YEW_JS_OBJ || n == 0u)
            return NULL;
        value = NULL;
        for (i = 0u; i < root->obj.n; i++) {
            const JsonMember *m = &root->obj.m[i];

            if (m->klen == n && memcmp(m->key, part, n) == 0) {
                value = m->val;
                break;
            }
        }
        if (value == NULL || dot == NULL)
            return value;
        root = value;
        part = dot + 1;
    }
    return value;
}

bool yew_json_eq(const JsonValue *a, const JsonValue *b)
{
    u32 i;

    if (a == b)
        return true;
    if (a == NULL || b == NULL || a->kind != b->kind)
        return false;
    switch (a->kind) {
    case YEW_JS_NULL: return true;
    case YEW_JS_BOOL: return a->b == b->b;
    case YEW_JS_INT: return a->i == b->i;
    case YEW_JS_REAL: return a->d == b->d;
    case YEW_JS_STR:
        return a->s.len == b->s.len &&
               memcmp(a->s.p, b->s.p, a->s.len) == 0;
    case YEW_JS_ARR:
        if (a->arr.n != b->arr.n)
            return false;
        for (i = 0u; i < a->arr.n; i++) {
            if (!yew_json_eq(a->arr.v[i], b->arr.v[i]))
                return false;
        }
        return true;
    case YEW_JS_OBJ:
        if (a->obj.n != b->obj.n)
            return false;
        for (i = 0u; i < a->obj.n; i++) {
            if (a->obj.m[i].klen != b->obj.m[i].klen ||
                memcmp(a->obj.m[i].key, b->obj.m[i].key,
                       a->obj.m[i].klen) != 0 ||
                !yew_json_eq(a->obj.m[i].val, b->obj.m[i].val))
                return false;
        }
        return true;
    default: return false;
    }
}

static bool jsonw_bit(const JsonW *w, u16 depth)
{
    return (w->need_comma[depth / 8u] & (u8)(1u << (depth % 8u))) != 0u;
}

static void jsonw_set_bit(JsonW *w, u16 depth, bool value)
{
    u8 mask = (u8)(1u << (depth % 8u));

    if (value)
        w->need_comma[depth / 8u] |= mask;
    else
        w->need_comma[depth / 8u] &= (u8)~mask;
}

static void jsonw_before_value(JsonW *w)
{
    if (w->depth == 0u) {
        if (w->root_written)
            YEW_BUG("JSON writer received a second root value");
        w->root_written = true;
        return;
    }
    if (w->kind[w->depth - 1u] == YEW_JS_OBJ) {
        if (!w->key_pending)
            YEW_BUG("JSON object value has no key");
        w->key_pending = false;
        return;
    }
    if (jsonw_bit(w, w->depth - 1u))
        bytebuf_push_u8(w->out, (u8)',');
    jsonw_set_bit(w, w->depth - 1u, true);
}

static void jsonw_quoted(JsonW *w, const u8 *s, u32 n)
{
    static const char hex[] = "0123456789abcdef";
    size_t valid;
    u32 i = 0u;

    bytebuf_push_u8(w->out, (u8)'"');
    valid = yew_utf8_validate(s, n);
    while (i < n) {
        u8 c = s[i];

        if ((size_t)i == valid) {
            u32 cp;
            size_t consumed = yew_utf8_decode(s + i, (size_t)n - i, &cp);

            if (yew_utf8_is_escape(cp)) {
                u8 raw = yew_utf8_escape_byte(cp);
                const u8 esc[] = {'\\', 'u', 'D', 'C',
                                  (u8)hex[raw >> 4u],
                                  (u8)hex[raw & 0x0fu]};

                bytebuf_append(w->out, esc, sizeof(esc));
                i += (u32)consumed;
                valid = i + yew_utf8_validate(s + i, (size_t)n - i);
                continue;
            }
        }
        switch (c) {
        case (u8)'"': bytebuf_append(w->out, "\\\"", 2u); break;
        case (u8)'\\': bytebuf_append(w->out, "\\\\", 2u); break;
        case (u8)'\b': bytebuf_append(w->out, "\\b", 2u); break;
        case (u8)'\f': bytebuf_append(w->out, "\\f", 2u); break;
        case (u8)'\n': bytebuf_append(w->out, "\\n", 2u); break;
        case (u8)'\r': bytebuf_append(w->out, "\\r", 2u); break;
        case (u8)'\t': bytebuf_append(w->out, "\\t", 2u); break;
        default:
            if (c < 0x20u) {
                const u8 esc[] = {'\\', 'u', '0', '0',
                                  (u8)hex[c >> 4u],
                                  (u8)hex[c & 0x0fu]};

                bytebuf_append(w->out, esc, sizeof(esc));
            } else {
                bytebuf_push_u8(w->out, c);
            }
            break;
        }
        i++;
    }
    bytebuf_push_u8(w->out, (u8)'"');
}

void yew_jsonw_init(JsonW *w, Bytebuf *out)
{
    memset(w, 0, sizeof(*w));
    w->out = out;
}

static void jsonw_begin(JsonW *w, JsonKind kind, u8 open)
{
    jsonw_before_value(w);
    if (w->depth >= YEW_JSON_MAX_DEPTH)
        YEW_BUG("JSON writer nesting exceeds %u", YEW_JSON_MAX_DEPTH);
    bytebuf_push_u8(w->out, open);
    w->kind[w->depth] = (u8)kind;
    jsonw_set_bit(w, w->depth, false);
    w->depth++;
}

void yew_jsonw_obj(JsonW *w)
{
    jsonw_begin(w, YEW_JS_OBJ, (u8)'{');
}

void yew_jsonw_arr(JsonW *w)
{
    jsonw_begin(w, YEW_JS_ARR, (u8)'[');
}

static void jsonw_end(JsonW *w, JsonKind kind, u8 close)
{
    if (w->depth == 0u || w->kind[w->depth - 1u] != kind)
        YEW_BUG("JSON writer container end mismatch");
    if (kind == YEW_JS_OBJ && w->key_pending)
        YEW_BUG("JSON object key has no value");
    w->depth--;
    bytebuf_push_u8(w->out, close);
}

void yew_jsonw_obj_end(JsonW *w)
{
    jsonw_end(w, YEW_JS_OBJ, (u8)'}');
}

void yew_jsonw_arr_end(JsonW *w)
{
    jsonw_end(w, YEW_JS_ARR, (u8)']');
}

void yew_jsonw_key(JsonW *w, const char *k)
{
    size_t n;

    if (w->depth == 0u || w->kind[w->depth - 1u] != YEW_JS_OBJ ||
        w->key_pending)
        YEW_BUG("JSON key outside object or without prior value");
    if (jsonw_bit(w, w->depth - 1u))
        bytebuf_push_u8(w->out, (u8)',');
    jsonw_set_bit(w, w->depth - 1u, true);
    n = strlen(k);
    if (n > UINT32_MAX)
        YEW_BUG("JSON object key is too long");
    jsonw_quoted(w, (const u8 *)k, (u32)n);
    bytebuf_push_u8(w->out, (u8)':');
    w->key_pending = true;
}

static void jsonw_keyn(JsonW *w, const u8 *k, u32 n)
{
    if (w->depth == 0u || w->kind[w->depth - 1u] != YEW_JS_OBJ ||
        w->key_pending)
        YEW_BUG("JSON key outside object or without prior value");
    if (jsonw_bit(w, w->depth - 1u))
        bytebuf_push_u8(w->out, (u8)',');
    jsonw_set_bit(w, w->depth - 1u, true);
    jsonw_quoted(w, k, n);
    bytebuf_push_u8(w->out, (u8)':');
    w->key_pending = true;
}

void yew_jsonw_str(JsonW *w, const u8 *s, u32 n)
{
    jsonw_before_value(w);
    jsonw_quoted(w, s, n);
}

void yew_jsonw_cstr(JsonW *w, const char *s)
{
    size_t n = strlen(s);

    if (n > UINT32_MAX)
        YEW_BUG("JSON string is too long");
    yew_jsonw_str(w, (const u8 *)s, (u32)n);
}

void yew_jsonw_int(JsonW *w, i64 v)
{
    jsonw_before_value(w);
    bytebuf_printf(w->out, "%" PRId64, v);
}

void yew_jsonw_real(JsonW *w, double v)
{
    char buf[32];
    int n;

    if (!isfinite(v))
        YEW_BUG("JSON writer cannot emit a non-finite real");
    jsonw_before_value(w);
    /* The sole floating formatter; C locale is permanent (see parser). */
    n = snprintf(buf, sizeof(buf), "%.17g", v);
    if (n < 0 || (size_t)n >= sizeof(buf))
        YEW_BUG("JSON real formatting failed");
    bytebuf_append(w->out, buf, (size_t)n);
    if (strchr(buf, '.') == NULL && strchr(buf, 'e') == NULL &&
        strchr(buf, 'E') == NULL)
        bytebuf_append(w->out, ".0", 2u);
}

void yew_jsonw_bool(JsonW *w, bool v)
{
    jsonw_before_value(w);
    bytebuf_append(w->out, v ? "true" : "false", v ? 4u : 5u);
}

void yew_jsonw_null(JsonW *w)
{
    jsonw_before_value(w);
    bytebuf_append(w->out, "null", 4u);
}

void yew_jsonw_raw(JsonW *w, const u8 *json, u32 n)
{
    jsonw_before_value(w);
    bytebuf_append(w->out, json, n);
}

void yew_jsonw_value(JsonW *w, const JsonValue *v)
{
    u32 i;

    if (v == NULL)
        YEW_BUG("JSON writer received a NULL tree");
    switch (v->kind) {
    case YEW_JS_NULL: yew_jsonw_null(w); break;
    case YEW_JS_BOOL: yew_jsonw_bool(w, v->b); break;
    case YEW_JS_INT: yew_jsonw_int(w, v->i); break;
    case YEW_JS_REAL: yew_jsonw_real(w, v->d); break;
    case YEW_JS_STR: yew_jsonw_str(w, v->s.p, v->s.len); break;
    case YEW_JS_ARR:
        yew_jsonw_arr(w);
        for (i = 0u; i < v->arr.n; i++)
            yew_jsonw_value(w, v->arr.v[i]);
        yew_jsonw_arr_end(w);
        break;
    case YEW_JS_OBJ:
        yew_jsonw_obj(w);
        for (i = 0u; i < v->obj.n; i++) {
            JsonMember *m = &v->obj.m[i];

            jsonw_keyn(w, m->key, m->klen);
            yew_jsonw_value(w, m->val);
        }
        yew_jsonw_obj_end(w);
        break;
    default: YEW_BUG("JSON writer received invalid value kind");
    }
}
