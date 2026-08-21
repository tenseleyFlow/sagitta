#include "mod/git/git.h"

#include <limits.h>
#include <stdalign.h>
#include <stdio.h>
#include <string.h>

typedef struct ByteSpan {
    const u8 *p;
    size_t n;
} ByteSpan;

static bool parse_error(GitParseErr *err, u64 off, const char *message)
{
    if (err) {
        err->off = off;
        (void)snprintf(err->message, sizeof(err->message), "%s", message);
    }
    return false;
}

static char *span_dup(Arena *a, ByteSpan s)
{
    return arena_strndup(a, (const char *)s.p, s.n);
}

static bool span_eq(ByteSpan s, const char *text)
{
    size_t n = strlen(text);
    return s.n == n && (n == 0U || memcmp(s.p, text, n) == 0);
}

static bool take_word(ByteSpan rec, size_t *at, ByteSpan *word)
{
    size_t start = *at;

    if (start >= rec.n)
        return false;
    while (*at < rec.n && rec.p[*at] != (u8)' ')
        (*at)++;
    if (*at == start || *at == rec.n)
        return false;
    word->p = rec.p + start;
    word->n = *at - start;
    (*at)++;
    return true;
}

static bool decimal_u64(ByteSpan s, u64 limit, u64 *out)
{
    u64 value = 0;
    size_t i;

    if (s.n == 0U)
        return false;
    for (i = 0; i < s.n; i++) {
        u8 digit = s.p[i];
        if (digit < (u8)'0' || digit > (u8)'9')
            return false;
        digit = (u8)(digit - (u8)'0');
        if (value > (limit - digit) / 10U)
            return false;
        value = value * 10U + digit;
    }
    *out = value;
    return true;
}

static bool decimal_i64(ByteSpan s, i64 *out)
{
    bool negative = false;
    u64 magnitude;
    u64 limit = (u64)INT64_MAX;

    if (s.n != 0U && (s.p[0] == (u8)'+' || s.p[0] == (u8)'-')) {
        negative = s.p[0] == (u8)'-';
        s.p++;
        s.n--;
    }
    if (negative)
        limit++;
    if (!decimal_u64(s, limit, &magnitude))
        return false;
    if (negative && magnitude == (u64)INT64_MAX + 1U)
        *out = INT64_MIN;
    else
        *out = negative ? -(i64)magnitude : (i64)magnitude;
    return true;
}

static bool is_hex_oid(ByteSpan s)
{
    size_t i;

    if (s.n != 40U && s.n != 64U)
        return false;
    for (i = 0; i < s.n; i++) {
        u8 c = s.p[i];
        if (!((c >= (u8)'0' && c <= (u8)'9') ||
              (c >= (u8)'a' && c <= (u8)'f') ||
              (c >= (u8)'A' && c <= (u8)'F')))
            return false;
    }
    return true;
}

static bool valid_mode(ByteSpan s)
{
    size_t i;
    if (s.n != 6U)
        return false;
    for (i = 0; i < s.n; i++)
        if (s.p[i] < (u8)'0' || s.p[i] > (u8)'7')
            return false;
    return true;
}

static bool valid_sub(ByteSpan s, bool *submodule)
{
    if (s.n != 4U)
        return false;
    if (memcmp(s.p, "N...", 4U) == 0) {
        *submodule = false;
        return true;
    }
    if (s.p[0] != (u8)'S')
        return false;
    if ((s.p[1] != (u8)'C' && s.p[1] != (u8)'.') ||
        (s.p[2] != (u8)'M' && s.p[2] != (u8)'.') ||
        (s.p[3] != (u8)'U' && s.p[3] != (u8)'.'))
        return false;
    *submodule = true;
    return true;
}

static bool valid_xy(ByteSpan xy)
{
    static const char alphabet[] = ".MTADRC";
    return xy.n == 2U && strchr(alphabet, (char)xy.p[0]) != NULL &&
           strchr(alphabet, (char)xy.p[1]) != NULL;
}

static bool valid_conflict(ByteSpan xy)
{
    static const char pairs[][3] = {"DD", "AU", "UD", "UA",
                                     "DU", "AA", "UU"};
    size_t i;
    if (xy.n != 2U)
        return false;
    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        if (memcmp(xy.p, pairs[i], 2U) == 0)
            return true;
    return false;
}

