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
#include "edit/option.h"
#ifndef YEW_WITH_PLUGINS
#define YEW_WITH_PLUGINS 0
#endif
#if YEW_WITH_PLUGINS
#include "mod/plug/plug.h"
#endif
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

/*
 * No FzMatch here, deliberately.  Match positions are 130 bytes and the
 * heap sifts by copying whole structs -- carrying them made a 10 000-entry
 * re-rank move ~40 MB through path_candidate_swap, which cost more than
 * the readdir it followed.  Only the <= YEW_COMP_MAX survivors need
 * positions, and rescoring those at finish time is a few hundred scans.
 */
typedef struct {
    char *name;
    size_t len; /* strlen(name); `cap` only grows, so it cannot stand in */
    size_t cap;
    i32 score;
    unsigned char dtype;
} PathCandidate;

VEC_DECL(PathCandidateVec, PathCandidate);

static bool force_dtype_unknown;
static u32 test_lstat_calls;
/*
 * Test-only override of YEW_COMP_LIST_MAX.  The overflow path is
 * otherwise only reachable with 50 000 entries on disk, which is not a
 * unit test — and it is the path that has to move the cache key across
 * an invalidate rather than free it, so leaving it uncovered would leave
 * a use-after-free to the sanitizer lane and a big enough directory.
 */
static u32 test_list_max;

static bool starts_with(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);

    return strncmp(s, prefix, n) == 0;
}

/*
 * Sprint 18.5 §2: one candidate's rank key, the single-item form of
 * yew_fz_rank's ordering.  The streaming path enumerator cannot hand
 * yew_fz_rank a whole array -- it never holds one -- so the tier rule
 * lives here as well, in one helper both shapes call.
 *
 * `match` is always a bare name (a command name, a buffer label, a
 * directory entry), never a path with slashes, so basename() would be
 * the identity and path_mode is irrelevant.
 */
static i32 comp_key(const char *stem, size_t stem_len, const char *match,
                    FzMatch *m)
{
    i32 score = yew_fz_score(stem, (u32)stem_len, match,
                             (u32)strlen(match), m);

    if (score == YEW_FZ_NO_MATCH)
        return YEW_FZ_NO_MATCH;
    if (score >= 5000)
        return score > INT32_MAX - YEW_FZ_BASENAME_TIER
                   ? INT32_MAX
                   : score + (i32)YEW_FZ_BASENAME_TIER;
    return score;
}

/*
 * Descending by key, then shorter-then-memcmp -- yew_fz_rank's tie rule,
 * so the two orderings cannot drift apart.
 *
 * Unlike yew_fz_rank, ties are broken by name even for the EMPTY stem.
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
            yew_xfree(v->data[i].match);
        yew_xfree(v->data[i].text);
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
    /* YEW_FZ_NO_MATCH, not `< 0`: the length penalty makes a genuine
     * match score negative, and a `< 0` test silently drops the longest
     * real candidates. */
    if (score == YEW_FZ_NO_MATCH)
        return false;
    item.text = yew_xmalloc(strlen(text) + 1U);
    (void)strcpy(item.text, text);
    if (strcmp(match, text) == 0) {
        item.match = item.text;
    } else {
        item.match = yew_xmalloc(strlen(match) + 1U);
        (void)strcpy(item.match, match);
    }
    item.detail = detail;
    item.is_dir = is_dir;
    item.deferred = deferred;
    item.score = score;
    CandidateVec_push(v, item);
    return true;
}

