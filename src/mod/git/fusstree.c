#include "mod/git/fusstree.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "util/sort.h"

u8 yew_fuss_marker_kinds(const FussNode *node, FussMarkerKind out[4])
{
    u8 n = 0U;

    if (node == NULL || out == NULL)
        return 0U;
    if (node->conflicted) {
        out[0] = YEW_FUSS_MARK_CONFLICT;
        return 1U;
    }
    if (node->staged)
        out[n++] = YEW_FUSS_MARK_STAGED;
    if (node->unstaged)
        out[n++] = YEW_FUSS_MARK_UNSTAGED;
    if (node->untracked)
        out[n++] = YEW_FUSS_MARK_UNTRACKED;
    if (node->incoming)
        out[n++] = YEW_FUSS_MARK_INCOMING;
    return n;
}

static void node_reserve(FussNodeList *v, size_t need)
{
    size_t cap;

    if (v->cap >= need)
        return;
    cap = v->cap ? v->cap : 16U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    v->data = yew_xreallocarray(v->data, cap, sizeof(*v->data));
    v->cap = cap;
}

static u32 node_push(FussNodeList *v, FussNode node)
{
    if (v->len >= UINT32_MAX)
        return 0U;
    node_reserve(v, v->len + 1U);
    v->data[v->len] = node;
    return (u32)v->len++;
}

static void item_reserve(FussItemList *v, size_t need)
{
    size_t cap;

    if (v->cap >= need)
        return;
    cap = v->cap ? v->cap : 16U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    v->data = yew_xreallocarray(v->data, cap, sizeof(*v->data));
    v->cap = cap;
}

static void item_push(FussItemList *v, FussItem item)
{
    item_reserve(v, v->len + 1U);
    v->data[v->len++] = item;
}

void yew_fuss_tree_init(FussTree *t)
{
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    arena_init(&t->a);
}

void yew_fuss_tree_drop(FussTree *t)
{
    if (!t)
        return;
    arena_free_all(&t->a);
    yew_xfree(t->nodes.data);
    yew_xfree(t->items.data);
    memset(t, 0, sizeof(*t));
}

static void tree_reset(FussTree *t)
{
    arena_free_all(&t->a);
    arena_init(&t->a);
    t->nodes.len = 0U;
    t->items.len = 0U;
}

static bool byte_equal(const char *a, u32 an, const char *b, u32 bn)
{
    return an == bn && (an == 0U || memcmp(a, b, an) == 0);
}

static u8 ascii_fold(u8 c)
{
    if (c >= (u8)'A' && c <= (u8)'Z')
        return (u8)(c + ((u8)'a' - (u8)'A'));
    return c;
}

static int name_case_cmp(const FussNode *a, const FussNode *b)
{
    u32 common = a->name_len < b->name_len ? a->name_len : b->name_len;
    u32 i;
    int cmp;

    for (i = 0U; i < common; i++) {
        u8 ac = ascii_fold((u8)a->name[i]);
        u8 bc = ascii_fold((u8)b->name[i]);
        if (ac != bc)
            return ac < bc ? -1 : 1;
    }
    if (a->name_len != b->name_len)
        return a->name_len < b->name_len ? -1 : 1;
    cmp = common ? memcmp(a->name, b->name, common) : 0;
    return (cmp > 0) - (cmp < 0);
}

static int child_cmp(const void *left, const void *right, void *ctx)
{
    const FussTree *t = ctx;
    const FussNode *a = &t->nodes.data[*(const u32 *)left];
    const FussNode *b = &t->nodes.data[*(const u32 *)right];

    if (a->is_file != b->is_file)
        return a->is_file ? 1 : -1;
    return name_case_cmp(a, b);
}

