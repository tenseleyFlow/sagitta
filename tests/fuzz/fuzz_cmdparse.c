#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/cmdparse.h"
#include "util/arena.h"

static Ed fuzz_editor;

static bool check_failure_token(const CmdParse *parsed, size_t len,
                                char *why, size_t why_cap)
{
    if (len == 0U) {
        if (parsed->err.tok_lo == 0U && parsed->err.tok_hi == 0U)
            return true;
    } else if (parsed->err.tok_lo < parsed->err.tok_hi &&
               parsed->err.tok_hi <= len) {
        return true;
    }
    (void)snprintf(why, why_cap,
                   "failure token [%u,%u) is outside input length %zu",
                   (unsigned)parsed->err.tok_lo,
                   (unsigned)parsed->err.tok_hi, len);
    return false;
}

static bool parse_one(const u8 *data, size_t len,
                      char *why, size_t why_cap)
{
    Arena arena;
    CmdParse parsed;
    bool ok;

    arena_init(&arena);
    ok = sag_cmd_parse(&fuzz_editor, (const char *)data, len, &arena,
                       &parsed);
    if (!ok && !check_failure_token(&parsed, len, why, why_cap)) {
        arena_free_all(&arena);
        return false;
    }
    arena_free_all(&arena);
    return true;
}

static bool check_cmdparse(const u8 *data, size_t len,
                           char *why, size_t why_cap)
{
    static const char prefix[] = "file.write ";
    size_t prefix_len = sizeof(prefix) - 1U;
    size_t shaped_len;
    u8 *shaped;
    bool ok;

    if (!parse_one(data, len, why, why_cap))
        return false;
    if (len > SIZE_MAX - prefix_len) {
        (void)snprintf(why, why_cap, "command-shaped input size overflow");
        return false;
    }
    shaped_len = prefix_len + len;
    shaped = malloc(shaped_len == 0U ? 1U : shaped_len);
    if (shaped == NULL) {
        (void)snprintf(why, why_cap, "out of memory");
        return false;
    }
    (void)memcpy(shaped, prefix, prefix_len);
    if (len != 0U)
        (void)memcpy(shaped + prefix_len, data, len);
    ok = parse_one(shaped, shaped_len, why, why_cap);
    free(shaped);
    return ok;
}

int main(int argc, char **argv)
{
    int status;

    sag_ed_init(&fuzz_editor);
    status = sag_fuzz_main(argc, argv, "fuzz_cmdparse", NULL,
                           check_cmdparse);
    sag_ed_free(&fuzz_editor);
    sag_cmd_shutdown();
    return status;
}