static u32 candidate_finish(const CompReq *req, YewCompKind kind,
                            CandidateVec *matches, Vec_CompItem *out)
{
    Arena *arena = req->arena;
    size_t i;
    size_t keep;
    u32 total = matches->len > UINT32_MAX ? UINT32_MAX : (u32)matches->len;

    yew_sort_stable(matches->data, matches->len, sizeof(matches->data[0]),
                    candidate_cmp, NULL);
    out->len = 0U;
    keep = matches->len < YEW_COMP_MAX ? matches->len : YEW_COMP_MAX;
    Vec_CompItem_reserve(out, keep);
    for (i = 0U; i < keep; i++) {
        const Candidate *src = &matches->data[i];
        CompItem item;

        item.text = kind == YEW_COMP_PATH ?
                    yew_comp_quote(arena, src->text) :
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
                             : (u16)YEW_COMP_NO_HIGHLIGHT;
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

    for (i = 0U; i < yew_cmd_count(); i++) {
        const CmdDesc *desc = yew_cmd_at(i);
        CmdId id = {i + 1U};
        const CmdEntry *entry = yew_cmd_entry(id);
        const char *name;
        bool deferred;

        /* yew_cmd_at exposes stable raw rows.  Completion is an active
         * inventory, so an origin-teardown tombstone must disappear. */
        if (desc == NULL || entry == NULL ||
            !starts_with(desc->name, "ed.") ||
            (desc->flags & YEW_CMD_INTERNAL) != 0U)
            continue;
        name = desc->name + 3U;
        /* A deferred command's help names its owning sprint, so the detail
         * column carries the reason without a second lookup. */
        deferred = (desc->flags & YEW_CMD_DEFERRED) != 0U;
        (void)candidate_add(&matches, stem, name, name, desc->help, false,
                            deferred);
        if (entry->abbrev != NULL &&
            strcmp(entry->abbrev, name) != 0)
            (void)candidate_add(&matches, stem, entry->abbrev,
                                entry->abbrev, name, false, deferred);
    }
    return candidate_finish(req, YEW_COMP_CMD, &matches, out);
}

static u32 enumerate_plugins(const CompReq *req, Vec_CompItem *out)
{
    CandidateVec matches = {0};
#if YEW_WITH_PLUGINS
    u32 i;

    for (i = 0U; i < yew_plug_count(req->ed); i++) {
        Plug *plug = yew_plug_at(req->ed, i);

        /* Only winners are command-addressable.  Shadowed records are
         * retained for diagnostics, but completing them would offer a
         * name whose lifecycle command resolves to a different row. */
        if (plug == NULL || !plug->winner || plug->mf.name_text == NULL)
            continue;
        (void)candidate_add(&matches, req->stem, plug->mf.name_text,
                            plug->mf.name_text,
                            plug->mf.desc == NULL ? "plugin" : plug->mf.desc,
                            false, false);
    }
#else
    (void)req;
#endif
    return candidate_finish(req, YEW_COMP_PLUGIN, &matches, out);
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
    return candidate_finish(req, YEW_COMP_BUFFER, &matches, out);
}

static bool unsafe_path_byte(unsigned char ch)
{
    static const char unsafe[] = " \t\"'\\$&|;<>()*?[]%";

    return ch < 0x20U || ch == 0x7fU || strchr(unsafe, (int)ch) != NULL;
}

char *yew_comp_quote(Arena *arena, const char *text)
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
        YEW_BUG("completion path size overflow");
    joined = yew_xmalloc(a + b + 1U);
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
        user = yew_xmalloc(user_len + 1U);
        (void)memcpy(user, head + 1U, user_len);
        user[user_len] = '\0';
        pw = getpwnam(user);
        yew_xfree(user);
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
        yew_xfree(with_slash);
        test_lstat_calls++;
        is_dir = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
        yew_xfree(path);
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
    /* Carried, not measured: this runs ~85 000 times per keystroke in a
     * 10 000-entry directory, and two strlen calls per comparison were a
     * measurable slice of the re-rank. */
    la = a->len;
    lb = b->len;
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
 * the best YEW_COMP_MAX ranks.  Its filename buffers are reused when the
 * root is replaced, so a large directory allocates and materializes only the
 * candidates the menu can display. */
static void path_heap_push(PathCandidateVec *heap, const char *name,
                           i32 score, unsigned char dtype)
{
    PathCandidate item;
    size_t at;
    size_t need = strlen(name) + 1U;

    item.name = yew_xmalloc(need);
    (void)memcpy(item.name, name, need);
    item.len = need - 1U;
    item.cap = need;
    item.score = score;
    item.dtype = dtype;
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
                                    i32 score, unsigned char dtype)
{
    PathCandidate *root = &heap->data[0];
    size_t need = strlen(name) + 1U;
    size_t at = 0U;

    if (root->cap < need) {
        root->name = yew_xrealloc(root->name, need);
        root->cap = need;
    }
    (void)memcpy(root->name, name, need);
    root->len = need - 1U;
    root->score = score;
    root->dtype = dtype;
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

    if (heap->len < YEW_COMP_MAX)
        return true;
    (void)memset(&preview, 0, sizeof(preview));
    preview.name = (char *)match;
    preview.len = strlen(match);
    preview.score = score;
    preview.dtype = DT_UNKNOWN;
    return path_candidate_rank_cmp(&preview, &heap->data[0]) < 0;
}

static void path_candidates_dispose(PathCandidateVec *paths)
{
    size_t i;

    for (i = 0U; i < paths->len; i++)
        yew_xfree(paths->data[i].name);
    PathCandidateVec_free(paths);
}

static void path_candidates_finish(const CompReq *req,
                                   PathCandidateVec *paths,
                                   const char *scan_dir, const char *head,
                                   Vec_CompItem *out)
{
    Arena *arena = req->arena;
    size_t head_len = strlen(head);
    const char *tail = req->stem + yew_comp_path_head_len(req->stem);
    size_t tail_len = strlen(tail);
    size_t i;

    yew_sort_stable(paths->data, paths->len, sizeof(paths->data[0]),
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
            yew_xfree(shown);
        } else {
            raw = shown;
        }
        item.text = yew_comp_quote(arena, raw);
        item.detail = NULL;
        item.kind = YEW_COMP_PATH;
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
        /* Rescored here rather than carried through the heap; see
         * PathCandidate.  Same pattern, same name, so the same result. */
        (void)comp_key(tail, tail_len, path->name, &item.m);
        item.match = arena_strdup(arena, path->name);
        if (strcmp(item.text, raw) != 0 ||
            head_len >= (size_t)YEW_COMP_NO_HIGHLIGHT) {
            item.m.n_pos = 0U;
            item.match_off = (u16)YEW_COMP_NO_HIGHLIGHT;
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
        yew_xfree(raw);
    }
    path_candidates_dispose(paths);
}

size_t yew_comp_path_head_len(const char *stem)
{
    const char *slash;

    if (stem == NULL)
        return 0U;
    slash = strrchr(stem, '/');
    return slash == NULL ? 0U : (size_t)(slash - stem) + 1U;
}

/*
 * Sprint 18.5 §4 / DoD 10: the cached directory listing.
 *
 * The ranked set is keyed on the PATTERN and so cannot answer a narrowed
 * one once it caps -- which made every keystroke past the directory head
 * pay another opendir over 10 000 entries.  This is keyed on the
 * DIRECTORY instead and holds every name in it, so any pattern re-ranks
 * from memory and the scan happens once.
 *
 * A file static, not per-CmdLine state: the core is single-threaded and
 * there is one prompt, and the alternative threads a path-only cache
 * through a CompSource interface that four other kinds share.
 */
/*
 * Names live in ONE growing blob addressed by offset, not in n separate
 * allocations.  A 10 000-entry directory is 10 000 mallocs the other way,
 * and that alone cost more than the readdir it was meant to save -- the
 * keystroke that scans has to stay inside the same 5 ms as the ones that
 * do not.  Offsets rather than pointers because the blob moves when it
 * grows.
 */
typedef struct DirListing {
    char *dir; /* the scan_dir these names came from */
    char *blob;
    size_t blob_len;
    size_t blob_cap;
    u32 *offs;
    u8 *dtypes;
    u32 n;
    /*
     * The directory had more than YEW_COMP_LIST_MAX entries, so the blob
     * is a PREFIX of it and narrowing from it would silently lose rows.
     * Nothing is cached in that case; the scan streams as it used to.
     */
    bool overflow;
    /*
     * THE SCAN IS RESUMABLE, and the open handle is what makes it so.
     *
     * Reading a 10 000-entry directory costs ~4 ms on a CI runner, which
     * is most of invariant 4's whole 5 ms keypress budget — and it landed
     * on ONE keystroke, the first character typed after the argument's
     * space.  perf-cmdcomp measured 4.737 ms there against 0.8 ms for
     * every key after it, and eventually tipped over.
     *
     * Moving the read to a different key only moves the spike, so it is
     * SLICED instead: each call reads for at most a time budget and
     * returns, and yew_cmdline_comp_tick resumes it on the idle path
     * exactly as Sprint 26 §7.2 does for the picker.  `dir` stays open
     * between slices because readdir has no seek that could resume a
     * closed one cheaply, and reopening would re-read from the top.
     *
     * `cap` lives here rather than on the stack for the same reason: it
     * is scan state now, not a local of one loop.
     */
    DIR *dir_handle;
    u32 cap;
    bool complete;
} DirListing;

static DirListing comp_listing;
static u64 comp_opendirs;

/*
 * Sprint 57.18 §2: an optional per-entry filter, applied while the slice
 * is being read.
 *
 * The path source keeps every name and decides what a name MEANS later;
 * the $PATH source has to know during the scan, because "is this an
 * executable" costs syscalls and those syscalls belong inside the slice
 * budget that the scan is already yielding on.  One readdir loop with a
 * predicate, rather than a second copy of the slicing rule.
 */
typedef bool (*ListingKeep)(const char *scan_dir, const char *name,
                            unsigned char dtype);

static const char *listing_name(const DirListing *l, u32 i)
{
    return l->blob + l->offs[i];
}

u64 yew_comp_listing_opendirs(void)
{
    return comp_opendirs;
}

static void listing_dispose(DirListing *l)
{
    if (l->dir_handle != NULL)
        (void)closedir(l->dir_handle);
    yew_xfree(l->blob);
    yew_xfree(l->offs);
    yew_xfree(l->dtypes);
    yew_xfree(l->dir);
    (void)memset(l, 0, sizeof(*l));
}

static void exec_cache_free(void);

void yew_comp_listing_invalidate(void)
{
    listing_dispose(&comp_listing);
    /*
     * The $PATH cache dies with the directory listing, for the same
     * reason and at the same moment: this is the "the prompt closed,
     * start over" hook, and a cache that outlived it would answer a
     * later prompt with a $PATH that has since changed.
     */
    exec_cache_free();
}

static char *dup_cstr(const char *s)
{
    size_t len = strlen(s) + 1U;
    char *copy = yew_xmalloc(len);

    (void)memcpy(copy, s, len);
    return copy;
}

static bool listing_push(DirListing *l, const char *name, u8 dtype,
                         u32 *cap)
{
    size_t len = strlen(name) + 1U;

    if (l->n == *cap) {
        u32 next = *cap == 0U ? 256U : *cap * 2U;
        u32 limit = test_list_max != 0U ? test_list_max
                                        : (u32)YEW_COMP_LIST_MAX;

        if (next > limit)
            next = limit;
        if (next == *cap)
            return false;
        l->offs = yew_xrealloc(l->offs, (size_t)next * sizeof(*l->offs));
        l->dtypes = yew_xrealloc(l->dtypes,
                                 (size_t)next * sizeof(*l->dtypes));
        *cap = next;
    }
    if (l->blob_len + len > l->blob_cap) {
        size_t next = l->blob_cap == 0U ? 8192U : l->blob_cap * 2U;

        while (next < l->blob_len + len)
            next *= 2U;
        l->blob = yew_xrealloc(l->blob, next);
        l->blob_cap = next;
    }
    /* Offsets are u32; YEW_COMP_LIST_MAX names of any sane length stay
     * far inside that, but a blob past 4 GiB would silently wrap. */
    if (l->blob_len > (size_t)UINT32_MAX)
        return false;
    l->offs[l->n] = (u32)l->blob_len;
    (void)memcpy(l->blob + l->blob_len, name, len);
    l->blob_len += len;
    l->dtypes[l->n] = dtype;
    l->n++;
    return true;
}

static i64 comp_now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (i64)ts.tv_sec * 1000000 + (i64)ts.tv_nsec / 1000;
}

