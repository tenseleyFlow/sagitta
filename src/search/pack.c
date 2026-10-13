#include "search/regex_internal.h"

#include <string.h>

#include "unicode/utf8.h"

enum {
    RE_PACK_HEADER = 96,
    RE_PACK_INST = 16,
    RE_PACK_RANGE = 8
};

static const u8 re_pack_magic_v1[8] =
    {'Y', 'E', 'W', 'R', 'E', 'P', '1', 0};
static const u8 re_pack_magic_v2[8] =
    {'Y', 'E', 'W', 'R', 'E', 'P', '2', 0};

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8U);
    p[2] = (u8)(v >> 16U);
    p[3] = (u8)(v >> 24U);
}

static u32 get32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8U | (u32)p[2] << 16U |
           (u32)p[3] << 24U;
}

static bool add_size(size_t *size, size_t count, size_t width)
{
    if (count != 0U && width > (SIZE_MAX - *size) / count)
        return false;
    *size += count * width;
    return true;
}

static void pack_inst(u8 *p, const ReInst *ins)
{
    (void)memset(p, 0, RE_PACK_INST);
    put32(p, ins->x);
    put32(p + 4U, ins->y);
    put32(p + 8U, ins->arg);
    p[12] = ins->op;
}

static bool inst_valid(const ReInst *ins, u32 nprog, u32 nclasses,
                       u32 ngroups)
{
    if (ins->op > RE_MATCH)
        return false;
    if (ins->op == RE_JMP && ins->x >= nprog)
        return false;
    if (ins->op == RE_SPLIT && (ins->x >= nprog || ins->y >= nprog))
        return false;
    if (ins->op == RE_CLASS && ins->arg >= nclasses)
        return false;
    if (ins->op == RE_SAVE && ins->arg >= ngroups * 2U)
        return false;
    if (ins->op == RE_CHAR && ins->arg > 0x10FFFFU)
        return false;
    if (ins->op == RE_ANY && ins->arg > 1U)
        return false;
    return true;
}

static bool inst_is(const ReInst *ins, ReOp op, u32 x, u32 y, u32 arg)
{
    return ins->op == (u8)op && ins->x == x && ins->y == y &&
           ins->arg == arg;
}

/* REP2 is emitted only by build_large_literal().  Unlike REP1, it bypasses
 * the Pike/DFA instruction stream at execution time, so independently valid
 * header fields are insufficient: the metadata and inert program must be the
 * one exact shape the compiler can produce. */
static bool rep2_header_valid(const YewRe *re, u32 bools)
{
    return re->nprog == 3U && re->nrprog == 3U &&
           re->nclasses == 0U && re->ngroups == 1U &&
           (re->flags & YEW_RE_ICASE) == 0U &&
           (bools & ~1U) == 0U &&
           re->min_len == re->max_len &&
           re->min_len > YEW_RE_MAX_PROG - 3U &&
           re->lit.kind == RE_LIT_WHOLE &&
           re->lit.n > sizeof(re->lit.small) && !re->lit.anchored;
}

static bool rep2_program_valid(const YewRe *re)
{
    return inst_is(&re->prog[0], RE_SAVE, 0U, 0U, 0U) &&
           inst_is(&re->prog[1], RE_SAVE, 0U, 0U, 1U) &&
           inst_is(&re->prog[2], RE_MATCH, 0U, 0U, 0U) &&
           inst_is(&re->rprog[0], RE_SAVE, 0U, 0U, 1U) &&
           inst_is(&re->rprog[1], RE_SAVE, 0U, 0U, 0U) &&
           inst_is(&re->rprog[2], RE_MATCH, 0U, 0U, 0U);
}

static bool rep2_literal_valid(const YewRe *re, const u8 *literal)
{
    size_t at = 0U;
    u32 codepoints = 0U;

    while (at < re->lit.n) {
        u32 cp;
        size_t step = yew_utf8_decode(literal + at, re->lit.n - at, &cp);

        if (step == 0U)
            return false;
        at += step;
        codepoints++;
    }
    return codepoints == re->min_len;
}

