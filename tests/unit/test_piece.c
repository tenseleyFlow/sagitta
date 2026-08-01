#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "text/piece.h"
#include "util/buf.h"

static Bytebuf textbuf_materialize(const TextBuf *tb)
{
    Bytebuf out;
    TextIter it;

    bytebuf_init(&out);
    if (sag_textiter_begin(&it, tb, BYTEOFF(0U))) {
        do {
            const u8 *chunk;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&it, tb, &chunk, &len));
            SAG_ASSERT(len > 0U);
            bytebuf_append(&out, chunk, (size_t)len);
        } while (sag_textiter_advance(&it, tb));
    }
    return out;
}

static Bytebuf textsnap_materialize(const TextBuf *tb, const TextSnap *snap)
{
    Bytebuf out;
    TextIter it;

    bytebuf_init(&out);
    if (sag_textsnap_iter(&it, snap, BYTEOFF(0U))) {
        do {
            const u8 *chunk;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&it, tb, &chunk, &len));
            SAG_ASSERT(len > 0U);
            bytebuf_append(&out, chunk, (size_t)len);
        } while (sag_textiter_advance(&it, tb));
    }
    return out;
}

static void assert_content(const TextBuf *tb, const void *expected, size_t len)
{
    Bytebuf actual = textbuf_materialize(tb);

    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), len);
    SAG_ASSERT_EQ_U64(actual.len, len);
    SAG_ASSERT_EQ_MEM(actual.data, expected, len);
    bytebuf_free(&actual);
}

static void assert_snap_content(const TextBuf *tb, const TextSnap *snap,
                                const void *expected, size_t len)
{
    Bytebuf actual = textsnap_materialize(tb, snap);

    SAG_ASSERT_EQ_U64(snap->len, len);
    SAG_ASSERT_EQ_U64(actual.len, len);
    SAG_ASSERT_EQ_MEM(actual.data, expected, len);
    bytebuf_free(&actual);
}

void test_piece_construct_empty(void)
{
    TextBuf *tb = sag_textbuf_new();

    SAG_ASSERT_NOT_NULL(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), 0U);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), 1U);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(tb), 0U);
    sag_textbuf_check(tb);
    assert_content(tb, NULL, 0U);
    sag_textbuf_free(tb);
}

void test_piece_construct_bytes(void)
{
    u8 source[] = "alpha\nbeta";
    static const u8 expected[] = "alpha\nbeta";
    TextBuf *tb = sag_textbuf_from_bytes(source, sizeof(source) - 1U);

    SAG_ASSERT_NOT_NULL(tb);
    source[0] = (u8)'X';
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), 2U);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(tb), 1U);
    assert_content(tb, expected, sizeof(expected) - 1U);
    sag_textbuf_free(tb);
}

void test_piece_construct_owned_bytes(void)
{
    u8 *bytes = sag_xmalloc(4U);
    TextBuf *tb;

    (void)memcpy(bytes, "a\nb", 4U);
    tb = sag_textbuf_from_owned_bytes(bytes, 4U);
    SAG_ASSERT(tb->orig.bytes == bytes);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), 4U);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), 2U);
    assert_content(tb, (const u8 *)"a\nb", 4U);
    sag_textbuf_check(tb);
    sag_textbuf_free(tb);
}

void test_piece_insert_positions(void)
{
    TextBuf *front = sag_textbuf_from_bytes((const u8 *)"bc", 2U);
    TextBuf *middle = sag_textbuf_from_bytes((const u8 *)"ac", 2U);
    TextBuf *end = sag_textbuf_from_bytes((const u8 *)"ab", 2U);

    sag_textbuf_insert(front, BYTEOFF(0U), (const u8 *)"a", 1U);
    sag_textbuf_check(front);
    assert_content(front, "abc", 3U);
    sag_textbuf_insert(middle, BYTEOFF(1U), (const u8 *)"b", 1U);
    sag_textbuf_check(middle);
    assert_content(middle, "abc", 3U);
    sag_textbuf_insert(end, BYTEOFF(2U), (const u8 *)"c", 1U);
    sag_textbuf_check(end);
    assert_content(end, "abc", 3U);
    sag_textbuf_free(front);
    sag_textbuf_free(middle);
    sag_textbuf_free(end);
}

