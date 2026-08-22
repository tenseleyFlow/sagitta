#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "mod/git/blame.h"

#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SHA_ZERO "0000000000000000000000000000000000000000"

static const u8 blame_a[] =
    SHA_A " 1 1 2\n"
    "author Jane\n"
    "author-mail <jane@example.test>\n"
    "author-time 1000\n"
    "author-tz +0000\n"
    "summary fix the CRLF case\n"
    "filename file.c\n";

static BlameCache *cache_ready_request(BlameRequest *request)
{
    BlameCache *cache = yew_blame_cache_new();

    YEW_ASSERT_NOT_NULL(cache);
    yew_blame_cache_observe(cache, 7U, 10U, LINENO(2U), LINENO(9U),
                            200U, 1000);
    YEW_ASSERT(!yew_blame_cache_take_request(cache, 1199, request));
    YEW_ASSERT(yew_blame_cache_take_request(cache, 1200, request));
    return cache;
}

void test_blamecache_quantizes_visible_ranges_to_64_lines(void)
{
    u32 lo;
    u32 hi;

    yew_blame_quantize(LINENO(63U), LINENO(64U), 1000U, &lo, &hi);
    YEW_ASSERT_EQ_U64(lo, 0U);
    YEW_ASSERT_EQ_U64(hi, 128U);
}

void test_blamecache_quantization_clamps_at_file_end(void)
{
    u32 lo;
    u32 hi;

    yew_blame_quantize(LINENO(70U), LINENO(999U), 91U, &lo, &hi);
    YEW_ASSERT_EQ_U64(lo, 64U);
    YEW_ASSERT_EQ_U64(hi, 91U);
}

void test_blamecache_quantization_handles_empty_buffer(void)
{
    u32 lo = 99U;
    u32 hi = 99U;

    yew_blame_quantize(LINENO(0U), LINENO(0U), 0U, &lo, &hi);
    YEW_ASSERT_EQ_U64(lo, 0U);
    YEW_ASSERT_EQ_U64(hi, 0U);
}

void test_blamecache_waits_for_200ms_of_viewport_quiet(void)
{
    BlameCache *cache = yew_blame_cache_new();
    BlameRequest request;

    yew_blame_cache_observe(cache, 1U, 1U, LINENO(0U), LINENO(10U),
                            500U, 10);
    yew_blame_cache_observe(cache, 1U, 1U, LINENO(64U), LINENO(70U),
                            500U, 100);
    YEW_ASSERT(!yew_blame_cache_take_request(cache, 299, &request));
    YEW_ASSERT(yew_blame_cache_take_request(cache, 300, &request));
    YEW_ASSERT_EQ_U64(request.range_lo, 64U);
    yew_blame_cache_free(cache);
}

void test_blamecache_caps_requests_at_two_inflight(void)
{
    BlameCache *cache = yew_blame_cache_new();
    BlameRequest a;
    BlameRequest b;
    BlameRequest c;

    yew_blame_cache_observe(cache, 1U, 1U, LINENO(0U), LINENO(1U),
                            500U, 0);
    YEW_ASSERT(yew_blame_cache_take_request(cache, 200, &a));
    yew_blame_cache_observe(cache, 2U, 1U, LINENO(64U), LINENO(65U),
                            500U, 201);
    YEW_ASSERT(yew_blame_cache_take_request(cache, 401, &b));
    yew_blame_cache_observe(cache, 3U, 1U, LINENO(128U), LINENO(129U),
                            500U, 402);
    YEW_ASSERT(!yew_blame_cache_take_request(cache, 602, &c));
    YEW_ASSERT_EQ_U64(yew_blame_cache_inflight(cache), 2U);
    yew_blame_cache_fail(cache, &a);
    YEW_ASSERT(yew_blame_cache_take_request(cache, 602, &c));
    yew_blame_cache_free(cache);
}