static int entry_compare(const void *left, const void *right, void *ctx)
{
    const GitEntry *a = left;
    const GitEntry *b = right;
    size_t common = a->path_len < b->path_len ? a->path_len : b->path_len;
    int cmp;
    (void)ctx;

    cmp = common ? memcmp(a->path, b->path, common) : 0;
    if (cmp != 0)
        return cmp;
    return (a->path_len > b->path_len) - (a->path_len < b->path_len);
}

static int path_compare(const void *left, const void *right, void *ctx)
{
    const GitPath *a = left;
    const GitPath *b = right;
    size_t common = a->len < b->len ? a->len : b->len;
    int cmp;
    (void)ctx;

    cmp = common ? memcmp(a->path, b->path, common) : 0;
    if (cmp != 0)
        return cmp;
    return (a->len > b->len) - (a->len < b->len);
}

static void sort_entries(Arena *a, GitEntry *items, size_t n)
{
    GitEntry *scratch;
    size_t i;
    size_t width;

    if (n < 2U)
        return;
    for (i = 1U; i < n; i++)
        if (entry_compare(&items[i], &items[i - 1U], NULL) < 0)
            break;
    if (i == n)
        return;
    scratch = arena_alloc(a, n * sizeof(*scratch), alignof(GitEntry));
    for (width = 1U; width < n;) {
        size_t lo;
        for (lo = 0U; lo < n; lo += width * 2U) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = mid + width < n ? mid + width : n;
            size_t left = lo;
            size_t right = mid;
            size_t out = lo;
            while (left < mid && right < hi) {
                if (entry_compare(&items[right], &items[left], NULL) < 0)
                    scratch[out++] = items[right++];
                else
                    scratch[out++] = items[left++];
            }
            while (left < mid)
                scratch[out++] = items[left++];
            while (right < hi)
                scratch[out++] = items[right++];
        }
        memcpy(items, scratch, n * sizeof(*items));
        if (width > n / 2U)
            break;
        width *= 2U;
    }
}

static void sort_paths(Arena *a, GitPath *items, size_t n)
{
    GitPath *scratch;
    size_t i;
    size_t width;

    if (n < 2U)
        return;
    for (i = 1U; i < n; i++)
        if (path_compare(&items[i], &items[i - 1U], NULL) < 0)
            break;
    if (i == n)
        return;
    scratch = arena_alloc(a, n * sizeof(*scratch), alignof(GitPath));
    for (width = 1U; width < n;) {
        size_t lo;
        for (lo = 0U; lo < n; lo += width * 2U) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = mid + width < n ? mid + width : n;
            size_t left = lo;
            size_t right = mid;
            size_t out = lo;
            while (left < mid && right < hi) {
                if (path_compare(&items[right], &items[left], NULL) < 0)
                    scratch[out++] = items[right++];
                else
                    scratch[out++] = items[left++];
            }
            while (left < mid)
                scratch[out++] = items[left++];
            while (right < hi)
                scratch[out++] = items[right++];
        }
        memcpy(items, scratch, n * sizeof(*items));
        if (width > n / 2U)
            break;
        width *= 2U;
    }
}

static bool count_status_records(const u8 *buf, u64 n, size_t *count,
                                 GitParseErr *err)
{
    u64 at = 0U;
    size_t total = 0U;

    while (at < n) {
        u64 record_at = at;
        const u8 *nul = memchr(buf + at, 0, (size_t)(n - at));
        size_t rec_len;
        u8 kind;
        if (!nul)
            return parse_error(err, at, "unterminated status record");
        rec_len = (size_t)(nul - (buf + at));
        if (rec_len == 0U)
            return parse_error(err, at, "empty status record");
        kind = buf[record_at];
        at = (u64)(nul - buf) + 1U;
        if (kind == (u8)'2') {
            if (at >= n || !(nul = memchr(buf + at, 0, (size_t)(n - at))))
                return parse_error(err, at, "unterminated rename source");
            at = (u64)(nul - buf) + 1U;
        }
        if (kind == (u8)'1' || kind == (u8)'2' || kind == (u8)'u' ||
            kind == (u8)'?' || kind == (u8)'!')
            total++;
    }
    *count = total;
    return true;
}