typedef enum ListingState {
    /* Could not be opened, or held more than YEW_COMP_LIST_MAX entries:
     * nothing is cached and the caller streams the directory instead. */
    LISTING_UNUSABLE,
    /* Usable as far as it goes, with more still to read. */
    LISTING_PARTIAL,
    LISTING_COMPLETE
} ListingState;

/*
 * Read `scan_dir` into the cache, for at most `slice_us`.
 *
 * `slice_us <= 0` means "no limit", which is what a Tab asks for: the
 * user pressed a key and is waiting for an exact answer, so the scan
 * runs to the end however long it takes.  A LIVE keystroke passes a
 * slice and gets whatever fits, and the idle tick brings the rest.
 *
 * Resuming is why the handle is held open across calls (see DirListing).
 * The directory is opened ONCE per scan no matter how many slices it
 * takes — perf-cmdcomp asserts opendirs=1 and would catch a version that
 * reopened per slice.
 */
static ListingState listing_step(DirListing *l, const char *scan_dir,
                                 i64 slice_us, ListingKeep keep)
{
    i64 started;
    struct dirent *entry;
    u32 checked = 0U;

    if (l->dir != NULL && strcmp(l->dir, scan_dir) == 0) {
        if (l->overflow)
            return LISTING_UNUSABLE;
        if (l->complete)
            return LISTING_COMPLETE;
    } else {
        DIR *dir;

        listing_dispose(l);
        dir = opendir(scan_dir);
        comp_opendirs++;
        if (dir == NULL)
            return LISTING_UNUSABLE;
        l->dir_handle = dir;
        l->dir = dup_cstr(scan_dir);
    }
    started = slice_us > 0 ? comp_now_us() : 0;
    while ((entry = readdir(l->dir_handle)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        /* Dot files are kept and filtered at RANK time: whether they are
         * wanted depends on the pattern, which changes per keystroke, and
         * a listing that already dropped them could not answer ".git". */
        checked++;
        if (keep != NULL && !keep(l->dir, entry->d_name,
                                  (unsigned char)entry->d_type))
            goto tick;
        if (!listing_push(l, entry->d_name, (u8)entry->d_type, &l->cap)) {
            /*
             * Too big to hold.  Drop the partial blob rather than let a
             * PREFIX of the directory masquerade as all of it — narrowing
             * from it would silently lose rows.
             *
             * The key is MOVED across the dispose rather than freed and
             * re-duplicated: yew_comp_listing_advance resumes by passing
             * l->dir straight back in, so `scan_dir` can BE this
             * pointer, and disposing it here would leave the
             * re-duplication reading freed memory.
             */
            char *key = l->dir;

            l->dir = NULL;
            listing_dispose(l);
            l->dir = key;
            l->overflow = true;
            l->complete = true;
            return LISTING_UNUSABLE;
        }
tick:
        /* Clock read every 256 entries, not every one: the syscall would
         * otherwise cost more than the readdir it is timing.  Counted
         * over entries READ rather than entries KEPT, because a filtered
         * entry still cost the stat that the budget is bounding. */
        if (slice_us > 0 && (checked & 0xFFU) == 0U &&
            comp_now_us() - started >= slice_us)
            return LISTING_PARTIAL;
    }
    (void)closedir(l->dir_handle);
    l->dir_handle = NULL;
    l->complete = true;
    return LISTING_COMPLETE;
}

static bool listing_pending(const DirListing *l)
{
    return l->dir_handle != NULL && !l->complete;
}

static bool exec_scan_pending(void);
static bool exec_scan_advance(i64 slice_us);

bool yew_comp_listing_pending(void)
{
    return listing_pending(&comp_listing) || exec_scan_pending();
}

bool yew_comp_listing_advance(i64 slice_us)
{
    char *dir = comp_listing.dir;

    /*
     * ONE idle-tick driver for both sliced scans.  yew_loop_deadline
     * stops sleeping on yew_comp_listing_pending, so a second pending
     * predicate the tick did not drain would busy-loop the event loop.
     */
    if (listing_pending(&comp_listing)) {
        /* `dir` is the cache's own key, and listing_step compares against
         * it by value; passing it back in is the "same directory, keep
         * going" case by construction. */
        if (listing_step(&comp_listing, dir, slice_us, NULL) ==
            LISTING_PARTIAL)
            return true;
        return exec_scan_pending();
    }
    return exec_scan_advance(slice_us);
}

/* Rank one candidate name into the bounded heap.  Shared by the cached
 * path and the streaming fallback so the two cannot drift. */
static void path_rank_one(PathCandidateVec *paths, const char *name,
                          u8 dtype, const char *tail, size_t tail_len,
                          u32 *total)
{
    i32 score;

    if (name[0] == '.' && tail[0] != '.')
        return;
    /* NULL: positions are recomputed for survivors in
     * path_candidates_finish, so the scan never fills one. */
    score = comp_key(tail, tail_len, name, NULL);
    if (score == YEW_FZ_NO_MATCH)
        return;
    if (*total != UINT32_MAX)
        (*total)++;
    if (!path_candidate_wanted(paths, name, score))
        return;
    if (paths->len < YEW_COMP_MAX)
        path_heap_push(paths, name, score, dtype);
    else
        path_heap_replace_worst(paths, name, score, dtype);
}

static u32 enumerate_paths(const CompReq *req, Vec_CompItem *out)
{
    PathCandidateVec paths = {0};
    Ed *ed = req->ed;
    const char *stem = req->stem;
    size_t head_len = yew_comp_path_head_len(stem);
    const char *tail = stem + head_len;
    size_t tail_len = strlen(tail);
    char *head = yew_xmalloc(head_len + 1U);
    char *expanded;
    char *scan_dir;
    DIR *dir;
    struct dirent *entry;
    u32 total = 0U;

    (void)memcpy(head, stem, head_len);
    head[head_len] = '\0';
    expanded = expand_home_head(head);
    if (expanded == NULL) {
        yew_xfree(head);
        out->len = 0U;
        return 0U;
    }
    if (expanded[0] == '/')
        scan_dir = join2("", expanded);
    else {
        char *root_slash = join2(yew_ws_root(ed), "/");
        scan_dir = join2(root_slash, expanded);
        yew_xfree(root_slash);
    }
    if (scan_dir[0] == '\0') {
        yew_xfree(scan_dir);
        scan_dir = join2("", yew_ws_root(ed));
    }
    /* A fresh request also RETIRES the cache: whoever asked for one did
     * so because the directory may have changed, and a later cached read
     * must not go on trusting the listing they distrusted. */
    if (!req->allow_cache)
        yew_comp_listing_invalidate();
    if (req->allow_cache &&
        listing_step(&comp_listing, scan_dir, req->budget_us, NULL) !=
            LISTING_UNUSABLE) {
        /*
         * The common path: at most one slice of readdir, then a re-rank
         * of what is held.  A complete listing does no syscall at all.
         *
         * Ranking a PARTIAL listing is deliberate — the menu shows the
         * best of what has been read rather than nothing, and
         * yew_cmdline_comp_tick brings the rest on the idle path.  The
         * alternative, blocking until the directory is fully read, is
         * the 4 ms keystroke this slicing exists to remove.
         */
        u32 i;

        for (i = 0U; i < comp_listing.n; i++)
            path_rank_one(&paths, listing_name(&comp_listing, i),
                          comp_listing.dtypes[i], tail, tail_len, &total);
    } else {
        /* Unopenable, or too big to hold: stream it as before.  A
         * directory this size is Sprint 26's problem, not the prompt's. */
        dir = opendir(scan_dir);
        comp_opendirs++;
        if (dir == NULL) {
            yew_xfree(scan_dir);
            yew_xfree(expanded);
            yew_xfree(head);
            out->len = 0U;
            return 0U;
        }
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            path_rank_one(&paths, entry->d_name, (u8)entry->d_type, tail,
                          tail_len, &total);
        }
        (void)closedir(dir);
    }
    path_candidates_finish(req, &paths, scan_dir, head, out);
    yew_xfree(scan_dir);
    yew_xfree(expanded);
    yew_xfree(head);
    return total;
}

