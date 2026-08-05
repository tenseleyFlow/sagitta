#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text/edit.h"
#include "unicode/coords.h"
#include "util/buf.h"

static const u64 prop_seeds[] = {
    UINT64_C(1), UINT64_C(0x243f6a8885a308d3),
    UINT64_C(0x9e3779b97f4a7c15), UINT64_C(0xd1b54a32d192ed03)
};

static u64 prop_rng(u64 *state)
{
    u64 x = *state;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static u64 prop_cases(void)
{
    const char *value = getenv("SAG_PROP_N");
    char *end;
    unsigned long long parsed;

    if (value == NULL || *value == '\0')
        return 500U;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    SAG_ASSERT(errno == 0 && *end == '\0' && parsed > 0U);
    return (u64)parsed;
}

static void prop_fill(Bytebuf *buf, u64 *rng, size_t max_len)
{
    static const u8 special[] = {
        '\n', '\r', '\0', 0x80U, 0xffU, 0xccU, 0x81U, 0xe6U,
        0xbcU, 0xa2U, '\t'
    };
    size_t len = (size_t)(prop_rng(rng) % (max_len + 1U));
    size_t i;

    bytebuf_init(buf);
    bytebuf_reserve(buf, len);
    for (i = 0U; i < len; i++) {
        u64 value = prop_rng(rng);
        u8 byte = (value & 3U) == 0U
                      ? special[(size_t)(value % SAG_ARRAY_LEN(special))]
                      : (u8)value;

        bytebuf_append(buf, &byte, 1U);
    }
}

static Bytebuf prop_materialize(const TextBuf *tb)
{
    Bytebuf out;
    TextIter it;

    bytebuf_init(&out);
    if (sag_textiter_begin(&it, tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&it, tb, &bytes, &len));
            SAG_ASSERT(len > 0U);
            bytebuf_append(&out, bytes, (size_t)len);
        } while (sag_textiter_advance(&it, tb));
    }
    return out;
}