static bool parse_header(GitSnapshot *snap, ByteSpan rec, u64 off,
                         GitParseErr *err, bool *saw_upstream)
{
    ByteSpan value;
    size_t prefix;
    i64 number;

    if (rec.n >= 13U && memcmp(rec.p, "# branch.oid ", 13U) == 0) {
        value.p = rec.p + 13U;
        value.n = rec.n - 13U;
        if (!span_eq(value, "(initial)") && !is_hex_oid(value))
            return parse_error(err, off + 13U, "invalid branch oid");
        if (snap) {
            snap->unborn = span_eq(value, "(initial)");
            snap->head_oid = snap->unborn ? NULL : span_dup(&snap->a, value);
        }
        return true;
    }
    if (rec.n >= 14U && memcmp(rec.p, "# branch.head ", 14U) == 0) {
        value.p = rec.p + 14U;
        value.n = rec.n - 14U;
        if (value.n == 0U)
            return parse_error(err, off + 14U, "empty branch head");
        if (snap) {
            snap->detached = span_eq(value, "(detached)");
            snap->branch = snap->detached ? NULL : span_dup(&snap->a, value);
        }
        return true;
    }
    if (rec.n >= 18U && memcmp(rec.p, "# branch.upstream ", 18U) == 0) {
        value.p = rec.p + 18U;
        value.n = rec.n - 18U;
        if (value.n == 0U)
            return parse_error(err, off + 18U, "empty branch upstream");
        *saw_upstream = true;
        if (snap)
            snap->upstream = span_dup(&snap->a, value);
        return true;
    }
    if (rec.n >= 12U && memcmp(rec.p, "# branch.ab ", 12U) == 0) {
        size_t split = 12U;
        ByteSpan ahead;
        ByteSpan behind;
        while (split < rec.n && rec.p[split] != (u8)' ')
            split++;
        if (split == rec.n || split == 12U || split + 1U == rec.n)
            return parse_error(err, off + 12U, "invalid branch divergence");
        ahead.p = rec.p + 12U;
        ahead.n = split - 12U;
        behind.p = rec.p + split + 1U;
        behind.n = rec.n - split - 1U;
        if (ahead.p[0] != (u8)'+' || behind.p[0] != (u8)'-' ||
            !decimal_i64(ahead, &number) || number < 0 || number > INT32_MAX)
            return parse_error(err, off + 12U, "invalid ahead count");
        if (snap)
            snap->ahead = (i32)number;
        if (!decimal_i64(behind, &number) || number > 0 || number < INT32_MIN)
            return parse_error(err, off + split + 1U, "invalid behind count");
        if (snap)
            snap->behind = (i32)-number;
        return true;
    }
    prefix = sizeof("# stash ") - 1U;
    if (rec.n >= prefix && memcmp(rec.p, "# stash ", prefix) == 0) {
        value.p = rec.p + prefix;
        value.n = rec.n - prefix;
        if (!decimal_i64(value, &number) || number < 0)
            return parse_error(err, off + prefix, "invalid stash count");
    }
    return true;
}

