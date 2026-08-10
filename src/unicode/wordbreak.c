#include "wordbreak.h"

#include "tables.h"

static u8 wb_high_record(u32 cp)
{
    size_t lo = 0;
    size_t hi = yew_wb_hi_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (cp < yew_wb_hi[mid].lo)
            hi = mid;
        else if (cp > yew_wb_hi[mid].hi)
            lo = mid + 1u;
        else
            return yew_wb_hi[mid].rec;
    }
    return 0;
}

static u8 wb_record(u32 cp)
{
    if (cp < YEW_WB_TRIE_HI) {
        size_t block = (size_t)yew_wb_stage1[cp >> YEW_WB_TRIE_SHIFT];
        size_t slot = (size_t)(cp & (YEW_WB_TRIE_BLOCK - 1u));
        return yew_wb_stage2[(block << YEW_WB_TRIE_SHIFT) + slot];
    }
    return wb_high_record(cp);
}

YewWb yew_wb_prop(u32 cp)
{
    return (YewWb)(wb_record(cp) & YEW_WB_RECORD_MASK);
}

bool yew_wb_is_ignored(YewWb prop)
{
    return prop == YEW_WB_EXTEND || prop == YEW_WB_FORMAT ||
           prop == YEW_WB_ZWJ;
}

bool yew_unicode_is_white_space(u32 cp)
{
    return (wb_record(cp) & YEW_WB_RECORD_WHITE_SPACE) != 0;
}

bool yew_unicode_is_extended_pictographic(u32 cp)
{
    return (yew_u_rec(cp) & YEW_U_EXT_PICT) != 0;
}
