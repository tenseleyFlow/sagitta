#ifndef YEW_JSON_H
#define YEW_JSON_H

#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

#define YEW_JSON_MAX_DEPTH 128
#define YEW_JSON_MAX_BYTES (64u * 1024u * 1024u)
#define YEW_JSON_MAX_NODES (4u * 1024u * 1024u)

typedef enum {
    YEW_JS_NULL = 0,
    YEW_JS_BOOL,
    YEW_JS_INT,
    YEW_JS_REAL,
    YEW_JS_STR,
    YEW_JS_ARR,
    YEW_JS_OBJ
} JsonKind;

enum {
    YEW_JSF_LOSSY_NUM = 1u << 0,
    /* A string with this flag is deliberately not valid UTF-8. */
    YEW_JSF_RAW_BYTE = 1u << 1,
    YEW_JSF_DUP_KEY = 1u << 2
};

typedef struct JsonValue JsonValue;

typedef struct JsonMember {
    const u8 *key;
    u32 klen;
    JsonValue *val;
} JsonMember;

struct JsonValue {
    u8 kind;
    u8 flags;
    u16 _pad;
    union {
        bool b;
        i64 i;
        double d;
        /* JSON strings are length-carrying and are never NUL-terminated. */
        struct {
            const u8 *p;
            u32 len;
        } s;
        struct {
            JsonValue **v;
            u32 n;
        } arr;
        /* Members remain in insertion order. */
        struct {
            JsonMember *m;
            u32 n;
        } obj;
    };
};

_Static_assert(sizeof(JsonValue) <= 24, "json value bloat");

typedef struct JsonErr {
    u32 line;
    u32 col;
    u64 off;
    char msg[96];
} JsonErr;

/* Arena-only. The arena may contain garbage after a failed parse. */
JsonValue *yew_json_parse(Arena *a, const u8 *s, u64 len, JsonErr *err);

const JsonValue *yew_json_get(const JsonValue *o, const char *key);
const JsonValue *yew_json_at(const JsonValue *a, u32 i);
u32 yew_json_len(const JsonValue *v);
i64 yew_json_int(const JsonValue *v, i64 dflt);
double yew_json_real(const JsonValue *v, double dflt);
bool yew_json_bool(const JsonValue *v, bool dflt);
const u8 *yew_json_str(const JsonValue *v, u32 *len);
bool yew_json_streq(const JsonValue *v, const char *s);
const JsonValue *yew_json_path(const JsonValue *root, const char *path);
bool yew_json_eq(const JsonValue *a, const JsonValue *b);

typedef struct JsonW {
    Bytebuf *out;
    u8 need_comma[YEW_JSON_MAX_DEPTH / 8];
    u8 kind[YEW_JSON_MAX_DEPTH];
    u16 depth;
    bool key_pending;
    bool root_written;
} JsonW;

void yew_jsonw_init(JsonW *w, Bytebuf *out);
void yew_jsonw_obj(JsonW *w);
void yew_jsonw_obj_end(JsonW *w);
void yew_jsonw_arr(JsonW *w);
void yew_jsonw_arr_end(JsonW *w);
void yew_jsonw_key(JsonW *w, const char *k);
void yew_jsonw_str(JsonW *w, const u8 *s, u32 n);
void yew_jsonw_cstr(JsonW *w, const char *s);
void yew_jsonw_int(JsonW *w, i64 v);
/* Originated protocol fields avoid reals; this is for tree re-emission. */
void yew_jsonw_real(JsonW *w, double v);
void yew_jsonw_bool(JsonW *w, bool v);
void yew_jsonw_null(JsonW *w);
void yew_jsonw_raw(JsonW *w, const u8 *json, u32 n);
void yew_jsonw_value(JsonW *w, const JsonValue *v);

#endif