static bool parse_entry(Arena *a, ByteSpan rec, ByteSpan orig, GitEntry *out,
                        u64 off, GitParseErr *err)
{
    ByteSpan fields[10];
    ByteSpan path;
    size_t at = 2U;
    size_t i;
    size_t fixed;
    bool submodule = false;

    memset(out, 0, sizeof(*out));
    out->x = '.';
    out->y = '.';
    if (rec.n < 3U || rec.p[1] != (u8)' ')
        return parse_error(err, off, "malformed status record");
    if (rec.p[0] == (u8)'?' || rec.p[0] == (u8)'!') {
        path.p = rec.p + 2U;
        path.n = rec.n - 2U;
        if (path.n == 0U || path.n > UINT32_MAX)
            return parse_error(err, off + 2U, "invalid status path");
        out->kind = rec.p[0] == (u8)'?' ? GIT_E_UNTRACKED : GIT_E_IGNORED;
        out->path = span_dup(a, path);
        out->path_len = (u32)path.n;
        out->is_dir = path.p[path.n - 1U] == (u8)'/';
        out->untracked = out->kind == GIT_E_UNTRACKED;
        return true;
    }

    fixed = rec.p[0] == (u8)'1' ? 7U : rec.p[0] == (u8)'2' ? 8U : 9U;
    for (i = 0; i < fixed; i++)
        if (!take_word(rec, &at, &fields[i]))
            return parse_error(err, off + at, "truncated status fields");
    path.p = rec.p + at;
    path.n = rec.n - at;
    if (path.n == 0U || path.n > UINT32_MAX)
        return parse_error(err, off + at, "invalid status path");

    if (rec.p[0] == (u8)'u') {
        if (!valid_conflict(fields[0]) || !valid_sub(fields[1], &submodule))
            return parse_error(err, off + 2U, "invalid unmerged state");
        for (i = 2U; i < 6U; i++)
            if (!valid_mode(fields[i]))
                return parse_error(err, off, "invalid unmerged mode");
        for (i = 6U; i < 9U; i++)
            if (!is_hex_oid(fields[i]))
                return parse_error(err, off, "invalid unmerged oid");
        out->kind = GIT_E_UNMERGED;
        out->conflicted = true;
    } else {
        if (!valid_xy(fields[0]) || !valid_sub(fields[1], &submodule))
            return parse_error(err, off + 2U, "invalid ordinary state");
        for (i = 2U; i < 5U; i++)
            if (!valid_mode(fields[i]))
                return parse_error(err, off, "invalid ordinary mode");
        if (!is_hex_oid(fields[5]) || !is_hex_oid(fields[6]))
            return parse_error(err, off, "invalid ordinary oid");
        if (fields[6].n >= sizeof(out->index_oid))
            return parse_error(err, off, "index oid too long");
        memcpy(out->index_oid, fields[6].p, fields[6].n);
        out->index_oid[fields[6].n] = '\0';
        out->kind = rec.p[0] == (u8)'1' ? GIT_E_ORDINARY : GIT_E_RENAME;
        out->staged = fields[0].p[0] != (u8)'.';
        out->unstaged = fields[0].p[1] != (u8)'.';
        if (out->kind == GIT_E_RENAME) {
            u64 score;
            ByteSpan similarity = fields[7];
            if (orig.n == 0U || orig.n > UINT32_MAX || similarity.n < 2U ||
                (similarity.p[0] != (u8)'R' && similarity.p[0] != (u8)'C'))
                return parse_error(err, off, "invalid rename record");
            similarity.p++;
            similarity.n--;
            if (!decimal_u64(similarity, 100U, &score))
                return parse_error(err, off, "invalid rename score");
            out->score = (u8)score;
            out->orig_path = span_dup(a, orig);
            out->orig_len = (u32)orig.n;
        }
    }
    out->x = (char)fields[0].p[0];
    out->y = (char)fields[0].p[1];
    out->submodule = submodule;
    out->path = span_dup(a, path);
    out->path_len = (u32)path.n;
    return true;
}

bool yew_git_parse_status(GitSnapshot *snap, const u8 *buf, u64 n,
                          GitParseErr *err)
{
    GitEntry *tmp = NULL;
    size_t len = 0U;
    size_t count = 0U;
    u64 at = 0U;
    bool saw_upstream = false;

    if (!snap || (!buf && n != 0U))
        return parse_error(err, 0U, "invalid status input");
    if (!count_status_records(buf, n, &count, err))
        return false;
    if (count)
        tmp = arena_alloc(&snap->a, count * sizeof(*tmp), alignof(GitEntry));
    snap->branch = NULL;
    snap->upstream = NULL;
    snap->head_oid = NULL;
    snap->ahead = -1;
    snap->behind = -1;
    snap->detached = false;
    snap->unborn = false;
    snap->conflicted = false;
    snap->entries.data = NULL;
    snap->entries.len = 0U;
    while (at < n) {
        const u8 *nul = memchr(buf + at, 0, (size_t)(n - at));
        ByteSpan rec;
        ByteSpan orig = {NULL, 0U};
        GitEntry entry;
        u64 next;

        if (!nul) {
            return parse_error(err, at, "unterminated status record");
        }
        rec.p = buf + at;
        rec.n = (size_t)(nul - (buf + at));
        next = (u64)(nul - buf) + 1U;
        if (rec.n == 0U) {
            return parse_error(err, at, "empty status record");
        }
        if (rec.p[0] == (u8)'#') {
            if (!parse_header(snap, rec, at, err, &saw_upstream)) {
                return false;
            }
            at = next;
            continue;
        }
        if (rec.p[0] == (u8)'2') {
            const u8 *orig_nul;
            if (next >= n || !(orig_nul = memchr(buf + next, 0,
                                                 (size_t)(n - next)))) {
                return parse_error(err, next, "unterminated rename source");
            }
            orig.p = buf + next;
            orig.n = (size_t)(orig_nul - (buf + next));
            next = (u64)(orig_nul - buf) + 1U;
        }
        if (rec.p[0] != (u8)'1' && rec.p[0] != (u8)'2' &&
            rec.p[0] != (u8)'u' && rec.p[0] != (u8)'?' &&
            rec.p[0] != (u8)'!') {
            at = next;
            continue;
        }
        if (!parse_entry(&snap->a, rec, orig, &entry, at, err)) {
            return false;
        }
        tmp[len++] = entry;
        if (entry.conflicted)
            snap->conflicted = true;
        at = next;
    }
    sort_entries(&snap->a, tmp, len);
    snap->entries.data = tmp;
    snap->entries.len = len;
    if (snap->conflicted)
        snap->state = YEW_GIT_CONFLICTED;
    else if (snap->unborn)
        snap->state = YEW_GIT_NO_HEAD;
    else if (snap->detached)
        snap->state = YEW_GIT_DETACHED;
    else if (!saw_upstream)
        snap->state = YEW_GIT_NO_UPSTREAM;
    else
        snap->state = YEW_GIT_OK;
    return true;
}

