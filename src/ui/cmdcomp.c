#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "ui/cmdcomp.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "edit/ed.h"
#include "ui/cmdparse.h"
#include "unicode/utf8.h"
#include "util/buf.h"
#include "util/log.h"
#include "util/sort.h"

typedef struct {
    char *match;
    char *text;
    const char *detail;
    bool is_dir;
    bool deferred;
    i32 score;
    FzMatch m;
} Candidate;

VEC_DECL(CandidateVec, Candidate);

typedef struct {
    char *name;
    size_t cap;
    i32 score;
    unsigned char dtype;
    FzMatch m;
} PathCandidate;

VEC_DECL(PathCandidateVec, PathCandidate);

static bool force_dtype_unknown;
static u32 test_lstat_calls;

static bool starts_with(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);

    return strncmp(s, prefix, n) == 0;
}

/*
 * Sprint 18.5 §2: one candidate's rank key, the single-item form of
 * sag_fz_rank's ordering.  The streaming path enumerator cannot hand
 * sag_fz_rank a whole array -- it never holds one -- so the tier rule
 * lives here as well, in one helper both shapes call.
 *
 * `match` is always a bare name (a command name, a buffer label, a
 * directory entry), never a path with slashes, so basename() would be
 * the identity and path_mode is irrelevant.
 */
static i32 comp_key(const char *stem, size_t stem_len, const char *match,
                    FzMatch *m)
{
    i32 score = sag_fz_score(stem, (u32)stem_len, match,
                             (u32)strlen(match), m);

    if (score == SAG_FZ_NO_MATCH)
        return SAG_FZ_NO_MATCH;
    if (score >= 5000)
        return score > INT32_MAX - SAG_FZ_BASENAME_TIER
                   ? INT32_MAX
                   : score + (i32)SAG_FZ_BASENAME_TIER;
    return score;
}

/*
 * Descending by key, then shorter-then-memcmp -- sag_fz_rank's tie rule,
 * so the two orderings cannot drift apart.
 *
 * Unlike sag_fz_rank, ties are broken by name even for the EMPTY stem.
 * There the "preserve source order" rule has nothing to preserve:
 * readdir order is filesystem order, it differs between ext4 and xfs and
 * between two identical checkouts, and letting it through would make
 * every completion golden filesystem-dependent (invariant 5).
 */
static int candidate_cmp(const void *left, const void *right, void *ctx)
{
    const Candidate *a = left;
    const Candidate *b = right;
    int by_name;
    size_t la;
    size_t lb;

    (void)ctx;
    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    la = strlen(a->match);
    lb = strlen(b->match);
    if (la != lb)
        return la < lb ? -1 : 1;
    by_name = strcmp(a->match, b->match);
    if (by_name != 0)
        return by_name;
    return strcmp(a->text, b->text);
}

static void candidate_dispose(CandidateVec *v)
{
    size_t i;

    for (i = 0U; i < v->len; i++) {
        if (v->data[i].match != v->data[i].text)
            free(v->data[i].match);
        free(v->data[i].text);
    }
    CandidateVec_free(v);
}

static bool candidate_add(CandidateVec *v, const char *stem,
                          const char *match, const char *text,
                          const char *detail, bool is_dir, bool deferred)
{
    Candidate item;
    i32 score;

    (void)memset(&item.m, 0, sizeof(item.m));
    score = comp_key(stem, strlen(stem), match, &item.m);
    /* SAG_FZ_NO_MATCH, not `< 0`: the length penalty makes a genuine
     * match score negative, and a `< 0` test silently drops the longest
     * real candidates. */
    if (score == SAG_FZ_NO_MATCH)
        return false;
    item.text = sag_xmalloc(strlen(text) + 1U);
    (void)strcpy(item.text, text);
    if (strcmp(match, text) == 0) {
        item.match = item.text;
    } else {
        item.match = sag_xmalloc(strlen(match) + 1U);
        (void)strcpy(item.match, match);
    }
    item.detail = detail;
    item.is_dir = is_dir;
    item.deferred = deferred;
    item.score = score;
    CandidateVec_push(v, item);
    return true;
}

