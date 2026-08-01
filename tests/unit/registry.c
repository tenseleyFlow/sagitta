#include "harness.h"
#include "tests.h"

#define T(n) { #n, test_##n }

const SagTest sag_tests[] = {
    T(arena_align),
    T(arena_strdup),
    T(vec_growth),
    T(vec_free_resets),
    T(strmap_order),
    T(strmap_replace_keeps_order),
    T(intern_roundtrip),
    T(intern_id_stability),
    T(sort_stable_ties),
    T(sort_empty),
    T(bytebuf),
    T(bytebuf_binary),
    T(log_capture),
    T(log_levels),
    T(mod_require_message),
    T(args_parse_version),
    T(args_parse_help),
    T(args_parse_unknown),
    T(args_parse_file),
    T(args_parse_batch),
    T(args_parse_batch_missing),
    T(args_parse_end_options),
    T(harness_assert_once),
    T(harness_filter_selects),
    T(harness_list_order),
    T(harness_failure_isolated),
    T(harness_intentional_failure),
    T(utf8_valid_decode),
    T(utf8_reject_classes),
    T(utf8_truncated_decode),
    T(utf8_incremental_valid),
    T(utf8_incremental_recovery),
    T(utf8_incremental_finish),
    T(utf8_decode_prev),
    T(utf8_encode_edges),
    T(utf8_escape_helpers),
    T(utf8_validate),
    T(utf8_boundaries),
    T(utf8_exhaustive_scalars),
    T(utf8_exhaustive_surrogates),
    T(utf8_exhaustive_short_strings),
};

const size_t sag_tests_len = SAG_ARRAY_LEN(sag_tests);

#undef T