static void sort_children(FussTree *t)
{
    u32 *order;
    size_t parent;

    if (t->nodes.len < 2U)
        return;
    order = yew_xreallocarray(NULL, t->nodes.len, sizeof(*order));
    for (parent = 0U; parent < t->nodes.len; parent++) {
        size_t n = 0U;
        u32 child;
        size_t i;

        for (child = t->nodes.data[parent].first_child; child != 0U;
             child = t->nodes.data[child].next_sibling)
            order[n++] = child;
        if (n < 2U)
            continue;
        for (i = 1U; i < n; i++)
            if (child_cmp(&order[i - 1U], &order[i], t) > 0)
                break;
        if (i == n)
            continue;
        yew_sort_stable(order, n, sizeof(*order), child_cmp, t);
        t->nodes.data[parent].first_child = order[0];
        for (i = 1U; i < n; i++)
            t->nodes.data[order[i - 1U]].next_sibling = order[i];
        t->nodes.data[order[n - 1U]].next_sibling = 0U;
    }
    yew_xfree(order);
}

static u32 find_child(const FussTree *t, u32 parent,
                      const char *name, u32 name_len)
{
    u32 child;

    if (parent >= t->nodes.len)
        return 0U;
    for (child = t->nodes.data[parent].first_child; child != 0U;
         child = t->nodes.data[child].next_sibling) {
        const FussNode *n = &t->nodes.data[child];
        if (byte_equal(n->name, n->name_len, name, name_len))
            return child;
    }
    return 0U;
}

static u32 add_child(FussTree *t, u32 parent, const char *name, u32 name_len,
                     bool is_file)
{
    FussNode node;
    const FussNode *p;
    size_t path_len;
    char *path;
    u32 index;

    memset(&node, 0, sizeof(node));
    p = &t->nodes.data[parent];
    path_len = (size_t)p->path_len + (parent == 0U ? 0U : 1U) + name_len;
    if (path_len > UINT32_MAX)
        return 0U;
    path = arena_alloc(&t->a, path_len + 1U, 1U);
    if (parent != 0U) {
        memcpy(path, p->path, p->path_len);
        path[p->path_len] = '/';
        memcpy(path + p->path_len + 1U, name, name_len);
    } else if (name_len) {
        memcpy(path, name, name_len);
    }
    path[path_len] = '\0';
    node.name = arena_strndup(&t->a, name, name_len);
    node.name_len = name_len;
    node.path = path;
    node.path_len = (u32)path_len;
    node.parent = parent;
    node.is_file = is_file;
    node.expanded = !is_file;
    index = node_push(&t->nodes, node);
    if (index == 0U)
        return 0U;
    t->nodes.data[index].next_sibling = t->nodes.data[parent].first_child;
    t->nodes.data[parent].first_child = index;
    return index;
}

static bool path_hidden(const char *path, u32 len)
{
    u32 at = 0U;

    while (at < len) {
        u32 start = at;
        while (at < len && path[at] != '/')
            at++;
        if (at > start && path[start] == '.')
            return true;
        at++;
    }
    return false;
}

static u32 add_path(FussTree *t, const char *path, u32 len, bool is_dir)
{
    u32 parent = 0U;
    u32 at = 0U;

    while (len > 0U && path[len - 1U] == '/')
        len--;
    while (at < len) {
        u32 start = at;
        u32 child;
        bool last;

        while (at < len && path[at] != '/')
            at++;
        if (at == start) {
            at++;
            continue;
        }
        last = at == len;
        child = find_child(t, parent, path + start, at - start);
        if (child == 0U)
            child = add_child(t, parent, path + start, at - start,
                              last && !is_dir);
        if (child == 0U)
            return 0U;
        if (!last)
            t->nodes.data[child].is_file = false;
        parent = child;
        at++;
    }
    return parent;
}

typedef struct SortedPathState {
    const char *previous;
    u32 previous_len;
    u32 *nodes;
    size_t cap;
    u32 depth;
    u32 *tails;
    size_t tails_cap;
} SortedPathState;