static Bytebuf prop_snap_materialize(const TextBuf *tb, const TextSnap *snap)
{
    Bytebuf out;
    TextIter it;

    bytebuf_init(&out);
    if (sag_textsnap_iter(&it, snap, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&it, tb, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (sag_textiter_advance(&it, tb));
    }
    return out;
}

static void prop_assert_bytes(const TextBuf *tb, const Bytebuf *want)
{
    Bytebuf got = prop_materialize(tb);

    SAG_ASSERT_EQ_U64(got.len, want->len);
    SAG_ASSERT_EQ_MEM(got.data, want->data, want->len);
    bytebuf_free(&got);
}

static void prop_copy_range(const TextBuf *tb, Span range, Bytebuf *out)
{
    Bytebuf all = prop_materialize(tb);
    u64 len = range.hi - range.lo;

    bytebuf_init(out);
    if (len != 0U)
        bytebuf_append(out, all.data + (size_t)range.lo, (size_t)len);
    bytebuf_free(&all);
}

static void prop_oracle_insert(Bytebuf *bytes, size_t at,
                               const u8 *payload, size_t len)
{
    bytebuf_reserve(bytes, bytes->len + len);
    (void)memmove(bytes->data + at + len, bytes->data + at,
                  bytes->len - at);
    (void)memcpy(bytes->data + at, payload, len);
    bytes->len += len;
}

static u64 prop_count_lfs(const Bytebuf *bytes)
{
    size_t i;
    u64 count = 0U;

    for (i = 0U; i < bytes->len; i++)
        count += bytes->data[i] == (u8)'\n';
    return count;
}

static Cursor prop_cursor(u64 pos, u64 goal)
{
    Cursor c;

    c.pos = BYTEOFF(pos);
    c.anchor = c.pos;
    c.goal_col = (GCol){goal};
    return c;
}

typedef struct {
    TextBuf *tb;
    MarkSet *marks;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
} PropEdit;

static void prop_edit_init(PropEdit *f, const u8 *bytes, u64 len)
{
    f->tb = sag_textbuf_from_bytes(bytes, len);
    f->marks = sag_marks_new();
    sag_cset_init(&f->cursors, prop_cursor(0U, 17U));
    f->undo = sag_undo_new(f->tb);
    f->edit = (EditCtx){f->tb, f->marks, &f->cursors, 1U, NULL,
                       f->undo, NULL, NULL, NULL, 0};
}

static void prop_edit_free(PropEdit *f)
{
    sag_undo_free(f->undo);
    sag_cset_free(&f->cursors);
    sag_marks_free(f->marks);
    sag_textbuf_free(f->tb);
}

static void prop_random_edit(PropEdit *f, u64 *rng)
{
    u64 len = sag_textbuf_len(f->tb);

    sag_undo_boundary(f->undo);
    if (len == 0U || (prop_rng(rng) & 1U) == 0U) {
        u8 bytes[4];
        u64 n = 1U + prop_rng(rng) % SAG_ARRAY_LEN(bytes);
        u64 at = prop_rng(rng) % (len + 1U);
        u64 i;

        for (i = 0U; i < n; i++)
            bytes[i] = (u8)prop_rng(rng);
        sag_undo_begin(&f->edit, SAG_TXN_TYPE);
        sag_edit_insert(&f->edit, BYTEOFF(at), bytes, n);
        sag_cset_normalize(f->tb, &f->cursors);
        sag_undo_end(&f->edit);
    } else {
        u64 lo = prop_rng(rng) % len;
        u64 hi = lo + 1U + prop_rng(rng) % (len - lo);

        sag_undo_begin(&f->edit, SAG_TXN_ERASE);
        sag_edit_delete(&f->edit, (Span){lo, hi});
        sag_cset_normalize(f->tb, &f->cursors);
        sag_undo_end(&f->edit);
    }
}

void test_textbuf_prop_p1_splice_inverse(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            Bytebuf base;
            Bytebuf payload;
            TextBuf *tb;
            u64 at;
            u32 pieces;
            u64 lines;

            prop_fill(&base, &rng, 64U);
            prop_fill(&payload, &rng, 16U);
            if (payload.len == 0U)
                bytebuf_append(&payload, "x", 1U);
            tb = sag_textbuf_from_bytes(base.data, base.len);
            at = prop_rng(&rng) % (base.len + 1U);
            pieces = sag_textbuf_piece_count(tb);
            lines = sag_textbuf_line_count(tb);
            sag_textbuf_insert(tb, BYTEOFF(at), payload.data, payload.len);
            sag_textbuf_delete(tb, (Span){at, at + payload.len});
            sag_textbuf_check(tb);
            prop_assert_bytes(tb, &base);
            SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb), lines);
            SAG_ASSERT(sag_textbuf_piece_count(tb) <= pieces + 2U);
            sag_textbuf_free(tb);
            bytebuf_free(&payload);
            bytebuf_free(&base);
        }
    }
}

void test_textbuf_prop_p2_delete_inverse(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            Bytebuf base;
            Bytebuf removed;
            TextBuf *tb;
            u64 lo;
            u64 hi;

            prop_fill(&base, &rng, 96U);
            if (base.len == 0U)
                bytebuf_append(&base, "x", 1U);
            tb = sag_textbuf_from_bytes(base.data, base.len);
            lo = prop_rng(&rng) % base.len;
            hi = lo + prop_rng(&rng) % (base.len - lo + 1U);
            prop_copy_range(tb, (Span){lo, hi}, &removed);
            sag_textbuf_delete(tb, (Span){lo, hi});
            sag_textbuf_insert(tb, BYTEOFF(lo), removed.data, removed.len);
            sag_textbuf_check(tb);
            prop_assert_bytes(tb, &base);
            sag_textbuf_free(tb);
            bytebuf_free(&removed);
            bytebuf_free(&base);
        }
    }
}

