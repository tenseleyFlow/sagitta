#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "text/piece.h"
#include "util/buf.h"

enum { DIFF_OPS = 10000 };

typedef struct {
    Bytebuf bytes;
    u32 pieces;
    u64 lines;
    u64 trace_hash;
    u64 tree_hash;
} DiffResult;

static u64 diff_rng(u64 *state)
{
    u64 x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void oracle_insert(Bytebuf *oracle, size_t at,
                          const u8 *bytes, size_t len)
{
    bytebuf_reserve(oracle, oracle->len + len);
    (void)memmove(oracle->data + at + len, oracle->data + at,
                  oracle->len - at);
    (void)memcpy(oracle->data + at, bytes, len);
    oracle->len += len;
}

static void oracle_delete(Bytebuf *oracle, size_t lo, size_t hi)
{
    (void)memmove(oracle->data + lo, oracle->data + hi, oracle->len - hi);
    oracle->len -= hi - lo;
}

static u64 oracle_line_count(const Bytebuf *oracle)
{
    size_t i;
    u64 lines = 1U;

    for (i = 0U; i < oracle->len; i++) {
        if (oracle->data[i] == (u8)'\n')
            lines++;
    }
    return lines;
}

static u64 oracle_line_of(const Bytebuf *oracle, size_t off)
{
    size_t i;
    u64 line = 0U;

    for (i = 0U; i < off; i++) {
        if (oracle->data[i] == (u8)'\n')
            line++;
    }
    return line;
}

static size_t oracle_line_start(const Bytebuf *oracle, u64 line)
{
    size_t i;
    u64 remaining = line;

    if (line == 0U)
        return 0U;
    for (i = 0U; i < oracle->len; i++) {
        if (oracle->data[i] == (u8)'\n' && --remaining == 0U)
            return i + 1U;
    }
    SAG_ASSERT(false);
    return 0U;
}

static Bytebuf materialize(const TextBuf *tb)
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

static void assert_matches_oracle(const TextBuf *tb, const Bytebuf *oracle)
{
    Bytebuf actual = materialize(tb);
    u64 line_count = oracle_line_count(oracle);
    size_t off;
    u64 line;

    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), oracle->len);
    SAG_ASSERT_EQ_U64(actual.len, oracle->len);
    SAG_ASSERT_EQ_MEM(actual.data, oracle->data, oracle->len);
    SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), line_count);
    for (off = 0U; off <= oracle->len; off++)
        SAG_ASSERT_EQ_U64(sag_textbuf_line_of(tb, BYTEOFF(off)).v,
                          oracle_line_of(oracle, off));
    for (line = 0U; line < line_count; line++) {
        size_t lo = oracle_line_start(oracle, line);
        size_t hi = line + 1U < line_count
                        ? oracle_line_start(oracle, line + 1U)
                        : oracle->len;
        Span span = sag_textbuf_line_span(tb, LINENO(line));

        SAG_ASSERT_EQ_U64(sag_textbuf_line_start(tb, LINENO(line)).v, lo);
        SAG_ASSERT_EQ_U64(span.lo, lo);
        SAG_ASSERT_EQ_U64(span.hi, hi);
    }
    bytebuf_free(&actual);
}