static void assert_insert_span_reuses_store(u8 src)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"orig\n", 5U);
    Span span;
    u64 add_len;
    u64 gen;

    sag_textbuf_insert(tb, BYTEOFF(5U), (const u8 *)"add\n", 4U);
    span = src == SAG_STORE_ORIG ? (Span){0U, 5U} : (Span){0U, 4U};
    add_len = tb->add.len;
    gen = tb->gen;
    sag_textbuf_insert_span(tb, BYTEOFF(0U), src, span);
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(tb->add.len, add_len);
    SAG_ASSERT_EQ_U64(tb->gen, gen + 1U);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), 4U);
    if (src == SAG_STORE_ORIG)
        assert_content(tb, "orig\norig\nadd\n", 14U);
    else
        assert_content(tb, "add\norig\nadd\n", 13U);
    sag_textbuf_free(tb);
}

void test_piece_insert_every_seam(void)
{
    TextBuf *left_seam = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    TextBuf *right_seam = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);

    sag_textbuf_insert(left_seam, BYTEOFF(3U), (const u8 *)"X", 1U);
    sag_textbuf_check(left_seam);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(left_seam), 3U);
    sag_textbuf_insert(left_seam, BYTEOFF(3U), (const u8 *)"L", 1U);
    sag_textbuf_check(left_seam);
    assert_content(left_seam, "abcLXdef", 8U);

    sag_textbuf_insert(right_seam, BYTEOFF(3U), (const u8 *)"X", 1U);
    sag_textbuf_check(right_seam);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(right_seam), 3U);
    sag_textbuf_insert(right_seam, BYTEOFF(4U), (const u8 *)"R", 1U);
    sag_textbuf_check(right_seam);
    assert_content(right_seam, "abcXRdef", 8U);
    sag_textbuf_free(left_seam);
    sag_textbuf_free(right_seam);

    assert_insert_span_reuses_store(SAG_STORE_ORIG);
    assert_insert_span_reuses_store(SAG_STORE_ADD);
}

void test_piece_insert_span_recoalesces_boundaries(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);

    sag_textbuf_delete(tb, (Span){2U, 4U});
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(tb), 2U);
    sag_textbuf_insert_span(tb, BYTEOFF(2U), SAG_STORE_ORIG,
                            (Span){2U, 4U});
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(tb), 1U);
    assert_content(tb, "abcdef", 6U);
    sag_textbuf_free(tb);
}

void test_piece_insert_accepts_exposed_chunk_alias(void)
{
    TextBuf *tb = sag_textbuf_new();
    Bytebuf before;
    Bytebuf after;
    TextIter it;
    const u8 *aliased;
    u64 aliased_len;
    u8 *fill;
    u64 fill_len;

    sag_textbuf_insert(tb, BYTEOFF(0U), (const u8 *)"a", 1U);
    sag_textbuf_check(tb);
    fill_len = tb->add.cap - tb->add.len;
    fill = sag_xmalloc((size_t)fill_len);
    (void)memset(fill, 'b', (size_t)fill_len);
    sag_textbuf_insert(tb, BYTEOFF(sag_textbuf_len(tb)), fill, fill_len);
    sag_textbuf_check(tb);
    free(fill);
    SAG_ASSERT_EQ_U64(tb->add.len, tb->add.cap);
    before = textbuf_materialize(tb);
    SAG_ASSERT(sag_textiter_begin(&it, tb, BYTEOFF(0U)));
    SAG_ASSERT(sag_textiter_chunk(&it, tb, &aliased, &aliased_len));
    SAG_ASSERT_EQ_U64(aliased_len, before.len);
    sag_textbuf_insert(tb, BYTEOFF(sag_textbuf_len(tb)), aliased, aliased_len);
    sag_textbuf_check(tb);
    after = textbuf_materialize(tb);
    SAG_ASSERT_EQ_U64(after.len, before.len * 2U);
    SAG_ASSERT_EQ_MEM(after.data, before.data, before.len);
    SAG_ASSERT_EQ_MEM(after.data + before.len, before.data, before.len);
    bytebuf_free(&before);
    bytebuf_free(&after);
    sag_textbuf_free(tb);
}

void test_piece_insert_recoalesces_after_delete(void)
{
    TextBuf *tb = sag_textbuf_new();

    sag_textbuf_insert(tb, BYTEOFF(0U), (const u8 *)"ab", 2U);
    sag_textbuf_check(tb);
    sag_textbuf_delete(tb, (Span){0U, 1U});
    sag_textbuf_check(tb);
    sag_textbuf_insert(tb, BYTEOFF(1U), (const u8 *)"c", 1U);
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(tb), 1U);
    assert_content(tb, "bc", 2U);
    sag_textbuf_free(tb);
}