static void prop_assert_iter_suffix(const TextBuf *tb, const Bytebuf *all,
                                    u64 at)
{
    Bytebuf suffix;
    TextIter it;

    bytebuf_init(&suffix);
    if (sag_textiter_begin(&it, tb, BYTEOFF(at))) {
        do {
            const u8 *bytes;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&it, tb, &bytes, &len));
            bytebuf_append(&suffix, bytes, (size_t)len);
        } while (sag_textiter_advance(&it, tb));
    }
    SAG_ASSERT_EQ_U64(suffix.len, all->len - (size_t)at);
    if (suffix.len != 0U)
        SAG_ASSERT_EQ_MEM(suffix.data, all->data + (size_t)at, suffix.len);
    bytebuf_free(&suffix);
}

static void prop_visit_seams(const PieceNode *node, u64 base,
                             const TextBuf *tb, const Bytebuf *all)
{
    u64 left;
    u64 end;

    if (node == NULL)
        return;
    left = node->left == NULL ? 0U : node->left->sub_bytes;
    prop_visit_seams(node->left, base, tb, all);
    end = base + left + node->span.hi - node->span.lo;
    prop_assert_iter_suffix(tb, all, base + left);
    prop_assert_iter_suffix(tb, all, end);
    prop_visit_seams(node->right, end, tb, all);
}

void test_textbuf_prop_p3_iterator_materialize(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            Bytebuf base;
            Bytebuf all;
            TextBuf *tb;
            u64 i;

            prop_fill(&base, &rng, 64U);
            tb = sag_textbuf_from_bytes(base.data, base.len);
            for (i = 0U; i < 6U; i++) {
                u8 byte = (u8)prop_rng(&rng);
                u64 at = prop_rng(&rng) % (sag_textbuf_len(tb) + 1U);

                sag_textbuf_insert(tb, BYTEOFF(at), &byte, 1U);
                prop_oracle_insert(&base, (size_t)at, &byte, 1U);
            }
            all = prop_materialize(tb);
            SAG_ASSERT_EQ_U64(all.len, base.len);
            SAG_ASSERT_EQ_MEM(all.data, base.data, base.len);
            prop_assert_iter_suffix(tb, &all, 0U);
            prop_assert_iter_suffix(tb, &all,
                                    prop_rng(&rng) % (all.len + 1U));
            prop_visit_seams(tb->root, 0U, tb, &all);
            bytebuf_free(&all);
            sag_textbuf_free(tb);
            bytebuf_free(&base);
        }
    }
}

void test_textbuf_prop_p4_line_index_recount(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            Bytebuf base;
            TextBuf *tb;
            u64 line = 0U;
            size_t i;

            prop_fill(&base, &rng, 128U);
            tb = sag_textbuf_from_bytes(base.data, base.len);
            SAG_ASSERT_EQ_U64(sag_textbuf_line_count(tb),
                              prop_count_lfs(&base) + 1U);
            SAG_ASSERT_EQ_U64(sag_textbuf_line_start(tb, LINENO(0U)).v, 0U);
            for (i = 0U; i < base.len; i++) {
                if (base.data[i] == (u8)'\n') {
                    line++;
                    SAG_ASSERT_EQ_U64(
                        sag_textbuf_line_start(tb, LINENO(line)).v, i + 1U);
                }
            }
            sag_textbuf_free(tb);
            bytebuf_free(&base);
        }
    }
}