static u32 add_sorted_child(FussTree *t, SortedPathState *state, u32 parent,
                            const char *name, u32 name_len, bool is_file)
{
    u32 old_first;
    u32 child;

    if ((size_t)parent >= state->tails_cap) {
        size_t old_cap = state->tails_cap;
        size_t cap = old_cap == 0U ? 16U : old_cap;

        while (cap <= (size_t)parent)
            cap *= 2U;
        state->tails = yew_xreallocarray(state->tails, cap,
                                          sizeof(*state->tails));
        (void)memset(state->tails + old_cap, 0,
                     (cap - old_cap) * sizeof(*state->tails));
        state->tails_cap = cap;
    }
    old_first = t->nodes.data[parent].first_child;
    child = add_child(t, parent, name, name_len, is_file);
    if (child == 0U)
        return 0U;
    if (state->tails[parent] == 0U) {
        state->tails[parent] = child;
    } else {
        t->nodes.data[parent].first_child = old_first;
        t->nodes.data[state->tails[parent]].next_sibling = child;
        t->nodes.data[child].next_sibling = 0U;
        state->tails[parent] = child;
    }
    return child;
}

static u32 add_sorted_path(FussTree *t, SortedPathState *state,
                           const char *path, u32 len, bool is_dir)
{
    u32 previous_at = 0U;
    u32 at = 0U;
    u32 common = 0U;
    u32 depth = 0U;
    u32 parent = 0U;

    while (len > 0U && path[len - 1U] == '/')
        len--;
    while (state->previous != NULL && previous_at < state->previous_len &&
           at < len) {
        u32 previous_start = previous_at;
        u32 start = at;

        while (previous_at < state->previous_len &&
               state->previous[previous_at] != '/')
            previous_at++;
        while (at < len && path[at] != '/')
            at++;
        if (previous_at - previous_start != at - start ||
            memcmp(state->previous + previous_start, path + start,
                   at - start) != 0)
            break;
        common++;
        previous_at++;
        at++;
    }
    if (common > state->depth)
        common = state->depth;
    at = 0U;
    while (at < len) {
        u32 start = at;
        u32 child;
        bool last;

        while (at < len && path[at] != '/')
            at++;
        if (at == start) {
            at++;
            continue;
        }
        last = at == len;
        if ((size_t)depth == state->cap) {
            size_t cap = state->cap == 0U ? 8U : state->cap * 2U;

            state->nodes = yew_xreallocarray(state->nodes, cap,
                                               sizeof(*state->nodes));
            state->cap = cap;
        }
        if (depth < common)
            child = state->nodes[depth];
        else
            child = add_sorted_child(t, state, parent, path + start,
                                     at - start, last && !is_dir);
        if (child == 0U)
            return 0U;
        state->nodes[depth] = child;
        if (!last)
            t->nodes.data[child].is_file = false;
        parent = child;
        depth++;
        at++;
    }
    state->previous = path;
    state->previous_len = len;
    state->depth = depth;
    return parent;
}

static bool path_below_untracked_dir(const FussTree *t, const char *path,
                                     u32 len)
{
    u32 parent = 0U;
    u32 at = 0U;

    while (at < len) {
        u32 start = at;
        u32 child;

        while (at < len && path[at] != '/')
            at++;
        if (at == start) {
            at++;
            continue;
        }
        child = find_child(t, parent, path + start, at - start);
        if (child == 0U)
            return false;
        if (at < len && t->nodes.data[child].untracked_dir)
            return true;
        parent = child;
        at++;
    }
    return false;
}

static void node_apply_entry(FussNode *node, const GitEntry *entry,
                             bool ignored)
{
    node->staged |= entry->staged;
    node->unstaged |= entry->unstaged;
    node->untracked |= entry->untracked;
    node->incoming |= entry->incoming;
    node->conflicted |= entry->conflicted;
    node->ignored |= ignored;
    if (!node->is_file && entry->untracked) {
        node->untracked_dir = true;
        node->expanded = false;
    }
}

static int entry_ptr_cmp(const void *left, const void *right, void *ctx)
{
    const GitEntry *const *a = left;
    const GitEntry *const *b = right;
    u32 common = (*a)->path_len < (*b)->path_len ?
                 (*a)->path_len : (*b)->path_len;
    int cmp;
    (void)ctx;

    cmp = common ? memcmp((*a)->path, (*b)->path, common) : 0;
    if (cmp != 0)
        return (cmp > 0) - (cmp < 0);
    return ((*a)->path_len > (*b)->path_len) -
           ((*a)->path_len < (*b)->path_len);
}