static u32 candidate_finish(const CompReq *req, SagCompKind kind,
                            CandidateVec *matches, Vec_CompItem *out)
{
    Arena *arena = req->arena;
    size_t i;
    size_t keep;
    u32 total = matches->len > UINT32_MAX ? UINT32_MAX : (u32)matches->len;

    sag_sort_stable(matches->data, matches->len, sizeof(matches->data[0]),
                    candidate_cmp, NULL);
    out->len = 0U;
    keep = matches->len < SAG_COMP_MAX ? matches->len : SAG_COMP_MAX;
    Vec_CompItem_reserve(out, keep);
    for (i = 0U; i < keep; i++) {
        const Candidate *src = &matches->data[i];
        CompItem item;

        item.text = kind == SAG_COMP_PATH ?
                    sag_comp_quote(arena, src->text) :
                    arena_strdup(arena, src->text);
        item.detail = src->detail == NULL ? NULL :
                      arena_strdup(arena, src->detail);
        item.kind = (u8)kind;
        item.is_dir = src->is_dir;
        item.deferred = src->deferred;
        item.score = src->score;
        /* Positions index `match`, which for these sources IS the drawn
         * text -- except for a quoted path, handled in
         * path_candidates_finish. */
        item.m = src->m;
        item.match = arena_strdup(arena, src->match);
        item.match_off = strcmp(item.text, item.match) == 0
                             ? 0U
                             : (u16)SAG_COMP_NO_HIGHLIGHT;
        Vec_CompItem_push(out, item);
    }
    candidate_dispose(matches);
    return total;
}

static u32 enumerate_commands(const CompReq *req, Vec_CompItem *out)
{
    CandidateVec matches = {0};
    const char *stem = req->stem;
    u32 i;

    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *desc = sag_cmd_at(i);
        CmdId id;
        const CmdEntry *entry;
        const char *name;
        bool deferred;

        if (desc == NULL || !starts_with(desc->name, "ed.") ||
            (desc->flags & SAG_CMD_INTERNAL) != 0U)
            continue;
        name = desc->name + 3U;
        /* A deferred command's help already reads "Sprint 23: open a
         * file", so the detail column names the sprint for free. */
        deferred = (desc->flags & SAG_CMD_DEFERRED) != 0U;
        (void)candidate_add(&matches, stem, name, name, desc->help, false,
                            deferred);
        id = sag_cmd_lookup(desc->name, (u32)strlen(desc->name));
        entry = sag_cmd_entry(id);
        if (entry != NULL && entry->abbrev != NULL &&
            strcmp(entry->abbrev, name) != 0)
            (void)candidate_add(&matches, stem, entry->abbrev,
                                entry->abbrev, name, false, deferred);
    }
    return candidate_finish(req, SAG_COMP_CMD, &matches, out);
}

static const char *buffer_name(const Buffer *buffer)
{
    const char *slash;

    if (buffer->path == NULL)
        return "[No Name]";
    slash = strrchr(buffer->path, '/');
    return slash == NULL ? buffer->path : slash + 1U;
}

static u32 enumerate_buffers(const CompReq *req, Vec_CompItem *out)
{
    CandidateVec matches = {0};
    Ed *ed = req->ed;
    const char *stem = req->stem;
    u32 i;

    for (i = 0U; i < ed->ws.nbufs; i++) {
        const Buffer *buffer = ed->ws.bufs[i];
        const char *name = buffer_name(buffer);
        char number[32];

        (void)candidate_add(&matches, stem, name, name, buffer->path, false,
                            false);
        (void)snprintf(number, sizeof(number), "%u", (unsigned)(i + 1U));
        (void)candidate_add(&matches, stem, number, number, name, false,
                            false);
    }
    return candidate_finish(req, SAG_COMP_BUFFER, &matches, out);
}

