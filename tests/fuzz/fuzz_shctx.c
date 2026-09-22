/*
 * Sprint 57.23: the shell context lexer under arbitrary bytes.
 *
 * The input is a `:!` body.  If it holds the corpus's caret marker `‸`
 * (the seed corpus is tests/unit/shctx_corpus.h's rows, one file each),
 * the first one is removed and marks the caret; the lexer is also run
 * with the caret at the end and at a byte-derived position, so a mutated
 * input reaches every state with more than one caret.
 *
 * Asserted: termination (the harness watchdog), replace.lo <= replace.hi
 * == caret, arg_index <= argc, argv NULL-terminated, depth <= 64, and --
 * in the SAN=1 build -- no ASan/UBSan report.  The quoting of every stem
 * in every state and the `:!` point parser's rebasing ride along.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "ui/cmdparse.h"
#include "ui/shctx.h"
#include "util/arena.h"

static Ed fuzz_editor;

static bool check_one(const char *line, size_t len, size_t caret, Arena *a,
                      char *why, size_t why_cap)
{
    YewShCtx ctx;
    u32 q;

    if (!yew_shctx_at(line, len, caret, a, &ctx)) {
        (void)snprintf(why, why_cap, "refused caret %zu of %zu", caret, len);
        return false;
    }
    if (ctx.replace.lo > ctx.replace.hi || ctx.replace.hi != caret) {
        (void)snprintf(why, why_cap, "replace [%llu,%llu) caret %zu",
                       (unsigned long long)ctx.replace.lo,
                       (unsigned long long)ctx.replace.hi, caret);
        return false;
    }
    if (ctx.arg_index > ctx.argc || ctx.argv == NULL ||
        ctx.argv[ctx.argc] != NULL || ctx.stem == NULL) {
        (void)snprintf(why, why_cap, "argv shape argc=%u arg_index=%u",
                       (unsigned)ctx.argc, (unsigned)ctx.arg_index);
        return false;
    }
    if (ctx.depth > YEW_SH_DEPTH_MAX) {
        (void)snprintf(why, why_cap, "depth %u", (unsigned)ctx.depth);
        return false;
    }
    for (q = YEW_SH_Q_NONE; q <= YEW_SH_Q_DOLLAR; q++) {
        const char *closing = NULL;

        (void)yew_shq_insert(a, ctx.stem, strlen(ctx.stem), 0U,
                             (YewShQuote)q, &closing);
        if (closing == NULL) {
            (void)snprintf(why, why_cap, "no closing for state %u",
                           (unsigned)q);
            return false;
        }
    }
    return true;
}

static bool check_shctx(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    static const u8 mark[] = {0xE2U, 0x80U, 0xB8U};
    Arena arena;
    char *line;
    size_t n = len;
    size_t caret = len;
    size_t i;
    bool ok = true;

    line = malloc(len == 0U ? 1U : len + 3U);
    if (line == NULL) {
        (void)snprintf(why, why_cap, "out of memory");
        return false;
    }
    for (i = 0U; i + 3U <= len; i++) {
        if (memcmp(data + i, mark, 3U) == 0)
            break;
    }
    if (i + 3U <= len) {
        (void)memcpy(line, data, i);
        (void)memcpy(line + i, data + i + 3U, len - i - 3U);
        n = len - 3U;
        caret = i;
    } else if (len != 0U) {
        (void)memcpy(line, data, len);
    }
    arena_init(&arena);
    ok = check_one(line, n, caret, &arena, why, why_cap) &&
         check_one(line, n, n, &arena, why, why_cap) &&
         check_one(line, n, n == 0U ? 0U : (size_t)data[0] % (n + 1U),
                   &arena, why, why_cap);
    /* The point parser rebases the body's span onto the prompt. */
    if (ok && memchr(line, '\0', n) == NULL && n < 4096U) {
        char *prompt = malloc(n + 3U);
        CmdParsePoint point;

        if (prompt == NULL) {
            (void)snprintf(why, why_cap, "out of memory");
            ok = false;
        } else {
            prompt[0] = ':';
            prompt[1] = '!';
            if (n != 0U)
                (void)memcpy(prompt + 2U, line, n);
            if (yew_cmd_parse_point(&fuzz_editor, prompt, n + 2U,
                                    caret + 2U, &arena, &point) &&
                point.bang_body &&
                (point.shell == NULL || point.token.hi != caret + 2U ||
                 point.token.lo < 2U)) {
                (void)snprintf(why, why_cap, "point token [%llu,%llu)",
                               (unsigned long long)point.token.lo,
                               (unsigned long long)point.token.hi);
                ok = false;
            }
            free(prompt);
        }
    }
    arena_free_all(&arena);
    free(line);
    return ok;
}

int main(int argc, char **argv)
{
    int status;

    yew_ed_init(&fuzz_editor);
    status = yew_fuzz_main(argc, argv, "fuzz_shctx", NULL, check_shctx);
    yew_ed_free(&fuzz_editor);
    yew_cmd_shutdown();
    return status;
}
