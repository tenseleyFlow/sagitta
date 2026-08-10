#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text/edit.h"

enum {
    YEW_UNDO_FUZZ_MIN_ITERS = 10000,
    YEW_UNDO_FUZZ_MAX_TEXT = 128
};

typedef struct {
    u8 *bytes;
    size_t len;
    u32 parent;
    u32 redo_child;
    bool present;
} OracleNode;

typedef struct {
    OracleNode *data;
    size_t cap;
} Oracle;

typedef struct {
    TextBuf *tb;
    UndoTree *undo;
    EditCtx edit;
    Oracle oracle;
    u64 rng;
    u64 hash;
    size_t edits;
    size_t undos;
    size_t redos;
    size_t branches;
} Run;

static u64 random_next(Run *run)
{
    u64 x = run->rng;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    run->rng = x;
    return x * UINT64_C(2685821657736338717);
}

static size_t choose(Run *run, size_t limit)
{
    return limit == 0U ? 0U : (size_t)(random_next(run) % (u64)limit);
}

static bool parse_u64(const char *arg, const char *prefix, u64 *out)
{
    char *end;
    unsigned long long value;
    size_t len = strlen(prefix);

    if (strncmp(arg, prefix, len) != 0)
        return false;
    errno = 0;
    value = strtoull(arg + len, &end, 0);
    if (errno != 0 || end == arg + len || *end != '\0')
        return false;
    *out = (u64)value;
    return true;
}

static bool parse_size(const char *arg, const char *prefix, size_t *out)
{
    u64 value;

    if (!parse_u64(arg, prefix, &value) || value > SIZE_MAX)
        return false;
    *out = (size_t)value;
    return true;
}

static bool oracle_reserve(Oracle *oracle, u32 id)
{
    size_t cap;
    OracleNode *data;

    if ((size_t)id < oracle->cap)
        return true;
    cap = oracle->cap == 0U ? 16U : oracle->cap;
    while (cap <= (size_t)id) {
        if (cap > SIZE_MAX / 2U)
            return false;
        cap *= 2U;
    }
    data = realloc(oracle->data, cap * sizeof(*data));
    if (data == NULL)
        return false;
    (void)memset(data + oracle->cap, 0,
                 (cap - oracle->cap) * sizeof(*data));
    oracle->data = data;
    oracle->cap = cap;
    return true;
}

static bool oracle_store(Oracle *oracle, u32 id, u32 parent,
                         const u8 *bytes, size_t len)
{
    u8 *copy = malloc(len == 0U ? 1U : len);

    if (copy == NULL || !oracle_reserve(oracle, id)) {
        free(copy);
        return false;
    }
    if (len != 0U)
        (void)memcpy(copy, bytes, len);
    free(oracle->data[id].bytes);
    oracle->data[id].bytes = copy;
    oracle->data[id].len = len;
    oracle->data[id].parent = parent;
    oracle->data[id].redo_child = 0U;
    oracle->data[id].present = true;
    return true;
}

static bool oracle_note_jump(Oracle *oracle, u32 from, u32 target)
{
    bool *target_path;
    u32 lca;
    u32 id;

    target_path = calloc(oracle->cap, sizeof(*target_path));
    if (target_path == NULL)
        return false;
    for (id = target; id != 0U; id = oracle->data[id].parent)
        target_path[id] = true;
    for (lca = from; !target_path[lca]; lca = oracle->data[lca].parent)
        ;
    for (id = from; id != lca; id = oracle->data[id].parent)
        oracle->data[oracle->data[id].parent].redo_child = id;
    for (id = target; id != lca; id = oracle->data[id].parent)
        oracle->data[oracle->data[id].parent].redo_child = id;
    free(target_path);
    return true;
}

static bool text_matches(const TextBuf *tb, const OracleNode *want)
{
    TextIter iter;
    size_t at = 0U;

    if (yew_textbuf_len(tb) != want->len)
        return false;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(0U)))
        return want->len == 0U;
    do {
        const u8 *bytes;
        u64 len;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &len) ||
            len > want->len - at ||
            memcmp(bytes, want->bytes + at, (size_t)len) != 0)
            return false;
        at += (size_t)len;
    } while (yew_textiter_advance(&iter, tb));
    return at == want->len;
}

static void hash_state(Run *run, u32 id)
{
    const OracleNode *node = &run->oracle.data[id];
    size_t i;

    run->hash ^= id;
    run->hash *= UINT64_C(1099511628211);
    for (i = 0U; i < node->len; i++) {
        run->hash ^= node->bytes[i];
        run->hash *= UINT64_C(1099511628211);
    }
}

