#include "wordbreak.h"

#include "tables.h"

static u8 wb_high_record(u32 cp)
{
    size_t lo = 0;
    size_t hi = sag_wb_hi_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (cp < sag_wb_hi[mid].lo)
            hi = mid;
        else if (cp > sag_wb_hi[mid].hi)
            lo = mid + 1u;
        else
            return sag_wb_hi[mid].rec;
    }
    return 0;
}

static u8 wb_record(u32 cp)
{
    if (cp < SAG_WB_TRIE_HI) {
        size_t block = (size_t)sag_wb_stage1[cp >> SAG_WB_TRIE_SHIFT];
        size_t slot = (size_t)(cp & (SAG_WB_TRIE_BLOCK - 1u));
        return sag_wb_stage2[(block << SAG_WB_TRIE_SHIFT) + slot];
    }
    return wb_high_record(cp);
}

SagWb sag_wb_prop(u32 cp)
{
    return (SagWb)(wb_record(cp) & SAG_WB_RECORD_MASK);
}

bool sag_wb_is_ignored(SagWb prop)
{
    return prop == SAG_WB_EXTEND || prop == SAG_WB_FORMAT ||
           prop == SAG_WB_ZWJ;
}

bool sag_unicode_is_white_space(u32 cp)
{
    return (wb_record(cp) & SAG_WB_RECORD_WHITE_SPACE) != 0;
}

bool sag_unicode_is_extended_pictographic(u32 cp)
{
    return (sag_u_rec(cp) & SAG_U_EXT_PICT) != 0;
}