static u64 hash_u64(u64 hash, u64 value)
{
    size_t i;

    for (i = 0U; i < sizeof(value); i++) {
        hash ^= (value >> (i * 8U)) & 0xffU;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static u64 tree_digest(const PieceNode *node, u64 hash)
{
    if (node == NULL)
        return hash_u64(hash, UINT64_C(0x9b));
    hash = hash_u64(hash, UINT64_C(0x71));
    hash = hash_u64(hash, node->src);
    hash = hash_u64(hash, node->span.lo);
    hash = hash_u64(hash, node->span.hi);
    hash = hash_u64(hash, node->lf_first);
    hash = hash_u64(hash, node->lf_count);
    hash = hash_u64(hash, node->sub_bytes);
    hash = hash_u64(hash, node->sub_lfs);
    hash = hash_u64(hash, node->sub_count);
    hash = tree_digest(node->left, hash);
    return tree_digest(node->right, hash);
}

static DiffResult run_diff(u64 seed)
{
    static const u64 hash_prime = UINT64_C(1099511628211);
    DiffResult result;
    Bytebuf oracle;
    TextBuf *tb;
    u64 rng = seed;
    u64 hash = UINT64_C(1469598103934665603);
    size_t op;

    bytebuf_init(&oracle);
    tb = sag_textbuf_new();
    SAG_ASSERT_NOT_NULL(tb);
    for (op = 0U; op < DIFF_OPS; op++) {
        u64 choice = diff_rng(&rng);

        hash = (hash ^ choice) * hash_prime;
        if (oracle.len == 0U || (choice & 1U) == 0U) {
            u8 inserted[8];
            size_t len = 1U + (size_t)(diff_rng(&rng) % 8U);
            size_t at = (size_t)(diff_rng(&rng) % (oracle.len + 1U));
            size_t i;

            for (i = 0U; i < len; i++) {
                u64 value = diff_rng(&rng);

                inserted[i] = (value & 7U) == 0U
                                  ? (u8)'\n' : (u8)(value & 0xffU);
            }
            sag_textbuf_insert(tb, BYTEOFF(at), inserted, len);
            oracle_insert(&oracle, at, inserted, len);
            hash = (hash ^ (u64)at ^ ((u64)len << 32)) * hash_prime;
        } else {
            size_t a = (size_t)(diff_rng(&rng) % oracle.len);
            size_t b = (size_t)(diff_rng(&rng) % oracle.len);
            size_t lo = a < b ? a : b;
            size_t hi = a < b ? b : a;

            hi++;
            sag_textbuf_delete(tb, (Span){(u64)lo, (u64)hi});
            oracle_delete(&oracle, lo, hi);
            hash = (hash ^ (u64)lo ^ ((u64)hi << 32)) * hash_prime;
        }
        sag_textbuf_check(tb);
        if ((op + 1U) % 100U == 0U)
            assert_matches_oracle(tb, &oracle);
    }
    assert_matches_oracle(tb, &oracle);
    result.bytes = materialize(tb);
    result.pieces = sag_textbuf_piece_count(tb);
    result.lines = sag_textbuf_line_count(tb);
    result.trace_hash = hash;
    result.tree_hash = tree_digest(tb->root,
                                  UINT64_C(1469598103934665603));
    sag_textbuf_free(tb);
    bytebuf_free(&oracle);
    return result;
}

static void assert_seed(u64 seed)
{
    DiffResult result = run_diff(seed);

    SAG_ASSERT(result.trace_hash != 0U);
    SAG_ASSERT(result.lines >= 1U);
    bytebuf_free(&result.bytes);
}

void test_piece_diff_seed_0000000000000001(void)
{
    assert_seed(UINT64_C(1));
}

void test_piece_diff_seed_243f6a8885a308d3(void)
{
    assert_seed(UINT64_C(0x243f6a8885a308d3));
}

void test_piece_diff_seed_9e3779b97f4a7c15(void)
{
    assert_seed(UINT64_C(0x9e3779b97f4a7c15));
}

void test_piece_diff_seed_d1b54a32d192ed03(void)
{
    assert_seed(UINT64_C(0xd1b54a32d192ed03));
}

void test_piece_diff_is_deterministic(void)
{
    DiffResult first = run_diff(UINT64_C(0x6a09e667f3bcc909));
    DiffResult second = run_diff(UINT64_C(0x6a09e667f3bcc909));

    SAG_ASSERT_EQ_U64(first.trace_hash, second.trace_hash);
    SAG_ASSERT_EQ_U64(first.tree_hash, second.tree_hash);
    SAG_ASSERT_EQ_U64(first.pieces, second.pieces);
    SAG_ASSERT_EQ_U64(first.lines, second.lines);
    SAG_ASSERT_EQ_U64(first.bytes.len, second.bytes.len);
    SAG_ASSERT_EQ_MEM(first.bytes.data, second.bytes.data, first.bytes.len);
    bytebuf_free(&first.bytes);
    bytebuf_free(&second.bytes);
}