/* ---------------------------------------------------------------- */
/* Sprint 57.18 §2: the $PATH executable source                     */
/* ---------------------------------------------------------------- */

/* Defined with the live filter below; the two are the same "copy this
 * many bytes and NUL-terminate" and a second one would be noise. */
static char *dup_range(const char *s, size_t len);

/*
 * One PATH element and what was read out of it.
 *
 * The listing is the SAME sliced DirListing the path source uses, one
 * per element, so a $PATH with a slow network mount on it yields to the
 * live budget and resumes on the idle tick exactly as a big directory
 * does.  (dev, ino, mtime) is the cache key the sprint asks for: an
 * element whose stat is unchanged is never re-read, however often the
 * source is asked.  Second-granularity mtime is the known limit -- a
 * binary installed within the same second as the last scan is missed
 * until the next prompt, which is the same bargain every shell's command
 * hash makes, and strictly better than the shell's because a new prompt
 * always re-stats.
 */
typedef struct ExecDir {
    DirListing listing;
    char *dir; /* the element as $PATH spells it; "" is "." per POSIX */
    dev_t dev;
    ino_t ino;
    time_t mtime;
    bool stat_ok;
    /* Read to the end, or refused (unopenable, or past
     * YEW_COMP_LIST_MAX).  Either way there is nothing more to read. */
    bool done;
} ExecDir;

/*
 * The merged, deduplicated name set the ranker sees.
 *
 * Deduplicated by basename with the FIRST element winning, which is what
 * the shell would actually run.  Held as one blob addressed by offset
 * for the same reason DirListing is: a few thousand names is a few
 * thousand mallocs the other way, and that alone costs more than the
 * readdir it is meant to amortize.
 *
 * `slots` is an open-addressed index over the blob.  It exists so the
 * merge is linear rather than quadratic, and so the <= YEW_COMP_MAX
 * survivors can be told which element they came from without a second
 * pass over every name.
 */
typedef struct ExecCache {
    char *path_env;
    ExecDir *dirs;
    u32 n;
    char *blob;
    size_t blob_len;
    size_t blob_cap;
    u32 *offs;
    u32 *from;
    u32 n_names;
    u32 *slots;
    u32 nslots;
    bool merged;
} ExecCache;

static ExecCache exec_cache;