void test_textbuf_prop_p5_line_roundtrip(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            Bytebuf base;
            TextBuf *tb;
            u64 lines;
            u64 line;
            u64 off;

            prop_fill(&base, &rng, 96U);
            tb = sag_textbuf_from_bytes(base.data, base.len);
            lines = sag_textbuf_line_count(tb);
            for (line = 0U; line < lines; line++) {
                ByteOff start = sag_textbuf_line_start(tb, LINENO(line));

                SAG_ASSERT_EQ_U64(sag_textbuf_line_of(tb, start).v, line);
            }
            for (off = 0U; off <= base.len; off++) {
                line = sag_textbuf_line_of(tb, BYTEOFF(off)).v;
                SAG_ASSERT(sag_textbuf_line_start(tb, LINENO(line)).v <= off);
                if (line + 1U < lines)
                    SAG_ASSERT(off < sag_textbuf_line_start(
                                         tb, LINENO(line + 1U)).v);
                else
                    SAG_ASSERT(off < base.len + 1U);
            }
            sag_textbuf_free(tb);
            bytebuf_free(&base);
        }
    }
}

void test_textbuf_prop_p6_undo_redo_identity(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            PropEdit f;
            Bytebuf final;
            Cursor saved;
            u32 node;
            u64 edits = 1U + prop_rng(&rng) % 12U;
            u64 done = 0U;

            prop_edit_init(&f, (const u8 *)"root\n", 5U);
            while (done++ < edits)
                prop_random_edit(&f, &rng);
            final = prop_materialize(f.tb);
            saved = f.cursors.curs.data[f.cursors.primary];
            node = sag_undo_current(f.undo);
            done = 0U;
            while (sag_undo(&f.edit))
                done++;
            while (done != 0U) {
                SAG_ASSERT(sag_redo(&f.edit));
                done--;
            }
            prop_assert_bytes(f.tb, &final);
            SAG_ASSERT_EQ_U64(sag_textbuf_line_count(f.tb),
                              prop_count_lfs(&final) + 1U);
            SAG_ASSERT_EQ_U64(f.cursors.curs.data[f.cursors.primary].pos.v,
                              saved.pos.v);
            SAG_ASSERT_EQ_U64(
                f.cursors.curs.data[f.cursors.primary].anchor.v,
                saved.anchor.v);
            SAG_ASSERT_EQ_U64(
                f.cursors.curs.data[f.cursors.primary].goal_col.v,
                saved.goal_col.v);
            SAG_ASSERT_EQ_U64(sag_undo_current(f.undo), node);
            bytebuf_free(&final);
            prop_edit_free(&f);
        }
    }
}

void test_textbuf_prop_p7_undo_to_root(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            Bytebuf initial;
            PropEdit f;
            u64 i;

            prop_fill(&initial, &rng, 48U);
            prop_edit_init(&f, initial.data, initial.len);
            for (i = 0U; i < 8U; i++)
                prop_random_edit(&f, &rng);
            while (sag_undo(&f.edit))
                ;
            prop_assert_bytes(f.tb, &initial);
            prop_edit_free(&f);
            bytebuf_free(&initial);
        }
    }
}

void test_textbuf_prop_p8_branch_preservation(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            PropEdit f;
            u32 parent;
            u64 depth;

            prop_edit_init(&f, NULL, 0U);
            parent = sag_undo_current(f.undo);
            for (depth = 0U; depth < 8U; depth++) {
                Bytebuf branch_text[3];
                u32 branch[3];
                size_t width;

                for (width = 0U; width < 3U; width++) {
                    u8 byte = (u8)('A' + depth * 3U + width);

                    SAG_ASSERT(sag_undo_to(&f.edit, parent));
                    sag_undo_boundary(f.undo);
                    sag_edit_insert(&f.edit,
                                    BYTEOFF(sag_textbuf_len(f.tb)),
                                    &byte, 1U);
                    branch[width] = sag_undo_current(f.undo);
                    branch_text[width] = prop_materialize(f.tb);
                }
                for (width = 0U; width < 3U; width++) {
                    SAG_ASSERT(sag_undo_to(&f.edit, branch[width]));
                    prop_assert_bytes(f.tb, &branch_text[width]);
                }
                width = (size_t)(prop_rng(&rng) % 3U);
                parent = branch[width];
                SAG_ASSERT(sag_undo_to(&f.edit, parent));
                for (width = 0U; width < 3U; width++)
                    bytebuf_free(&branch_text[width]);
            }
            prop_edit_free(&f);
        }
    }
}

