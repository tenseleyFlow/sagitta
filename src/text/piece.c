#include "text/piece.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/coords_internal.h"
#include "util/log.h"

static u64 node_bytes(const PieceNode *node)
{
    return node != NULL ? node->sub_bytes : 0;
}

static u64 node_lfs(const PieceNode *node)
{
    return node != NULL ? node->sub_lfs : 0;
}

static u32 node_count(const PieceNode *node)
{
    return node != NULL ? node->sub_count : 0;
}

static u64 node_weight(const PieceNode *node)
{
    return (u64)node_count(node) + 1U;
}

static void node_ref(PieceNode *node)
{
    if (node == NULL)
        return;
    if (node->refs == UINT32_MAX)
        YEW_BUG("piece node reference count overflow");
    node->refs++;
}

static void node_fix(PieceNode *node)
{
    u64 piece_len = node->span.hi - node->span.lo;
    u64 bytes = node_bytes(node->left);
    u64 lfs = node_lfs(node->left);
    u64 count = (u64)node_count(node->left) + 1U;

    if (bytes > UINT64_MAX - piece_len ||
        bytes + piece_len > UINT64_MAX - node_bytes(node->right))
        YEW_BUG("piece tree byte count overflow");
    if (lfs > UINT64_MAX - node->lf_count ||
        lfs + node->lf_count > UINT64_MAX - node_lfs(node->right))
        YEW_BUG("piece tree newline count overflow");
    count += node_count(node->right);
    if (count > UINT32_MAX)
        YEW_BUG("piece tree node count overflow");
    node->sub_bytes = bytes + piece_len + node_bytes(node->right);
    node->sub_lfs = lfs + node->lf_count + node_lfs(node->right);
    node->sub_count = (u32)count;
}

static PieceNode *node_new(Piece piece)
{
    PieceNode *node = yew_xmalloc(sizeof(*node));

    node->left = NULL;
    node->right = NULL;
    node->span = piece.span;
    node->sub_bytes = piece.span.hi - piece.span.lo;
    node->sub_lfs = piece.lf_count;
    node->lf_count = piece.lf_count;
    node->sub_count = 1U;
    node->refs = 1U;
    node->src = piece.src;
    node->lf_first = piece.lf_first;
    return node;
}

/* Consumes one reference and returns a uniquely-owned replacement. */
static PieceNode *node_own(PieceNode *node)
{
    PieceNode *copy;

    if (node == NULL || node->refs == 1U)
        return node;
    copy = yew_xmalloc(sizeof(*copy));
    *copy = *node;
    copy->refs = 1U;
    node_ref(copy->left);
    node_ref(copy->right);
    node->refs--;
    return copy;
}

static void node_release(PieceNode *root)
{
    PieceNode *stack[YEW_PIECE_MAX_DEPTH * 2U];
    u32 depth = 0;

    if (root == NULL)
        return;
    stack[depth++] = root;
    while (depth != 0U) {
        PieceNode *node = stack[--depth];
        PieceNode *left;
        PieceNode *right;

        if (node->refs == 0U)
            YEW_BUG("piece node reference count underflow");
        node->refs--;
        if (node->refs != 0U)
            continue;
        left = node->left;
        right = node->right;
        free(node);
        if (left != NULL) {
            if (depth == YEW_ARRAY_LEN(stack))
                YEW_BUG("piece release stack overflow");
            stack[depth++] = left;
        }
        if (right != NULL) {
            if (depth == YEW_ARRAY_LEN(stack))
                YEW_BUG("piece release stack overflow");
            stack[depth++] = right;
        }
    }
}

static PieceNode *rotate_left(PieceNode *node)
{
    PieceNode *right = node_own(node->right);

    if (right == NULL)
        YEW_BUG("piece tree left rotation without right child");
    node->right = right->left;
    right->left = node;
    node_fix(node);
    node_fix(right);
    return right;
}

static PieceNode *rotate_right(PieceNode *node)
{
    PieceNode *left = node_own(node->left);

    if (left == NULL)
        YEW_BUG("piece tree right rotation without left child");
    node->left = left->right;
    left->right = node;
    node_fix(node);
    node_fix(left);
    return left;
}

static PieceNode *rotate_left_double(PieceNode *node)
{
    PieceNode *right = node_own(node->right);
    PieceNode *middle;

    if (right == NULL)
        YEW_BUG("piece tree double rotation without right child");
    middle = node_own(right->left);
    if (middle == NULL)
        YEW_BUG("piece tree double rotation without middle child");
    node->right = middle->left;
    right->left = middle->right;
    middle->left = node;
    middle->right = right;
    node_fix(node);
    node_fix(right);
    node_fix(middle);
    return middle;
}

static PieceNode *rotate_right_double(PieceNode *node)
{
    PieceNode *left = node_own(node->left);
    PieceNode *middle;

    if (left == NULL)
        YEW_BUG("piece tree double rotation without left child");
    middle = node_own(left->right);
    if (middle == NULL)
        YEW_BUG("piece tree double rotation without middle child");
    node->left = middle->right;
    left->right = middle->left;
    middle->right = node;
    middle->left = left;
    node_fix(node);
    node_fix(left);
    node_fix(middle);
    return middle;
}

