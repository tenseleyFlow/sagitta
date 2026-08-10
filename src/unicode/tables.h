#ifndef YEW_UNICODE_TABLES_H
#define YEW_UNICODE_TABLES_H

#include "../util/base.h"

#define YEW_TRIE_HI 0x30000u
#define YEW_TRIE_SHIFT 7u
#define YEW_TRIE_BLOCK (1u << YEW_TRIE_SHIFT)

enum {
    YEW_U_GCB_MASK = 0x000Fu,
    YEW_U_INCB_SHIFT = 4,
    YEW_U_INCB_MASK = 0x0030u,
    YEW_U_EXT_PICT = 0x0040u,
    YEW_U_EAW_SHIFT = 7,
    YEW_U_EAW_MASK = 0x0380u,
    YEW_U_ZERO_WIDTH = 0x0400u,
    YEW_U_EMOJI = 0x0800u,
    YEW_U_EMOJI_PRESENTATION = 0x1000u
};

struct YewURange {
    u32 lo;
    u32 hi;
    u16 rec;
};

extern const u16 yew_u_stage1[YEW_TRIE_HI >> YEW_TRIE_SHIFT];
extern const u8 yew_u_stage2[];
extern const u16 yew_u_pal[];
extern const struct YewURange yew_u_hi[];
extern const u32 yew_u_hi_len;

u16 yew_u_hi_lookup(u32 cp);

static inline u16 yew_u_rec(u32 cp)
{
    if (cp < YEW_TRIE_HI) {
        size_t block = (size_t)yew_u_stage1[cp >> YEW_TRIE_SHIFT];
        size_t slot = (size_t)(cp & (YEW_TRIE_BLOCK - 1u));
        return yew_u_pal[yew_u_stage2[(block << YEW_TRIE_SHIFT) + slot]];
    }
    return yew_u_hi_lookup(cp);
}

#endif
