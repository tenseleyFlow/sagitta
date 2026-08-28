#include "mod/git/diffview.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/grapheme.h"

typedef struct LineSlice {
    size_t off;
    size_t len;
} LineSlice;

typedef struct ClusterSlice {
    u32 off;
    u32 len;
} ClusterSlice;

VEC_DECL(ClusterSliceVec, ClusterSlice);

static bool rowmap_pair(DiffRowMap *map, i32 left, i32 right)
{
    if (map->left.len == SIZE_MAX || map->right.len == SIZE_MAX)
        return false;
    DiffI32Vec_push(&map->left, left);
    DiffI32Vec_push(&map->right, right);
    return true;
}

static bool rowmap_range(DiffRowMap *map, u32 left, u32 right, u32 count)
{
    u32 i;

    for (i = 0U; i < count; i++)
        if (!rowmap_pair(map, (i32)(left + i), (i32)(right + i)))
            return false;
    return true;
}

void yew_diff_rowmap_init(DiffRowMap *map)
{
    if (map != NULL)
        (void)memset(map, 0, sizeof(*map));
}

void yew_diff_rowmap_drop(DiffRowMap *map)
{
    if (map == NULL)
        return;
    DiffI32Vec_free(&map->left);
    DiffI32Vec_free(&map->right);
}

bool yew_diff_rowmap_build(DiffRowMap *map, u32 base_lines, u32 buf_lines,
                           const GitHunk *hunks, size_t nhunks)
{
    DiffRowMap built = {0};
    u32 base_at = 0U;
    u32 buf_at = 0U;
    size_t h;
    bool ok = false;

    if (map == NULL || (nhunks != 0U && hunks == NULL) ||
        base_lines > (u32)INT32_MAX || buf_lines > (u32)INT32_MAX)
        return false;
    for (h = 0U; h < nhunks; h++) {
        const GitHunk *gh = &hunks[h];
        u32 base_lo;
        u32 buf_lo;
        u32 base_n;
        u32 buf_n;
        u32 unchanged;
        u32 changed;
        u32 i;

        if (gh->base_lo.v > UINT32_MAX || gh->buf_lo.v > UINT32_MAX ||
            gh->base_n.v > UINT32_MAX || gh->buf_n.v > UINT32_MAX)
            goto done;
        base_lo = (u32)gh->base_lo.v;
        buf_lo = (u32)gh->buf_lo.v;
        base_n = (u32)gh->base_n.v;
        buf_n = (u32)gh->buf_n.v;
        if (base_lo < base_at || buf_lo < buf_at ||
            base_lo > base_lines || buf_lo > buf_lines ||
            base_n > base_lines - base_lo || buf_n > buf_lines - buf_lo ||
            base_lo - base_at != buf_lo - buf_at)
            goto done;
        unchanged = base_lo - base_at;
        if (!rowmap_range(&built, base_at, buf_at, unchanged))
            goto done;
        changed = base_n > buf_n ? base_n : buf_n;
        for (i = 0U; i < changed; i++) {
            i32 left = i < base_n ? (i32)(base_lo + i) : YEW_DIFF_FILLER_ROW;
            i32 right = i < buf_n ? (i32)(buf_lo + i) : YEW_DIFF_FILLER_ROW;

            if (!rowmap_pair(&built, left, right))
                goto done;
        }
        base_at = base_lo + base_n;
        buf_at = buf_lo + buf_n;
    }
    if (base_lines - base_at != buf_lines - buf_at ||
        !rowmap_range(&built, base_at, buf_at, base_lines - base_at))
        goto done;
    yew_diff_rowmap_drop(map);
    *map = built;
    (void)memset(&built, 0, sizeof(built));
    ok = true;
done:
    yew_diff_rowmap_drop(&built);
    return ok;
}

size_t yew_diff_rowmap_len(const DiffRowMap *map)
{
    if (map == NULL || map->left.len != map->right.len)
        return 0U;
    return map->left.len;
}