static PieceNode *node_balance(PieceNode *node)
{
    u64 wl;
    u64 wr;

    if (node == NULL)
        return NULL;
    wl = node_weight(node->left);
    wr = node_weight(node->right);
    if (wr > 3U * wl) {
        PieceNode *right = node->right;
        if (right != NULL &&
            (u64)node_weight(right->left) >=
                2U * (u64)node_weight(right->right))
            return rotate_left_double(node);
        return rotate_left(node);
    }
    if (wl > 3U * wr) {
        PieceNode *left = node->left;
        if (left != NULL &&
            (u64)node_weight(left->right) >=
                2U * (u64)node_weight(left->left))
            return rotate_right_double(node);
        return rotate_right(node);
    }
    return node;
}

/* node is a singleton; left/node/right are consumed. */
static PieceNode *node_link(PieceNode *node, PieceNode *left,
                            PieceNode *right)
{
    u64 wl = node_weight(left);
    u64 wr = node_weight(right);

    if (wl > 3U * wr) {
        left = node_own(left);
        left->right = node_link(node, left->right, right);
        node_fix(left);
        return node_balance(left);
    }
    if (wr > 3U * wl) {
        right = node_own(right);
        right->left = node_link(node, left, right->left);
        node_fix(right);
        return node_balance(right);
    }
    node->left = left;
    node->right = right;
    node_fix(node);
    return node;
}

/* Consumes root and returns its minimum singleton plus the remaining tree. */
static PieceNode *node_take_min(PieceNode *root, PieceNode **rest)
{
    PieceNode *minimum;

    root = node_own(root);
    if (root->left == NULL) {
        *rest = root->right;
        root->right = NULL;
        node_fix(root);
        return root;
    }
    minimum = node_take_min(root->left, &root->left);
    node_fix(root);
    *rest = node_balance(root);
    return minimum;
}

/* Consumes root and returns its maximum singleton plus the remaining tree. */
static PieceNode *node_take_max(PieceNode *root, PieceNode **rest)
{
    PieceNode *maximum;

    root = node_own(root);
    if (root->right == NULL) {
        *rest = root->left;
        root->left = NULL;
        node_fix(root);
        return root;
    }
    maximum = node_take_max(root->right, &root->right);
    node_fix(root);
    *rest = node_balance(root);
    return maximum;
}

/* Consumes both trees. Every byte in left precedes every byte in right. */
static PieceNode *node_concat(PieceNode *left, PieceNode *right)
{
    const PieceNode *left_edge;
    const PieceNode *right_edge;
    PieceNode *middle;

    if (left == NULL)
        return right;
    if (right == NULL)
        return left;
    left_edge = left;
    while (left_edge->right != NULL)
        left_edge = left_edge->right;
    right_edge = right;
    while (right_edge->left != NULL)
        right_edge = right_edge->left;
    if (left_edge->src == right_edge->src &&
        left_edge->span.hi == right_edge->span.lo) {
        PieceNode *left_rest;
        PieceNode *right_rest;
        PieceNode *successor;

        middle = node_take_max(left, &left_rest);
        successor = node_take_min(right, &right_rest);
        if (middle->lf_count > UINT64_MAX - successor->lf_count)
            YEW_BUG("coalesced piece newline count overflow");
        middle->span.hi = successor->span.hi;
        middle->lf_count += successor->lf_count;
        node_fix(middle);
        node_release(successor);
        return node_link(middle, left_rest, right_rest);
    }
    middle = node_take_min(right, &right);
    return node_link(middle, left, right);
}