static bool check_current(Run *run, size_t op)
{
    u32 id = yew_undo_current(run->undo);

    if ((size_t)id >= run->oracle.cap || !run->oracle.data[id].present ||
        !text_matches(run->tb, &run->oracle.data[id])) {
        (void)fprintf(stderr,
                      "fuzz_undo: state mismatch seed=%llu op=%zu node=%u\n",
                      (unsigned long long)run->rng, op, id);
        return false;
    }
    yew_textbuf_check(run->tb);
    hash_state(run, id);
    return true;
}

static bool edit_once(Run *run)
{
    u32 parent = yew_undo_current(run->undo);
    const OracleNode *before = &run->oracle.data[parent];
    size_t at;
    size_t len;
    u8 inserted[4];
    u8 *after;
    u32 id;
    size_t i;
    bool insert = before->len == 0U ||
                  (before->len < YEW_UNDO_FUZZ_MAX_TEXT &&
                   choose(run, 2U) == 0U);

    if (insert) {
        len = 1U + choose(run, sizeof(inserted));
        if (len > YEW_UNDO_FUZZ_MAX_TEXT - before->len)
            len = YEW_UNDO_FUZZ_MAX_TEXT - before->len;
        at = choose(run, before->len + 1U);
        after = malloc(before->len + len);
        if (after == NULL)
            return false;
        for (i = 0U; i < len; i++)
            inserted[i] = (u8)random_next(run);
        (void)memcpy(after, before->bytes, at);
        (void)memcpy(after + at, inserted, len);
        (void)memcpy(after + at + len, before->bytes + at,
                     before->len - at);
        yew_undo_begin(&run->edit, YEW_TXN_PASTE);
        yew_edit_insert(&run->edit, BYTEOFF(at), inserted, len);
        yew_undo_end(&run->edit);
    } else {
        at = choose(run, before->len);
        len = 1U + choose(run, before->len - at);
        after = malloc(before->len - len == 0U ? 1U : before->len - len);
        if (after == NULL)
            return false;
        (void)memcpy(after, before->bytes, at);
        (void)memcpy(after + at, before->bytes + at + len,
                     before->len - at - len);
        yew_undo_begin(&run->edit, YEW_TXN_CUT);
        yew_edit_delete(&run->edit, (Span){at, at + len});
        yew_undo_end(&run->edit);
    }
    id = yew_undo_current(run->undo);
    if (id == 0U || id == parent ||
        ((size_t)id < run->oracle.cap && run->oracle.data[id].present) ||
        (size_t)id > run->undo->nodes.len ||
        run->undo->nodes.data[id - 1U].parent != parent ||
        !oracle_store(&run->oracle, id, parent, after,
                      insert ? before->len + len : before->len - len)) {
        free(after);
        return false;
    }
    run->oracle.data[parent].redo_child = id;
    free(after);
    run->edits++;
    return true;
}

static bool undo_once(Run *run)
{
    u32 current = yew_undo_current(run->undo);
    u32 expected = run->oracle.data[current].parent;
    bool moved = yew_undo(&run->edit);

    if ((expected != 0U) != moved ||
        (moved && yew_undo_current(run->undo) != expected))
        return false;
    if (moved) {
        run->oracle.data[expected].redo_child = current;
        run->undos++;
    }
    return true;
}

static bool redo_once(Run *run)
{
    u32 current = yew_undo_current(run->undo);
    u32 expected = run->oracle.data[current].redo_child;
    bool moved = yew_redo(&run->edit);

    if ((expected != 0U) != moved ||
        (moved && yew_undo_current(run->undo) != expected))
        return false;
    if (moved)
        run->redos++;
    return true;
}