i32 yew_diff_row_source(const DiffRowMap *map, DiffSide side,
                        size_t display_row)
{
    const DiffI32Vec *rows;

    if (map == NULL)
        return YEW_DIFF_FILLER_ROW;
    rows = side == YEW_DIFF_LEFT ? &map->left :
           side == YEW_DIFF_RIGHT ? &map->right : NULL;
    return rows != NULL && display_row < rows->len ? rows->data[display_row] :
                                                    YEW_DIFF_FILLER_ROW;
}

void yew_diff_scratch_init(DiffScratch *scratch)
{
    if (scratch == NULL)
        return;
    bytebuf_init(&scratch->bytes);
    (void)memset(&scratch->source_rows, 0, sizeof(scratch->source_rows));
}

void yew_diff_scratch_drop(DiffScratch *scratch)
{
    if (scratch == NULL)
        return;
    bytebuf_free(&scratch->bytes);
    DiffI32Vec_free(&scratch->source_rows);
}

static bool source_lines(const u8 *source, size_t len, LineSlice **out,
                         size_t *out_n)
{
    size_t count = len == 0U ? 0U : 1U;
    size_t start = 0U;
    size_t at = 0U;
    LineSlice *lines;
    size_t i;

    if ((source == NULL && len != 0U) || out == NULL || out_n == NULL)
        return false;
    for (i = 0U; i < len; i++)
        if (source[i] == '\n') {
            if (count == SIZE_MAX)
                return false;
            count++;
        }
    lines = count == 0U ? NULL : yew_xcalloc(count, sizeof(*lines));
    for (i = 0U; i < len; i++) {
        if (source[i] != '\n')
            continue;
        lines[at++] = (LineSlice){start, i - start};
        start = i + 1U;
    }
    if (count != 0U)
        lines[at++] = (LineSlice){start, len - start};
    if (at != count) {
        yew_xfree(lines);
        return false;
    }
    *out = lines;
    *out_n = count;
    return true;
}

bool yew_diff_scratch_build(DiffScratch *scratch, const u8 *source,
                            size_t source_len, const DiffI32Vec *rows)
{
    DiffScratch built;
    LineSlice *lines = NULL;
    size_t nlines = 0U;
    size_t i;
    bool ok = false;

    if (scratch == NULL || rows == NULL ||
        !source_lines(source, source_len, &lines, &nlines))
        return false;
    yew_diff_scratch_init(&built);
    for (i = 0U; i < rows->len; i++) {
        i32 row = rows->data[i];

        if (row < YEW_DIFF_FILLER_ROW ||
            (row >= 0 && (size_t)row >= nlines))
            goto done;
        if (i != 0U)
            bytebuf_push_u8(&built.bytes, '\n');
        if (row == YEW_DIFF_FILLER_ROW) {
            bytebuf_push_u8(&built.bytes, '~');
        } else {
            const LineSlice *line = &lines[(size_t)row];

            bytebuf_append(&built.bytes, source + line->off, line->len);
        }
        DiffI32Vec_push(&built.source_rows, row);
    }
    yew_diff_scratch_drop(scratch);
    *scratch = built;
    (void)memset(&built, 0, sizeof(built));
    ok = true;
done:
    yew_xfree(lines);
    yew_diff_scratch_drop(&built);
    return ok;
}

i32 yew_diff_scratch_source_row(const DiffScratch *scratch,
                                size_t display_row)
{
    if (scratch == NULL || display_row >= scratch->source_rows.len)
        return YEW_DIFF_FILLER_ROW;
    return scratch->source_rows.data[display_row];
}

void yew_diff_scratch_pair_init(DiffScratchPair *pair)
{
    if (pair == NULL)
        return;
    yew_diff_scratch_init(&pair->left);
    yew_diff_scratch_init(&pair->right);
}

void yew_diff_scratch_pair_drop(DiffScratchPair *pair)
{
    if (pair == NULL)
        return;
    yew_diff_scratch_drop(&pair->left);
    yew_diff_scratch_drop(&pair->right);
}

