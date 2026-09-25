#include "fuzz/cov.h"

#include <stdio.h>
#include <string.h>

static int check(bool condition, const char *message)
{
    if (condition)
        return 0;
    (void)fprintf(stderr, "cov-selftest: %s\n", message);
    return 1;
}

int main(void)
{
    u32 first[3] = {0U, 0U, 0U};
    u32 second[2] = {0U, 0U};
    u32 novel[3] = {0U, 0U, 0U};
    u32 novel_len;
    u32 zero;
    u64 hash;
    unsigned i;

    yew_cov_test_clear_all();
    __sanitizer_cov_trace_pc_guard_init(first, first + 3U);
    __sanitizer_cov_trace_pc_guard_init(second, second + 2U);
    if (check(first[0] == 1U && first[1] == 2U && first[2] == 3U &&
              second[0] == 4U && second[1] == 5U,
              "guard ids are not assigned in deterministic call order"))
        return 1;
    for (i = 0U; i < 300U; i++)
        __sanitizer_cov_trace_pc_guard(&first[0]);
    if (check(yew_cov_map[1] == UINT8_MAX,
              "8-bit hit counter did not saturate"))
        return 1;
    if (check(yew_cov_new_edges() == 1U,
              "first edge was not classified as new"))
        return 1;
    yew_cov_merge();
    hash = yew_cov_hash();
    if (check(yew_cov_total_edges() == 1U && yew_cov_new_edges() == 0U,
              "merge did not preserve the global seen set"))
        return 1;

    yew_cov_reset();
    __sanitizer_cov_trace_pc_guard(&first[0]);
    if (check(yew_cov_new_edges() == 0U,
              "replayed edge was admitted a second time"))
        return 1;
    __sanitizer_cov_trace_pc_guard(&second[0]);
    if (check(yew_cov_new_edges() == 1U,
              "newly reached edge was not admitted"))
        return 1;
    yew_cov_merge();
    if (check(yew_cov_total_edges() == 2U && yew_cov_hash() != hash,
              "coverage hash did not reflect the newly merged edge"))
        return 1;

    /* Stable-edge calibration: pin the novel ids of one execution, then ask
     * whether a later execution reproduced them.  An id reached only by the
     * first execution (a one-shot edge) must not count as reproduced. */
    yew_cov_reset();
    __sanitizer_cov_trace_pc_guard(&first[1]);
    __sanitizer_cov_trace_pc_guard(&first[2]);
    __sanitizer_cov_trace_pc_guard(&second[0]);
    novel_len = yew_cov_novel_ids(novel, 3U);
    if (check(novel_len == 2U && novel[0] == 2U && novel[1] == 3U,
              "novel ids were not reported in first-reached order"))
        return 1;
    if (check(yew_cov_novel_ids(novel, 1U) == 1U && novel[0] == 2U,
              "novel id capture overran its capacity"))
        return 1;
    novel[0] = 2U;
    novel[1] = 3U;
    yew_cov_reset();
    __sanitizer_cov_trace_pc_guard(&first[2]);
    if (check(!yew_cov_hit_all(novel, 2U) && yew_cov_hit_all(&novel[1], 1U),
              "one-shot edge was treated as reproduced"))
        return 1;
    if (check(yew_cov_hit_all(NULL, 0U) && !yew_cov_hit_all(NULL, 1U),
              "empty edge set handling is wrong"))
        return 1;
    zero = 0U;
    if (check(!yew_cov_hit_all(&zero, 1U), "guard id 0 counted as reached"))
        return 1;
    if (check(yew_cov_merge_ids(novel, 2U) == 2U &&
              yew_cov_merge_ids(novel, 2U) == 0U &&
              yew_cov_total_edges() == 4U && yew_cov_new_edges() == 0U,
              "explicit id merge did not update the seen set exactly once"))
        return 1;

    yew_cov_test_clear_all();
    (void)memset(first, 0, sizeof(first));
    (void)memset(second, 0, sizeof(second));
    __sanitizer_cov_trace_pc_guard_init(first, first + 3U);
    __sanitizer_cov_trace_pc_guard_init(second, second + 2U);
    if (check(first[0] == 1U && second[1] == 5U,
              "guard assignment changed across identical runs"))
        return 1;

    (void)printf("cov-selftest: ok\n");
    return 0;
}