static bool branch_once(Run *run)
{
    u32 current = yew_undo_current(run->undo);
    u32 redo = run->oracle.data[current].redo_child;
    size_t child_count = 0U;
    size_t index = 0U;
    size_t selected_index;
    u32 expected = 0U;
    u32 id;
    i32 delta = choose(run, 2U) == 0U ? -1 : 1;
    u32 selected = yew_undo_branch_cycle(run->undo, delta);

    for (id = 1U; (size_t)id < run->oracle.cap; id++) {
        const OracleNode *node = &run->oracle.data[id];

        if (!node->present || node->parent != current)
            continue;
        if (id == redo)
            index = child_count;
        child_count++;
    }
    if (child_count == 0U)
        return selected == 0U;
    if (delta > 0)
        selected_index = (index + 1U) % child_count;
    else
        selected_index = (index + child_count - 1U) % child_count;
    index = 0U;
    for (id = 1U; (size_t)id < run->oracle.cap; id++) {
        const OracleNode *node = &run->oracle.data[id];

        if (!node->present || node->parent != current)
            continue;
        if (index == selected_index) {
            expected = id;
            break;
        }
        index++;
    }
    if (selected != expected)
        return false;
    run->oracle.data[current].redo_child = selected;
    if (child_count > 1U)
        run->branches++;
    return true;
}

static bool jump_once(Run *run)
{
    u32 max = run->undo->nodes.len;
    u32 from = yew_undo_current(run->undo);
    u32 target;

    if (max == 0U)
        return false;
    do {
        target = 1U + (u32)choose(run, max);
    } while ((size_t)target >= run->oracle.cap ||
             !run->oracle.data[target].present);
    if (!yew_undo_to(&run->edit, target) ||
        yew_undo_current(run->undo) != target)
        return false;
    return oracle_note_jump(&run->oracle, from, target);
}

static bool initialize(Run *run, u64 seed)
{
    run->tb = yew_textbuf_new();
    if (run->tb == NULL)
        return false;
    run->undo = yew_undo_new(run->tb);
    if (run->undo == NULL)
        return false;
    run->edit.tb = run->tb;
    run->edit.undo = run->undo;
    run->rng = seed == 0U ? UINT64_C(0x9e3779b97f4a7c15) : seed;
    run->hash = UINT64_C(1469598103934665603);
    yew_undo_set_limits(run->undo, UINT64_MAX, 0U,
                        YEW_UNDO_PERSIST_BYTES_MAX);
    return oracle_store(&run->oracle, yew_undo_current(run->undo), 0U,
                        NULL, 0U);
}

static void dispose(Run *run)
{
    size_t i;

    for (i = 0U; i < run->oracle.cap; i++)
        free(run->oracle.data[i].bytes);
    free(run->oracle.data);
    if (run->undo != NULL)
        yew_undo_free(run->undo);
    if (run->tb != NULL)
        yew_textbuf_free(run->tb);
}

int main(int argc, char **argv)
{
    u64 seed = 1U;
    size_t iterations = YEW_UNDO_FUZZ_MIN_ITERS;
    Run run;
    size_t op;
    size_t i;

    for (i = 1U; i < (size_t)argc; i++) {
        if (parse_u64(argv[i], "--seed=", &seed))
            continue;
        if (parse_size(argv[i], "--iters=", &iterations))
            continue;
        (void)fprintf(stderr, "usage: %s [--seed=N] [--iters=N]\n",
                      argv[0]);
        return 2;
    }
    if (iterations < YEW_UNDO_FUZZ_MIN_ITERS)
        iterations = YEW_UNDO_FUZZ_MIN_ITERS;
    (void)memset(&run, 0, sizeof(run));
    if (!initialize(&run, seed)) {
        dispose(&run);
        return 2;
    }
    for (op = 0U; op < iterations; op++) {
        size_t choice = choose(&run, 100U);
        bool ok;

        if (op == 0U || op == 2U)
            ok = edit_once(&run);
        else if (op == 1U || op == 3U)
            ok = undo_once(&run);
        else if (op == 4U)
            ok = branch_once(&run);
        else if (op == 5U)
            ok = redo_once(&run);
        else if (choice < 45U)
            ok = edit_once(&run);
        else if (choice < 65U)
            ok = undo_once(&run);
        else if (choice < 80U)
            ok = redo_once(&run);
        else if (choice < 90U)
            ok = branch_once(&run);
        else
            ok = jump_once(&run);
        if (!ok || !check_current(&run, op)) {
            dispose(&run);
            return 1;
        }
    }
    (void)printf("fuzz_undo: seed=%llu iters=%zu hash=%016llx "
                 "edits=%zu undo=%zu redo=%zu branches=%zu ok\n",
                 (unsigned long long)seed, iterations,
                 (unsigned long long)run.hash, run.edits, run.undos,
                 run.redos, run.branches);
    if (run.edits == 0U || run.undos == 0U || run.redos == 0U ||
        run.branches == 0U) {
        dispose(&run);
        return 1;
    }
    dispose(&run);
    return 0;
}