bool yew_re_pack(const YewRe *re, Bytebuf *out)
{
    size_t size = RE_PACK_HEADER;
    size_t start;
    u8 *p;
    u32 i;
    u32 flags = 0U;
    bool long_literal;

    if (re == NULL || out == NULL || re->nprog > YEW_RE_MAX_PROG ||
        re->nrprog > YEW_RE_MAX_PROG || re->nclasses > YEW_RE_MAX_CLASSES ||
        re->ngroups == 0U || re->ngroups > YEW_RE_MAX_GROUPS ||
        re->lit.n > YEW_RE_MAX_PATTERN ||
        (re->lit.n != 0U && re->lit.s == NULL) ||
        (re->lit.n > sizeof(re->lit.small) &&
         re->lit.kind != RE_LIT_WHOLE))
        return false;
    long_literal = re->lit.n > sizeof(re->lit.small);
    if (!add_size(&size, re->nprog, RE_PACK_INST) ||
        !add_size(&size, re->nrprog, RE_PACK_INST))
        return false;
    for (i = 0U; i < re->nclasses; i++) {
        if (!add_size(&size, 1U, 4U) ||
            !add_size(&size, re->classes[i].n, RE_PACK_RANGE))
            return false;
    }
    if (long_literal && !add_size(&size, re->lit.n, 1U))
        return false;
    start = out->len;
    if (size > SIZE_MAX - start)
        return false;
    bytebuf_reserve(out, start + size);
    out->len = start + size;
    p = out->data + start;
    (void)memset(p, 0, size);
    (void)memcpy(p, long_literal ? re_pack_magic_v2 : re_pack_magic_v1,
                 sizeof(re_pack_magic_v1));
    put32(p + 8U, re->nprog);
    put32(p + 12U, re->nrprog);
    put32(p + 16U, re->nclasses);
    put32(p + 20U, re->ngroups);
    put32(p + 24U, re->flags);
    put32(p + 28U, re->min_len);
    put32(p + 32U, re->max_len);
    if (re->saw_upper_literal)
        flags |= 1U;
    if (re->force_icase)
        flags |= 2U;
    if (re->force_case)
        flags |= 4U;
    put32(p + 36U, flags);
    put32(p + 40U, re->lit.kind);
    put32(p + 44U, re->lit.n);
    put32(p + 48U, re->lit.anchored ? 1U : 0U);
    if (!long_literal && re->lit.n != 0U)
        (void)memcpy(p + 64U, re->lit.s, re->lit.n);
    p += RE_PACK_HEADER;
    for (i = 0U; i < re->nprog; i++, p += RE_PACK_INST)
        pack_inst(p, &re->prog[i]);
    for (i = 0U; i < re->nrprog; i++, p += RE_PACK_INST)
        pack_inst(p, &re->rprog[i]);
    for (i = 0U; i < re->nclasses; i++) {
        u32 j;

        put32(p, re->classes[i].n);
        p += 4U;
        for (j = 0U; j < re->classes[i].n; j++, p += RE_PACK_RANGE) {
            put32(p, re->classes[i].r[j].lo);
            put32(p + 4U, re->classes[i].r[j].hi);
        }
    }
    if (long_literal) {
        (void)memcpy(p, re->lit.s, re->lit.n);
        p += re->lit.n;
    }
    return (size_t)(p - (out->data + start)) == size;
}

static bool take(size_t *at, size_t len, size_t count, size_t width)
{
    if (*at > len || (count != 0U && width > (len - *at) / count))
        return false;
    *at += count * width;
    return true;
}