/* A #define, not an enum: an enumerator has to fit in an int, and this
 * is deliberately the whole u32 range's top value. */
#define EXEC_NO_SLOT 0xFFFFFFFFU

static void exec_merged_free(void)
{
    yew_xfree(exec_cache.blob);
    yew_xfree(exec_cache.offs);
    yew_xfree(exec_cache.from);
    yew_xfree(exec_cache.slots);
    exec_cache.blob = NULL;
    exec_cache.offs = NULL;
    exec_cache.from = NULL;
    exec_cache.slots = NULL;
    exec_cache.blob_len = 0U;
    exec_cache.blob_cap = 0U;
    exec_cache.n_names = 0U;
    exec_cache.nslots = 0U;
    exec_cache.merged = false;
}

static void exec_dirs_free(ExecDir *dirs, u32 n)
{
    u32 i;

    for (i = 0U; i < n; i++) {
        listing_dispose(&dirs[i].listing);
        yew_xfree(dirs[i].dir);
    }
    yew_xfree(dirs);
}

static void exec_cache_free(void)
{
    exec_dirs_free(exec_cache.dirs, exec_cache.n);
    exec_cache.dirs = NULL;
    exec_cache.n = 0U;
    yew_xfree(exec_cache.path_env);
    exec_cache.path_env = NULL;
    exec_merged_free();
}

static bool exec_scan_pending(void)
{
    u32 i;

    for (i = 0U; i < exec_cache.n; i++) {
        if (!exec_cache.dirs[i].done)
            return true;
    }
    return false;
}

/*
 * Is this entry something the shell would run?
 *
 * `access(X_OK)` is the authority on "executable by THIS user" -- a mode
 * bit test would offer root's binaries to everyone -- and the stat is
 * what excludes a directory, which is X_OK by virtue of being
 * searchable.  Two syscalls per entry is the honest price; they are paid
 * inside listing_step's slice, so a 3 000-entry /usr/bin yields to the
 * live budget rather than landing on one keystroke.
 */
static bool exec_keep(const char *scan_dir, const char *name,
                      unsigned char dtype)
{
    char *with_slash;
    char *path;
    struct stat st;
    bool ok;

    if (force_dtype_unknown)
        dtype = DT_UNKNOWN;
    /* A d_type that is already decisive saves the syscalls.  DT_REG,
     * DT_LNK and DT_UNKNOWN all still need the stat: a symlink's target
     * decides, and DT_UNKNOWN says nothing. */
    if (dtype != DT_REG && dtype != DT_LNK && dtype != DT_UNKNOWN)
        return false;
    with_slash = join2(scan_dir,
                       scan_dir[0] != '\0' &&
                       scan_dir[strlen(scan_dir) - 1U] == '/' ? "" : "/");
    path = join2(with_slash, name);
    yew_xfree(with_slash);
    ok = stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
         access(path, X_OK) == 0;
    yew_xfree(path);
    return ok;
}

/*
 * Read one slice across the elements that still need reading.
 *
 * The budget is spent across ELEMENTS, not per element: `$PATH` is one
 * answer, and giving each of a dozen directories its own 1500 us would
 * make the first keystroke after `:!` cost eighteen milliseconds.
 */
static ListingState exec_scan_step(i64 slice_us)
{
    i64 started = slice_us > 0 ? comp_now_us() : 0;
    u32 i;

    for (i = 0U; i < exec_cache.n; i++) {
        ExecDir *d = &exec_cache.dirs[i];
        ListingState state;
        i64 left = 0;

        if (d->done)
            continue;
        if (slice_us > 0) {
            left = slice_us - (comp_now_us() - started);
            if (left <= 0)
                return LISTING_PARTIAL;
        }
        state = listing_step(&d->listing, d->dir, left, exec_keep);
        exec_cache.merged = false;
        if (state == LISTING_PARTIAL)
            return LISTING_PARTIAL;
        /*
         * COMPLETE or UNUSABLE, and both mean "nothing more here".  An
         * element that cannot be opened contributes nothing, which is
         * what the shell does with it too; one holding more than
         * YEW_COMP_LIST_MAX executables is not a $PATH element, and the
         * path source's streaming fallback exists for a directory the
         * user asked about by name, which this is not.
         */
        d->done = true;
    }
    return LISTING_COMPLETE;
}

static bool exec_scan_advance(i64 slice_us)
{
    if (!exec_scan_pending())
        return false;
    return exec_scan_step(slice_us) == LISTING_PARTIAL;
}

/* FNV-1a over a NUL-terminated name.  A hash, not a checksum: it indexes
 * the merge's dedup slots and nothing is stored under it. */
static u32 exec_hash(const char *s)
{
    u32 h = 2166136261U;

    while (*s != '\0') {
        h ^= (u32)(unsigned char)*s++;
        h *= 16777619U;
    }
    return h;
}

static const char *exec_name(u32 i)
{
    return exec_cache.blob + exec_cache.offs[i];
}

/* The slot this name occupies or would occupy.  Linear probing over a
 * power-of-two table kept at most half full, so the probe is short. */
static u32 exec_slot_of(const char *name)
{
    u32 mask = exec_cache.nslots - 1U;
    u32 at = exec_hash(name) & mask;

    for (;;) {
        u32 held = exec_cache.slots[at];

        if (held == EXEC_NO_SLOT || strcmp(exec_name(held), name) == 0)
            return at;
        at = (at + 1U) & mask;
    }
}

static bool exec_lookup_from(const char *name, u32 *from)
{
    u32 at;
    u32 held;

    if (exec_cache.nslots == 0U)
        return false;
    at = exec_slot_of(name);
    held = exec_cache.slots[at];
    if (held == EXEC_NO_SLOT)
        return false;
    *from = exec_cache.from[held];
    return true;
}

static void exec_merge_push(const char *name, u32 from)
{
    size_t len = strlen(name) + 1U;
    u32 at;

    at = exec_slot_of(name);
    if (exec_cache.slots[at] != EXEC_NO_SLOT)
        return; /* an earlier $PATH element already won this name */
    if (exec_cache.blob_len + len > exec_cache.blob_cap) {
        size_t next = exec_cache.blob_cap == 0U ? 8192U
                                                : exec_cache.blob_cap * 2U;

        while (next < exec_cache.blob_len + len)
            next *= 2U;
        exec_cache.blob = yew_xrealloc(exec_cache.blob, next);
        exec_cache.blob_cap = next;
    }
    exec_cache.offs[exec_cache.n_names] = (u32)exec_cache.blob_len;
    exec_cache.from[exec_cache.n_names] = from;
    (void)memcpy(exec_cache.blob + exec_cache.blob_len, name, len);
    exec_cache.blob_len += len;
    exec_cache.slots[at] = exec_cache.n_names;
    exec_cache.n_names++;
}