static bool unsafe_path_byte(unsigned char ch)
{
    static const char unsafe[] = " \t\"'\\$&|;<>()*?[]%";

    return ch < 0x20U || ch == 0x7fU || strchr(unsafe, (int)ch) != NULL;
}

char *sag_comp_quote(Arena *arena, const char *text)
{
    Bytebuf quoted;
    const unsigned char *p;
    bool unsafe = false;

    for (p = (const unsigned char *)text; *p != '\0'; p++) {
        if (unsafe_path_byte(*p)) {
            unsafe = true;
            break;
        }
    }
    if (!unsafe)
        return arena_strdup(arena, text);
    bytebuf_init(&quoted);
    bytebuf_push_u8(&quoted, (u8)'"');
    for (p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            bytebuf_push_u8(&quoted, (u8)'\\');
            bytebuf_push_u8(&quoted, *p);
        } else if (*p == '\n') {
            bytebuf_append(&quoted, "\\n", 2U);
        } else if (*p == '\r') {
            bytebuf_push_u8(&quoted, (u8)' ');
        } else if (*p == '\t') {
            bytebuf_append(&quoted, "\\t", 2U);
        } else if (*p == '%') {
            bytebuf_append(&quoted, "\\%", 2U);
        } else {
            bytebuf_push_u8(&quoted, *p);
        }
    }
    bytebuf_push_u8(&quoted, (u8)'"');
    {
        char *result = arena_strndup(arena, (const char *)quoted.data,
                                     quoted.len);
        bytebuf_free(&quoted);
        return result;
    }
}

static char *join2(const char *left, const char *right)
{
    size_t a = strlen(left);
    size_t b = strlen(right);
    char *joined;

    if (a > SIZE_MAX - b - 1U)
        SAG_BUG("completion path size overflow");
    joined = sag_xmalloc(a + b + 1U);
    (void)memcpy(joined, left, a);
    (void)memcpy(joined + a, right, b + 1U);
    return joined;
}

static char *expand_home_head(const char *head)
{
    const char *slash;
    const char *home;
    struct passwd *pw;
    char *user;
    size_t user_len;
    char *expanded;

    if (head[0] != '~')
        return join2("", head);
    slash = strchr(head, '/');
    user_len = slash == NULL ? strlen(head + 1U) :
               (size_t)(slash - (head + 1U));
    if (user_len == 0U) {
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            pw = getpwuid(getuid());
            home = pw == NULL ? NULL : pw->pw_dir;
        }
    } else {
        user = sag_xmalloc(user_len + 1U);
        (void)memcpy(user, head + 1U, user_len);
        user[user_len] = '\0';
        pw = getpwnam(user);
        free(user);
        home = pw == NULL ? NULL : pw->pw_dir;
    }
    if (home == NULL)
        return NULL;
    expanded = join2(home, slash == NULL ? "" : slash);
    return expanded;
}

static bool path_is_dir(const char *scan_dir, const char *name,
                        unsigned char entry_dtype)
{
    unsigned char dtype = force_dtype_unknown ? DT_UNKNOWN : entry_dtype;

    if (dtype == DT_DIR)
        return true;
    if (dtype != DT_UNKNOWN)
        return false;
    {
        char *path;
        char *with_slash;
        struct stat st;
        bool is_dir;

        with_slash = join2(scan_dir,
                           scan_dir[0] != '\0' &&
                           scan_dir[strlen(scan_dir) - 1U] == '/' ? "" : "/");
        path = join2(with_slash, name);
        free(with_slash);
        test_lstat_calls++;
        is_dir = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
        free(path);
        return is_dir;
    }
}

static int path_candidate_cmp(const void *left, const void *right, void *ctx)
{
    const PathCandidate *a = left;
    const PathCandidate *b = right;
    size_t la;
    size_t lb;

    (void)ctx;
    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    la = strlen(a->name);
    lb = strlen(b->name);
    if (la != lb)
        return la < lb ? -1 : 1;
    return strcmp(a->name, b->name);
}

