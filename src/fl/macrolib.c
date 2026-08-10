#include "fl/macrolib.h"

#include <string.h>

static void header_field(SagMacroText *field, const char *s, size_t n)
{
    while (n != 0U && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n != 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\t' ||
                       s[n - 1U] == '\r'))
        n--;
    field->s = s;
    field->len = n > UINT32_MAX ? UINT32_MAX : (u32)n;
    field->present = true;
}

static bool header_key(const char *line, size_t n, const char *key,
                       const char **value, size_t *value_len)
{
    size_t kn = strlen(key);

    if (n < kn || memcmp(line, key, kn) != 0)
        return false;
    *value = line + kn;
    *value_len = n - kn;
    return true;
}

SagMacroHeaderStatus sag_macro_header_parse(const char *source, size_t len,
                                             SagMacroHeader *out)
{
    size_t at = 0U;

    if (out == NULL)
        return SAG_MACRO_HEADER_UNSUPPORTED;
    (void)memset(out, 0, sizeof(*out));
    if (source == NULL)
        return len == 0U ? SAG_MACRO_HEADER_OK :
                           SAG_MACRO_HEADER_UNSUPPORTED;
    while (at < len) {
        size_t end = at;
        const char *line;
        size_t n;
        const char *value;
        size_t value_len;

        while (end < len && source[end] != '\n')
            end++;
        line = source + at;
        n = end - at;
        while (n != 0U && (*line == ' ' || *line == '\t')) {
            line++;
            n--;
        }
        if (n == 0U) {
            at = end < len ? end + 1U : end;
            continue;
        }
        if (*line != '#')
            break;
        line++;
        n--;
        while (n != 0U && (*line == ' ' || *line == '\t')) {
            line++;
            n--;
        }
        if (header_key(line, n, "sagitta-macro:", &value, &value_len)) {
            SagMacroText schema = {0};

            header_field(&schema, value, value_len);
            out->has_schema = true;
            if (schema.len == 1U && schema.s[0] >= '0' &&
                schema.s[0] <= '9')
                out->schema = (u32)(schema.s[0] - '0');
            else
                out->schema = UINT32_MAX;
        } else if (header_key(line, n, "recorded-with:", &value,
                              &value_len)) {
            header_field(&out->recorded_with, value, value_len);
        } else if (header_key(line, n, "keymap:", &value, &value_len)) {
            header_field(&out->keymap, value, value_len);
        } else if (header_key(line, n, "recorded:", &value, &value_len)) {
            header_field(&out->recorded, value, value_len);
        } else if (header_key(line, n, "describe:", &value, &value_len)) {
            header_field(&out->describe, value, value_len);
        }
        at = end < len ? end + 1U : end;
    }
    return out->has_schema && out->schema != 1U ?
           SAG_MACRO_HEADER_UNSUPPORTED : SAG_MACRO_HEADER_OK;
}