/*
 * Merge every element's listing into one deduplicated set.
 *
 * Rebuilt whenever a scan slice added names and never otherwise, so the
 * steady state -- the user typing into an already-scanned $PATH -- does
 * this exactly once and then re-ranks from memory.
 */
static void exec_merge(void)
{
    size_t total = 0U;
    size_t slots = 16U;
    u32 i;
    u32 j;

    if (exec_cache.merged)
        return;
    exec_merged_free();
    for (i = 0U; i < exec_cache.n; i++)
        total += exec_cache.dirs[i].listing.n;
    exec_cache.merged = true;
    if (total == 0U)
        return;
    /* At most half full: linear probing degrades sharply past that, and
     * the table is thrown away with the cache. */
    while (slots < total * 2U)
        slots *= 2U;
    exec_cache.nslots = (u32)slots;
    exec_cache.slots = yew_xmalloc(slots * sizeof(*exec_cache.slots));
    for (i = 0U; i < exec_cache.nslots; i++)
        exec_cache.slots[i] = EXEC_NO_SLOT;
    exec_cache.offs = yew_xmalloc(total * sizeof(*exec_cache.offs));
    exec_cache.from = yew_xmalloc(total * sizeof(*exec_cache.from));
    for (i = 0U; i < exec_cache.n; i++) {
        const DirListing *l = &exec_cache.dirs[i].listing;

        for (j = 0U; j < l->n; j++)
            exec_merge_push(listing_name(l, j), i);
    }
}

/* How many ':'-separated elements `env` names.  An empty element is the
 * current directory, exactly as POSIX says, so it still counts. */
static u32 exec_path_count(const char *env)
{
    u32 n = 1U;
    const char *at;

    for (at = env; *at != '\0'; at++) {
        if (*at == ':')
            n++;
    }
    return n;
}

static void exec_dir_stat(ExecDir *d)
{
    struct stat st;

    d->stat_ok = stat(d->dir, &st) == 0;
    if (!d->stat_ok) {
        d->dev = 0;
        d->ino = 0;
        d->mtime = 0;
        return;
    }
    d->dev = st.st_dev;
    d->ino = st.st_ino;
    d->mtime = st.st_mtime;
}

static bool exec_dir_unchanged(const ExecDir *d)
{
    struct stat st;

    if (!d->stat_ok)
        return false;
    if (stat(d->dir, &st) != 0)
        return false;
    return st.st_dev == d->dev && st.st_ino == d->ino &&
           st.st_mtime == d->mtime;
}

/*
 * Bring the element vector in line with the current $PATH.
 *
 * A changed $PATH rebuilds the vector wholesale -- the answer it
 * produces is a function of the whole variable, and salvage that got the
 * ORDER wrong would offer the name the shell would not run.  What is
 * salvaged is the expensive part: an element that appears in both and
 * whose (dev, ino, mtime) still matches keeps its listing, so changing
 * $PATH costs one opendir per genuinely new directory and none for the
 * rest.  Elements are also re-stat'd when $PATH did NOT change, which is
 * what makes a binary installed while the editor is running appear.
 */
static void exec_reconcile(void)
{
    const char *env = getenv("PATH");
    ExecDir *fresh;
    u32 n;
    u32 i;
    u32 at;
    const char *scan;

    if (env == NULL)
        env = "";
    if (exec_cache.path_env != NULL &&
        strcmp(exec_cache.path_env, env) == 0) {
        bool changed = false;

        for (i = 0U; i < exec_cache.n; i++) {
            if (exec_dir_unchanged(&exec_cache.dirs[i]))
                continue;
            listing_dispose(&exec_cache.dirs[i].listing);
            exec_dir_stat(&exec_cache.dirs[i]);
            exec_cache.dirs[i].done = false;
            changed = true;
        }
        if (changed)
            exec_cache.merged = false;
        return;
    }
    if (env[0] == '\0') {
        /* No $PATH is no candidates, not the compiled-in default: the
         * shell yew spawns inherits this same empty variable. */
        exec_cache_free();
        exec_cache.path_env = dup_cstr(env);
        return;
    }
    n = exec_path_count(env);
    fresh = yew_xcalloc(n, sizeof(*fresh));
    scan = env;
    for (i = 0U; i < n; i++) {
        const char *end = strchr(scan, ':');
        size_t len = end == NULL ? strlen(scan) : (size_t)(end - scan);

        fresh[i].dir = len == 0U ? dup_cstr(".") : dup_range(scan, len);
        scan = end == NULL ? scan + len : end + 1U;
        exec_dir_stat(&fresh[i]);
        for (at = 0U; at < exec_cache.n; at++) {
            ExecDir *old = &exec_cache.dirs[at];

            if (old->dir == NULL || strcmp(old->dir, fresh[i].dir) != 0)
                continue;
            if (!old->stat_ok || !fresh[i].stat_ok ||
                old->dev != fresh[i].dev || old->ino != fresh[i].ino ||
                old->mtime != fresh[i].mtime)
                break;
            /* Unchanged on disk: move the listing rather than re-read it.
             * The old slot is left with an empty listing, which
             * exec_dirs_free disposes harmlessly. */
            fresh[i].listing = old->listing;
            fresh[i].done = old->done;
            (void)memset(&old->listing, 0, sizeof(old->listing));
            break;
        }
    }
    exec_dirs_free(exec_cache.dirs, exec_cache.n);
    exec_cache.dirs = fresh;
    exec_cache.n = n;
    yew_xfree(exec_cache.path_env);
    exec_cache.path_env = dup_cstr(env);
    exec_merged_free();
}

static void exec_candidates_finish(const CompReq *req,
                                   PathCandidateVec *paths,
                                   Vec_CompItem *out)
{
    Arena *arena = req->arena;
    const char *stem = req->stem;
    size_t stem_len = strlen(stem);
    size_t i;

    yew_sort_stable(paths->data, paths->len, sizeof(paths->data[0]),
                    path_candidate_cmp, NULL);
    out->len = 0U;
    Vec_CompItem_reserve(out, paths->len);
    for (i = 0U; i < paths->len; i++) {
        PathCandidate *path = &paths->data[i];
        CompItem item;
        u32 from = 0U;

        /* Quoted on the way out like a path: an executable with a space
         * in its name is legal, and the prompt must be able to read back
         * what the menu inserts (DoD 2). */
        item.text = yew_comp_quote(arena, path->name);
        item.detail = exec_lookup_from(path->name, &from) &&
                              from < exec_cache.n
                          ? arena_strdup(arena, exec_cache.dirs[from].dir)
                          : NULL;
        item.kind = (u8)YEW_COMP_EXEC;
        item.is_dir = false;
        item.deferred = false;
        item.score = path->score;
        /* Rescored for its match positions, exactly as the path source
         * does: the heap deliberately does not carry them. */
        (void)comp_key(stem, stem_len, path->name, &item.m);
        item.match = arena_strdup(arena, path->name);
        if (strcmp(item.text, item.match) != 0) {
            /* Quoting inserted a `"` and escapes, so there is no honest
             * byte mapping back to the ranked name. */
            item.m.n_pos = 0U;
            item.match_off = (u16)YEW_COMP_NO_HIGHLIGHT;
        } else {
            item.match_off = 0U;
        }
        Vec_CompItem_push(out, item);
    }
    path_candidates_dispose(paths);
}

