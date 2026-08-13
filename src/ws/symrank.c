#include "ws/symidx.h"

#include <limits.h>

static const i32 PROX_MUL[YEW_PROX_N] = {150, 130, 115, 100};
static const i32 REC_MUL[8] = {160, 140, 125, 115, 108, 104, 102, 100};
static const i32 KIND_MUL[YEW_SYMK_NKIND] = {100, 118, 112, 105, 90};

static i32 rec_mul(u32 age)
{
    u32 bucket = 0U;

    while ((age >> bucket) != 0U && bucket < 7U)
        bucket++;
    return REC_MUL[bucket];
}

i32 yew_sym_rank(i32 fuzzy, u32 age, SymProx prox, u8 kind, u16 hits)
{
    i64 rank;

    /* Corrupt or future enum values must not become table indices.  The
     * neutral tiers are deterministic and avoid rewarding invalid input. */
    if ((u32)prox >= (u32)YEW_PROX_N)
        prox = YEW_PROX_WS;
    if (kind >= (u8)YEW_SYMK_NKIND)
        kind = (u8)YEW_SYMK_WORD;

    rank = (i64)fuzzy * PROX_MUL[prox] / 100;
    rank = rank * rec_mul(age) / 100;
    rank = rank * KIND_MUL[kind] / 100;
    rank += hits > 16U ? 16 : (i64)hits;

    if (rank > (i64)INT32_MAX)
        return INT32_MAX;
    if (rank < (i64)INT32_MIN)
        return INT32_MIN;
    return (i32)rank;
}