static int path_candidate_rank_cmp(const PathCandidate *a,
                                   const PathCandidate *b)
{
    return path_candidate_cmp(a, b, NULL);
}

static void path_candidate_swap(PathCandidate *a, PathCandidate *b)
{
    PathCandidate tmp = *a;

    *a = *b;
    *b = tmp;
}

/* Paths have unique names within one directory.  Keep a worst-first heap of
 * the best SAG_COMP_MAX ranks.  Its filename buffers are reused when the
 * root is replaced, so a large directory allocates and materializes only the
 * candidates the menu can display. */
static void path_heap_push(PathCandidateVec *heap, const char *name,
                           i32 score, unsigned char dtype, const FzMatch *m)
{
    PathCandidate item;
    size_t at;
    size_t need = strlen(name) + 1U;

    item.name = sag_xmalloc(need);
    (void)memcpy(item.name, name, need);
    item.cap = need;
    item.score = score;
    item.dtype = dtype;
    item.m = *m;
    PathCandidateVec_push(heap, item);
    at = heap->len - 1U;
    while (at != 0U) {
        size_t parent = (at - 1U) / 2U;

        if (path_candidate_rank_cmp(&heap->data[at],
                                    &heap->data[parent]) <= 0)
            break;
        path_candidate_swap(&heap->data[at], &heap->data[parent]);
        at = parent;
    }
}

static void path_heap_replace_worst(PathCandidateVec *heap, const char *name,
                                    i32 score, unsigned char dtype,
                                    const FzMatch *m)
{
    PathCandidate *root = &heap->data[0];
    size_t need = strlen(name) + 1U;
    size_t at = 0U;

    if (root->cap < need) {
        root->name = sag_xrealloc(root->name, need);
        root->cap = need;
    }
    (void)memcpy(root->name, name, need);
    root->score = score;
    root->dtype = dtype;
    root->m = *m;
    for (;;) {
        size_t worst = at;
        size_t left = at * 2U + 1U;
        size_t right = left + 1U;

        if (left < heap->len &&
            path_candidate_rank_cmp(&heap->data[left],
                                    &heap->data[worst]) > 0)
            worst = left;
        if (right < heap->len &&
            path_candidate_rank_cmp(&heap->data[right],
                                    &heap->data[worst]) > 0)
            worst = right;
        if (worst == at)
            break;
        path_candidate_swap(&heap->data[at], &heap->data[worst]);
        at = worst;
    }
}

static bool path_candidate_wanted(const PathCandidateVec *heap,
                                  const char *match, i32 score)
{
    PathCandidate preview;

    if (heap->len < SAG_COMP_MAX)
        return true;
    (void)memset(&preview, 0, sizeof(preview));
    preview.name = (char *)match;
    preview.score = score;
    preview.dtype = DT_UNKNOWN;
    return path_candidate_rank_cmp(&preview, &heap->data[0]) < 0;
}

static void path_candidates_dispose(PathCandidateVec *paths)
{
    size_t i;

    for (i = 0U; i < paths->len; i++)
        free(paths->data[i].name);
    PathCandidateVec_free(paths);
}