static int cstr_ptr_cmp(const void *left, const void *right, void *ctx)
{
    const char *const *a = left;
    const char *const *b = right;
    int cmp;
    (void)ctx;

    cmp = strcmp(*a, *b);
    return (cmp > 0) - (cmp < 0);
}

static size_t path_cstr_len(const char *path)
{
    const char *end = path;

    while (*end != '\0')
        end++;
    return (size_t)(end - path);
}

static void aggregate_flags(FussTree *t)
{
    size_t i;

    for (i = t->nodes.len; i > 1U; i--) {
        FussNode *node = &t->nodes.data[i - 1U];
        FussNode *parent = &t->nodes.data[node->parent];
        parent->staged |= node->staged;
        parent->unstaged |= node->unstaged;
        parent->untracked |= node->untracked;
        parent->incoming |= node->incoming;
        parent->conflicted |= node->conflicted;
        parent->ignored |= node->ignored;
    }
}

typedef struct CachedUntrackedDir {
    char *path;
    u32 path_len;
    GitPathList children;
} CachedUntrackedDir;

static size_t cache_untracked_dirs(const FussTree *t, Arena *a,
                                   CachedUntrackedDir **out)
{
    CachedUntrackedDir *dirs;
    size_t count = 0U;
    size_t i;

    *out = NULL;
    for (i = 1U; i < t->nodes.len; i++)
        if (t->nodes.data[i].untracked_dir &&
            t->nodes.data[i].untracked_loaded)
            count++;
    if (count == 0U)
        return 0U;
    dirs = arena_alloc(a, count * sizeof(*dirs),
                       _Alignof(CachedUntrackedDir));
    count = 0U;
    for (i = 1U; i < t->nodes.len; i++) {
        const FussNode *node = &t->nodes.data[i];
        CachedUntrackedDir *cached;
        size_t child_count = 0U;
        u32 child;
        size_t at = 0U;

        if (!node->untracked_dir || !node->untracked_loaded)
            continue;
        for (child = node->first_child; child != 0U;
             child = t->nodes.data[child].next_sibling)
            child_count++;
        cached = &dirs[count++];
        memset(cached, 0, sizeof(*cached));
        cached->path = arena_strndup(a, node->path, node->path_len);
        cached->path_len = node->path_len;
        if (child_count != 0U) {
            cached->children.data = arena_alloc(
                a, child_count * sizeof(*cached->children.data),
                _Alignof(GitPath));
        }
        cached->children.len = child_count;
        for (child = node->first_child; child != 0U;
             child = t->nodes.data[child].next_sibling) {
            const FussNode *n = &t->nodes.data[child];
            GitPath *path = &cached->children.data[at++];
            size_t stored_len = (size_t)n->path_len + (n->is_file ? 0U : 1U);
            char *copy = arena_alloc(a, stored_len + 1U, 1U);

            memcpy(copy, n->path, n->path_len);
            if (!n->is_file)
                copy[n->path_len] = '/';
            copy[stored_len] = '\0';
            path->path = copy;
            path->len = (u32)stored_len;
            path->is_dir = !n->is_file;
        }
    }
    *out = dirs;
    return count;
}

static u32 node_for_path(const FussTree *t, const char *path, u32 len)
{
    size_t i;

    for (i = 1U; i < t->nodes.len; i++)
        if (byte_equal(t->nodes.data[i].path, t->nodes.data[i].path_len,
                       path, len))
            return (u32)i;
    return 0U;
}

