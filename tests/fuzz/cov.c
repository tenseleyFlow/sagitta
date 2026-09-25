#include "cov.h"

#include <string.h>

u8 yew_cov_map[YEW_COV_SIZE];

static u8 seen[YEW_COV_SIZE];
static u32 touched[YEW_COV_SIZE];
static u32 touched_len;
static u32 next_guard = 1U;
static u32 total_edges;

void __sanitizer_cov_trace_pc_guard_init(u32 *start, u32 *stop)
{
    if (start == NULL || stop == NULL || start == stop || *start != 0U)
        return;
    while (start < stop) {
        if (next_guard < YEW_COV_SIZE)
            *start = next_guard++;
        else
            *start = 0U;
        start++;
    }
}

void __sanitizer_cov_trace_pc_guard(u32 *guard)
{
    u32 id;
    u8 value;

    if (guard == NULL)
        return;
    id = *guard;
    if (id == 0U || id >= YEW_COV_SIZE)
        return;
    value = yew_cov_map[id];
    if (value == 0U && touched_len < YEW_COV_SIZE)
        touched[touched_len++] = id;
    if (value != UINT8_MAX)
        yew_cov_map[id] = (u8)(value + 1U);
}

void yew_cov_reset(void)
{
    u32 i;

    for (i = 0U; i < touched_len; i++)
        yew_cov_map[touched[i]] = 0U;
    touched_len = 0U;
}

u32 yew_cov_new_edges(void)
{
    u32 count = 0U;
    u32 i;

    for (i = 0U; i < touched_len; i++) {
        u32 id = touched[i];

        if (yew_cov_map[id] != 0U && seen[id] == 0U)
            count++;
    }
    return count;
}

void yew_cov_merge(void)
{
    u32 i;

    for (i = 0U; i < touched_len; i++) {
        u32 id = touched[i];

        if (yew_cov_map[id] != 0U && seen[id] == 0U) {
            seen[id] = 1U;
            total_edges++;
        }
    }
}

u32 yew_cov_novel_ids(u32 *out, u32 cap)
{
    u32 count = 0U;
    u32 i;

    if (out == NULL)
        return 0U;
    for (i = 0U; i < touched_len && count < cap; i++) {
        u32 id = touched[i];

        if (yew_cov_map[id] != 0U && seen[id] == 0U)
            out[count++] = id;
    }
    return count;
}

bool yew_cov_hit_all(const u32 *ids, u32 count)
{
    u32 i;

    if (ids == NULL && count != 0U)
        return false;
    for (i = 0U; i < count; i++) {
        if (ids[i] == 0U || ids[i] >= YEW_COV_SIZE ||
            yew_cov_map[ids[i]] == 0U)
            return false;
    }
    return true;
}

u32 yew_cov_merge_ids(const u32 *ids, u32 count)
{
    u32 merged = 0U;
    u32 i;

    if (ids == NULL)
        return 0U;
    for (i = 0U; i < count; i++) {
        u32 id = ids[i];

        if (id != 0U && id < YEW_COV_SIZE && seen[id] == 0U) {
            seen[id] = 1U;
            total_edges++;
            merged++;
        }
    }
    return merged;
}

u64 yew_cov_hash(void)
{
    u64 hash = UINT64_C(1469598103934665603);
    u32 i;

    for (i = 0U; i < YEW_COV_SIZE; i++) {
        hash ^= seen[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

u32 yew_cov_total_edges(void)
{
    return total_edges;
}

void yew_cov_report(FILE *out)
{
    if (out != NULL)
        (void)fprintf(out, "coverage edges=%u hash=%016llx",
                      total_edges, (unsigned long long)yew_cov_hash());
}

void yew_cov_test_clear_all(void)
{
    (void)memset(yew_cov_map, 0, sizeof(yew_cov_map));
    (void)memset(seen, 0, sizeof(seen));
    (void)memset(touched, 0, sizeof(touched));
    touched_len = 0U;
    next_guard = 1U;
    total_edges = 0U;
}