void test_piece_delete_single_byte(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abc", 3U);

    sag_textbuf_delete(tb, (Span){1U, 2U});
    sag_textbuf_check(tb);
    assert_content(tb, "ac", 2U);
    sag_textbuf_free(tb);
}

void test_piece_delete_cross_piece(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);

    sag_textbuf_insert(tb, BYTEOFF(3U), (const u8 *)"XYZ", 3U);
    sag_textbuf_check(tb);
    sag_textbuf_delete(tb, (Span){2U, 7U});
    sag_textbuf_check(tb);
    assert_content(tb, "abef", 4U);
    sag_textbuf_free(tb);
}

void test_piece_delete_whole_buffer(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abc\ndef", 7U);

    sag_textbuf_delete(tb, (Span){0U, 7U});
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), 1U);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(tb), 0U);
    assert_content(tb, NULL, 0U);
    sag_textbuf_free(tb);
}

void test_piece_delete_empty_range(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abc", 3U);
    u64 gen = tb->gen;

    sag_textbuf_delete(tb, (Span){1U, 1U});
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(tb->gen, gen);
    assert_content(tb, "abc", 3U);
    sag_textbuf_free(tb);
}

void test_piece_delete_recoalesces_adjacent_spans(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);

    sag_textbuf_insert(tb, BYTEOFF(3U), (const u8 *)"X", 1U);
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(tb), 3U);
    sag_textbuf_delete(tb, (Span){3U, 4U});
    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_piece_count(tb), 1U);
    assert_content(tb, "abcdef", 6U);
    sag_textbuf_free(tb);
}

void test_piece_iterator_chunks_are_maximal_after_delete(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    TextIter it;
    const u8 *chunk;
    u64 len;

    sag_textbuf_insert(tb, BYTEOFF(3U), (const u8 *)"X", 1U);
    sag_textbuf_check(tb);
    sag_textbuf_delete(tb, (Span){3U, 4U});
    sag_textbuf_check(tb);
    SAG_ASSERT(sag_textiter_begin(&it, tb, BYTEOFF(0U)));
    SAG_ASSERT(sag_textiter_chunk(&it, tb, &chunk, &len));
    SAG_ASSERT_EQ_U64(len, 6U);
    SAG_ASSERT_EQ_MEM(chunk, "abcdef", 6U);
    SAG_ASSERT(!sag_textiter_advance(&it, tb));
    sag_textbuf_free(tb);
}

void test_piece_line_mapping_edges(void)
{
    static const u8 with_eof_lf[] = "\n\nx\nlast\n";
    static const u64 starts[] = {0U, 1U, 2U, 4U, 9U};
    static const Span spans[] = {
        {0U, 1U}, {1U, 2U}, {2U, 4U}, {4U, 9U}, {9U, 9U}
    };
    TextBuf *tb = sag_textbuf_from_bytes(with_eof_lf,
                                         sizeof(with_eof_lf) - 1U);
    TextBuf *no_eof = sag_textbuf_from_bytes((const u8 *)"a\nlast", 6U);
    size_t i;

    sag_textbuf_check(tb);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), SAG_ARRAY_LEN(starts));
    for (i = 0U; i < SAG_ARRAY_LEN(starts); i++) {
        ByteOff start = sag_textbuf_line_start(tb, LINENO(i));
        Span span = sag_textbuf_line_span(tb, LINENO(i));

        SAG_ASSERT_EQ_U64(start.v, starts[i]);
        SAG_ASSERT_EQ_U64(sag_textbuf_line_of(tb, start).v, i);
        SAG_ASSERT_EQ_U64(span.lo, spans[i].lo);
        SAG_ASSERT_EQ_U64(span.hi, spans[i].hi);
    }
    SAG_ASSERT_EQ_U64(sag_textbuf_line_span(no_eof, LINENO(1U)).lo, 2U);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_span(no_eof, LINENO(1U)).hi, 6U);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_of(no_eof, BYTEOFF(6U)).v, 1U);
    sag_textbuf_free(tb);
    sag_textbuf_free(no_eof);
}

