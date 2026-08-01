#ifndef SAG_TEST_TESTS_H
#define SAG_TEST_TESTS_H

void test_arena_align(void);
void test_arena_strdup(void);
void test_vec_growth(void);
void test_vec_free_resets(void);
void test_strmap_order(void);
void test_strmap_replace_keeps_order(void);
void test_intern_roundtrip(void);
void test_intern_id_stability(void);
void test_sort_stable_ties(void);
void test_sort_empty(void);
void test_bytebuf(void);
void test_bytebuf_binary(void);
void test_log_capture(void);
void test_log_levels(void);
void test_mod_require_message(void);
void test_args_parse_version(void);
void test_args_parse_help(void);
void test_args_parse_unknown(void);
void test_args_parse_file(void);
void test_args_parse_batch(void);
void test_args_parse_batch_missing(void);
void test_args_parse_end_options(void);
void test_harness_assert_once(void);
void test_harness_filter_selects(void);
void test_harness_list_order(void);
void test_harness_failure_isolated(void);
void test_harness_intentional_failure(void);

#endif