static void path_candidates_finish(const CompReq *req,
                                   PathCandidateVec *paths,
                                   const char *scan_dir, const char *head,
                                   Vec_CompItem *out)
{
    Arena *arena = req->arena;
    size_t head_len = strlen(head);
    size_t i;

    sag_sort_stable(paths->data, paths->len, sizeof(paths->data[0]),
                    path_candidate_cmp, NULL);
    out->len = 0U;
    Vec_CompItem_reserve(out, paths->len);
    for (i = 0U; i < paths->len; i++) {
        PathCandidate *path = &paths->data[i];
        bool is_dir = path_is_dir(scan_dir, path->name, path->dtype);
        char *shown = join2(head, path->name);
        char *raw;
        CompItem item;

        if (is_dir) {
            raw = join2(shown, "/");
            free(shown);
        } else {
            raw = shown;
        }
        item.text = sag_comp_quote(arena, raw);
        item.detail = NULL;
        item.kind = SAG_COMP_PATH;
        item.is_dir = is_dir;
        item.deferred = false;
        item.score = path->score;
        /*
         * Ranking saw the bare entry name; the menu draws head + name.
         * Shift the positions across the head, and give up entirely when
         * the path had to be quoted -- quoting inserts a leading `"` and
         * escapes, so there is no honest byte mapping back, and a
         * highlight on the wrong columns is worse than none.
         */
        item.m = path->m;
        item.match = arena_strdup(arena, path->name);
        if (strcmp(item.text, raw) != 0 ||
            head_len >= (size_t)SAG_COMP_NO_HIGHLIGHT) {
            item.m.n_pos = 0U;
            item.match_off = (u16)SAG_COMP_NO_HIGHLIGHT;
        } else {
            u16 p;

            item.match_off = (u16)head_len;
            for (p = 0U; p < item.m.n_pos; p++) {
                size_t at = (size_t)item.m.pos[p] + head_len;

                item.m.pos[p] = at > (size_t)UINT16_MAX ? (u16)UINT16_MAX
                                                        : (u16)at;
            }
        }
        Vec_CompItem_push(out, item);
        free(raw);
    }
    path_candidates_dispose(paths);
}

size_t sag_comp_path_head_len(const char *stem)
{
    const char *slash;

    if (stem == NULL)
        return 0U;
    slash = strrchr(stem, '/');
    return slash == NULL ? 0U : (size_t)(slash - stem) + 1U;
}

static u32 enumerate_paths(const CompReq *req, Vec_CompItem *out)
{
    PathCandidateVec paths = {0};
    Ed *ed = req->ed;
    const char *stem = req->stem;
    size_t head_len = sag_comp_path_head_len(stem);
    const char *tail = stem + head_len;
    size_t tail_len = strlen(tail);
    char *head = sag_xmalloc(head_len + 1U);
    char *expanded;
    char *scan_dir;
    DIR *dir;
    struct dirent *entry;
    u32 total = 0U;

    (void)memcpy(head, stem, head_len);
    head[head_len] = '\0';
    expanded = expand_home_head(head);
    if (expanded == NULL) {
        free(head);
        out->len = 0U;
        return 0U;
    }
    if (expanded[0] == '/')
        scan_dir = join2("", expanded);
    else {
        char *root_slash = join2(sag_ws_root(ed), "/");
        scan_dir = join2(root_slash, expanded);
        free(root_slash);
    }
    if (scan_dir[0] == '\0') {
        free(scan_dir);
        scan_dir = join2("", sag_ws_root(ed));
    }
    dir = opendir(scan_dir);
    if (dir == NULL) {
        free(scan_dir);
        free(expanded);
        free(head);
        out->len = 0U;
        return 0U;
    }
    while ((entry = readdir(dir)) != NULL) {
        FzMatch m;
        i32 score;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (entry->d_name[0] == '.' && tail[0] != '.')
            continue;
        score = comp_key(tail, tail_len, entry->d_name, &m);
        if (score == SAG_FZ_NO_MATCH)
            continue;
        if (total != UINT32_MAX)
            total++;
        if (!path_candidate_wanted(&paths, entry->d_name, score))
            continue;
        if (paths.len < SAG_COMP_MAX)
            path_heap_push(&paths, entry->d_name, score, entry->d_type, &m);
        else
            path_heap_replace_worst(&paths, entry->d_name, score,
                                    entry->d_type, &m);
    }
    (void)closedir(dir);
    path_candidates_finish(req, &paths, scan_dir, head, out);
    free(scan_dir);
    free(expanded);
    free(head);
    return total;
}

/*
 * Sprint 36 fills these in.  An empty provider is DATA, not a stub: it
 * answers "no candidates" honestly, and the options model replaces the
 * function without touching the plumbing around it.
 */
static u32 enumerate_empty(const CompReq *req, Vec_CompItem *out)
{
    (void)req;
    out->len = 0U;
    return 0U;
}

static struct {
    CompSource v[SAG_COMP_KIND__N];
    bool initialized;
} comp_registry;