static u64 store_lower_bound(const TextStore *store, u64 off)
{
    u64 lo = 0;
    u64 hi = (u64)store->lfs.len;

    while (lo < hi) {
        u64 mid = lo + (hi - lo) / 2U;
        if (store->lfs.data[(size_t)mid] < off)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo;
}

static const TextStore *text_store(const TextBacking *backing, u8 src)
{
    if (src == YEW_STORE_ORIG)
        return &backing->orig;
    if (src == YEW_STORE_ADD)
        return &backing->add;
    YEW_BUG("piece references unknown store %u", (unsigned)src);
}

static Piece piece_make(const TextBacking *backing, u8 src, Span span)
{
    const TextStore *store = text_store(backing, src);
    Piece piece;
    u64 end;

    if (span.lo >= span.hi || span.hi > store->len)
        YEW_BUG("invalid piece span [%llu,%llu) for store length %llu",
                (unsigned long long)span.lo, (unsigned long long)span.hi,
                (unsigned long long)store->len);
    piece.src = src;
    piece.span = span;
    piece.lf_first = store_lower_bound(store, span.lo);
    end = store_lower_bound(store, span.hi);
    piece.lf_count = end - piece.lf_first;
    return piece;
}

static void node_make_singleton(PieceNode *node)
{
    node->left = NULL;
    node->right = NULL;
    node_fix(node);
}

/* Consumes root and partitions it at a byte seam, splitting one piece. */
static void node_split(TextBuf *tb, PieceNode *root, u64 at,
                       PieceNode **left, PieceNode **right)
{
    u64 left_bytes;
    u64 piece_len;
    PieceNode *a;
    PieceNode *b;

    if (root == NULL) {
        if (at != 0U)
            YEW_BUG("piece split offset outside empty tree");
        *left = NULL;
        *right = NULL;
        return;
    }
    if (at > root->sub_bytes)
        YEW_BUG("piece split offset outside tree");
    root = node_own(root);
    left_bytes = node_bytes(root->left);
    piece_len = root->span.hi - root->span.lo;
    if (at < left_bytes) {
        PieceNode *old_right = root->right;

        node_split(tb, root->left, at, left, &a);
        node_make_singleton(root);
        *right = node_link(root, a, old_right);
        return;
    }
    if (at > left_bytes + piece_len) {
        PieceNode *old_left = root->left;

        node_split(tb, root->right, at - left_bytes - piece_len, &b,
                   right);
        node_make_singleton(root);
        *left = node_link(root, old_left, b);
        return;
    }
    a = root->left;
    b = root->right;
    node_make_singleton(root);
    if (at == left_bytes) {
        *left = a;
        *right = node_link(root, NULL, b);
    } else if (at == left_bytes + piece_len) {
        *left = node_link(root, a, NULL);
        *right = b;
    } else {
        u64 split = root->span.lo + at - left_bytes;
        Piece tail = piece_make(tb->backing, root->src,
                                (Span){split, root->span.hi});
        PieceNode *tail_node = node_new(tail);
        Piece head = piece_make(tb->backing, root->src,
                                (Span){root->span.lo, split});

        root->span = head.span;
        root->lf_first = head.lf_first;
        root->lf_count = head.lf_count;
        node_fix(root);
        *left = node_link(root, a, NULL);
        *right = node_link(tail_node, NULL, b);
    }
}

/* Consumes root and inserted; inserts one piece at the byte seam. */
static PieceNode *node_insert(TextBuf *tb, PieceNode *root, u64 at,
                              PieceNode *inserted)
{
    u64 left_bytes;
    u64 piece_len;

    if (root == NULL) {
        if (at != 0U)
            YEW_BUG("piece insert offset outside empty tree");
        return inserted;
    }
    if (at > root->sub_bytes)
        YEW_BUG("piece insert offset outside tree");
    root = node_own(root);
    left_bytes = node_bytes(root->left);
    piece_len = root->span.hi - root->span.lo;
    if (at <= left_bytes) {
        root->left = node_insert(tb, root->left, at, inserted);
        node_fix(root);
        return node_balance(root);
    }
    if (at >= left_bytes + piece_len) {
        root->right = node_insert(tb, root->right,
                                  at - left_bytes - piece_len, inserted);
        node_fix(root);
        return node_balance(root);
    }
    {
        PieceNode *old_left = root->left;
        PieceNode *old_right = root->right;
        u64 split = root->span.lo + at - left_bytes;
        Piece tail = piece_make(tb->backing, root->src,
                                (Span){split, root->span.hi});
        Piece head = piece_make(tb->backing, root->src,
                                (Span){root->span.lo, split});
        PieceNode *tail_tree;
        PieceNode *inserted_tree;

        root->span = head.span;
        root->lf_first = head.lf_first;
        root->lf_count = head.lf_count;
        node_make_singleton(root);
        tail_tree = node_link(node_new(tail), NULL, old_right);
        inserted_tree = node_link(inserted, NULL, tail_tree);
        return node_link(root, old_left, inserted_tree);
    }
}

static void store_append(TextStore *store, const u8 *bytes, u64 len)
{
    u64 old_len = store->len;
    u64 need;
    u64 cap;
    u64 i;

    if (len == 0U)
        return;
    if (bytes == NULL)
        YEW_BUG("text store append with NULL bytes");
    if (len > UINT64_MAX - store->len)
        YEW_BUG("text store length overflow");
    need = store->len + len;
    if (need > SIZE_MAX)
        YEW_BUG("text store exceeds addressable memory");
    if (need > store->cap) {
        cap = store->cap != 0U ? store->cap : 64U;
        while (cap < need) {
            if (cap > (u64)SIZE_MAX / 2U) {
                cap = need;
                break;
            }
            cap *= 2U;
        }
        store->bytes = yew_xrealloc(store->bytes, (size_t)cap);
        store->cap = cap;
    }
    memcpy(store->bytes + (size_t)old_len, bytes, (size_t)len);
    store->len = need;
    for (i = 0U; i < len; i++) {
        if (bytes[(size_t)i] == (u8)'\n')
            YewU64Vec_push(&store->lfs, old_len + i);
    }
}

static void store_index_original(TextStore *store)
{
    size_t size = (size_t)store->len;
    size_t pos = 0U;

    while (pos < size) {
        const u8 *lf = memchr(store->bytes + pos, '\n', size - pos);
        size_t off;

        if (lf == NULL)
            break;
        off = (size_t)(lf - store->bytes);
        YewU64Vec_push(&store->lfs, (u64)off);
        pos = off + 1U;
        /* memchr is ideal for ordinary sparse newlines, but calling it
         * once per byte makes the all-newline budget fixture needlessly
         * quadratic in call overhead. Consume the adjacent dense run in
         * one linear pass, then resume the memchr sweep. */
        while (pos < size && store->bytes[pos] == (u8)'\n') {
            YewU64Vec_push(&store->lfs, (u64)pos);
            pos++;
        }
    }
}

static void store_init_original(TextStore *store, const u8 *bytes, u64 len)
{
    if (len == 0U)
        return;
    if (bytes == NULL)
        YEW_BUG("original text store initialized with NULL bytes");
    if (len > SIZE_MAX)
        YEW_BUG("original text store exceeds addressable memory");
    store->bytes = yew_xmalloc((size_t)len);
    memcpy(store->bytes, bytes, (size_t)len);
    store->len = len;
    store->cap = len;
    store_index_original(store);
}

static void store_init_original_owned(TextStore *store, u8 *bytes, u64 len)
{
    if (len > SIZE_MAX)
        YEW_BUG("owned original text store exceeds addressable memory");
    if (len != 0U && bytes == NULL)
        YEW_BUG("owned original text store initialized with NULL bytes");
    if (len == 0U) {
        free(bytes);
        return;
    }
    store->bytes = bytes;
    store->len = len;
    store->cap = len;
    store_index_original(store);
}

static void store_free(TextStore *store)
{
    free(store->bytes);
    YewU64Vec_free(&store->lfs);
    memset(store, 0, sizeof(*store));
}

static TextBacking *backing_new(void)
{
    TextBacking *backing = yew_xcalloc(1U, sizeof(*backing));

    backing->refs = 1U;
    return backing;
}

static void backing_ref(TextBacking *backing)
{
    if (backing == NULL || backing->refs == 0U)
        YEW_BUG("invalid text backing reference");
    if (backing->refs == UINT32_MAX)
        YEW_BUG("text backing reference count overflow");
    backing->refs++;
}

static void backing_release(TextBacking *backing)
{
    if (backing == NULL || backing->refs == 0U)
        YEW_BUG("text backing reference count underflow");
    backing->refs--;
    if (backing->refs != 0U)
        return;
    store_free(&backing->orig);
    store_free(&backing->add);
    free(backing);
}

static void textbuf_sync_store_views(TextBuf *tb)
{
    tb->orig = tb->backing->orig;
    tb->add = tb->backing->add;
}

static void textbuf_require_edit_generation(const TextBuf *tb)
{
    if (tb->gen == UINT64_MAX)
        YEW_BUG("text buffer generation overflow");
}

TextBuf *yew_textbuf_new(void)
{
    TextBuf *tb = yew_xcalloc(1U, sizeof(*tb));

    tb->backing = backing_new();
    textbuf_sync_store_views(tb);
    yew_coords_index_seed(tb);
    return tb;
}

TextBuf *yew_textbuf_from_bytes(const u8 *bytes, u64 len)
{
    TextBuf *tb = yew_textbuf_new();

    if (len != 0U) {
        Piece piece;

        store_init_original(&tb->backing->orig, bytes, len);
        textbuf_sync_store_views(tb);
        piece = piece_make(tb->backing, YEW_STORE_ORIG, (Span){0U, len});
        tb->root = node_new(piece);
    }
    yew_coords_index_seed(tb);
    return tb;
}

TextBuf *yew_textbuf_from_owned_bytes(u8 *bytes, u64 len)
{
    TextBuf *tb = yew_textbuf_new();

    store_init_original_owned(&tb->backing->orig, bytes, len);
    textbuf_sync_store_views(tb);
    if (len != 0U) {
        Piece piece = piece_make(tb->backing, YEW_STORE_ORIG,
                                 (Span){0U, len});

        tb->root = node_new(piece);
    }
    yew_coords_index_seed(tb);
    return tb;
}

void yew_textbuf_free(TextBuf *tb)
{
    if (tb == NULL)
        return;
    yew_coords_index_dispose(tb);
    node_release(tb->root);
    backing_release(tb->backing);
    free(tb);
}

u64 yew_textbuf_len(const TextBuf *tb)
{
    if (tb == NULL)
        YEW_BUG("yew_textbuf_len: NULL buffer");
    return node_bytes(tb->root);
}

u64 yew_textbuf_line_count(const TextBuf *tb)
{
    if (tb == NULL)
        YEW_BUG("yew_textbuf_line_count: NULL buffer");
    if (node_lfs(tb->root) == UINT64_MAX)
        YEW_BUG("text buffer line count overflow");
    return node_lfs(tb->root) + 1U;
}

u32 yew_textbuf_piece_count(const TextBuf *tb)
{
    if (tb == NULL)
        YEW_BUG("yew_textbuf_piece_count: NULL buffer");
    return node_count(tb->root);
}

static bool node_extend_predecessor(TextBuf *tb, PieceNode **slot, u64 at,
                                    u64 old_add_len, u64 new_add_len)
{
    PieceNode *node = *slot;
    u64 left_bytes;
    u64 piece_len;
    bool changed;

    if (node == NULL)
        return false;
    node = node_own(node);
    *slot = node;
    left_bytes = node_bytes(node->left);
    piece_len = node->span.hi - node->span.lo;
    if (at <= left_bytes) {
        changed = node_extend_predecessor(tb, &node->left, at,
                                          old_add_len, new_add_len);
    } else if (at == left_bytes + piece_len) {
        changed = node->src == YEW_STORE_ADD &&
                  node->span.hi == old_add_len;
        if (changed) {
            Piece piece = piece_make(tb->backing, YEW_STORE_ADD,
                                     (Span){node->span.lo, new_add_len});
            node->span = piece.span;
            node->lf_first = piece.lf_first;
            node->lf_count = piece.lf_count;
        }
    } else if (at > left_bytes + piece_len) {
        changed = node_extend_predecessor(tb, &node->right,
                                          at - left_bytes - piece_len,
                                          old_add_len, new_add_len);
    } else {
        changed = false;
    }
    if (changed)
        node_fix(node);
    return changed;
}

static const PieceNode *node_ending_at(const PieceNode *node, u64 at)
{
    while (node != NULL) {
        u64 left_bytes = node_bytes(node->left);
        u64 piece_len = node->span.hi - node->span.lo;

        if (at <= left_bytes) {
            node = node->left;
        } else if (at == left_bytes + piece_len) {
            return node;
        } else if (at < left_bytes + piece_len) {
            return NULL;
        } else {
            at -= left_bytes + piece_len;
            node = node->right;
        }
    }
    return NULL;
}

static bool payload_aliases_store(const u8 *bytes, u64 len,
                                  const TextStore *store)
{
    uintptr_t start;
    uintptr_t base;
    uintptr_t end;

    if (bytes == NULL || store->bytes == NULL || store->len == 0U)
        return false;
    start = (uintptr_t)bytes;
    base = (uintptr_t)store->bytes;
    if (store->len > UINTPTR_MAX - base)
        YEW_BUG("text store address range overflow");
    end = base + (uintptr_t)store->len;
    if (start < base || start >= end)
        return false;
    if (len > (u64)(end - start))
        YEW_BUG("insert payload extends beyond its backing store");
    return true;
}

void yew_textbuf_insert(TextBuf *tb, ByteOff at, const u8 *bytes, u64 len)
{
    u64 buffer_len;
    u64 old_add_len;
    u64 old_gen;
    Span old_affected;
    PieceNode *middle;
    u8 *staged = NULL;
    const u8 *payload = bytes;

    if (tb == NULL)
        YEW_BUG("yew_textbuf_insert: NULL buffer");
    buffer_len = node_bytes(tb->root);
    if (at.v > buffer_len)
        YEW_BUG("insert offset %llu beyond buffer length %llu",
                (unsigned long long)at.v,
                (unsigned long long)buffer_len);
    if (len == 0U)
        return;
    if (len > UINT64_MAX - buffer_len)
        YEW_BUG("insert length overflows text buffer");
    textbuf_require_edit_generation(tb);
    old_gen = tb->gen;
    old_affected = yew_textbuf_line_span(
        tb, yew_textbuf_line_of(tb, at));
    if (payload_aliases_store(bytes, len, &tb->backing->add)) {
        if (len > SIZE_MAX)
            YEW_BUG("insert payload exceeds addressable memory");
        staged = yew_xmalloc((size_t)len);
        memcpy(staged, bytes, (size_t)len);
        payload = staged;
    } else {
        (void)payload_aliases_store(bytes, len, &tb->backing->orig);
    }
    old_add_len = tb->backing->add.len;
    store_append(&tb->backing->add, payload, len);
    tb->add = tb->backing->add;
    free(staged);
    if ((!tb->add_tail_known || tb->add_tail_at == at.v) &&
        node_extend_predecessor(tb, &tb->root, at.v, old_add_len,
                                tb->backing->add.len)) {
        tb->gen++;
        yew_coords_index_note_edit(tb, (Span){at.v, at.v}, len,
                                   old_affected, old_gen);
        tb->add_tail_at = at.v + len;
        tb->add_tail_known = true;
        return;
    }
    middle = node_new(piece_make(tb->backing, YEW_STORE_ADD,
                                 (Span){old_add_len,
                                        tb->backing->add.len}));
    tb->root = node_insert(tb, tb->root, at.v, middle);
    tb->gen++;
    yew_coords_index_note_edit(tb, (Span){at.v, at.v}, len,
                               old_affected, old_gen);
    tb->add_tail_at = at.v + len;
    tb->add_tail_known = true;
}

void yew_textbuf_insert_span(TextBuf *tb, ByteOff at, u8 src, Span span)
{
    const TextStore *store;
    u64 buffer_len;
    u64 inserted_len;
    u64 old_gen;
    Span old_affected;
    PieceNode *before;
    PieceNode *after;
    PieceNode *middle;

    if (tb == NULL)
        YEW_BUG("yew_textbuf_insert_span: NULL buffer");
    buffer_len = node_bytes(tb->root);
    if (at.v > buffer_len)
        YEW_BUG("span insert offset %llu beyond buffer length %llu",
                (unsigned long long)at.v,
                (unsigned long long)buffer_len);
    store = text_store(tb->backing, src);
    if (span.lo > span.hi || span.hi > store->len)
        YEW_BUG("invalid inserted span [%llu,%llu) for store length %llu",
                (unsigned long long)span.lo,
                (unsigned long long)span.hi,
                (unsigned long long)store->len);
    inserted_len = span.hi - span.lo;
    if (inserted_len == 0U)
        return;
    if (inserted_len > UINT64_MAX - buffer_len)
        YEW_BUG("span insert length overflows text buffer");
    textbuf_require_edit_generation(tb);
    old_gen = tb->gen;
    old_affected = yew_textbuf_line_span(
        tb, yew_textbuf_line_of(tb, at));
    middle = node_new(piece_make(tb->backing, src, span));
    node_split(tb, tb->root, at.v, &before, &after);
    tb->root = node_concat(node_concat(before, middle), after);
    tb->gen++;
    yew_coords_index_note_edit(tb, (Span){at.v, at.v}, inserted_len,
                               old_affected, old_gen);
    tb->add_tail_known = false;
}

void yew_textbuf_delete(TextBuf *tb, Span range)
{
    PieceNode *before_end;
    PieceNode *after;
    PieceNode *before;
    PieceNode *removed;
    Span old_affected;
    LineNo first_line;
    LineNo last_line;
    u64 old_gen;
    u64 len;

    if (tb == NULL)
        YEW_BUG("yew_textbuf_delete: NULL buffer");
    len = yew_textbuf_len(tb);
    if (range.lo > range.hi || range.hi > len)
        YEW_BUG("delete range [%llu,%llu) beyond buffer length %llu",
                (unsigned long long)range.lo,
                (unsigned long long)range.hi,
                (unsigned long long)len);
    if (range.lo == range.hi)
        return;
    textbuf_require_edit_generation(tb);
    old_gen = tb->gen;
    first_line = yew_textbuf_line_of(tb, BYTEOFF(range.lo));
    last_line = yew_textbuf_line_of(tb, BYTEOFF(range.hi));
    old_affected.lo = yew_textbuf_line_start(tb, first_line).v;
    old_affected.hi = yew_textbuf_line_span(tb, last_line).hi;
    node_split(tb, tb->root, range.hi, &before_end, &after);
    node_split(tb, before_end, range.lo, &before, &removed);
    node_release(removed);
    tb->root = node_concat(before, after);
    tb->gen++;
    yew_coords_index_note_edit(tb, range, 0U, old_affected, old_gen);
    tb->add_tail_known = false;
}

ByteOff yew_textbuf_line_start(const TextBuf *tb, LineNo line)
{
    const PieceNode *node;
    u64 remaining;
    u64 before = 0U;

    if (tb == NULL)
        YEW_BUG("yew_textbuf_line_start: NULL buffer");
    if (line.v >= yew_textbuf_line_count(tb))
        YEW_BUG("line %llu outside buffer", (unsigned long long)line.v);
    if (line.v == 0U)
        return BYTEOFF(0U);
    node = tb->root;
    remaining = line.v;
    while (node != NULL) {
        u64 left_lfs = node_lfs(node->left);
        u64 left_bytes = node_bytes(node->left);

        if (remaining <= left_lfs) {
            node = node->left;
            continue;
        }
        before += left_bytes;
        remaining -= left_lfs;
        if (remaining <= node->lf_count) {
            const TextStore *store = text_store(tb->backing, node->src);
            u64 lf = store->lfs.data[(size_t)(node->lf_first +
                                               remaining - 1U)];
            return BYTEOFF(before + lf + 1U - node->span.lo);
        }
        remaining -= node->lf_count;
        before += node->span.hi - node->span.lo;
        node = node->right;
    }
    YEW_BUG("piece newline index is inconsistent");
}

LineNo yew_textbuf_line_of(const TextBuf *tb, ByteOff off)
{
    const PieceNode *node;
    u64 pos;
    u64 lines = 0U;

    if (tb == NULL)
        YEW_BUG("yew_textbuf_line_of: NULL buffer");
    if (off.v > yew_textbuf_len(tb))
        YEW_BUG("offset %llu outside buffer", (unsigned long long)off.v);
    node = tb->root;
    pos = off.v;
    while (node != NULL) {
        u64 left_bytes = node_bytes(node->left);
        u64 piece_len = node->span.hi - node->span.lo;

        if (pos < left_bytes) {
            node = node->left;
            continue;
        }
        if (pos <= left_bytes + piece_len) {
            const TextStore *store = text_store(tb->backing, node->src);
            u64 target = node->span.lo + pos - left_bytes;
            u64 first = node->lf_first;
            u64 end = first + node->lf_count;
            u64 lo = first;

            lines += node_lfs(node->left);
            while (lo < end) {
                u64 mid = lo + (end - lo) / 2U;
                if (store->lfs.data[(size_t)mid] < target)
                    lo = mid + 1U;
                else
                    end = mid;
            }
            return LINENO(lines + lo - first);
        }
        lines += node_lfs(node->left) + node->lf_count;
        pos -= left_bytes + piece_len;
        node = node->right;
    }
    return LINENO(lines);
}

Span yew_textbuf_line_span(const TextBuf *tb, LineNo line)
{
    u64 count = yew_textbuf_line_count(tb);
    ByteOff lo = yew_textbuf_line_start(tb, line);
    ByteOff hi;

    if (line.v + 1U < count)
        hi = yew_textbuf_line_start(tb, LINENO(line.v + 1U));
    else
        hi = BYTEOFF(yew_textbuf_len(tb));
    return (Span){lo.v, hi.v};
}

static bool textiter_seek(TextIter *it, const PieceNode *root, u64 len,
                          u64 gen, bool snapshot, const TextBuf *owner,
                          TextBacking *backing, ByteOff at)
{
    const PieceNode *node = root;
    u64 pos = at.v;

    memset(it, 0, sizeof(*it));
    it->owner = owner;
    it->backing = backing;
    it->gen = gen;
    it->snapshot = snapshot;
    if (at.v > len)
        YEW_BUG("iterator offset %llu outside text length %llu",
                (unsigned long long)at.v, (unsigned long long)len);
    if (at.v == len)
        return false;
    while (node != NULL) {
        u64 left_bytes;
        u64 piece_len;

        if (it->depth == YEW_PIECE_MAX_DEPTH)
            YEW_BUG("piece iterator stack overflow");
        it->stack[it->depth++] = node;
        left_bytes = node_bytes(node->left);
        piece_len = node->span.hi - node->span.lo;
        if (pos < left_bytes) {
            node = node->left;
        } else if (pos < left_bytes + piece_len) {
            it->skip = pos - left_bytes;
            return true;
        } else {
            pos -= left_bytes + piece_len;
            node = node->right;
        }
    }
    YEW_BUG("piece iterator could not resolve a valid offset");
}

static void textiter_validate(const TextIter *it, const TextBuf *tb)
{
    if (it == NULL)
        YEW_BUG("piece iterator called with NULL iterator");
    if (it->snapshot) {
        if (it->backing == NULL)
            YEW_BUG("snapshot iterator has no backing store");
        if (tb != NULL && tb->backing != it->backing)
            YEW_BUG("snapshot iterator used with a different text buffer");
        return;
    }
    if (tb == NULL || it->owner != tb || it->backing != tb->backing)
        YEW_BUG("live iterator used with a different text buffer");
    if (it->gen != tb->gen)
        YEW_BUG("piece iterator invalidated by edit");
}

bool yew_textiter_begin(TextIter *it, const TextBuf *tb, ByteOff at)
{
    if (it == NULL || tb == NULL)
        YEW_BUG("yew_textiter_begin: NULL argument");
    return textiter_seek(it, tb->root, yew_textbuf_len(tb), tb->gen, false,
                         tb, tb->backing, at);
}

bool yew_textiter_chunk(TextIter *it, const TextBuf *tb,
                        const u8 **bytes, u64 *len)
{
    const PieceNode *node;
    const TextStore *store;
    u64 piece_len;

    textiter_validate(it, tb);
    if (bytes == NULL || len == NULL)
        YEW_BUG("yew_textiter_chunk: NULL output");
    if (it->depth == 0U) {
        *bytes = NULL;
        *len = 0U;
        return false;
    }
    node = it->stack[it->depth - 1U];
    piece_len = node->span.hi - node->span.lo;
    if (it->skip >= piece_len)
        YEW_BUG("piece iterator skip outside current piece");
    store = text_store(it->backing, node->src);
    *bytes = store->bytes + (size_t)(node->span.lo + it->skip);
    *len = piece_len - it->skip;
    return true;
}

bool yew_textiter_advance(TextIter *it, const TextBuf *tb)
{
    const PieceNode *node;
    const PieceNode *child;

    textiter_validate(it, tb);
    if (it->depth == 0U)
        return false;
    node = it->stack[it->depth - 1U];
    it->skip = 0U;
    if (node->right != NULL) {
        node = node->right;
        for (;;) {
            if (it->depth == YEW_PIECE_MAX_DEPTH)
                YEW_BUG("piece iterator stack overflow");
            it->stack[it->depth++] = node;
            if (node->left == NULL)
                return true;
            node = node->left;
        }
    }
    child = node;
    it->depth--;
    while (it->depth != 0U) {
        node = it->stack[it->depth - 1U];
        if (node->left == child)
            return true;
        child = node;
        it->depth--;
    }
    return false;
}

bool yew_lineiter_begin(LineIter *it, const TextBuf *tb, LineNo at)
{
    if (it == NULL || tb == NULL)
        YEW_BUG("yew_lineiter_begin: NULL argument");
    if (at.v > yew_textbuf_line_count(tb))
        YEW_BUG("line iterator starts outside buffer");
    it->owner = tb;
    it->next_line = at.v;
    it->gen = tb->gen;
    return at.v < yew_textbuf_line_count(tb);
}

bool yew_lineiter_next(LineIter *it, const TextBuf *tb, Span *line)
{
    if (it == NULL || tb == NULL || line == NULL)
        YEW_BUG("yew_lineiter_next: NULL argument");
    if (it->owner != tb)
        YEW_BUG("line iterator used with a different text buffer");
    if (it->gen != tb->gen)
        YEW_BUG("line iterator invalidated by edit");
    if (it->next_line >= yew_textbuf_line_count(tb))
        return false;
    *line = yew_textbuf_line_span(tb, LINENO(it->next_line));
    it->next_line++;
    return true;
}

TextSnap yew_textbuf_snap(TextBuf *tb)
{
    TextSnap snap;

    if (tb == NULL)
        YEW_BUG("yew_textbuf_snap: NULL buffer");
    if (tb->backing == NULL || tb->backing->refs == UINT32_MAX ||
        (tb->root != NULL && tb->root->refs == UINT32_MAX))
        YEW_BUG("text snapshot reference count overflow");
    node_ref(tb->root);
    backing_ref(tb->backing);
    snap.root = tb->root;
    snap.backing = tb->backing;
    snap.len = yew_textbuf_len(tb);
    snap.gen = tb->gen;
    snap.active = true;
    return snap;
}

void yew_textsnap_release(TextBuf *tb, TextSnap *snap)
{
    if (snap == NULL)
        YEW_BUG("yew_textsnap_release: NULL snapshot");
    if (!snap->active)
        YEW_BUG("yew_textsnap_release: snapshot already released");
    if (snap->backing == NULL)
        YEW_BUG("yew_textsnap_release: snapshot has no backing store");
    if (tb != NULL && snap->backing != tb->backing)
        YEW_BUG("yew_textsnap_release: snapshot belongs to another buffer");
    node_release(snap->root);
    backing_release(snap->backing);
    memset(snap, 0, sizeof(*snap));
}

bool yew_textsnap_iter(TextIter *it, const TextSnap *snap, ByteOff at)
{
    if (it == NULL || snap == NULL)
        YEW_BUG("yew_textsnap_iter: NULL argument");
    if (!snap->active || snap->backing == NULL)
        YEW_BUG("yew_textsnap_iter: released snapshot");
    return textiter_seek(it, snap->root, snap->len, snap->gen, true,
                         NULL, snap->backing, at);
}

typedef struct {
    u64 bytes;
    u64 lfs;
    u32 count;
} CheckTotals;

static CheckTotals node_check(const TextBuf *tb, const PieceNode *node,
                              u32 depth)
{
    CheckTotals left;
    CheckTotals right;
    CheckTotals total;
    const TextStore *store;
    u64 first;
    u64 end;
    u64 piece_len;
    u64 wl;
    u64 wr;

    if (node == NULL)
        return (CheckTotals){0U, 0U, 0U};
    if (depth >= YEW_PIECE_MAX_DEPTH)
        YEW_BUG("piece tree exceeds maximum depth");
    if (node->refs == 0U)
        YEW_BUG("piece tree contains unreferenced node");
    if (node->src != YEW_STORE_ORIG && node->src != YEW_STORE_ADD)
        YEW_BUG("piece tree contains invalid store id");
    store = text_store(tb->backing, node->src);
    if (node->span.lo >= node->span.hi || node->span.hi > store->len)
        YEW_BUG("piece tree contains invalid or empty span");
    first = store_lower_bound(store, node->span.lo);
    end = store_lower_bound(store, node->span.hi);
    if (node->lf_first != first || node->lf_count != end - first)
        YEW_BUG("piece tree newline metadata mismatch");
    left = node_check(tb, node->left, depth + 1U);
    right = node_check(tb, node->right, depth + 1U);
    piece_len = node->span.hi - node->span.lo;
    if (left.bytes > UINT64_MAX - piece_len ||
        left.bytes + piece_len > UINT64_MAX - right.bytes)
        YEW_BUG("piece checker byte count overflow");
    if (left.lfs > UINT64_MAX - node->lf_count ||
        left.lfs + node->lf_count > UINT64_MAX - right.lfs)
        YEW_BUG("piece checker newline count overflow");
    if ((u64)left.count + 1U + right.count > UINT32_MAX)
        YEW_BUG("piece checker node count overflow");
    total.bytes = left.bytes + piece_len + right.bytes;
    total.lfs = left.lfs + node->lf_count + right.lfs;
    total.count = left.count + 1U + right.count;
    if (node->sub_bytes != total.bytes || node->sub_lfs != total.lfs ||
        node->sub_count != total.count)
        YEW_BUG("piece tree subtree metadata mismatch");
    wl = (u64)left.count + 1U;
    wr = (u64)right.count + 1U;
    if (wl > 3U * wr || wr > 3U * wl)
        YEW_BUG("piece tree weight-balance invariant violated");
    return total;
}

static void node_check_canonical(const PieceNode *root)
{
    const PieceNode *stack[YEW_PIECE_MAX_DEPTH];
    const PieceNode *node = root;
    const PieceNode *previous = NULL;
    u32 depth = 0U;

    while (node != NULL || depth != 0U) {
        while (node != NULL) {
            if (depth == YEW_PIECE_MAX_DEPTH)
                YEW_BUG("piece canonical-check stack overflow");
            stack[depth++] = node;
            node = node->left;
        }
        node = stack[--depth];
        if (previous != NULL && previous->src == node->src &&
            previous->span.hi == node->span.lo)
            YEW_BUG("piece tree contains coalescible adjacent spans");
        previous = node;
        node = node->right;
    }
}

void yew_textbuf_check(const TextBuf *tb)
{
    const PieceNode *add_tail;

    if (tb == NULL)
        YEW_BUG("yew_textbuf_check: NULL buffer");
    if (tb->backing == NULL || tb->backing->refs == 0U)
        YEW_BUG("text buffer has no live backing store");
    if (tb->orig.bytes != tb->backing->orig.bytes ||
        tb->orig.len != tb->backing->orig.len ||
        tb->orig.cap != tb->backing->orig.cap ||
        tb->orig.lfs.data != tb->backing->orig.lfs.data ||
        tb->orig.lfs.len != tb->backing->orig.lfs.len ||
        tb->orig.lfs.cap != tb->backing->orig.lfs.cap ||
        tb->add.bytes != tb->backing->add.bytes ||
        tb->add.len != tb->backing->add.len ||
        tb->add.cap != tb->backing->add.cap ||
        tb->add.lfs.data != tb->backing->add.lfs.data ||
        tb->add.lfs.len != tb->backing->add.lfs.len ||
        tb->add.lfs.cap != tb->backing->add.lfs.cap)
        YEW_BUG("text buffer store views are stale");
    /* The store indexes are append-only and are constructed in ascending
     * order by store_index_original/store_append. Validate the live tree's
     * index windows below; rescanning every historical LF here would make
     * this structural check O(total edit history), not O(live pieces). */
    (void)node_check(tb, tb->root, 0U);
    node_check_canonical(tb->root);
    if (tb->add_tail_known) {
        if (tb->add_tail_at > node_bytes(tb->root))
            YEW_BUG("text buffer add-tail position is outside the tree");
        add_tail = node_ending_at(tb->root, tb->add_tail_at);
        if (add_tail == NULL || add_tail->src != YEW_STORE_ADD ||
            add_tail->span.hi != tb->backing->add.len)
            YEW_BUG("text buffer add-tail cache is stale");
    }
}