void test_piece_line_mapping_mixed_fixture(void)
{
    enum { FIXTURE_LEN = 64 * 1024 };
    u8 *fixture = sag_xmalloc(FIXTURE_LEN);
    TextBuf *tb;
    u64 off;

    for (off = 0U; off < FIXTURE_LEN; off++) {
        if (off % 97U == 0U)
            fixture[off] = (u8)'\n';
        else if (off % 211U == 0U)
            fixture[off] = (u8)'\r';
        else
            fixture[off] = (u8)('a' + off % 26U);
    }
    tb = sag_textbuf_from_bytes(fixture, FIXTURE_LEN);
    free(fixture);
    sag_textbuf_check(tb);
    for (off = 0U; off <= FIXTURE_LEN; off++) {
        LineNo line = sag_textbuf_line_of(tb, BYTEOFF(off));
        ByteOff start = sag_textbuf_line_start(tb, line);

        SAG_ASSERT(start.v <= off);
        SAG_ASSERT_EQ_U64(sag_textbuf_line_of(tb, start).v, line.v);
    }
    sag_textbuf_free(tb);
}

void test_piece_line_iterator_yields_spans(void)
{
    static const Span expected[] = {{2U, 3U}, {3U, 4U}};
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"a\n\nb", 4U);
    LineIter it;
    Span span;
    size_t i = 0U;

    SAG_ASSERT(sag_lineiter_begin(&it, tb, LINENO(1U)));
    while (sag_lineiter_next(&it, tb, &span)) {
        SAG_ASSERT(i < SAG_ARRAY_LEN(expected));
        SAG_ASSERT_EQ_U64(span.lo, expected[i].lo);
        SAG_ASSERT_EQ_U64(span.hi, expected[i].hi);
        i++;
    }
    SAG_ASSERT_EQ_U64(i, SAG_ARRAY_LEN(expected));
    sag_textbuf_free(tb);
}

void test_piece_sequential_insert_coalesces(void)
{
    TextBuf *tb = sag_textbuf_new();
    Bytebuf actual;
    u64 i;

    for (i = 0U; i < 10000U; i++) {
        sag_textbuf_insert(tb, BYTEOFF(i), (const u8 *)"q", 1U);
        sag_textbuf_check(tb);
    }
    SAG_ASSERT(sag_textbuf_piece_count(tb) <= 3U);
    actual = textbuf_materialize(tb);
    SAG_ASSERT_EQ_U64(actual.len, 10000U);
    for (i = 0U; i < actual.len; i++)
        SAG_ASSERT_EQ_U64(actual.data[i], (u8)'q');
    bytebuf_free(&actual);
    sag_textbuf_free(tb);
}

void test_piece_interleaved_insert_stays_distinct(void)
{
    TextBuf *tb = sag_textbuf_new();
    u64 i;

    for (i = 0U; i < 100U; i++) {
        u8 byte = (u8)('a' + i % 26U);

        sag_textbuf_insert(tb, BYTEOFF(0U), &byte, 1U);
        sag_textbuf_check(tb);
    }
    SAG_ASSERT(sag_textbuf_piece_count(tb) > 3U);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), 100U);
    sag_textbuf_free(tb);
}

void test_piece_snapshot_preserves_content(void)
{
    static const u8 original[] = "snapshot\ncontent";
    TextBuf *tb = sag_textbuf_from_bytes(original, sizeof(original) - 1U);
    TextSnap snap = sag_textbuf_snap(tb);
    u64 i;

    for (i = 0U; i < 100U; i++) {
        u8 byte = (u8)('A' + i % 26U);

        sag_textbuf_insert(tb, BYTEOFF(sag_textbuf_len(tb)), &byte, 1U);
        sag_textbuf_check(tb);
    }
    assert_snap_content(tb, &snap, original, sizeof(original) - 1U);
    sag_textsnap_release(tb, &snap);
    sag_textbuf_free(tb);
}