static u32 enumerate_exec(const CompReq *req, Vec_CompItem *out)
{
    PathCandidateVec paths = {0};
    const char *stem = req->stem;
    size_t stem_len = strlen(stem);
    u32 total = 0U;
    u32 i;

    /* A fresh request RETIRES the cache, as the path source's does:
     * whoever asked for one did so because $PATH or its directories may
     * have changed underneath. */
    if (!req->allow_cache)
        exec_cache_free();
    exec_reconcile();
    (void)exec_scan_step(req->allow_cache ? req->budget_us : 0);
    exec_merge();
    /*
     * Ranking a PARTIAL scan is deliberate, exactly as it is for a
     * partial directory listing: the menu shows the best of what has
     * been read and yew_cmdline_comp_tick brings the rest on the idle
     * path.  Blocking until a dozen directories are read is the
     * multi-millisecond keystroke the slicing exists to remove.
     */
    for (i = 0U; i < exec_cache.n_names; i++)
        path_rank_one(&paths, exec_name(i), (u8)DT_REG, stem, stem_len,
                      &total);
    exec_candidates_finish(req, &paths, out);
    return total;
}

/*
 * Sprint 36 fills these in.  An empty provider is DATA, not a stub: it
 * answers "no candidates" honestly, and the options model replaces the
 * function without touching the plumbing around it.
 */
static u32 enumerate_options(const CompReq *req, Vec_CompItem *out)
{
    CandidateVec matches = {0};
    const OptProvider *provider = yew_opt_provider(req->ed);
    const char **names;
    u32 count;
    u32 i;

    count = provider->list(req->ed, NULL, 0U);
    names = yew_xcalloc(count == 0U ? 1U : count, sizeof(*names));
    count = provider->list(req->ed, names, count);
    for (i = 0U; i < count; i++) {
        const OptDesc *desc = yew_opt_desc_for(req->ed, names[i],
                                               (u32)strlen(names[i]));

        (void)candidate_add(&matches, req->stem, names[i], names[i],
                            desc == NULL ? "option" : desc->help,
                            false, false);
    }
    yew_xfree(names);
    return candidate_finish(req, YEW_COMP_OPTION, &matches, out);
}

static u32 enumerate_option_values(const CompReq *req, Vec_CompItem *out)
{
    static const char *const values[] = {
        "false", "true", "off", "abs", "rel", "both", "gcol",
        "gcol_ccol", "yank", "all", "unnamed"
    };
    CandidateVec matches = {0};
    u32 i;

    for (i = 0U; i < (u32)YEW_ARRAY_LEN(values); i++)
        (void)candidate_add(&matches, req->stem, values[i], values[i],
                            "option value", false, false);
    return candidate_finish(req, YEW_COMP_VALUE, &matches, out);
}

static struct {
    CompSource v[YEW_COMP_KIND__N];
    bool initialized;
} comp_registry;

static void comp_init(void)
{
    static const CompSource builtins[] = {
        {YEW_COMP_CMD, "cmd", enumerate_commands, YEW_COMP_SRC_CACHEABLE},
        /* SLOW: one opendir of a directory that may hold 10 000 entries,
         * which is why §4 keys its cache on the directory head. */
        {YEW_COMP_PATH, "path", enumerate_paths,
         YEW_COMP_SRC_CACHEABLE | YEW_COMP_SRC_SLOW},
        {YEW_COMP_BUFFER, "buffer", enumerate_buffers,
         YEW_COMP_SRC_CACHEABLE},
        {YEW_COMP_OPTION, "option", enumerate_options,
         YEW_COMP_SRC_CACHEABLE},
        {YEW_COMP_VALUE, "value", enumerate_option_values,
         YEW_COMP_SRC_CACHEABLE},
        {YEW_COMP_PLUGIN, "plugin", enumerate_plugins,
         YEW_COMP_SRC_CACHEABLE},
        /* Sprint 57.18 §2.  SLOW for the same reason the path source is:
         * a $PATH element may hold thousands of entries, and each one
         * costs a stat before it can be called executable. */
        {YEW_COMP_EXEC, "exec", enumerate_exec,
         YEW_COMP_SRC_CACHEABLE | YEW_COMP_SRC_SLOW},
    };
    size_t i;

    if (comp_registry.initialized)
        return;
    comp_registry.initialized = true;
    for (i = 0U; i < YEW_ARRAY_LEN(builtins); i++)
        yew_comp_source_register(&builtins[i]);
}

void yew_comp_source_register(const CompSource *src)
{
    if (src == NULL || src->enumerate == NULL || src->name == NULL)
        YEW_BUG("completion source needs a name and an enumerator");
    if ((u32)src->kind >= (u32)YEW_COMP_KIND__N)
        YEW_BUG("completion source has an invalid kind");
    comp_registry.initialized = true;
    comp_registry.v[src->kind] = *src;
}

const CompSource *yew_comp_source(YewCompKind kind)
{
    comp_init();
    if ((u32)kind >= (u32)YEW_COMP_KIND__N ||
        comp_registry.v[kind].enumerate == NULL)
        return NULL;
    return &comp_registry.v[kind];
}

u32 yew_comp_source_count(void)
{
    u32 n = 0U;
    u32 i;

    comp_init();
    for (i = 0U; i < (u32)YEW_COMP_KIND__N; i++) {
        if (comp_registry.v[i].enumerate != NULL)
            n++;
    }
    return n;
}

u32 yew_comp_request(const CompReq *req, Vec_CompItem *out)
{
    const CompSource *source;

    if (req == NULL || out == NULL || req->ed == NULL ||
        req->stem == NULL || req->arena == NULL)
        return 0U;
    source = yew_comp_source(req->kind);
    if (source == NULL)
        return 0U;
    return source->enumerate(req, out);
}