bool yew_git_parse_z_paths(Arena *a, const u8 *buf, u64 n,
                           GitPathList *paths, GitParseErr *err)
{
    GitPath *tmp = NULL;
    size_t len = 0U;
    size_t count = 0U;
    u64 at = 0U;

    if (!a || !paths || (!buf && n != 0U))
        return parse_error(err, 0U, "invalid path input");
    paths->data = NULL;
    paths->len = 0U;
    while (at < n) {
        const u8 *nul = memchr(buf + at, 0, (size_t)(n - at));
        size_t path_len;
        if (!nul)
            return parse_error(err, at, "unterminated path");
        path_len = (size_t)(nul - (buf + at));
        if (path_len == 0U || path_len > UINT32_MAX)
            return parse_error(err, at, "invalid path length");
        count++;
        at = (u64)(nul - buf) + 1U;
    }
    if (count)
        tmp = arena_alloc(a, count * sizeof(*tmp), alignof(GitPath));
    at = 0U;
    while (at < n) {
        const u8 *nul = memchr(buf + at, 0, (size_t)(n - at));
        ByteSpan path;
        GitPath item;
        if (!nul) {
            return parse_error(err, at, "unterminated path");
        }
        path.p = buf + at;
        path.n = (size_t)(nul - (buf + at));
        if (path.n == 0U || path.n > UINT32_MAX) {
            return parse_error(err, at, "invalid path length");
        }
        item.path = span_dup(a, path);
        item.len = (u32)path.n;
        item.is_dir = path.p[path.n - 1U] == (u8)'/';
        tmp[len++] = item;
        at = (u64)(nul - buf) + 1U;
    }
    sort_paths(a, tmp, len);
    paths->data = tmp;
    paths->len = len;
    return true;
}

bool yew_git_parse_ignore(Arena *a, const u8 *buf, u64 n,
                          GitIgnoreSet *set, GitParseErr *err)
{
    GitPathList paths;
    if (!set)
        return parse_error(err, 0U, "invalid ignore output");
    if (!yew_git_parse_z_paths(a, buf, n, &paths, err))
        return false;
    set->data = paths.data;
    set->len = paths.len;
    return true;
}

static bool path_equal(const GitPath *item, const char *path, u32 len)
{
    return item->len == len && (len == 0U || memcmp(item->path, path, len) == 0);
}

static bool ignore_exact(const GitIgnoreSet *set, const char *path, u32 len)
{
    size_t lo = 0U;
    size_t hi = set->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        GitPath key;
        int cmp;
        key.path = (char *)path;
        key.len = len;
        key.is_dir = false;
        cmp = path_compare(&set->data[mid], &key, NULL);
        if (cmp < 0)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo < set->len && path_equal(&set->data[lo], path, len);
}

bool yew_git_ignored(const GitIgnoreSet *set, const char *path, u32 len)
{
    u32 i;
    if (!set || (!path && len != 0U))
        return false;
    if (ignore_exact(set, path, len))
        return true;
    for (i = len; i > 0U; i--)
        if (path[i - 1U] == '/' && ignore_exact(set, path, i))
            return true;
    return false;
}