typedef struct {
    u64 bytes;
    u64 lfs;
    u32 count;
} PropTree;

static PropTree prop_check_tree(const PieceNode *node)
{
    PropTree out = {0U, 0U, 0U};

    if (node != NULL) {
        PropTree left = prop_check_tree(node->left);
        PropTree right = prop_check_tree(node->right);
        u64 own = node->span.hi - node->span.lo;

        SAG_ASSERT(own > 0U);
        out.bytes = left.bytes + own + right.bytes;
        out.lfs = left.lfs + node->lf_count + right.lfs;
        out.count = left.count + 1U + right.count;
        SAG_ASSERT_EQ_U64(node->sub_bytes, out.bytes);
        SAG_ASSERT_EQ_U64(node->sub_lfs, out.lfs);
        SAG_ASSERT_EQ_U64(node->sub_count, out.count);
    }
    return out;
}

void test_textbuf_prop_p9_coalescing_invariants(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            TextBuf *seq = sag_textbuf_new();
            TextBuf *scattered;
            u8 base[64];
            u64 k = 8U + prop_rng(&rng) % 16U;
            u64 i;
            u32 start;

            (void)memset(base, 'b', sizeof(base));
            for (i = 0U; i < k; i++)
                sag_textbuf_insert(seq, BYTEOFF(i), (const u8 *)"x", 1U);
            SAG_ASSERT(sag_textbuf_piece_count(seq) <= 2U);
            (void)prop_check_tree(seq->root);
            scattered = sag_textbuf_from_bytes(base, sizeof(base));
            start = sag_textbuf_piece_count(scattered);
            for (i = 0U; i < k; i++)
                sag_textbuf_insert(scattered, BYTEOFF(i * 3U),
                                   (const u8 *)"x", 1U);
            /* Each insert can split at most one piece and add one, hence
             * the structural upper bound is 2*ops+1. */
            SAG_ASSERT(sag_textbuf_piece_count(scattered) <= 2U * k + 1U);
            SAG_ASSERT(sag_textbuf_piece_count(scattered) - start >= k - 1U);
            (void)prop_check_tree(scattered->root);
            sag_textbuf_check(seq);
            sag_textbuf_check(scattered);
            sag_textbuf_free(scattered);
            sag_textbuf_free(seq);
        }
    }
}

void test_textbuf_prop_p10_snapshot_immutability(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            Bytebuf initial;
            TextBuf *tb;
            TextSnap snap;
            u64 i;

            prop_fill(&initial, &rng, 80U);
            tb = sag_textbuf_from_bytes(initial.data, initial.len);
            snap = sag_textbuf_snap(tb);
            for (i = 0U; i < 16U; i++) {
                u8 byte = (u8)prop_rng(&rng);
                u64 at = prop_rng(&rng) % (sag_textbuf_len(tb) + 1U);

                sag_textbuf_insert(tb, BYTEOFF(at), &byte, 1U);
            }
            {
                Bytebuf got = prop_snap_materialize(tb, &snap);
                SAG_ASSERT_EQ_U64(got.len, initial.len);
                SAG_ASSERT_EQ_MEM(got.data, initial.data, initial.len);
                bytebuf_free(&got);
            }
            if ((case_i & 1U) == 0U) {
                sag_textsnap_release(tb, &snap);
                sag_textbuf_free(tb);
            } else {
                sag_textbuf_free(tb);
                sag_textsnap_release(NULL, &snap);
            }
            bytebuf_free(&initial);
        }
    }
}