void yew_fuss_build(FussTree *t, const GitSnapshot *s, const FussOpts *o)
{
    FussOpts opts = {false, false};
    Arena collapsed_arena;
    char **collapsed = NULL;
    u32 collapsed_n = 0U;
    CachedUntrackedDir *cached_dirs = NULL;
    size_t cached_n = 0U;
    size_t i;
    const GitEntry **ordered = NULL;
    size_t ordered_n = 0U;
    SortedPathState paths = {0};
    FussNode root;

    if (!t || !s)
        return;
    if (o)
        opts = *o;
    if (t->nodes.len != 0U && t->opts_valid && t->snap_gen == s->gen &&
        t->all_files == opts.all_files && t->show_hidden == opts.show_hidden)
        return;

    arena_init(&collapsed_arena);
    if (t->nodes.len != 0U) {
        collapsed_n = yew_fuss_harvest_collapsed(t, &collapsed_arena,
                                                  &collapsed);
        /* A lazy walk is filtered using the visibility policy active when it
         * runs.  Do not carry that result across a hidden-policy change. */
        if (!t->opts_valid || t->show_hidden == opts.show_hidden)
            cached_n = cache_untracked_dirs(t, &collapsed_arena,
                                            &cached_dirs);
    }
    tree_reset(t);
    memset(&root, 0, sizeof(root));
    root.name = arena_strdup(&t->a, "");
    root.path = root.name;
    root.expanded = true;
    (void)node_push(&t->nodes, root);

    if (s->entries.data != NULL && s->entries.len != 0U) {
        ordered = yew_xreallocarray(NULL, s->entries.len, sizeof(*ordered));
        for (i = 0U; i < s->entries.len; i++)
            if (s->entries.data[i].path != NULL &&
                s->entries.data[i].path_len != 0U)
                ordered[ordered_n++] = &s->entries.data[i];
        for (i = 1U; i < ordered_n; i++)
            if (entry_ptr_cmp(&ordered[i - 1U], &ordered[i], NULL) > 0) {
                yew_sort_stable(ordered, ordered_n, sizeof(*ordered),
                                entry_ptr_cmp, NULL);
                break;
            }
    }
    for (i = 0U; i < ordered_n; i++) {
        const GitEntry *entry = ordered[i];
        bool ignored;
        bool dirty;
        u32 node;

        ignored = entry->kind == GIT_E_IGNORED ||
                  yew_git_ignored(&s->ignored, entry->path, entry->path_len);
        dirty = entry->staged || entry->unstaged || entry->untracked ||
                entry->incoming || entry->conflicted || ignored;
        if (!dirty && !opts.all_files)
            continue;
        if (!opts.show_hidden &&
            (ignored || path_hidden(entry->path, entry->path_len)))
            continue;
        node = add_sorted_path(t, &paths, entry->path, entry->path_len,
                               entry->is_dir ||
                               entry->path[entry->path_len - 1U] == '/');
        if (node != 0U)
            node_apply_entry(&t->nodes.data[node], entry, ignored);
    }
    yew_xfree(ordered);
    yew_xfree(paths.nodes);
    yew_xfree(paths.tails);

    aggregate_flags(t);
    sort_children(t);
    t->snap_gen = s->gen;
    t->opts_valid = true;
    t->all_files = opts.all_files;
    t->show_hidden = opts.show_hidden;
    t->files_merged = false;
    for (i = 0U; i < cached_n; i++) {
        u32 node = node_for_path(t, cached_dirs[i].path,
                                 cached_dirs[i].path_len);
        if (node != 0U && t->nodes.data[node].untracked_dir)
            (void)yew_fuss_expand_untracked(t, node,
                                             &cached_dirs[i].children);
    }
    if (collapsed_n != 0U)
        yew_fuss_restore_collapsed(t, collapsed, collapsed_n);
    else
        yew_fuss_flatten(t);
    arena_free_all(&collapsed_arena);
}