static bool next_line(const u8 *buf, u64 n, u64 *at, ByteSpan *line,
                      GitParseErr *err)
{
    const u8 *newline;
    u64 start = *at;

    if (start >= n)
        return parse_error(err, start, "missing blame line");
    newline = memchr(buf + start, '\n', (size_t)(n - start));
    if (!newline)
        return parse_error(err, start, "unterminated blame line");
    line->p = buf + start;
    line->n = (size_t)(newline - (buf + start));
    *at = (u64)(newline - buf) + 1U;
    return true;
}

static bool blame_header(ByteSpan line, ByteSpan *sha, u32 *final_line,
                         u32 *line_count, bool *continuation)
{
    ByteSpan fields[4];
    size_t at = 0U;
    size_t field_count = 0U;
    u64 value;

    while (at < line.n && field_count < 4U) {
        size_t start = at;
        while (at < line.n && line.p[at] != (u8)' ')
            at++;
        if (at == start)
            return false;
        fields[field_count].p = line.p + start;
        fields[field_count].n = at - start;
        field_count++;
        if (at < line.n) {
            at++;
            if (at == line.n)
                return false;
        }
    }
    if (at != line.n || (field_count != 3U && field_count != 4U))
        return false;
    if (!is_hex_oid(fields[0]) || !decimal_u64(fields[1], UINT32_MAX, &value))
        return false;
    if (!decimal_u64(fields[2], UINT32_MAX, &value) || value == 0U)
        return false;
    *final_line = (u32)value;
    if (field_count == 4U) {
        if (!decimal_u64(fields[3], UINT32_MAX, &value) || value == 0U)
            return false;
        *line_count = (u32)value;
    } else {
        *line_count = 1U;
    }
    *continuation = field_count == 3U;
    *sha = fields[0];
    return true;
}

static bool metadata_value(ByteSpan line, const char *key, ByteSpan *value)
{
    size_t n = strlen(key);
    if (line.n < n || memcmp(line.p, key, n) != 0)
        return false;
    value->p = line.p + n;
    value->n = line.n - n;
    return true;
}

static bool parse_timezone(ByteSpan value, i16 *minutes)
{
    u32 hh;
    u32 mm;
    int sign;
    if (value.n != 5U || (value.p[0] != (u8)'+' && value.p[0] != (u8)'-') ||
        value.p[1] < (u8)'0' || value.p[1] > (u8)'9' ||
        value.p[2] < (u8)'0' || value.p[2] > (u8)'9' ||
        value.p[3] < (u8)'0' || value.p[3] > (u8)'9' ||
        value.p[4] < (u8)'0' || value.p[4] > (u8)'9')
        return false;
    hh = (u32)(value.p[1] - (u8)'0') * 10U +
         (u32)(value.p[2] - (u8)'0');
    mm = (u32)(value.p[3] - (u8)'0') * 10U +
         (u32)(value.p[4] - (u8)'0');
    if (hh > 23U || mm > 59U)
        return false;
    sign = value.p[0] == (u8)'-' ? -1 : 1;
    *minutes = (i16)(sign * (int)(hh * 60U + mm));
    return true;
}

static size_t find_commit(const GitCommitMeta *commits, size_t n, ByteSpan sha)
{
    size_t i;
    for (i = 0U; i < n; i++)
        if (strlen(commits[i].sha) == sha.n &&
            memcmp(commits[i].sha, sha.p, sha.n) == 0)
            return i;
    return SIZE_MAX;
}