static void comp_init(void)
{
    static const CompSource builtins[] = {
        {SAG_COMP_CMD, "cmd", enumerate_commands, SAG_COMP_SRC_CACHEABLE},
        /* SLOW: one opendir of a directory that may hold 10 000 entries,
         * which is why §4 keys its cache on the directory head. */
        {SAG_COMP_PATH, "path", enumerate_paths,
         SAG_COMP_SRC_CACHEABLE | SAG_COMP_SRC_SLOW},
        {SAG_COMP_BUFFER, "buffer", enumerate_buffers,
         SAG_COMP_SRC_CACHEABLE},
        {SAG_COMP_OPTION, "option", enumerate_empty,
         SAG_COMP_SRC_CACHEABLE},
        {SAG_COMP_VALUE, "value", enumerate_empty, SAG_COMP_SRC_CACHEABLE},
    };
    size_t i;

    if (comp_registry.initialized)
        return;
    comp_registry.initialized = true;
    for (i = 0U; i < SAG_ARRAY_LEN(builtins); i++)
        sag_comp_source_register(&builtins[i]);
}

void sag_comp_source_register(const CompSource *src)
{
    if (src == NULL || src->enumerate == NULL || src->name == NULL)
        SAG_BUG("completion source needs a name and an enumerator");
    if ((u32)src->kind >= (u32)SAG_COMP_KIND__N)
        SAG_BUG("completion source has an invalid kind");
    comp_registry.initialized = true;
    comp_registry.v[src->kind] = *src;
}

const CompSource *sag_comp_source(SagCompKind kind)
{
    comp_init();
    if ((u32)kind >= (u32)SAG_COMP_KIND__N ||
        comp_registry.v[kind].enumerate == NULL)
        return NULL;
    return &comp_registry.v[kind];
}

u32 sag_comp_source_count(void)
{
    u32 n = 0U;
    u32 i;

    comp_init();
    for (i = 0U; i < (u32)SAG_COMP_KIND__N; i++) {
        if (comp_registry.v[i].enumerate != NULL)
            n++;
    }
    return n;
}

u32 sag_comp_request(const CompReq *req, Vec_CompItem *out)
{
    const CompSource *source;

    if (req == NULL || out == NULL || req->ed == NULL ||
        req->stem == NULL || req->arena == NULL)
        return 0U;
    source = sag_comp_source(req->kind);
    if (source == NULL)
        return 0U;
    return source->enumerate(req, out);
}

u32 sag_comp_enumerate(Ed *ed, SagCompKind kind, const char *stem,
                       Vec_CompItem *out)
{
    CompReq req;

    (void)memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.stem = stem;
    req.ed = ed;
    /* The unbudgeted convenience form allocates from the editor arena,
     * which lives as long as the editor -- callers wanting a resettable
     * lifetime go through sag_comp_filter_run. */
    req.arena = ed == NULL ? NULL : &ed->arena;
    req.budget_us = 0; /* a Tab: the user is waiting, take the time */
    return sag_comp_request(&req, out);
}

/* ---------------------------------------------------------------- */
/* Sprint 18.5 §4: the live filter                                  */
/* ---------------------------------------------------------------- */

static u32 test_enumerate_calls;

void sag_comp_test_reset_enumerate_count(void)
{
    test_enumerate_calls = 0U;
}

u32 sag_comp_test_enumerate_count(void)
{
    return test_enumerate_calls;
}

void sag_comp_filter_init(CompFilter *f)
{
    if (f == NULL)
        return;
    (void)memset(f, 0, sizeof(*f));
}

void sag_comp_filter_invalidate(CompFilter *f)
{
    if (f == NULL)
        return;
    /* base's strings belong to the caller's arena, so only the vector
     * and the two keys are ours to release. */
    f->base.len = 0U;
    free(f->head);
    free(f->pattern);
    f->head = NULL;
    f->pattern = NULL;
    f->valid = false;
    f->capped = false;
    f->total = 0U;
}