bool yew_fuss_merge_files(FussTree *t, const FileList *files,
                          const GitSnapshot *s, const FussOpts *o)
{
    FussOpts opts = {false, false};
    char **ordered = NULL;
    size_t ordered_n = 0U;
    size_t i;

    if (!t || !files || !s ||
        (!files->paths.data && files->paths.len != 0U))
        return false;
    if (o)
        opts = *o;
    /* A FileList is a completed inventory, not an append-only delta.  Force
     * the status-only base back into place before replacing an older walk;
     * yew_fuss_build still harvests collapse and lazy-directory caches. */
    if (opts.all_files && t->files_merged)
        t->opts_valid = false;
    yew_fuss_build(t, s, &opts);
    if (!opts.all_files)
        return false;
    if (files->paths.len != 0U)
        ordered = yew_xreallocarray(NULL, files->paths.len,
                                    sizeof(*ordered));
    for (i = 0U; i < files->paths.len; i++) {
        const char *path = files->paths.data[i];
        size_t len;

        if (!path)
            continue;
        len = path_cstr_len(path);
        if (len == 0U || len > UINT32_MAX)
            continue;
        if (!opts.show_hidden &&
            (path_hidden(path, (u32)len) ||
             yew_git_ignored(&s->ignored, path, (u32)len)))
            continue;
        ordered[ordered_n++] = (char *)path;
    }
    yew_sort_stable(ordered, ordered_n, sizeof(*ordered),
                    cstr_ptr_cmp, NULL);
    for (i = 0U; i < ordered_n; i++) {
        size_t len = path_cstr_len(ordered[i]);
        if (!path_below_untracked_dir(t, ordered[i], (u32)len)) {
            u32 node = add_path(t, ordered[i], (u32)len, false);

            if (node != 0U)
                t->nodes.data[node].ignored |=
                    yew_git_ignored(&s->ignored, ordered[i], (u32)len);
        }
    }
    yew_xfree(ordered);
    aggregate_flags(t);
    sort_children(t);
    yew_fuss_flatten(t);
    t->files_merged = true;
    return true;
}

void yew_fuss_flatten(FussTree *t)
{
    u32 node;
    u32 depth = 0U;

    if (!t)
        return;
    t->items.len = 0U;
    if (t->nodes.len == 0U)
        return;
    node = t->nodes.data[0].first_child;
    while (node != 0U) {
        FussNode *n = &t->nodes.data[node];
        FussItem item;

        item.path = n->path;
        item.path_len = n->path_len;
        item.node = node;
        item.depth = depth > UINT16_MAX ? UINT16_MAX : (u16)depth;
        item.is_file = n->is_file;
        item_push(&t->items, item);
        if (!n->is_file && n->expanded && n->first_child != 0U) {
            node = n->first_child;
            depth++;
            continue;
        }
        while (node != 0U && t->nodes.data[node].next_sibling == 0U) {
            node = t->nodes.data[node].parent;
            if (node != 0U && depth != 0U)
                depth--;
        }
        if (node != 0U)
            node = t->nodes.data[node].next_sibling;
    }
}

static i32 normalized_row(const FussTree *t, i32 row)
{
    if (!t || t->items.len == 0U)
        return -1;
    if (row < 0 || (size_t)row >= t->items.len)
        return 0;
    return row;
}

i32 yew_fuss_nav_step(const FussTree *t, i32 row, i32 dir)
{
    i32 selected = normalized_row(t, row);
    i32 count;
    i32 i;
    u16 depth;

    if (selected < 0 || dir == 0)
        return selected;
    count = (i32)t->items.len;
    dir = dir > 0 ? 1 : -1;
    depth = t->items.data[selected].depth;
    for (i = selected + dir; i >= 0 && i < count; i += dir)
        if (t->items.data[i].depth == depth)
            return i;
    i = dir > 0 ? 0 : count - 1;
    while (i != selected) {
        if (t->items.data[i].depth == depth)
            return i;
        i += dir;
    }
    return selected;
}

i32 yew_fuss_nav_raw(const FussTree *t, i32 row, i32 dir)
{
    i32 selected = normalized_row(t, row);
    i32 count;

    if (selected < 0 || dir == 0)
        return selected;
    count = (i32)t->items.len;
    if (dir > 0)
        return selected + 1 < count ? selected + 1 : selected;
    return selected > 0 ? selected - 1 : selected;
}