bool yew_diff_scratch_pair_build(DiffScratchPair *pair,
                                 const u8 *base, size_t base_len,
                                 const u8 *buf, size_t buf_len,
                                 const DiffRowMap *map)
{
    DiffScratchPair built;

    if (pair == NULL || map == NULL || map->left.len != map->right.len)
        return false;
    yew_diff_scratch_pair_init(&built);
    if (!yew_diff_scratch_build(&built.left, base, base_len, &map->left) ||
        !yew_diff_scratch_build(&built.right, buf, buf_len, &map->right)) {
        yew_diff_scratch_pair_drop(&built);
        return false;
    }
    yew_diff_scratch_pair_drop(pair);
    *pair = built;
    return true;
}

void yew_diff_intraline_init(DiffIntraline *diff)
{
    if (diff != NULL)
        (void)memset(diff, 0, sizeof(*diff));
}

void yew_diff_intraline_drop(DiffIntraline *diff)
{
    if (diff == NULL)
        return;
    DiffIntraSpanVec_free(&diff->left);
    DiffIntraSpanVec_free(&diff->right);
}

static bool cluster_slices(const u8 *bytes, size_t len, ClusterSliceVec *out)
{
    size_t pos = 0U;
    YewCluster cluster;

    if (bytes == NULL && len != 0U)
        return false;
    while (yew_cluster_next(bytes, len, &pos, &cluster)) {
        if (cluster.off > UINT32_MAX || cluster.len > UINT32_MAX)
            return false;
        ClusterSliceVec_push(out,
                            (ClusterSlice){(u32)cluster.off, (u32)cluster.len});
    }
    return pos == len;
}

static bool cluster_equal(const u8 *left, const ClusterSlice *a,
                          const u8 *right, const ClusterSlice *b)
{
    return a->len == b->len &&
           (a->len == 0U || memcmp(left + a->off, right + b->off,
                                   a->len) == 0);
}

static void spans_from_marks(DiffIntraSpanVec *out,
                             const ClusterSliceVec *clusters,
                             const bool *changed)
{
    size_t i = 0U;

    while (i < clusters->len) {
        size_t first;
        size_t end;

        if (!changed[i]) {
            i++;
            continue;
        }
        first = i;
        while (i < clusters->len && changed[i])
            i++;
        end = (size_t)clusters->data[i - 1U].off +
              clusters->data[i - 1U].len;
        DiffIntraSpanVec_push(out,
                             (DiffIntraSpan){clusters->data[first].off,
                                             (u32)(end -
                                             clusters->data[first].off)});
    }
}

DiffIntraResult yew_diff_intraline_build(DiffIntraline *diff,
                                         const u8 *left, size_t left_len,
                                         const u8 *right, size_t right_len)
{
    ClusterSliceVec lc = {0};
    ClusterSliceVec rc = {0};
    DiffIntraline built = {0};
    u16 *lcs = NULL;
    bool *lchanged = NULL;
    bool *rchanged = NULL;
    size_t cols;
    size_t cells;
    size_t i;
    size_t j;
    DiffIntraResult result = YEW_DIFF_INTRA_ERROR;

    if (diff == NULL || (left == NULL && left_len != 0U) ||
        (right == NULL && right_len != 0U))
        return YEW_DIFF_INTRA_ERROR;
    if (left_len > YEW_DIFF_INTRALINE_MAX ||
        right_len > YEW_DIFF_INTRALINE_MAX) {
        yew_diff_intraline_drop(diff);
        yew_diff_intraline_init(diff);
        return YEW_DIFF_INTRA_WHOLE_LINE;
    }
    if (!cluster_slices(left, left_len, &lc) ||
        !cluster_slices(right, right_len, &rc))
        goto done;
    if (rc.len == SIZE_MAX || lc.len == SIZE_MAX)
        goto done;
    cols = rc.len + 1U;
    if (lc.len + 1U > SIZE_MAX / cols)
        goto done;
    cells = (lc.len + 1U) * cols;
    lcs = yew_xcalloc(cells, sizeof(*lcs));
    lchanged = yew_xcalloc(lc.len == 0U ? 1U : lc.len, sizeof(*lchanged));
    rchanged = yew_xcalloc(rc.len == 0U ? 1U : rc.len, sizeof(*rchanged));
    for (i = lc.len; i-- > 0U;) {
        for (j = rc.len; j-- > 0U;) {
            size_t at = i * cols + j;

            if (cluster_equal(left, &lc.data[i], right, &rc.data[j]))
                lcs[at] = (u16)(1U + lcs[(i + 1U) * cols + j + 1U]);
            else
                lcs[at] = lcs[(i + 1U) * cols + j] >= lcs[i * cols + j + 1U]
                          ? lcs[(i + 1U) * cols + j]
                          : lcs[i * cols + j + 1U];
        }
    }
    i = 0U;
    j = 0U;
    while (i < lc.len || j < rc.len) {
        if (i < lc.len && j < rc.len &&
            cluster_equal(left, &lc.data[i], right, &rc.data[j])) {
            i++;
            j++;
        } else if (i < lc.len &&
                   (j == rc.len || lcs[(i + 1U) * cols + j] >=
                                   lcs[i * cols + j + 1U])) {
            lchanged[i++] = true;
        } else {
            rchanged[j++] = true;
        }
    }
    spans_from_marks(&built.left, &lc, lchanged);
    spans_from_marks(&built.right, &rc, rchanged);
    yew_diff_intraline_drop(diff);
    *diff = built;
    (void)memset(&built, 0, sizeof(built));
    result = diff->left.len == 0U && diff->right.len == 0U ?
             YEW_DIFF_INTRA_EQUAL : YEW_DIFF_INTRA_SPANS;
done:
    yew_xfree(lcs);
    yew_xfree(lchanged);
    yew_xfree(rchanged);
    ClusterSliceVec_free(&lc);
    ClusterSliceVec_free(&rc);
    yew_diff_intraline_drop(&built);
    return result;
}