void test_textbuf_prop_p11_save_load_roundtrip(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            char path[] = "/tmp/sag-prop-XXXXXX";
            Bytebuf initial;
            TextBuf *tb;
            TextBuf *loaded = NULL;
            FileMeta save_meta;
            FileMeta load_meta;
            int fd;

            prop_fill(&initial, &rng, 96U);
            switch (case_i % 4U) {
            case 0U:
                bytebuf_append(&initial, "\r\n", 2U);
                break;
            case 1U: {
                static const u8 binary_invalid[] = {0U, 0xffU, 0xc0U, 0x80U};
                bytebuf_append(&initial, binary_invalid,
                               sizeof(binary_invalid));
                break;
            }
            case 2U:
                if (initial.len == 0U)
                    bytebuf_append(&initial, "n", 1U);
                else if (initial.data[initial.len - 1U] == (u8)'\n')
                    initial.data[initial.len - 1U] = (u8)'n';
                break;
            default: {
                static const u8 utf8[] = {
                    0xe6U, 0xbcU, 0xa2U, 0xf0U, 0x9fU, 0x91U, 0x8dU,
                    0xf0U, 0x9fU, 0x8fU, 0xbdU
                };
                bytebuf_append(&initial, utf8, sizeof(utf8));
                break;
            }
            }
            if (initial.len >= 3U && initial.data[0] == 0xefU &&
                initial.data[1] == 0xbbU && initial.data[2] == 0xbfU)
                initial.data[0] = (u8)'B';
            fd = mkstemp(path);
            SAG_ASSERT(fd >= 0);
            SAG_ASSERT_EQ_I64(close(fd), 0);
            SAG_ASSERT_EQ_I64(unlink(path), 0);
            tb = sag_textbuf_from_bytes(initial.data, initial.len);
            sag_filemeta_init(&save_meta);
            SAG_ASSERT_EQ_U64(sag_file_save(tb, &save_meta, path),
                              SAG_SAVE_OK);
            SAG_ASSERT_EQ_U64(sag_file_load(path, &loaded, &load_meta),
                              SAG_LOAD_OK);
            prop_assert_bytes(loaded, &initial);
            sag_textbuf_free(loaded);
            sag_filemeta_dispose(&load_meta);
            sag_textbuf_free(tb);
            sag_filemeta_dispose(&save_meta);
            SAG_ASSERT_EQ_I64(unlink(path), 0);
            bytebuf_free(&initial);
        }
    }
}

void test_textbuf_prop_p12_cursor_mark_invariants(void)
{
    size_t seed_i;
    u64 n = prop_cases();

    for (seed_i = 0U; seed_i < SAG_ARRAY_LEN(prop_seeds); seed_i++) {
        u64 rng = prop_seeds[seed_i];
        u64 case_i;

        for (case_i = 0U; case_i < n; case_i++) {
            PropEdit f;
            MarkId marks[8];
            size_t mark_i;
            u64 op;

            prop_edit_init(&f, (const u8 *)"e\xcc\x81\ntext", 8U);
            for (mark_i = 0U; mark_i < SAG_ARRAY_LEN(marks); mark_i++) {
                u64 pos = prop_rng(&rng) % (sag_textbuf_len(f.tb) + 1U);
                marks[mark_i] = sag_mark_add(
                    f.marks, BYTEOFF(pos),
                    (mark_i & 1U) == 0U ? SAG_BIAS_LEFT : SAG_BIAS_RIGHT);
            }
            for (op = 0U; op < 24U; op++) {
                prop_random_edit(&f, &rng);
                sag_cset_normalize(f.tb, &f.cursors);
                SAG_ASSERT(sag_is_grapheme_boundary(
                    f.tb, f.cursors.curs.data[f.cursors.primary].pos));
                for (mark_i = 0U; mark_i < SAG_ARRAY_LEN(marks); mark_i++)
                    SAG_ASSERT(sag_mark_pos(f.marks, marks[mark_i]).v <=
                               sag_textbuf_len(f.tb));
                sag_textbuf_check(f.tb);
            }
            prop_edit_free(&f);
        }
    }
}
