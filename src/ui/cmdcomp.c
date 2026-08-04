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
    bool prefix;
    i32 score;
} Candidate;

VEC_DECL(CandidateVec, Candidate);

typedef struct {
    char *name;
    size_t cap;
    i32 score;
    unsigned char dtype;
    bool prefix;
} PathCandidate;

VEC_DECL(PathCandidateVec, PathCandidate);

static bool force_dtype_unknown;
static u32 test_lstat_calls;

static bool starts_with(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);

    return strncmp(s, prefix, n) == 0;
}

static i32 comp_score_n(const char *stem, size_t stem_len, const char *cand)
{
    size_t si = 0U;
    size_t ci;
    i64 score = 0;
    size_t previous = SIZE_MAX;

    if (stem_len == 0U)
        return 0;
    for (ci = 0U; cand[ci] != '\0' && si < stem_len; ci++) {
        unsigned char ch = (unsigned char)cand[ci];

        if (ch != (unsigned char)stem[si])
            continue;
        score++;
        if (ci == 0U || cand[ci - 1U] == '_' || cand[ci - 1U] == '/' ||
            cand[ci - 1U] == '.' || cand[ci - 1U] == '-')
            score += 6;
        else if (isupper(ch) && islower((unsigned char)cand[ci - 1U]))
            score += 4;
        if (previous != SIZE_MAX && ci == previous + 1U)
            score += 3;
        previous = ci;
        si++;
    }
    if (si != stem_len)
        return -1;
    if (ci == stem_len) {
        score += stem_len > (size_t)INT32_MAX ? INT32_MAX : (i64)stem_len;
    }
    return score > INT32_MAX ? INT32_MAX : (i32)score;
}

i32 sag_comp_score(const char *stem, const char *cand)
{
    if (stem == NULL || cand == NULL)
        return -1;
    return comp_score_n(stem, strlen(stem), cand);
}

static int candidate_cmp(const void *left, const void *right, void *ctx)
{
    const Candidate *a = left;
    const Candidate *b = right;
    int by_name;

    (void)ctx;
    if (a->prefix != b->prefix)
        return a->prefix ? -1 : 1;
    if (!a->prefix && a->score != b->score)
        return a->score > b->score ? -1 : 1;
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
                          const char *detail, bool is_dir)
{
    i32 score = sag_comp_score(stem, match);
    Candidate item;

    if (score < 0)
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
    item.prefix = starts_with(match, stem);
    item.score = score;
    CandidateVec_push(v, item);
    return true;
}

static u32 candidate_finish(Ed *ed, SagCompKind kind, CandidateVec *matches,
                            Vec_CompItem *out)
{
    Arena *arena = ed->cmdline.active ? &ed->cmdline.comp_arena :
                                        &ed->arena;
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
        item.score = src->score;
        Vec_CompItem_push(out, item);
    }
    candidate_dispose(matches);
    return total;
}

static u32 enumerate_commands(Ed *ed, const char *stem, Vec_CompItem *out)
{
    CandidateVec matches = {0};
    u32 i;

    for (i = 0U; i < sag_cmd_count(); i++) {
        const CmdDesc *desc = sag_cmd_at(i);
        CmdId id;
        const CmdEntry *entry;
        const char *name;

        if (desc == NULL || !starts_with(desc->name, "ed.") ||
            (desc->flags & SAG_CMD_INTERNAL) != 0U)
            continue;
        name = desc->name + 3U;
        (void)candidate_add(&matches, stem, name, name, desc->help, false);
        id = sag_cmd_lookup(desc->name, (u32)strlen(desc->name));
        entry = sag_cmd_entry(id);
        if (entry != NULL && entry->abbrev != NULL &&
            strcmp(entry->abbrev, name) != 0)
            (void)candidate_add(&matches, stem, entry->abbrev,
                                entry->abbrev, name, false);
    }
    return candidate_finish(ed, SAG_COMP_CMD, &matches, out);
}

static const char *buffer_name(const Buffer *buffer)
{
    const char *slash;

    if (buffer->path == NULL)
        return "[No Name]";
    slash = strrchr(buffer->path, '/');
    return slash == NULL ? buffer->path : slash + 1U;
}

