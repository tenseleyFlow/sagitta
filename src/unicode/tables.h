#ifndef SAG_UNICODE_TABLES_H
#define SAG_UNICODE_TABLES_H

#include "../util/base.h"

#define SAG_TRIE_HI 0x30000u
#define SAG_TRIE_SHIFT 7u
#define SAG_TRIE_BLOCK (1u << SAG_TRIE_SHIFT)

enum {
    SAG_U_GCB_MASK = 0x000Fu,
    SAG_U_INCB_SHIFT = 4,
    SAG_U_INCB_MASK = 0x0030u,
    SAG_U_EXT_PICT = 0x0040u,
    SAG_U_EAW_SHIFT = 7,
    SAG_U_EAW_MASK = 0x0380u,
    SAG_U_ZERO_WIDTH = 0x0400u,
    SAG_U_EMOJI = 0x0800u,
    SAG_U_EMOJI_PRESENTATION = 0x1000u
};

struct SagURange {
    u32 lo;
    u32 hi;
    u16 rec;
};

extern const u16 sag_u_stage1[SAG_TRIE_HI >> SAG_TRIE_SHIFT];
extern const u8 sag_u_stage2[];
extern const u16 sag_u_pal[];
extern const struct SagURange sag_u_hi[];
extern const u32 sag_u_hi_len;

u16 sag_u_hi_lookup(u32 cp);

static inline u16 sag_u_rec(u32 cp)
{
    if (cp < SAG_TRIE_HI) {
        size_t block = (size_t)sag_u_stage1[cp >> SAG_TRIE_SHIFT];
        size_t slot = (size_t)(cp & (SAG_TRIE_BLOCK - 1u));
        return sag_u_pal[sag_u_stage2[(block << SAG_TRIE_SHIFT) + slot]];
    }
    return sag_u_hi_lookup(cp);
}

#endif