u32 yew_git_parse_blame(Arena *a, const u8 *buf, u64 n,
                        GitBlameLineList *lines,
                        GitCommitMetaList *commits, GitParseErr *err)
{
    GitBlameLine *line_data;
    GitCommitMeta *commit_data;
    size_t max_groups = 0U;
    size_t max_lines = 0U;
    size_t line_len = 0U;
    size_t commit_len = 0U;
    u64 scan = 0U;
    u64 at = 0U;

    if (!a || !lines || !commits || (!buf && n != 0U)) {
        (void)parse_error(err, 0U, "invalid blame input");
        return 0U;
    }
    lines->data = NULL;
    lines->len = 0U;
    commits->data = NULL;
    commits->len = 0U;
    while (scan < n) {
        ByteSpan line;
        ByteSpan sha;
        u32 final_line;
        u32 count;
        bool continuation;
        if (!next_line(buf, n, &scan, &line, err))
            return 0U;
        if (blame_header(line, &sha, &final_line, &count, &continuation)) {
            (void)sha;
            (void)final_line;
            (void)continuation;
            if (max_lines > SIZE_MAX - count) {
                (void)parse_error(err, scan, "too many blame lines");
                return 0U;
            }
            max_lines += count;
            max_groups++;
        }
    }
    line_data = max_lines ? arena_alloc(a, max_lines * sizeof(*line_data),
                                        alignof(GitBlameLine)) : NULL;
    commit_data = max_groups ? arena_alloc(a, max_groups * sizeof(*commit_data),
                                           alignof(GitCommitMeta)) : NULL;
    while (at < n) {
        ByteSpan header;
        ByteSpan sha;
        u32 final_line;
        u32 count;
        size_t commit_index;
        bool got_filename = false;
        bool known_commit;
        bool continuation;
        u32 i;

        if (!next_line(buf, n, &at, &header, err) ||
            !blame_header(header, &sha, &final_line, &count, &continuation)) {
            (void)parse_error(err, at, "invalid blame group header");
            return 0U;
        }
        commit_index = find_commit(commit_data, commit_len, sha);
        known_commit = commit_index != SIZE_MAX;
        if (continuation && commit_index == SIZE_MAX) {
            (void)parse_error(err, at, "blame continuation without metadata");
            return 0U;
        }
        if (commit_index == SIZE_MAX) {
            GitCommitMeta *meta = &commit_data[commit_len];
            memset(meta, 0, sizeof(*meta));
            memcpy(meta->sha, sha.p, sha.n);
            meta->sha[sha.n] = '\0';
            commit_index = commit_len++;
        }
        while (!continuation && at < n) {
            u64 before = at;
            ByteSpan line;
            ByteSpan value;
            GitCommitMeta *meta = &commit_data[commit_index];
            i64 time_value;

            if (!next_line(buf, n, &at, &line, err))
                return 0U;
            if (line.n != 0U && line.p[0] == (u8)'\t') {
                at = before;
                break;
            }
            if (metadata_value(line, "filename ", &value)) {
                got_filename = true;
                break;
            }
            if (metadata_value(line, "author ", &value))
                meta->author = span_dup(a, value);
            else if (metadata_value(line, "author-mail ", &value))
                meta->author_mail = span_dup(a, value);
            else if (metadata_value(line, "summary ", &value))
                meta->summary = span_dup(a, value);
            else if (metadata_value(line, "author-time ", &value)) {
                if (!decimal_i64(value, &time_value)) {
                    (void)parse_error(err, before, "invalid blame author time");
                    return 0U;
                }
                meta->author_time = time_value;
            } else if (metadata_value(line, "author-tz ", &value)) {
                if (!parse_timezone(value, &meta->author_tz_min)) {
                    (void)parse_error(err, before, "invalid blame timezone");
                    return 0U;
                }
            } else if (span_eq(line, "boundary")) {
                meta->boundary = true;
            }
        }
        if (known_commit && at < n && buf[at] == (u8)'\t')
            got_filename = true;
        if (continuation)
            got_filename = true;
        if (!got_filename) {
            (void)parse_error(err, at, "blame group missing filename");
            return 0U;
        }
        if ((u64)final_line + (u64)count - 1U > UINT32_MAX) {
            (void)parse_error(err, at, "blame line range overflow");
            return 0U;
        }
        if (at < n && buf[at] == (u8)'\t') {
            ByteSpan content;
            if (!next_line(buf, n, &at, &content, err))
                return 0U;
            line_data[line_len].lineno = final_line;
            line_data[line_len].commit = (u32)commit_index;
            line_len++;
        } else {
            if (continuation) {
                (void)parse_error(err, at, "blame continuation missing content");
                return 0U;
            }
            for (i = 0U; i < count; i++) {
                line_data[line_len].lineno = final_line + i;
                line_data[line_len].commit = (u32)commit_index;
                line_len++;
            }
        }
    }
    lines->data = line_data;
    lines->len = line_len;
    commits->data = commit_data;
    commits->len = commit_len;
    if (line_len > UINT32_MAX) {
        (void)parse_error(err, n, "too many blame lines");
        return 0U;
    }
    return (u32)line_len;
}

