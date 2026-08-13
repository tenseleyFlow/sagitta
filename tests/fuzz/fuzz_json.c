#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>

#include "mod/lsp/json.h"
#include "util/arena.h"
#include "util/buf.h"

static bool check_json(const u8 *data, size_t len,
                       char *why, size_t why_cap)
{
    Arena parsed_arena;
    Arena reparsed_arena;
    JsonErr err;
    JsonValue *parsed;
    JsonValue *reparsed;
    Bytebuf first;
    Bytebuf second;
    JsonW writer;
    bool ok = true;

    arena_init(&parsed_arena);
    parsed = yew_json_parse(&parsed_arena, data, (u64)len, &err);
    if (parsed == NULL) {
        arena_free_all(&parsed_arena);
        return true;
    }

    bytebuf_init(&first);
    bytebuf_init(&second);
    yew_jsonw_init(&writer, &first);
    yew_jsonw_value(&writer, parsed);
    yew_jsonw_init(&writer, &second);
    yew_jsonw_value(&writer, parsed);
    if (first.len != second.len ||
        (first.len != 0U &&
         memcmp(first.data, second.data, first.len) != 0)) {
        (void)snprintf(why, why_cap,
                       "same JSON tree produced different bytes");
        ok = false;
        goto out_buffers;
    }

    arena_init(&reparsed_arena);
    reparsed = yew_json_parse(&reparsed_arena, first.data,
                              (u64)first.len, &err);
    if (reparsed == NULL) {
        (void)snprintf(why, why_cap,
                       "writer output did not parse at byte %llu: %s",
                       (unsigned long long)err.off, err.msg);
        ok = false;
    } else if (!yew_json_eq(parsed, reparsed)) {
        (void)snprintf(why, why_cap,
                       "parse/write/parse changed the JSON tree");
        ok = false;
    }
    arena_free_all(&reparsed_arena);

out_buffers:
    bytebuf_free(&second);
    bytebuf_free(&first);
    arena_free_all(&parsed_arena);
    return ok;
}

static bool check_string_shape(const u8 *data, size_t len,
                               char *why, size_t why_cap)
{
    JsonValue source;
    JsonValue *parsed;
    JsonW writer;
    JsonErr err;
    Bytebuf encoded;
    Arena arena;
    bool ok = true;

    (void)memset(&source, 0, sizeof source);
    source.kind = YEW_JS_STR;
    source.s.p = data;
    source.s.len = (u32)len;
    bytebuf_init(&encoded);
    yew_jsonw_init(&writer, &encoded);
    yew_jsonw_value(&writer, &source);
    arena_init(&arena);
    parsed = yew_json_parse(&arena, encoded.data, (u64)encoded.len, &err);
    if (parsed == NULL) {
        (void)snprintf(why, why_cap,
                       "arbitrary-byte string encoding did not parse: %s",
                       err.msg);
        ok = false;
    } else if (!yew_json_eq(&source, parsed)) {
        (void)snprintf(why, why_cap,
                       "arbitrary-byte string did not round-trip");
        ok = false;
    }
    arena_free_all(&arena);
    bytebuf_free(&encoded);
    return ok;
}

static bool check_input(const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    return check_json(data, len, why, why_cap) &&
           check_string_shape(data, len, why, why_cap);
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_json", NULL, check_input);
}