u32 yew_comp_enumerate(Ed *ed, YewCompKind kind, const char *stem,
                       Vec_CompItem *out)
{
    CompReq req;

    (void)memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.stem = stem;
    req.ed = ed;
    /* The unbudgeted convenience form allocates from the editor arena,
     * which lives as long as the editor -- callers wanting a resettable
     * lifetime go through yew_comp_filter_run. */
    req.arena = ed == NULL ? NULL : &ed->arena;
    req.budget_us = 0; /* a Tab: the user is waiting, take the time */
    /* Fresh by construction -- see CompReq.allow_cache. */
    req.allow_cache = false;
    return yew_comp_request(&req, out);
}

/* ---------------------------------------------------------------- */
/* Sprint 18.5 §4: the live filter                                  */
/* ---------------------------------------------------------------- */

static u32 test_enumerate_calls;

void yew_comp_test_reset_enumerate_count(void)
{
    test_enumerate_calls = 0U;
}

u32 yew_comp_test_enumerate_count(void)
{
    return test_enumerate_calls;
}

void yew_comp_filter_init(CompFilter *f)
{
    if (f == NULL)
        return;
    (void)memset(f, 0, sizeof(*f));
}

void yew_comp_filter_invalidate(CompFilter *f)
{
    if (f == NULL)
        return;
    /* base's strings belong to the caller's arena, so only the vector
     * and the two keys are ours to release. */
    f->base.len = 0U;
    yew_xfree(f->head);
    yew_xfree(f->pattern);
    f->head = NULL;
    f->pattern = NULL;
    f->valid = false;
    f->capped = false;
    f->total = 0U;
}

void yew_comp_filter_free(CompFilter *f)
{
    if (f == NULL)
        return;
    yew_comp_filter_invalidate(f);
    Vec_CompItem_free(&f->base);
}

static char *dup_range(const char *s, size_t len)
{
    char *copy = yew_xmalloc(len + 1U);

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
 * YEW_COMP_MAX best matches for the OLD pattern, and an entry that the
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

        if (score == YEW_FZ_NO_MATCH)
            continue;
        item.score = score;
        if (item.match_off == (u16)YEW_COMP_NO_HIGHLIGHT) {
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
    yew_sort_stable(out->data, out->len, sizeof(out->data[0]),
                    filter_item_cmp, NULL);
    return out->len > UINT32_MAX ? UINT32_MAX : (u32)out->len;
}

static bool filter_reusable(const CompFilter *f, YewCompKind kind,
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

u32 yew_comp_filter_run(Ed *ed, CompFilter *f, Arena *arena,
                        const YewCompQuery *q, i64 budget_us,
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
     * whole stem.  yew_comp_path_head_len is the ONE split rule, shared
     * with the path source itself.
     */
    head_len = q->kind == YEW_COMP_PATH ? yew_comp_path_head_len(q->stem)
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
        req.allow_cache = true;
        test_enumerate_calls++;
        f->total = yew_comp_request(&req, &f->base);
        f->capped = f->base.len < (size_t)f->total;
        yew_xfree(f->head);
        yew_xfree(f->pattern);
        f->head = head;
        f->pattern = dup_range(pattern, strlen(pattern));
        f->kind = q->kind;
        f->valid = true;
        head = NULL;
    }
    yew_xfree(head);
    {
        u32 matched = filter_rerank(f, pattern, out);

        /* A fresh enumerate knows the true pre-cap total; a narrowed
         * pass counted every survivor itself, and its base was uncapped
         * by construction, so the count is exact either way. */
        return reuse ? matched : f->total;
    }
}

const CompItem *yew_comp_sole(const Vec_CompItem *items, YewCompKind kind)
{
    if (items == NULL || items->len != 1U)
        return NULL;
    if (kind != YEW_COMP_KIND__N && items->data[0].kind != (u8)kind)
        return NULL;
    return &items->data[0];
}

bool yew_comp_kind_for(const CmdEntry *entry, u32 token_index,
                       bool bang_body, YewCompKind *kind)
{
    const char *spec;
    size_t len;
    size_t arg;
    char code;
    bool repeats;

    if (kind == NULL)
        return false;
    /* Sprint 57.18 §3; see the header for why this precedes the argspec
     * rather than being spelled as a new argspec letter. */
    if (bang_body) {
        *kind = token_index == 0U ? YEW_COMP_EXEC : YEW_COMP_PATH;
        return true;
    }
    if (token_index == 0U) {
        *kind = YEW_COMP_CMD;
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
        *kind = YEW_COMP_PATH;
    else if (code == 'b')
        *kind = YEW_COMP_BUFFER;
    else if (code == 'o')
        *kind = YEW_COMP_OPTION;
    else if (code == 'v')
        *kind = YEW_COMP_VALUE;
    else if (code == 'p')
        *kind = YEW_COMP_PLUGIN;
    else
        return false;
    return true;
}

bool yew_comp_query_at(Ed *ed, const CmdParsePoint *point,
                       YewCompQuery *out)
{
    const CmdEntry *entry = NULL;
    YewCompKind kind;

    (void)ed;
    if (out == NULL || point == NULL)
        return false;
    /* A bang body needs no resolved command: `:!` never resolves one
     * (loose_name stops at the bang), and the body's own word index is
     * the whole question. */
    if (!point->bang_body && point->token_index != 0U) {
        if (!point->command_known)
            return false;
        entry = yew_cmd_entry(point->command);
    }
    if (!yew_comp_kind_for(entry, point->token_index, point->bang_body,
                           &kind))
        return false;
    out->kind = kind;
    out->source = yew_comp_source(kind);
    out->stem = point->stem;
    out->replace = point->token;
    return true;
}

bool yew_comp_query(Ed *ed, const char *line, size_t len, size_t cursor,
                    Arena *scratch, YewCompQuery *out)
{
    CmdParsePoint point;

    if (out == NULL || scratch == NULL ||
        !yew_cmd_parse_point(ed, line, len, cursor, scratch, &point))
        return false;
    return yew_comp_query_at(ed, &point, out);
}

char *yew_comp_lcp(Arena *arena, const Vec_CompItem *items)
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
           !yew_utf8_is_boundary((const u8 *)items->data[0].text,
                                 strlen(items->data[0].text), common))
        common--;
    return arena_strndup(arena, items->data[0].text, common);
}

void yew_comp_test_set_list_max(u32 max)
{
    test_list_max = max;
    yew_comp_listing_invalidate();
}

void yew_comp_test_force_dtype_unknown(bool force)
{
    force_dtype_unknown = force;
    test_lstat_calls = 0U;
}

u32 yew_comp_test_lstat_count(void)
{
    return test_lstat_calls;
}
