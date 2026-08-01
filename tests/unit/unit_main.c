#include "mod/mods.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/sort.h"
#include "util/strmap.h"
#include "util/vec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdalign.h>
#include <string.h>

VEC_DECL(VecU32, u32);

typedef struct {
    alignas(16) unsigned char bytes[16];
} Aligned16;

typedef struct {
    int key;
    int original_pos;
} SortItem;

typedef struct {
    size_t calls;
    SagLogLevel level;
    char message[128];
} CapturedLog;

static int compare_sort_items(const void *lhs, const void *rhs, void *ctx)
{
    const SortItem *a = lhs;
    const SortItem *b = rhs;

    (void)ctx;
    return (a->key > b->key) - (a->key < b->key);
}

static void capture_log(void *user, SagLogLevel level, const char *message)
{
    CapturedLog *capture = user;

    capture->calls++;
    capture->level = level;
    (void)snprintf(capture->message, sizeof(capture->message), "%s", message);
}

static void test_arena_alignment_and_strings(void)
{
    Arena arena;
    void *first;
    Aligned16 *aligned;
    char *copy;

    arena_init(&arena);
    first = arena_alloc(&arena, 1U, 1U);
    aligned = arena_alloc(&arena, sizeof(*aligned), alignof(Aligned16));
    copy = arena_strdup(&arena, "arena-owned");

    assert(first != NULL);
    assert(aligned != NULL);
    assert((uintptr_t)aligned % 16U == 0U);
    assert(strcmp(copy, "arena-owned") == 0);
    arena_free_all(&arena);
}

static void test_vec_grows_and_preserves_values(void)
{
    VecU32 vec = {0};
    size_t i;

    for (i = 0U; i < 1024U; i++) {
        VecU32_push(&vec, (u32)(i * 3U));
    }

    assert(vec.len == 1024U);
    assert(vec.cap >= vec.len);
    assert(vec.data[0] == 0U);
    assert(vec.data[511] == 1533U);
    assert(vec.data[1023] == 3069U);
    VecU32_free(&vec);
    assert(vec.data == NULL);
    assert(vec.len == 0U);
    assert(vec.cap == 0U);
}

static void test_strmap_insertion_order_and_replacement(void)
{
    enum { ENTRY_COUNT = 10000 };
    Strmap map;
    char keys[ENTRY_COUNT][24];
    size_t values[ENTRY_COUNT];
    size_t replacement = 999999U;
    size_t i;
    StrmapIter iter;

    strmap_init(&map);
    for (i = 0U; i < ENTRY_COUNT; i++) {
        int n = snprintf(keys[i], sizeof(keys[i]), "key-%05zu", i);

        assert(n > 0);
        values[i] = i;
        strmap_put(&map, keys[i], (size_t)n, &values[i]);
    }

    assert(strmap_len(&map) == ENTRY_COUNT);
    assert(strmap_get(&map, keys[4321], strlen(keys[4321])) == &values[4321]);
    strmap_put(&map, keys[4321], strlen(keys[4321]), &replacement);
    assert(strmap_len(&map) == ENTRY_COUNT);
    assert(strmap_get(&map, keys[4321], strlen(keys[4321])) == &replacement);

    iter = strmap_iter(&map);
    for (i = 0U; i < ENTRY_COUNT; i++) {
        const char *key;
        size_t key_len;
        void *value;
        char expected[24];
        int expected_len = snprintf(expected, sizeof(expected), "key-%05zu", i);

        assert(strmap_iter_next(&iter, &key, &key_len, &value));
        assert(expected_len > 0);
        assert(key_len == (size_t)expected_len);
        assert(memcmp(key, expected, key_len) == 0);
        assert(value == (i == 4321U ? (void *)&replacement : (void *)&values[i]));
    }
    {
        const char *key;
        size_t key_len;
        void *value;

        assert(!strmap_iter_next(&iter, &key, &key_len, &value));
    }
    strmap_free(&map);
}