void yew_diff_scroll_registry_init(DiffScrollRegistry *registry)
{
    if (registry != NULL)
        (void)memset(registry, 0, sizeof(*registry));
}

void yew_diff_scroll_registry_drop(DiffScrollRegistry *registry)
{
    if (registry != NULL)
        DiffScrollMemberVec_free(&registry->members);
}

bool yew_diff_scroll_register(DiffScrollRegistry *registry, u32 link_id,
                              u32 member_id, DiffScrollApplyFn apply,
                              void *ctx)
{
    size_t i;

    if (registry == NULL || link_id == 0U || member_id == 0U || apply == NULL)
        return false;
    for (i = 0U; i < registry->members.len; i++) {
        DiffScrollMember *member = &registry->members.data[i];

        if (member->member_id == member_id) {
            *member = (DiffScrollMember){link_id, member_id, apply, ctx};
            return true;
        }
    }
    DiffScrollMemberVec_push(&registry->members,
                             (DiffScrollMember){link_id, member_id, apply,
                                                ctx});
    return true;
}

void yew_diff_scroll_unregister(DiffScrollRegistry *registry, u32 member_id)
{
    size_t i;

    if (registry == NULL || member_id == 0U)
        return;
    for (i = 0U; i < registry->members.len; i++) {
        if (registry->members.data[i].member_id != member_id)
            continue;
        (void)memmove(&registry->members.data[i],
                      &registry->members.data[i + 1U],
                      (registry->members.len - i - 1U) *
                      sizeof(*registry->members.data));
        registry->members.len--;
        return;
    }
}

/*
 * A viewport callback can run the ordinary viewport-change hook again.
 * Without this module-static guard, A updates B, B updates A, and the two
 * handlers recurse forever.  The core is single-threaded, so one guard also
 * correctly covers distinct scroll-link registries during a propagation.
 */
static bool syncing;
_Static_assert(sizeof(syncing) == sizeof(bool),
               "diff scroll recursion guard must remain a boolean");

bool yew_diff_scroll_sync(DiffScrollRegistry *registry, u32 link_id,
                          u32 source_member_id, u32 aligned_top)
{
    size_t i;

    if (registry == NULL || link_id == 0U || source_member_id == 0U || syncing)
        return false;
    syncing = true;
    for (i = 0U; i < registry->members.len; i++) {
        DiffScrollMember *member = &registry->members.data[i];

        if (member->link_id == link_id &&
            member->member_id != source_member_id)
            member->apply(member->ctx, aligned_top);
    }
    syncing = false;
    return true;
}