static u64 piece_test_random(u64 *state)
{
    u64 x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

void test_piece_snapshot_survives_middle_edits(void)
{
    enum { INITIAL_LEN = 2048, PREP_EDITS = 128, POST_EDITS = 500 };
    u8 *initial = sag_xmalloc(INITIAL_LEN);
    TextBuf *tb;
    TextSnap snap;
    Bytebuf expected;
    u64 rng = UINT64_C(0xd6e8feb86659fd93);
    u64 i;

    for (i = 0U; i < INITIAL_LEN; i++)
        initial[i] = (u8)('a' + i % 26U);
    tb = sag_textbuf_from_bytes(initial, INITIAL_LEN);
    free(initial);
    for (i = 0U; i < PREP_EDITS; i++) {
        u8 byte = (u8)('A' + i % 26U);
        u64 at = 1U + piece_test_random(&rng) % (sag_textbuf_len(tb) - 1U);

        sag_textbuf_insert(tb, BYTEOFF(at), &byte, 1U);
        sag_textbuf_check(tb);
    }
    expected = textbuf_materialize(tb);
    snap = sag_textbuf_snap(tb);
    for (i = 0U; i < POST_EDITS; i++) {
        u64 len = sag_textbuf_len(tb);
        u64 at = 1U + piece_test_random(&rng) % (len - 1U);

        if ((piece_test_random(&rng) & 1U) == 0U) {
            u8 byte = (u8)piece_test_random(&rng);

            sag_textbuf_insert(tb, BYTEOFF(at), &byte, 1U);
        } else {
            sag_textbuf_delete(tb, (Span){at, at + 1U});
        }
        sag_textbuf_check(tb);
        if ((i + 1U) % 50U == 0U)
            assert_snap_content(tb, &snap, expected.data, expected.len);
    }
    assert_snap_content(tb, &snap, expected.data, expected.len);
    sag_textsnap_release(tb, &snap);
    bytebuf_free(&expected);
    sag_textbuf_free(tb);
}

void test_piece_snapshot_release_before_buffer(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"pinned", 6U);
    TextSnap snap = sag_textbuf_snap(tb);

    sag_textsnap_release(tb, &snap);
    SAG_ASSERT_NULL(snap.root);
    sag_textbuf_check(tb);
    assert_content(tb, "pinned", 6U);
    sag_textbuf_free(tb);
}

void test_piece_buffer_release_before_snapshot(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"old root", 8U);
    TextSnap snap = sag_textbuf_snap(tb);

    sag_textbuf_free(tb);
    assert_snap_content(NULL, &snap, "old root", 8U);
    sag_textsnap_release(NULL, &snap);
    SAG_ASSERT_NULL(snap.root);
}

static int bug_action_exit(void (*action)(void *), void *context)
{
    pid_t child;
    pid_t waited;
    int status;

    SAG_ASSERT_EQ_I64(fflush(NULL), 0);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        (void)close(STDERR_FILENO);
        (void)setenv("SAG_LOG", "/dev/null", 1);
        action(context);
        _exit(0);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    return WEXITSTATUS(status);
}

static void check_buffer_action(void *context)
{
    sag_textbuf_check(context);
}

void test_piece_checker_rejects_corruption(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"broken", 6U);
    u64 valid_sub_bytes = tb->root->sub_bytes;

    tb->root->sub_bytes++;
    SAG_ASSERT_EQ_I64(bug_action_exit(check_buffer_action, tb), SAG_EXIT_BUG);
    tb->root->sub_bytes = valid_sub_bytes;
    sag_textbuf_check(tb);
    sag_textbuf_free(tb);
}

typedef struct {
    TextBuf *tb;
    TextIter *it;
} StaleIterContext;

static void stale_iterator_action(void *context)
{
    StaleIterContext *stale = context;
    const u8 *chunk;
    u64 len;

    (void)sag_textiter_chunk(stale->it, stale->tb, &chunk, &len);
}

void test_piece_live_iterator_rejects_edit(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abc", 3U);
    TextIter it;
    StaleIterContext context;

    SAG_ASSERT(sag_textiter_begin(&it, tb, BYTEOFF(0U)));
    sag_textbuf_insert(tb, BYTEOFF(1U), (const u8 *)"X", 1U);
    sag_textbuf_check(tb);
    context.tb = tb;
    context.it = &it;
    SAG_ASSERT_EQ_I64(bug_action_exit(stale_iterator_action, &context),
                      SAG_EXIT_BUG);
    sag_textbuf_free(tb);
}

typedef struct {
    TextBuf *tb;
    LineIter *it;
} WrongLineIterContext;

static void wrong_line_iterator_action(void *context)
{
    WrongLineIterContext *wrong = context;
    Span line;

    (void)sag_lineiter_next(wrong->it, wrong->tb, &line);
}

void test_piece_line_iterator_rejects_other_buffer(void)
{
    TextBuf *first = sag_textbuf_from_bytes((const u8 *)"a\n", 2U);
    TextBuf *second = sag_textbuf_from_bytes((const u8 *)"b\n", 2U);
    LineIter it;
    WrongLineIterContext context;

    SAG_ASSERT(sag_lineiter_begin(&it, first, LINENO(0U)));
    context.tb = second;
    context.it = &it;
    SAG_ASSERT_EQ_I64(bug_action_exit(wrong_line_iterator_action, &context),
                      SAG_EXIT_BUG);
    sag_textbuf_check(first);
    sag_textbuf_check(second);
    sag_textbuf_free(first);
    sag_textbuf_free(second);
}
