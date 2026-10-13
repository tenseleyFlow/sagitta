/*
 * YEW-F-085 — REP2 unpack accepts impossible direct-literal metadata.
 *
 * Correct behavior: bounds/shape validation rejects a REP2 claiming ICASE
 * or extra capture groups, because the only compiler-produced REP2 form is
 * a case-sensitive, one-group direct whole literal.
 *
 * Baseline failure: yew_re_unpack validates fields independently, accepts
 * both mutations, then exposes metadata the direct matcher does not honor.
 */
#include "audit.h"

#include <stdio.h>
#include <string.h>

#include "search/regex.h"
#include "util/arena.h"
#include "util/buf.h"

static void put32(u8 *p, u32 value)
{
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8U);
    p[2] = (u8)(value >> 16U);
    p[3] = (u8)(value >> 24U);
}

bool test_yew_f_085(char *why, size_t why_cap)
{
    enum { LITERAL_LEN = 4094 };
    u8 literal[LITERAL_LEN];
    Arena arena;
    Arena unpack_arena;
    Bytebuf packed;
    YewReErr err = {0};
    YewRe *re;
    bool passed = false;

    (void)memset(literal, 'a', sizeof(literal));
    bytebuf_init(&packed);
    arena_init(&arena);
    re = yew_re_compile(&arena, (const char *)literal, sizeof(literal),
                        YEW_RE_LITERAL, &err);
    if (re == NULL || !yew_re_pack(re, &packed) || packed.len < 96U) {
        (void)snprintf(why, why_cap, "could not build REP2 control");
        goto done;
    }
    put32(packed.data + 24U, YEW_RE_LITERAL | YEW_RE_ICASE);
    arena_init(&unpack_arena);
    if (yew_re_unpack(&unpack_arena, packed.data, packed.len, NULL) != NULL) {
        (void)snprintf(why, why_cap, "REP2 accepted impossible ICASE");
        arena_free_all(&unpack_arena);
        goto done;
    }
    arena_free_all(&unpack_arena);
    put32(packed.data + 24U, YEW_RE_LITERAL);
    put32(packed.data + 20U, 2U);
    arena_init(&unpack_arena);
    if (yew_re_unpack(&unpack_arena, packed.data, packed.len, NULL) != NULL) {
        (void)snprintf(why, why_cap, "REP2 accepted two capture groups");
        arena_free_all(&unpack_arena);
        goto done;
    }
    arena_free_all(&unpack_arena);
    passed = true;
done:
    arena_free_all(&arena);
    bytebuf_free(&packed);
    return passed;
}