YewRe *yew_re_unpack(Arena *a, const u8 *data, size_t len, size_t *used)
{
    YewRe *re;
    size_t at = RE_PACK_HEADER;
    u32 bools;
    u32 i;
    bool v2;

    if (used != NULL)
        *used = 0U;
    if (a == NULL || data == NULL || len < RE_PACK_HEADER)
        return NULL;
    v2 = memcmp(data, re_pack_magic_v2, sizeof(re_pack_magic_v2)) == 0;
    if (!v2 &&
        memcmp(data, re_pack_magic_v1, sizeof(re_pack_magic_v1)) != 0)
        return NULL;
    re = arena_alloc(a, sizeof(*re), _Alignof(YewRe));
    (void)memset(re, 0, sizeof(*re));
    re->nprog = get32(data + 8U);
    re->nrprog = get32(data + 12U);
    re->nclasses = get32(data + 16U);
    re->ngroups = get32(data + 20U);
    re->flags = get32(data + 24U);
    re->min_len = get32(data + 28U);
    re->max_len = get32(data + 32U);
    bools = get32(data + 36U);
    re->lit.kind = (u8)get32(data + 40U);
    re->lit.n = get32(data + 44U);
    re->lit.anchored = get32(data + 48U) != 0U;
    if (re->nprog == 0U || re->nprog > YEW_RE_MAX_PROG ||
        re->nrprog == 0U || re->nrprog > YEW_RE_MAX_PROG ||
        re->nclasses > YEW_RE_MAX_CLASSES || re->ngroups == 0U ||
        re->ngroups > YEW_RE_MAX_GROUPS ||
        (re->flags & ~(YEW_RE_ICASE | YEW_RE_DOTALL | YEW_RE_LITERAL |
                       YEW_RE_NOCAPTURE)) != 0U ||
        (re->max_len != UINT32_MAX && re->max_len < re->min_len) ||
        bools > 7U ||
        re->lit.kind > RE_LIT_WHOLE || re->lit.n > YEW_RE_MAX_PATTERN ||
        (!v2 && re->lit.n > sizeof(re->lit.small)) ||
        (v2 && re->lit.n <= sizeof(re->lit.small)) ||
        (re->lit.n > sizeof(re->lit.small) &&
         re->lit.kind != RE_LIT_WHOLE) ||
        (re->lit.kind == RE_LIT_NONE && re->lit.n != 0U) ||
        (re->lit.kind == RE_LIT_BYTE && re->lit.n != 1U) ||
        (re->lit.kind == RE_LIT_BMH && re->lit.n < 2U) ||
        (re->lit.kind == RE_LIT_WHOLE && re->lit.n == 0U) ||
        get32(data + 48U) > 1U ||
        (v2 && !rep2_header_valid(re, bools)) ||
        !take(&at, len, re->nprog, RE_PACK_INST) ||
        !take(&at, len, re->nrprog, RE_PACK_INST))
        return NULL;
    re->saw_upper_literal = (bools & 1U) != 0U;
    re->force_icase = (bools & 2U) != 0U;
    re->force_case = (bools & 4U) != 0U;
    re->prog = arena_alloc(a, (size_t)re->nprog * sizeof(*re->prog),
                           _Alignof(ReInst));
    re->rprog = arena_alloc(a, (size_t)re->nrprog * sizeof(*re->rprog),
                            _Alignof(ReInst));
    for (i = 0U; i < re->nprog; i++) {
        const u8 *p = data + RE_PACK_HEADER + (size_t)i * RE_PACK_INST;

        re->prog[i] = (ReInst){get32(p), get32(p + 4U), get32(p + 8U),
                               p[12]};
        if (!inst_valid(&re->prog[i], re->nprog, re->nclasses, re->ngroups))
            return NULL;
    }
    for (i = 0U; i < re->nrprog; i++) {
        const u8 *p = data + RE_PACK_HEADER +
                      (size_t)re->nprog * RE_PACK_INST +
                      (size_t)i * RE_PACK_INST;

        re->rprog[i] = (ReInst){get32(p), get32(p + 4U), get32(p + 8U),
                                p[12]};
        if (!inst_valid(&re->rprog[i], re->nrprog, re->nclasses, re->ngroups))
            return NULL;
    }
    if (v2 && !rep2_program_valid(re))
        return NULL;
    re->classes = arena_alloc(a, (size_t)re->nclasses * sizeof(*re->classes),
                              _Alignof(ReClass));
    for (i = 0U; i < re->nclasses; i++) {
        u32 j;
        u32 count;

        if (!take(&at, len, 1U, 4U))
            return NULL;
        count = get32(data + at - 4U);
        if (!take(&at, len, count, RE_PACK_RANGE))
            return NULL;
        re->classes[i].n = count;
        re->classes[i].r = arena_alloc(a, (size_t)count * sizeof(ReRange),
                                       _Alignof(ReRange));
        for (j = 0U; j < count; j++) {
            const u8 *p = data + at - (size_t)(count - j) * RE_PACK_RANGE;

            re->classes[i].r[j].lo = get32(p);
            re->classes[i].r[j].hi = get32(p + 4U);
            if (re->classes[i].r[j].lo > re->classes[i].r[j].hi ||
                re->classes[i].r[j].hi > 0x10FFFFU ||
                (j != 0U && re->classes[i].r[j - 1U].hi >=
                              re->classes[i].r[j].lo))
                return NULL;
        }
    }
    if (v2) {
        size_t literal_at = at;

        if (!take(&at, len, re->lit.n, 1U))
            return NULL;
        /* YEW-F-085: the direct matcher trusts min/max/group/flag metadata
         * rather than executing the serialized program.  Admit only the
         * exact literal cardinality promised by the REP2 header. */
        if (!rep2_literal_valid(re, data + literal_at))
            return NULL;
        yew_lit_prepare(&re->lit, a, data + literal_at, re->lit.n);
    } else if (re->lit.n != 0U) {
        yew_lit_prepare(&re->lit, a, data + 64U, re->lit.n);
    }
    if (used != NULL)
        *used = at;
    return re;
}