static bool split_fields(ByteSpan record, ByteSpan *fields, size_t separators)
{
    size_t at = 0U;
    size_t field;
    for (field = 0U; field < separators; field++) {
        const u8 *sep = memchr(record.p + at, 0x1f, record.n - at);
        if (!sep)
            return false;
        fields[field].p = record.p + at;
        fields[field].n = (size_t)(sep - (record.p + at));
        at = (size_t)(sep - record.p) + 1U;
    }
    fields[separators].p = record.p + at;
    fields[separators].n = record.n - at;
    return true;
}

static size_t pretty_record_count(const u8 *buf, u64 n)
{
    size_t count = 0U;
    u64 i;
    if (n == 0U)
        return 0U;
    for (i = 0U; i < n; i++)
        if (buf[i] == 0U)
            count++;
    if (buf[n - 1U] != 0U)
        count++;
    return count;
}

bool yew_git_parse_log(Arena *a, const u8 *buf, u64 n,
                       GitLogRecordList *records, GitParseErr *err)
{
    size_t count;
    size_t len = 0U;
    u64 at = 0U;

    if (!a || !records || (!buf && n != 0U))
        return parse_error(err, 0U, "invalid log input");
    records->data = NULL;
    records->len = 0U;
    count = pretty_record_count(buf, n);
    if (count)
        records->data = arena_alloc(a, count * sizeof(*records->data),
                                    alignof(GitLogRecord));
    while (at < n) {
        const u8 *nul = memchr(buf + at, 0, (size_t)(n - at));
        u64 end = nul ? (u64)(nul - buf) : n;
        ByteSpan record = {buf + at, (size_t)(end - at)};
        ByteSpan field[9];
        GitLogRecord *out;
        i64 timestamp;
        if (record.n == 0U)
            return parse_error(err, at, "empty log record");
        if (!split_fields(record, field, 8U))
            return parse_error(err, at, "truncated log record");
        if (!is_hex_oid(field[0]) || field[1].n == 0U ||
            !decimal_i64(field[2], &timestamp))
            return parse_error(err, at, "invalid log record");
        out = &records->data[len++];
        out->oid = span_dup(a, field[0]);
        out->short_oid = span_dup(a, field[1]);
        out->author_time = timestamp;
        out->author = span_dup(a, field[3]);
        out->author_mail = span_dup(a, field[4]);
        out->parents = span_dup(a, field[5]);
        out->refs = span_dup(a, field[6]);
        out->subject = span_dup(a, field[7]);
        out->body = span_dup(a, field[8]);
        at = nul ? end + 1U : n;
    }
    records->len = len;
    return true;
}

bool yew_git_parse_reflog(Arena *a, const u8 *buf, u64 n,
                          GitReflogRecordList *records, GitParseErr *err)
{
    size_t count;
    size_t len = 0U;
    u64 at = 0U;

    if (!a || !records || (!buf && n != 0U))
        return parse_error(err, 0U, "invalid reflog input");
    records->data = NULL;
    records->len = 0U;
    count = pretty_record_count(buf, n);
    if (count)
        records->data = arena_alloc(a, count * sizeof(*records->data),
                                    alignof(GitReflogRecord));
    while (at < n) {
        const u8 *nul = memchr(buf + at, 0, (size_t)(n - at));
        u64 end = nul ? (u64)(nul - buf) : n;
        ByteSpan record = {buf + at, (size_t)(end - at)};
        ByteSpan field[6];
        GitReflogRecord *out;
        i64 timestamp;
        if (record.n == 0U)
            return parse_error(err, at, "empty reflog record");
        if (!split_fields(record, field, 5U))
            return parse_error(err, at, "truncated reflog record");
        if (!is_hex_oid(field[0]) || field[1].n == 0U ||
            !decimal_i64(field[4], &timestamp))
            return parse_error(err, at, "invalid reflog record");
        out = &records->data[len++];
        out->oid = span_dup(a, field[0]);
        out->short_oid = span_dup(a, field[1]);
        out->selector = span_dup(a, field[2]);
        out->message = span_dup(a, field[3]);
        out->author_time = timestamp;
        out->subject = span_dup(a, field[5]);
        at = nul ? end + 1U : n;
    }
    records->len = len;
    return true;
}