i32 yew_fuss_nav_parent(const FussTree *t, i32 row)
{
    i32 selected = normalized_row(t, row);
    u16 depth;
    i32 i;

    if (selected < 0)
        return selected;
    depth = t->items.data[selected].depth;
    if (depth == 0U)
        return selected;
    for (i = selected - 1; i >= 0; i--)
        if (t->items.data[i].depth == (u16)(depth - 1U))
            return i;
    return selected;
}

static i32 row_for_node(const FussTree *t, u32 node)
{
    size_t i;
    for (i = 0U; i < t->items.len; i++)
        if (t->items.data[i].node == node)
            return (i32)i;
    return -1;
}

i32 yew_fuss_nav_enter(FussTree *t, i32 row)
{
    i32 selected = normalized_row(t, row);
    u32 node;
    u16 depth;

    if (selected < 0)
        return selected;
    node = t->items.data[selected].node;
    if (node >= t->nodes.len || t->nodes.data[node].is_file)
        return selected;
    if (t->nodes.data[node].untracked_dir &&
        !t->nodes.data[node].untracked_loaded &&
        t->nodes.data[node].first_child == 0U)
        return selected;
    depth = t->items.data[selected].depth;
    if (!t->nodes.data[node].expanded) {
        t->nodes.data[node].expanded = true;
        yew_fuss_flatten(t);
        selected = row_for_node(t, node);
    }
    if (selected >= 0 && (size_t)(selected + 1) < t->items.len &&
        t->items.data[selected + 1].depth == (u16)(depth + 1U))
        return selected + 1;
    return selected;
}

bool yew_fuss_nav_toggle(FussTree *t, i32 row)
{
    i32 selected = normalized_row(t, row);
    u32 node;

    if (selected < 0)
        return false;
    node = t->items.data[selected].node;
    if (node >= t->nodes.len || t->nodes.data[node].is_file)
        return false;
    if (t->nodes.data[node].untracked_dir &&
        !t->nodes.data[node].untracked_loaded &&
        t->nodes.data[node].first_child == 0U)
        return false;
    t->nodes.data[node].expanded = !t->nodes.data[node].expanded;
    yew_fuss_flatten(t);
    return true;
}

static bool child_belongs_to(const FussNode *parent, const GitPath *child)
{
    u32 rest;
    u32 i;

    if (!child->path || child->len <= parent->path_len ||
        memcmp(child->path, parent->path, parent->path_len) != 0 ||
        child->path[parent->path_len] != '/')
        return false;
    rest = child->len - parent->path_len - 1U;
    while (rest > 0U && child->path[parent->path_len + rest] == '/')
        rest--;
    if (rest == 0U)
        return false;
    for (i = 0U; i < rest; i++)
        if (child->path[parent->path_len + 1U + i] == '/')
            return false;
    return true;
}

bool yew_fuss_expand_untracked(FussTree *t, u32 node,
                               const GitPathList *children)
{
    size_t i;

    if (!t || node == 0U || node >= t->nodes.len ||
        t->nodes.data[node].is_file || !t->nodes.data[node].untracked_dir ||
        t->nodes.data[node].untracked_loaded ||
        !children || (!children->data && children->len != 0U))
        return false;
    for (i = 0U; i < children->len; i++)
        if (!child_belongs_to(&t->nodes.data[node], &children->data[i]))
            return false;
    for (i = 0U; i < children->len; i++) {
        const GitPath *path = &children->data[i];
        u32 child;

        if (!t->show_hidden && path_hidden(path->path, path->len))
            continue;
        child = add_path(t, path->path, path->len, path->is_dir);
        if (child == 0U)
            return false;
        if (!t->nodes.data[child].staged &&
            !t->nodes.data[child].unstaged &&
            !t->nodes.data[child].incoming &&
            !t->nodes.data[child].conflicted)
            t->nodes.data[child].untracked = true;
        if (!t->nodes.data[child].is_file) {
            t->nodes.data[child].untracked_dir = true;
            t->nodes.data[child].expanded = false;
        }
    }
    t->nodes.data[node].untracked_loaded = true;
    t->nodes.data[node].expanded = true;
    aggregate_flags(t);
    sort_children(t);
    yew_fuss_flatten(t);
    return true;
}