void test_blamecache_failed_request_can_retry_without_scrolling(void)
{
    BlameCache *cache = yew_blame_cache_new();
    BlameRequest first;
    BlameRequest retry;

    yew_blame_cache_observe(cache, 1U, 1U, LINENO(0U), LINENO(1U),
                            100U, 0);
    YEW_ASSERT(yew_blame_cache_take_request(cache, 200, &first));
    yew_blame_cache_fail(cache, &first);
    YEW_ASSERT(yew_blame_cache_take_request(cache, 200, &retry));
    YEW_ASSERT(first.token != retry.token);
    yew_blame_cache_free(cache);
}

void test_blamecache_ingests_incremental_lines(void)
{
    BlameRequest request;
    BlameCache *cache = cache_ready_request(&request);
    const BlameLine *line;

    YEW_ASSERT(yew_blame_cache_finish(cache, &request, blame_a,
                                      sizeof(blame_a) - 1U));
    line = yew_blame_cache_at(cache, 7U, 10U, LINENO(0U));
    YEW_ASSERT_NOT_NULL(line);
    YEW_ASSERT_EQ_STR(line->author, "Jane");
    YEW_ASSERT_EQ_STR(line->summary, "fix the CRLF case");
    YEW_ASSERT(!line->stale);
    yew_blame_cache_free(cache);
}

void test_blamecache_current_range_does_not_spawn_again(void)
{
    BlameRequest request;
    BlameRequest duplicate;
    BlameCache *cache = cache_ready_request(&request);

    YEW_ASSERT(yew_blame_cache_finish(cache, &request, blame_a,
                                      sizeof(blame_a) - 1U));
    yew_blame_cache_observe(cache, 7U, 10U, LINENO(2U), LINENO(9U),
                            200U, 2000);
    YEW_ASSERT(!yew_blame_cache_take_request(cache, 5000, &duplicate));
    yew_blame_cache_free(cache);
}

void test_blamecache_retains_stale_text_during_recomputation(void)
{
    BlameRequest old_request;
    BlameRequest new_request;
    BlameCache *cache = cache_ready_request(&old_request);
    const BlameLine *line;

    YEW_ASSERT(yew_blame_cache_finish(cache, &old_request, blame_a,
                                      sizeof(blame_a) - 1U));
    yew_blame_cache_observe(cache, 7U, 11U, LINENO(0U), LINENO(9U),
                            200U, 1300);
    line = yew_blame_cache_at(cache, 7U, 11U, LINENO(0U));
    YEW_ASSERT_NOT_NULL(line);
    YEW_ASSERT(line->stale);
    YEW_ASSERT_EQ_STR(line->summary, "fix the CRLF case");
    YEW_ASSERT(yew_blame_cache_take_request(cache, 1500, &new_request));
    line = yew_blame_cache_at(cache, 7U, 11U, LINENO(0U));
    YEW_ASSERT_NOT_NULL(line);
    YEW_ASSERT(line->stale);
    yew_blame_cache_free(cache);
}

void test_blamecache_reuses_metadata_across_range_results(void)
{
    static const u8 second_group[] =
        SHA_A " 9 65 1\n"
        "author Jane\n"
        "author-time 1000\n"
        "summary fix the CRLF case\n"
        "filename file.c\n";
    BlameCache *cache = yew_blame_cache_new();
    BlameRequest first;
    BlameRequest second;

    yew_blame_cache_observe(cache, 1U, 1U, LINENO(0U), LINENO(1U),
                            200U, 0);
    YEW_ASSERT(yew_blame_cache_take_request(cache, 200, &first));
    YEW_ASSERT(yew_blame_cache_finish(cache, &first, blame_a,
                                      sizeof(blame_a) - 1U));
    yew_blame_cache_observe(cache, 1U, 1U, LINENO(64U), LINENO(65U),
                            200U, 201);
    YEW_ASSERT(yew_blame_cache_take_request(cache, 401, &second));
    YEW_ASSERT(yew_blame_cache_finish(cache, &second, second_group,
                                      sizeof(second_group) - 1U));
    YEW_ASSERT_EQ_U64(yew_blame_cache_metadata_count(cache), 1U);
    YEW_ASSERT_EQ_STR(yew_blame_cache_at(cache, 1U, 1U, LINENO(64U))->author,
                      "Jane");
    yew_blame_cache_free(cache);
}