static u32 enumerate_buffers(Ed *ed, const char *stem, Vec_CompItem *out)
{
    CandidateVec matches = {0};
    u32 i;

    for (i = 0U; i < ed->ws.nbufs; i++) {
        const Buffer *buffer = ed->ws.bufs[i];
        const char *name = buffer_name(buffer);
        char number[32];

        (void)candidate_add(&matches, stem, name, name, buffer->path, false);
        (void)snprintf(number, sizeof(number), "%u", (unsigned)(i + 1U));
        (void)candidate_add(&matches, stem, number, number, name, false);
    }
    return candidate_finish(ed, SAG_COMP_BUFFER, &matches, out);
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

    (void)ctx;
    if (a->prefix != b->prefix)
        return a->prefix ? -1 : 1;
    if (!a->prefix && a->score != b->score)
        return a->score > b->score ? -1 : 1;
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
                           bool prefix, i32 score, unsigned char dtype)
{
    PathCandidate item;
    size_t at;
    size_t need = strlen(name) + 1U;

    item.name = sag_xmalloc(need);
    (void)memcpy(item.name, name, need);
    item.cap = need;
    item.score = score;
    item.dtype = dtype;
    item.prefix = prefix;
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
                                    bool prefix, i32 score,
                                    unsigned char dtype)
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
    root->prefix = prefix;
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
                                  const char *match, bool prefix, i32 score)
{
    PathCandidate preview;

    if (heap->len < SAG_COMP_MAX)
        return true;
    preview = (PathCandidate){(char *)match, 0U, score, DT_UNKNOWN, prefix};
    return path_candidate_rank_cmp(&preview, &heap->data[0]) < 0;
}

static void path_candidates_dispose(PathCandidateVec *paths)
{
    size_t i;

    for (i = 0U; i < paths->len; i++)
        free(paths->data[i].name);
    PathCandidateVec_free(paths);
}

static void path_candidates_finish(Ed *ed, PathCandidateVec *paths,
                                   const char *scan_dir, const char *head,
                                   const char *tail, size_t tail_len,
                                   Vec_CompItem *out)
{
    Arena *arena = ed->cmdline.active ? &ed->cmdline.comp_arena :
                                        &ed->arena;
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

        if (path->prefix)
            path->score = comp_score_n(tail, tail_len, path->name);
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
        item.score = path->score;
        Vec_CompItem_push(out, item);
        free(raw);
    }
    path_candidates_dispose(paths);
}

static u32 enumerate_paths(Ed *ed, const char *stem, Vec_CompItem *out)
{
    PathCandidateVec paths = {0};
    const char *slash = strrchr(stem, '/');
    size_t head_len = slash == NULL ? 0U : (size_t)(slash - stem) + 1U;
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
        i32 score;
        bool prefix;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (entry->d_name[0] == '.' && tail[0] != '.')
            continue;
        prefix = strncmp(entry->d_name, tail, tail_len) == 0;
        if (prefix)
            score = 0;
        else {
            score = comp_score_n(tail, tail_len, entry->d_name);
            if (score < 0)
                continue;
        }
        if (total != UINT32_MAX)
            total++;
        if (!path_candidate_wanted(&paths, entry->d_name, prefix, score))
            continue;
        if (paths.len < SAG_COMP_MAX)
            path_heap_push(&paths, entry->d_name, prefix, score,
                           entry->d_type);
        else
            path_heap_replace_worst(&paths, entry->d_name, prefix, score,
                                    entry->d_type);
    }
    (void)closedir(dir);
    path_candidates_finish(ed, &paths, scan_dir, head, tail, tail_len, out);
    free(scan_dir);
    free(expanded);
    free(head);
    return total;
}

static u32 enumerate_empty(Ed *ed, const char *stem, Vec_CompItem *out)
{
    (void)ed;
    (void)stem;
    out->len = 0U;
    return 0U;
}

static const CompSource sources[] = {
    {SAG_COMP_CMD, enumerate_commands},
    {SAG_COMP_PATH, enumerate_paths},
    {SAG_COMP_BUFFER, enumerate_buffers},
    {SAG_COMP_OPTION, enumerate_empty},
    {SAG_COMP_VALUE, enumerate_empty},
};

const CompSource *sag_comp_source(SagCompKind kind)
{
    if ((u32)kind >= SAG_ARRAY_LEN(sources))
        return NULL;
    return &sources[kind];
}

u32 sag_comp_enumerate(Ed *ed, SagCompKind kind, const char *stem,
                       Vec_CompItem *out)
{
    const CompSource *source = sag_comp_source(kind);

    if (ed == NULL || stem == NULL || out == NULL || source == NULL)
        return 0U;
    return source->enumerate(ed, stem, out);
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

void sag_comp_menu_init(CompMenu *menu)
{
    *menu = (CompMenu){0};
    menu->sel = -1;
}

void sag_comp_menu_free(CompMenu *menu)
{
    Vec_CompItem_free(&menu->items);
    sag_comp_menu_init(menu);
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