static int path_ptr_cmp(const void *left, const void *right, void *ctx)
{
    const char *const *a = left;
    const char *const *b = right;
    int cmp;
    (void)ctx;
    cmp = strcmp(*a, *b);
    return (cmp > 0) - (cmp < 0);
}

u32 yew_fuss_harvest_collapsed(const FussTree *old, Arena *a, char ***out)
{
    char **paths;
    size_t count = 0U;
    size_t i;

    if (out)
        *out = NULL;
    if (!old || !a || !out)
        return 0U;
    for (i = 1U; i < old->nodes.len; i++)
        if (!old->nodes.data[i].is_file && !old->nodes.data[i].expanded)
            count++;
    if (count == 0U)
        return 0U;
    if (count > UINT32_MAX)
        count = UINT32_MAX;
    paths = arena_alloc(a, count * sizeof(*paths), _Alignof(char *));
    count = 0U;
    for (i = 1U; i < old->nodes.len && count < UINT32_MAX; i++) {
        const FussNode *node = &old->nodes.data[i];
        if (!node->is_file && !node->expanded)
            paths[count++] = arena_strndup(a, node->path, node->path_len);
    }
    yew_sort_stable(paths, count, sizeof(*paths), path_ptr_cmp, NULL);
    *out = paths;
    return (u32)count;
}

static bool collapsed_has(char *const *paths, u32 n, const char *path)
{
    u32 lo = 0U;
    u32 hi = n;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2U;
        int cmp = strcmp(paths[mid], path);
        if (cmp < 0)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo < n && strcmp(paths[lo], path) == 0;
}

void yew_fuss_restore_collapsed(FussTree *nw, char *const *paths, u32 n)
{
    size_t i;

    if (!nw || (!paths && n != 0U))
        return;
    for (i = 1U; i < nw->nodes.len; i++) {
        FussNode *node = &nw->nodes.data[i];
        if (!node->is_file && collapsed_has(paths, n, node->path))
            node->expanded = false;
    }
    yew_fuss_flatten(nw);
}

void yew_fuss_sel_clear(FussSel *s)
{
    if (!s)
        return;
    yew_xfree(s->path);
    s->path = NULL;
    s->len = 0U;
}

void yew_fuss_sel_set(FussSel *s, const char *path, u32 len)
{
    char *copy;

    if (!s || (!path && len != 0U))
        return;
    copy = yew_xmalloc((size_t)len + 1U);
    if (len)
        memcpy(copy, path, len);
    copy[len] = '\0';
    yew_xfree(s->path);
    s->path = copy;
    s->len = len;
}

void yew_fuss_sel_from_row(FussSel *s, const FussTree *t, i32 row)
{
    row = normalized_row(t, row);
    if (row < 0) {
        yew_fuss_sel_clear(s);
        return;
    }
    yew_fuss_sel_set(s, t->items.data[row].path,
                     t->items.data[row].path_len);
}

static i32 exact_row(const FussTree *t, const char *path, u32 len,
                     bool dirs_only)
{
    size_t i;

    for (i = 0U; i < t->items.len; i++) {
        const FussItem *item = &t->items.data[i];
        if ((!dirs_only || !item->is_file) &&
            byte_equal(item->path, item->path_len, path, len))
            return (i32)i;
    }
    return -1;
}

i32 yew_fuss_row_of(const FussTree *t, const FussSel *s)
{
    u32 len;
    i32 row;

    if (!t || t->items.len == 0U)
        return -1;
    if (!s || !s->path)
        return 0;
    row = exact_row(t, s->path, s->len, false);
    if (row >= 0)
        return row;
    len = s->len;
    while (len > 0U) {
        while (len > 0U && s->path[len - 1U] != '/')
            len--;
        if (len == 0U)
            break;
        len--;
        row = exact_row(t, s->path, len, true);
        if (row >= 0)
            return row;
    }
    return 0;
}
