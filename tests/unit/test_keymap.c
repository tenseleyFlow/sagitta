#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "edit/cmd.h"
#include "edit/keymap.h"
#include "unicode/utf8.h"
#include "util/buf.h"

static void assert_build_fails(const BindRow *rows, u32 n)
{
    Keymap map = {0};

    SAG_ASSERT(!sag_keymap_build(&map, "invalid", rows, n));
    SAG_ASSERT_EQ_U64(map.nodes.len, 0U);
    SAG_ASSERT_EQ_U64(map.edges.len, 0U);
    SAG_ASSERT_EQ_U64(map.binds.len, 0U);
    sag_keymap_free(&map);
}

void test_keymap_build_validations(void)
{
    static const BindRow bad_seq[] = {
        {"<nope>", "ed.nop", 0, NULL},
    };
    static const BindRow unknown_cmd[] = {
        {"a", "ed.ui.definitely_missing", 0, NULL},
    };
    static const BindRow duplicate[] = {
        {"a", "ed.nop", 0, NULL}, {"a", "ed.nop", 0, NULL},
    };
    static const BindRow arity[] = {
        {"a", "ed.mode.enter", 0, NULL},
    };
    static const BindRow esc_prefix[] = {
        {"<esc> a", "ed.nop", 0, NULL},
    };
    static const BindRow too_long[] = {
        {"a b c d e f g h i", "ed.nop", 0, NULL},
    };

    sag_cmd_init();
    assert_build_fails(bad_seq, SAG_ARRAY_LEN(bad_seq));
    assert_build_fails(unknown_cmd, SAG_ARRAY_LEN(unknown_cmd));
    assert_build_fails(duplicate, SAG_ARRAY_LEN(duplicate));
    assert_build_fails(arity, SAG_ARRAY_LEN(arity));
    assert_build_fails(esc_prefix, SAG_ARRAY_LEN(esc_prefix));
    assert_build_fails(too_long, SAG_ARRAY_LEN(too_long));
}

enum { ORACLE_ROWS = 300 };

static i64 elapsed_ns(const struct timespec *start,
                      const struct timespec *end)
{
    return (i64)(end->tv_sec - start->tv_sec) * INT64_C(1000000000) +
           (i64)end->tv_nsec - (i64)start->tv_nsec;
}

void test_keymap_binary_lookup_matches_oracle_and_budget(void)
{
    BindRow rows[ORACLE_ROWS];
    char seq_text[ORACLE_ROWS][5];
    KeyId ids[ORACLE_ROWS];
    Keymap map = {0};
    struct timespec start;
    struct timespec end;
    volatile u64 checksum = 0U;
    u32 i;

    sag_cmd_init();
    for (i = 0U; i < ORACLE_ROWS; i++) {
        u8 encoded[SAG_UTF8_MAX];
        size_t n = sag_utf8_encode(0x1000U + i, encoded);

        SAG_ASSERT(n > 0U && n < sizeof(seq_text[i]));
        memcpy(seq_text[i], encoded, n);
        seq_text[i][n] = '\0';
        rows[i] = (BindRow){seq_text[i], "ed.tab.goto", (i64)i + 1, NULL};
        SAG_ASSERT_EQ_U64(sag_key_parse_seq(seq_text[i], &ids[i], 1U), 1U);
    }
    SAG_ASSERT(sag_keymap_build(&map, "oracle", rows, ORACLE_ROWS));
    SAG_ASSERT_EQ_U64(sag_keymap_binding_count(&map), ORACLE_ROWS);
    for (i = 0U; i < ORACLE_ROWS; i++) {
        const Binding *binding = NULL;

        SAG_ASSERT_EQ_I64(sag_keymap_lookup(&map, &ids[i], 1U, NULL,
                                            &binding), SAG_MATCH_FULL);
        SAG_ASSERT_NOT_NULL(binding);
        SAG_ASSERT_EQ_I64(binding->iarg, (i64)i + 1);
    }
    SAG_ASSERT_EQ_I64(clock_gettime(CLOCK_MONOTONIC, &start), 0);
    for (i = 0U; i < 1000000U; i++) {
        const Binding *binding = NULL;
        u32 row = i % ORACLE_ROWS;

        if (sag_keymap_lookup(&map, &ids[row], 1U, NULL, &binding) !=
            SAG_MATCH_FULL)
            sag_test_fail(__FILE__, __LINE__, "lookup budget miss");
        checksum += (u64)binding->iarg;
    }
    SAG_ASSERT_EQ_I64(clock_gettime(CLOCK_MONOTONIC, &end), 0);
    SAG_ASSERT(checksum != 0U);
    if (getenv("SAG_TEST_INSTRUMENTED") == NULL)
        SAG_ASSERT(elapsed_ns(&start, &end) < INT64_C(150000000));
    sag_keymap_free(&map);
}

typedef struct {
    Bytebuf *out;
} Listing;

static bool append_listing(const KeyId *seq, u32 n,
                           const Binding *binding, void *opaque)
{
    Listing *listing = opaque;
    const CmdDesc *desc = sag_cmd_desc(binding->cmd);

    sag_key_format_seq(seq, n, listing->out);
    bytebuf_push_u8(listing->out, (u8)'\t');
    bytebuf_append(listing->out, desc->name, strlen(desc->name));
    bytebuf_push_u8(listing->out, (u8)'\n');
    return true;
}

void test_keymap_layer_ownership_and_listing_determinism(void)
{
    static const BindRow mode_rows[] = {
        {"g g", "ed.nop", 0, NULL},
    };
    static const BindRow plugin_rows[] = {
        {"g", "ed.nop", 0, NULL}, {"g x", "ed.nop", 0, NULL},
    };
    Keymap mode = {0};
    Keymap user = {0};
    Keymap plugin = {0};
    KeyStack stack = {0};
    KeyId g;
    const Binding *binding = NULL;
    i32 layer = -1;
    u32 node = 0U;
    Bytebuf first;
    Bytebuf second;
    Listing a = {&first};
    Listing b = {&second};

    sag_cmd_init();
    SAG_ASSERT(sag_keymap_build(&mode, "mode:L", mode_rows,
                                SAG_ARRAY_LEN(mode_rows)));
    SAG_ASSERT(sag_keymap_build(&user, "user", NULL, 0U));
    SAG_ASSERT(sag_keymap_build(&plugin, "plug:vimish", plugin_rows,
                                SAG_ARRAY_LEN(plugin_rows)));
    stack.l[0] = &mode;
    stack.l[1] = &user;
    stack.l[2] = &plugin;
    stack.n = 3U;
    SAG_ASSERT_EQ_U64(sag_key_parse_seq("g", &g, 1U), 1U);
    SAG_ASSERT_EQ_I64(sag_keystack_lookup(&stack, &g, 1U, &layer, &node,
                                          &binding),
                      SAG_MATCH_FULL_PREFIX);
    SAG_ASSERT_EQ_I64(layer, 2);
    SAG_ASSERT_EQ_STR(stack.l[layer]->name, "plug:vimish");
    SAG_ASSERT_NOT_NULL(binding);

    bytebuf_init(&first);
    bytebuf_init(&second);
    SAG_ASSERT(sag_keymap_visit(&plugin, append_listing, &a));
    SAG_ASSERT(sag_keymap_visit(&plugin, append_listing, &b));
    SAG_ASSERT_EQ_U64(first.len, second.len);
    SAG_ASSERT_EQ_MEM(first.data, second.data, first.len);
    bytebuf_free(&second);
    bytebuf_free(&first);
    sag_keymap_free(&plugin);
    sag_keymap_free(&user);
    sag_keymap_free(&mode);
}