static void test_interner_ids_are_stable_and_round_trip(void)
{
    Arena arena;
    Interner interner;
    u32 alpha;
    u32 beta;
    u32 alpha_again;

    arena_init(&arena);
    interner_init(&interner, &arena);
    alpha = sag_intern(&interner, "alpha", 5U);
    beta = sag_intern_cstr(&interner, "beta");
    alpha_again = sag_intern_cstr(&interner, "alpha");

    assert(alpha == 1U);
    assert(beta == 2U);
    assert(alpha_again == alpha);
    assert(sag_intern_count(&interner) == 2U);
    assert(strcmp(sag_intern_str(&interner, alpha), "alpha") == 0);
    assert(strcmp(sag_intern_str(&interner, beta), "beta") == 0);
    interner_free(&interner);
    arena_free_all(&arena);
}

static void test_stable_sort_preserves_tie_order(void)
{
    SortItem items[] = {
        {2, 0}, {1, 1}, {2, 2}, {1, 3}, {3, 4}, {2, 5}
    };

    sag_sort_stable(items, SAG_ARRAY_LEN(items), sizeof(items[0]),
        compare_sort_items, NULL);

    assert(items[0].key == 1 && items[0].original_pos == 1);
    assert(items[1].key == 1 && items[1].original_pos == 3);
    assert(items[2].key == 2 && items[2].original_pos == 0);
    assert(items[3].key == 2 && items[3].original_pos == 2);
    assert(items[4].key == 2 && items[4].original_pos == 5);
    assert(items[5].key == 3 && items[5].original_pos == 4);
}

static void test_bytebuf_printf_and_binary_append(void)
{
    Bytebuf buf;
    static const unsigned char suffix[] = {0U, 0xffU};

    bytebuf_init(&buf);
    bytebuf_printf(&buf, "%s:%d", "value", 42);
    assert(buf.len == 8U);
    assert(memcmp(buf.data, "value:42", 8U) == 0);
    bytebuf_append(&buf, suffix, sizeof(suffix));
    assert(buf.len == 10U);
    assert(buf.data[8] == 0U);
    assert(buf.data[9] == 0xffU);
    bytebuf_free(&buf);
}

static void test_log_sink_receives_structured_message(void)
{
    CapturedLog capture = {0};
    SagLogSink sink = {capture_log, &capture};

    sag_log_set_sink(&sink);
    sag_log(SAG_LOG_WARN, "captured %s %d", "message", 7);
    sag_log_set_sink(NULL);

    assert(capture.calls == 1U);
    assert(capture.level == SAG_LOG_WARN);
    assert(strcmp(capture.message, "captured message 7") == 0);
}

static void test_module_require_reports_disabled_module(void)
{
    static const bool expected_enabled[SAG_MOD_COUNT] = {
        SAG_WITH_LSP != 0,
        SAG_WITH_AI != 0,
        SAG_WITH_FUSS != 0,
        SAG_WITH_PLUGINS != 0
    };
    SagMod mod;

    for (mod = SAG_MOD_LSP; mod < SAG_MOD_COUNT; mod++) {
        char err[160] = {0};

        assert(sag_mod_enabled(mod) == expected_enabled[mod]);
        if (expected_enabled[mod]) {
            assert(sag_mod_require(mod, err, sizeof(err)));
            assert(err[0] == '\0');
        } else {
            char expected[160];

            (void)snprintf(expected, sizeof(expected),
                "this build has no %s module; rebuild with 'make MODULES=\"… %s\"'",
                sag_mod_name(mod), sag_mod_name(mod));
            assert(!sag_mod_require(mod, err, sizeof(err)));
            assert(strcmp(err, expected) == 0);
        }
    }
}

int main(void)
{
    test_arena_alignment_and_strings();
    test_vec_grows_and_preserves_values();
    test_strmap_insertion_order_and_replacement();
    test_interner_ids_are_stable_and_round_trip();
    test_stable_sort_preserves_tie_order();
    test_bytebuf_printf_and_binary_append();
    test_log_sink_receives_structured_message();
    test_module_require_reports_disabled_module();
    return 0;
}