void test_blamecache_reuses_first_group_metadata_for_continuations(void)
{
    static const u8 groups[] =
        SHA_A " 1 1 1\n"
        "author Jane\n"
        "author-time 1000\n"
        "summary shared metadata\n"
        "filename file.c\n"
        SHA_A " 2 2\n"
        "\tsecond line\n";
    BlameRequest request;
    BlameCache *cache = cache_ready_request(&request);
    const BlameLine *second;

    YEW_ASSERT(yew_blame_cache_finish(cache, &request, groups,
                                      sizeof(groups) - 1U));
    second = yew_blame_cache_at(cache, 7U, 10U, LINENO(1U));
    YEW_ASSERT_NOT_NULL(second);
    YEW_ASSERT_EQ_STR(second->author, "Jane");
    YEW_ASSERT_EQ_STR(second->summary, "shared metadata");
    YEW_ASSERT_EQ_U64(yew_blame_cache_metadata_count(cache), 1U);
    yew_blame_cache_free(cache);
}

void test_blamecache_zero_sha_formats_uncommitted(void)
{
    static const u8 zero[] =
        SHA_ZERO " 1 1 1\n"
        "author Not Committed Yet\n"
        "author-time 2000\n"
        "summary local change\n"
        "filename file.c\n";
    BlameRequest request;
    BlameCache *cache = cache_ready_request(&request);
    const BlameLine *line;
    char text[128];

    YEW_ASSERT(yew_blame_cache_finish(cache, &request, zero,
                                      sizeof(zero) - 1U));
    line = yew_blame_cache_at(cache, 7U, 10U, LINENO(0U));
    YEW_ASSERT(line->uncommitted);
    (void)yew_blame_format(text, sizeof(text), line, 9000);
    YEW_ASSERT_EQ_STR(text, "  ▏ (uncommitted)");
    yew_blame_cache_free(cache);
}

void test_blamecache_relative_time_uses_explicit_now(void)
{
    char text[64];

    (void)yew_blame_relative_time(text, sizeof(text), 1000,
                                  1000 + 21 * 24 * 60 * 60);
    YEW_ASSERT_EQ_STR(text, "3 weeks ago");
}

void test_blamecache_relative_time_uses_singular_units(void)
{
    char text[64];

    (void)yew_blame_relative_time(text, sizeof(text), 1000, 1060);
    YEW_ASSERT_EQ_STR(text, "1 minute ago");
}

void test_blamecache_relative_time_clamps_future_authors_to_now(void)
{
    char text[64];

    (void)yew_blame_relative_time(text, sizeof(text), 2000, 1000);
    YEW_ASSERT_EQ_STR(text, "now");
}

void test_blamecache_formats_committed_annotation(void)
{
    BlameLine line = {SHA_B, "Jane", "fix the CRLF case", 1000,
                      false, false};
    char text[128];

    (void)yew_blame_format(text, sizeof(text), &line,
                           1000 + 21 * 24 * 60 * 60);
    YEW_ASSERT_EQ_STR(text,
                      "  ▏ Jane, 3 weeks ago · fix the CRLF case");
}

void test_blamecache_layout_omits_when_23_cells_remain(void)
{
    u16 col = 99U;
    u16 available = 99U;

    YEW_ASSERT(!yew_blame_layout(80U, 57U, false, true, &col, &available));
    YEW_ASSERT_EQ_U64(available, 23U);
}

void test_blamecache_layout_accepts_when_24_cells_remain(void)
{
    u16 col;
    u16 available;

    YEW_ASSERT(yew_blame_layout(80U, 56U, false, true, &col, &available));
    YEW_ASSERT_EQ_U64(col, 56U);
    YEW_ASSERT_EQ_U64(available, 24U);
}

void test_blamecache_layout_draws_only_final_wrapped_row(void)
{
    YEW_ASSERT(!yew_blame_layout(80U, 10U, true, false, NULL, NULL));
    YEW_ASSERT(yew_blame_layout(80U, 10U, true, true, NULL, NULL));
}