void sag_comp_filter_free(CompFilter *f)
{
    if (f == NULL)
        return;
    sag_comp_filter_invalidate(f);
    Vec_CompItem_free(&f->base);
}

static char *dup_range(const char *s, size_t len)
{
    char *copy = sag_xmalloc(len + 1U);

    if (len != 0U)
        (void)memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static int filter_item_cmp(const void *left, const void *right, void *ctx)
{
    const CompItem *a = left;
    const CompItem *b = right;
    size_t la;
    size_t lb;
    int by_bytes;

    (void)ctx;
    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    la = strlen(a->match);
    lb = strlen(b->match);
    if (la != lb)
        return la < lb ? -1 : 1;
    by_bytes = strcmp(a->match, b->match);
    if (by_bytes != 0)
        return by_bytes < 0 ? -1 : 1;
    return strcmp(a->text, b->text);
}

/*
 * Re-score the cached set against a longer pattern.
 *
 * Legal only when the base was NOT capped.  A capped base holds the
 * SAG_COMP_MAX best matches for the OLD pattern, and an entry that the
 * cap cut can still be among the best for the new one -- narrowing over
 * it would silently lose rows, which is the exact failure the CompReq
 * budget comment warns about.  So `filter_reusable` refuses, and the
 * cost is one more opendir in a directory big enough to cap.
 */
static u32 filter_rerank(CompFilter *f, const char *pattern,
                         Vec_CompItem *out)
{
    size_t pattern_len = strlen(pattern);
    size_t i;

    out->len = 0U;
    Vec_CompItem_reserve(out, f->base.len);
    for (i = 0U; i < f->base.len; i++) {
        CompItem item = f->base.data[i];
        FzMatch m;
        i32 score = comp_key(pattern, pattern_len, item.match, &m);

        if (score == SAG_FZ_NO_MATCH)
            continue;
        item.score = score;
        if (item.match_off == (u16)SAG_COMP_NO_HIGHLIGHT) {
            item.m.n_pos = 0U;
        } else {
            u16 p;

            for (p = 0U; p < m.n_pos; p++) {
                size_t at = (size_t)m.pos[p] + item.match_off;

                m.pos[p] = at > (size_t)UINT16_MAX ? (u16)UINT16_MAX
                                                   : (u16)at;
            }
            item.m = m;
        }
        Vec_CompItem_push(out, item);
    }
    sag_sort_stable(out->data, out->len, sizeof(out->data[0]),
                    filter_item_cmp, NULL);
    return out->len > UINT32_MAX ? UINT32_MAX : (u32)out->len;
}

static bool filter_reusable(const CompFilter *f, SagCompKind kind,
                            const char *head, const char *pattern)
{
    size_t old_len;

    if (!f->valid || f->kind != kind || f->capped)
        return false;
    if (strcmp(f->head, head) != 0)
        return false;
    /*
     * Appending can only SHRINK a subsequence match, never widen it, so
     * re-scoring the survivors is exact rather than an approximation.
     * Backspacing or a mid-token edit can widen it, and then the cached
     * set is missing candidates that never matched the longer pattern.
     */
    old_len = strlen(f->pattern);
    return strncmp(pattern, f->pattern, old_len) == 0;
}

u32 sag_comp_filter_run(Ed *ed, CompFilter *f, Arena *arena,
                        const SagCompQuery *q, i64 budget_us,
                        Vec_CompItem *out)
{
    size_t head_len;
    const char *pattern;
    char *head;
    bool reuse;

    if (ed == NULL || f == NULL || q == NULL || out == NULL ||
        q->stem == NULL) {
        if (out != NULL)
            out->len = 0U;
        return 0U;
    }
    /*
     * Only a path has a directory head; every other source ranks the
     * whole stem.  sag_comp_path_head_len is the ONE split rule, shared
     * with the path source itself.
     */
    head_len = q->kind == SAG_COMP_PATH ? sag_comp_path_head_len(q->stem)
                                        : 0U;
    pattern = q->stem + head_len;
    head = dup_range(q->stem, head_len);
    reuse = filter_reusable(f, q->kind, head, pattern);
    if (!reuse) {
        CompReq req;

        /* The arena backs the cached set, so it can only be reset when
         * that set is being replaced. */
        arena_free_all(arena);
        f->base.len = 0U;
        (void)memset(&req, 0, sizeof(req));
        req.kind = q->kind;
        req.stem = q->stem;
        req.ed = ed;
        req.arena = arena;
        req.budget_us = budget_us;
        test_enumerate_calls++;
        f->total = sag_comp_request(&req, &f->base);
        f->capped = f->base.len < (size_t)f->total;
        free(f->head);
        free(f->pattern);
        f->head = head;
        f->pattern = dup_range(pattern, strlen(pattern));
        f->kind = q->kind;
        f->valid = true;
        head = NULL;
    }
    free(head);
    {
        u32 matched = filter_rerank(f, pattern, out);

        /* A fresh enumerate knows the true pre-cap total; a narrowed
         * pass counted every survivor itself, and its base was uncapped
         * by construction, so the count is exact either way. */
        return reuse ? matched : f->total;
    }
}

bool sag_comp_kind_for(const CmdEntry *entry, u32 token_index,
                       SagCompKind *kind)
{
    const char *spec;
    size_t len;
    size_t arg;
    char code;
    bool repeats;

    if (kind == NULL)
        return false;
    if (token_index == 0U) {
        *kind = SAG_COMP_CMD;
        return true;
    }
    if (entry == NULL || entry->argspec == NULL)
        return false;
    spec = entry->argspec;
    len = strlen(spec);
    repeats = len > 0U && spec[len - 1U] == '*';
    if (repeats)
        len--;
    arg = (size_t)token_index - 1U;
    if (arg >= len) {
        if (!repeats || len == 0U)
            return false;
        code = spec[len - 1U];
    } else {
        code = spec[arg];
    }
    if (code == 'f')
        *kind = SAG_COMP_PATH;
    else if (code == 'b')
        *kind = SAG_COMP_BUFFER;
    else if (code == 'o')
        *kind = SAG_COMP_OPTION;
    else if (code == 'v')
        *kind = SAG_COMP_VALUE;
    else
        return false;
    return true;
}

bool sag_comp_query(Ed *ed, const char *line, size_t len, size_t cursor,
                    Arena *scratch, SagCompQuery *out)
{
    CmdParsePoint point;
    const CmdEntry *entry = NULL;
    SagCompKind kind;

    if (out == NULL || scratch == NULL ||
        !sag_cmd_parse_point(ed, line, len, cursor, scratch, &point))
        return false;
    if (point.token_index != 0U) {
        if (!point.command_known)
            return false;
        entry = sag_cmd_entry(point.command);
    }
    if (!sag_comp_kind_for(entry, point.token_index, &kind))
        return false;
    out->kind = kind;
    out->source = sag_comp_source(kind);
    out->stem = point.stem;
    out->replace = point.token;
    return true;
}

char *sag_comp_lcp(Arena *arena, const Vec_CompItem *items)
{
    size_t common;
    size_t i;

    if (items == NULL || items->len == 0U)
        return arena_strdup(arena, "");
    common = strlen(items->data[0].text);
    for (i = 1U; i < items->len && common > 0U; i++) {
        size_t j = 0U;
        const char *text = items->data[i].text;

        while (j < common && text[j] != '\0' &&
               text[j] == items->data[0].text[j])
            j++;
        common = j;
    }
    while (common > 0U &&
           !sag_utf8_is_boundary((const u8 *)items->data[0].text,
                                 strlen(items->data[0].text), common))
        common--;
    return arena_strndup(arena, items->data[0].text, common);
}

void sag_comp_test_force_dtype_unknown(bool force)
{
    force_dtype_unknown = force;
    test_lstat_calls = 0U;
}

u32 sag_comp_test_lstat_count(void)
{
    return test_lstat_calls;
}
