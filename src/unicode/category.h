#ifndef YEW_UNICODE_CATEGORY_H
#define YEW_UNICODE_CATEGORY_H

/*
 * Unicode general-category and core-property bits.
 *
 * Sprint 2 emitted grapheme, width and word-break properties, which is
 * everything the renderer and the unit engines need.  Sprint 20's regex
 * engine needs more: \w is Alphabetic ∪ Nd ∪ Pc, and the POSIX bracket
 * classes are defined directly over general categories.  Answering those
 * from word-break data alone silently excludes CJK from \w — Han is
 * word-break Other by design, so that segmentation does not glue
 * ideographs together — which is correct for word motion and wrong for
 * "match a letter".  Hence this table.
 */

#include "../util/base.h"

#define YEW_CAT_TRIE_HI 0x30000u
#define YEW_CAT_TRIE_SHIFT 7u
#define YEW_CAT_TRIE_BLOCK (1u << YEW_CAT_TRIE_SHIFT)

enum {
    YEW_CAT_ALPHA = 0x0001u,    /* Alphabetic (derived core property)   */
    YEW_CAT_ND = 0x0002u,       /* general category Nd                  */
    YEW_CAT_UPPER = 0x0004u,    /* Uppercase                            */
    YEW_CAT_LOWER = 0x0008u,    /* Lowercase                            */
    YEW_CAT_PUNCT = 0x0010u,    /* P* or S*                             */
    YEW_CAT_CNTRL = 0x0020u,    /* Cc                                   */
    YEW_CAT_ZS = 0x0040u,       /* Zs                                   */
    YEW_CAT_PC = 0x0080u,       /* Pc — connector punctuation ('_')     */
    YEW_CAT_ASSIGNED = 0x0100u
};

struct YewCatRange {
    u32 lo;
    u32 hi;
    u16 rec;
};

extern const u16 yew_cat_stage1[YEW_CAT_TRIE_HI >> YEW_CAT_TRIE_SHIFT];
extern const u8 yew_cat_stage2[];
extern const u16 yew_cat_pal[];
extern const struct YewCatRange yew_cat_hi[];
extern const u32 yew_cat_hi_len;

u16 yew_cat_hi_lookup(u32 cp);

static inline u16 yew_cat_rec(u32 cp)
{
    if (cp < YEW_CAT_TRIE_HI) {
        size_t block = (size_t)yew_cat_stage1[cp >> YEW_CAT_TRIE_SHIFT];
        size_t slot = (size_t)(cp & (YEW_CAT_TRIE_BLOCK - 1u));

        return yew_cat_pal[yew_cat_stage2[(block << YEW_CAT_TRIE_SHIFT) +
                                          slot]];
    }
    return yew_cat_hi_lookup(cp);
}

#endif
