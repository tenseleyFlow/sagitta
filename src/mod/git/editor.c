#include "mod/git/editor.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edit/ed.h"
#include "edit/job.h"
#include "edit/select.h"
#include "mod/git/blame.h"
#include "mod/git/diffview.h"
#include "mod/git/git.h"
#include "text/edit.h"
#include "text/piece.h"
#include "text/undo.h"
#include "syn/theme.h"
#include "term/grid.h"
#include "ui/gutter.h"
#include "ui/layout.h"
#include "ui/message.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/width.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

enum { GIT_EDIT_DEBOUNCE_MS = 150 };

typedef struct GitBufState {
    u32 buf_id;
    Bytebuf base;
    char base_oid[65];
    HunkList hunks;
    BlameCache *blame;
    u64 observed_gen;
    u64 hunk_revision;
    u32 base_snapshot_gen;
    i64 diff_due_ms;
    bool base_ready;
    bool base_pending;
    bool base_is_head;
    bool base_by_snapshot;
} GitBufState;

struct GitEditorState {
    GitBufState *v;
    size_t len;
    size_t cap;
    u32 next_scroll_link;
    bool warned_dirty_stage;
};

typedef struct BaseJob {
    u32 buf_id;
    char oid[65];
    u32 snapshot_gen;
    bool batch;
    bool base_is_head;
    bool by_snapshot;
} BaseJob;

typedef struct BlameJob {
    BlameCache *cache;
    BlameRequest request;
} BlameJob;

typedef struct StageJob {
    bool dirty;
} StageJob;

static void oid_copy(char dst[65], const char *src)
{
    size_t n = src == NULL ? 0U : strlen(src);

    if (n > 64U)
        n = 64U;
    if (n != 0U)
        (void)memcpy(dst, src, n);
    dst[n] = '\0';
}

static void hunk_list_init(HunkList *list)
{
    (void)memset(list, 0, sizeof(*list));
    arena_init(&list->a);
}

static void hunk_list_drop(HunkList *list)
{
    GitHunkVec_free(&list->h);
    arena_free_all(&list->a);
    (void)memset(list, 0, sizeof(*list));
}

static GitBufState *state_for(Ed *ed, Buffer *buf, bool create)
{
    GitEditorState *state;
    size_t i;

    if (ed == NULL || ed->git_editor == NULL || buf == NULL)
        return NULL;
    state = ed->git_editor;
    for (i = 0U; i < state->len; i++)
        if (state->v[i].buf_id == buf->id)
            return &state->v[i];
    if (!create)
        return NULL;
    if (state->len == state->cap) {
        size_t cap = state->cap == 0U ? 8U : state->cap * 2U;
        state->v = yew_xreallocarray(state->v, cap, sizeof(*state->v));
        state->cap = cap;
    }
    (void)memset(&state->v[state->len], 0, sizeof(state->v[state->len]));
    state->v[state->len].buf_id = buf->id;
    bytebuf_init(&state->v[state->len].base);
    hunk_list_init(&state->v[state->len].hunks);
    state->v[state->len].blame = yew_blame_cache_new();
    state->v[state->len].diff_due_ms = -1;
    return &state->v[state->len++];
}

static void text_bytes(const TextBuf *tb, Bytebuf *out)
{
    TextIter it;

    out->len = 0U;
    if (tb == NULL || !yew_textiter_begin(&it, tb, BYTEOFF(0U)))
        return;
    do {
        const u8 *bytes;
        u64 len;

        if (!yew_textiter_chunk(&it, tb, &bytes, &len))
            break;
        if (len > (u64)SIZE_MAX)
            YEW_BUG("git editor: text chunk exceeds address space");
        bytebuf_append(out, bytes, (size_t)len);
    } while (yew_textiter_advance(&it, tb));
}

static void text_range_bytes(const TextBuf *tb, Span span, Bytebuf *out)
{
    TextIter it;
    u64 at = span.lo;

    out->len = 0U;
    if (span.lo >= span.hi ||
        !yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        return;
    do {
        const u8 *bytes;
        u64 len;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &len))
            break;
        take = len < span.hi - at ? len : span.hi - at;
        bytebuf_append(out, bytes, (size_t)take);
        at += take;
    } while (at < span.hi && yew_textiter_advance(&it, tb));
}

static Span content_span(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (span.hi > span.lo &&
        yew_textiter_begin(&it, tb, BYTEOFF(span.hi - 1U)) &&
        yew_textiter_chunk(&it, tb, &bytes, &len) && len != 0U &&
        bytes[0] == '\n')
        span.hi--;
    return span;
}

static void diff_intraline_spans(Win *left, Win *right,
                                 const DiffRowMap *map,
                                 const HunkList *hunks)
{
    size_t map_at = 0U;
    size_t h;
    Bytebuf lb;
    Bytebuf rb;

    bytebuf_init(&lb);
    bytebuf_init(&rb);
    for (h = 0U; h < hunks->h.len; h++) {
        const GitHunk *gh = &hunks->h.data[h];
        u64 j;

        if (gh->kind != YEW_HUNK_MOD || gh->base_n.v != gh->buf_n.v)
            continue;
        for (j = 0U; j < gh->base_n.v; j++) {
            i32 want_left = (i32)(gh->base_lo.v + j);
            i32 want_right = (i32)(gh->buf_lo.v + j);
            Span ls;
            Span rs;
            DiffIntraline intra;
            DiffIntraResult result;
            size_t i;

            while (map_at < map->left.len &&
                   (map->left.data[map_at] != want_left ||
                    map->right.data[map_at] != want_right))
                map_at++;
            if (map_at >= map->left.len)
                break;
            ls = content_span(left->buf->tb, LINENO(map_at));
            rs = content_span(right->buf->tb, LINENO(map_at));
            text_range_bytes(left->buf->tb, ls, &lb);
            text_range_bytes(right->buf->tb, rs, &rb);
            yew_diff_intraline_init(&intra);
            result = yew_diff_intraline_build(&intra, lb.data, lb.len,
                                               rb.data, rb.len);
            if (result == YEW_DIFF_INTRA_WHOLE_LINE) {
                if (ls.lo != ls.hi) SpanVec_push(&left->git_diff_intra, ls);
                if (rs.lo != rs.hi) SpanVec_push(&right->git_diff_intra, rs);
            } else if (result == YEW_DIFF_INTRA_SPANS) {
                for (i = 0U; i < intra.left.len; i++)
                    SpanVec_push(&left->git_diff_intra,
                        ((Span){ls.lo + intra.left.data[i].off,
                                ls.lo + intra.left.data[i].off +
                                intra.left.data[i].len}));
                for (i = 0U; i < intra.right.len; i++)
                    SpanVec_push(&right->git_diff_intra,
                        ((Span){rs.lo + intra.right.data[i].off,
                                rs.lo + intra.right.data[i].off +
                                intra.right.data[i].len}));
            }
            yew_diff_intraline_drop(&intra);
            map_at++;
        }
    }
    bytebuf_free(&lb);
    bytebuf_free(&rb);
}

static const char *repo_path(const Ed *ed, const Buffer *buf)
{
    const GitRepo *repo = yew_git_repo_cached(ed);
    const char *path;
    size_t n;

    if (repo == NULL || repo->top_level == NULL || buf == NULL ||
        (buf->meta.realpath == NULL && buf->path == NULL))
        return NULL;
    path = buf->meta.realpath != NULL ? buf->meta.realpath : buf->path;
    n = strlen(repo->top_level);
    if (strncmp(path, repo->top_level, n) != 0)
        return NULL;
    if (path[n] == '/')
        return path + n + 1U;
    return path[n] == '\0' ? "." : NULL;
}

static const GitEntry *snapshot_entry(const GitSnapshot *snap,
                                      const char *path)
{
    size_t i;

    if (snap == NULL || path == NULL)
        return NULL;
    for (i = 0U; i < snap->entries.len; i++)
        if (snap->entries.data[i].path != NULL &&
            strcmp(snap->entries.data[i].path, path) == 0)
            return &snap->entries.data[i];
    return NULL;
}

static bool batch_payload(const YewJob *job, const u8 **bytes, size_t *len)
{
    const u8 *nl;
    const u8 *last;
    u64 size = 0U;
    const u8 *p;

    if (job == NULL || job->state != YEW_JOB_EXITED || job->exit_code != 0)
        return false;
    nl = memchr(job->collect.data, '\n', job->collect.len);
    if (nl == NULL || (size_t)(nl - job->collect.data) >= job->collect.len)
        return false;
    last = nl;
    while (last > job->collect.data && last[-1] != ' ')
        last--;
    if (last == job->collect.data)
        return false;
    for (p = last; p < nl; p++) {
        if (*p < '0' || *p > '9')
            return false;
        size = size * 10U + (u64)(*p - '0');
    }
    if (size > (u64)SIZE_MAX || (size_t)(nl + 1U - job->collect.data) >
        job->collect.len || (size_t)size > job->collect.len -
        (size_t)(nl + 1U - job->collect.data))
        return false;
    *bytes = nl + 1U;
    *len = (size_t)size;
    return true;
}

static bool batch_missing(const YewJob *job)
{
    const u8 *nl;
    size_t header_len;

    if (job == NULL || job->state != YEW_JOB_EXITED || job->exit_code != 0)
        return false;
    nl = memchr(job->collect.data, '\n', job->collect.len);
    if (nl == NULL)
        return false;
    header_len = (size_t)(nl - job->collect.data);
    return header_len >= sizeof(" missing") - 1U &&
        memcmp(nl - (sizeof(" missing") - 1U), " missing",
               sizeof(" missing") - 1U) == 0;
}

static void base_complete(void *owner, Ed *ed, const YewJob *job)
{
    BaseJob *request = owner;
    Buffer *buf = yew_ws_buf_by_id(ed, request->buf_id);
    GitBufState *state = state_for(ed, buf, false);
    const u8 *bytes = job->collect.data;
    size_t len = job->collect.len;
    bool missing;

    if (state == NULL)
        return;
    state->base_pending = false;
    missing = request->batch && batch_missing(job);
    if (request->batch && !missing && !batch_payload(job, &bytes, &len)) {
        yew_msg(ed, YEW_MSG_ERROR, "git base unavailable");
        return;
    }
    if (!request->batch &&
        (job->state != YEW_JOB_EXITED || job->exit_code != 0)) {
        yew_msg(ed, YEW_MSG_ERROR, "git base unavailable");
        return;
    }
    state->base.len = 0U;
    if (!missing && len != 0U)
        bytebuf_append(&state->base, bytes, len);
    (void)memcpy(state->base_oid, request->oid, sizeof(state->base_oid));
    state->base_snapshot_gen = request->snapshot_gen;
    state->base_by_snapshot = request->by_snapshot;
    state->base_is_head = request->base_is_head;
    state->base_ready = true;
    state->diff_due_ms = ed->now_ms;
    ed->full_damage = true;
}

static void owner_free(void *owner)
{
    free(owner);
}

static const YewJobCallbackOps base_ops = {base_complete, owner_free};

static void blame_complete(void *owner, Ed *ed, const YewJob *job)
{
    BlameJob *request = owner;

    if (job->state == YEW_JOB_EXITED && job->exit_code == 0)
        (void)yew_blame_cache_finish(request->cache, &request->request,
                                     job->collect.data,
                                     (u64)job->collect.len);
    else
        yew_blame_cache_fail(request->cache, &request->request);
    ed->full_damage = true;
}

static const YewJobCallbackOps blame_ops = {blame_complete, owner_free};

static void request_blame(Ed *ed, Buffer *buf, GitBufState *state, i64 now_ms)
{
    BlameRequest request;
    BlameJob *owner;
    Bytebuf input;
    char range[48];
    char err[160];
    const char *path = repo_path(ed, buf);
    char *argv[] = {(char *)"blame", (char *)"--incremental",
                    (char *)"-L", range, (char *)"--contents", (char *)"-",
                    (char *)"--", (char *)path, NULL};

    if (path == NULL || !yew_blame_cache_take_request(state->blame, now_ms,
                                                       &request))
        return;
    (void)snprintf(range, sizeof(range), "%u,%u", request.range_lo + 1U,
                   request.range_hi);
    bytebuf_init(&input);
    text_bytes(buf->tb, &input);
    owner = yew_xcalloc(1U, sizeof(*owner));
    owner->cache = state->blame;
    owner->request = request;
    if (yew_git_spawn_callback_input(ed, yew_git_verb("blame"), argv,
                                     input.data, (u64)input.len, owner,
                                     &blame_ops, err, sizeof(err)) == 0U) {
        yew_blame_cache_fail(state->blame, &request);
        free(owner);
    }
    bytebuf_free(&input);
}

static void request_base(Ed *ed, Buffer *buf, GitBufState *state)
{
    const GitSnapshot *snap = yew_git_snapshot_cached(ed);
    const char *path = repo_path(ed, buf);
    const GitEntry *entry = snapshot_entry(snap, path);
    BaseJob *request;
    char err[160];
    char *argv[] = {(char *)"cat-file", (char *)"--batch", NULL};
    Bytebuf input;
    const char *name;

    if (path == NULL || snap == NULL || state->base_pending)
        return;
    if (entry == NULL || entry->untracked) {
        if (state->base_ready && state->base_by_snapshot &&
            state->base_snapshot_gen == snap->gen)
            return;
    }
    if (entry != NULL && entry->untracked) {
        state->base.len = 0U;
        state->base_oid[0] = '\0';
        state->base_snapshot_gen = snap->gen;
        state->base_by_snapshot = true;
        state->base_is_head = false;
        state->base_ready = true;
        state->diff_due_ms = ed->now_ms;
        return;
    }
    name = entry == NULL ? NULL :
        (entry->conflicted ? snap->head_oid : entry->index_oid);
    if (name == NULL || name[0] == '\0') {
        if (entry == NULL)
            goto request_index_path;
        if (state->base_ready && state->base_by_snapshot &&
            state->base_snapshot_gen == snap->gen &&
            state->base_is_head == entry->conflicted)
            return;
        state->base.len = 0U;
        state->base_oid[0] = '\0';
        state->base_snapshot_gen = snap->gen;
        state->base_by_snapshot = true;
        state->base_ready = true;
        state->base_is_head = entry->conflicted;
        state->diff_due_ms = ed->now_ms;
        return;
    }
    if (name != NULL && name[0] != '\0' &&
        strcmp(state->base_oid, name) == 0) {
        if (state->base_is_head != entry->conflicted) {
            state->base_is_head = entry->conflicted;
            state->diff_due_ms = ed->now_ms;
        }
        state->base_by_snapshot = false;
        state->base_ready = true;
        return;
    }
    request = yew_xcalloc(1U, sizeof(*request));
    request->buf_id = buf->id;
    request->base_is_head = entry->conflicted;
    request->snapshot_gen = snap->gen;
    bytebuf_init(&input);
    if (entry->conflicted) {
        bytebuf_append(&input, "HEAD:", 5U);
        bytebuf_append(&input, path, strlen(path));
        oid_copy(request->oid, name == NULL ? "HEAD" : name);
    } else {
        bytebuf_append(&input, name, strlen(name));
        oid_copy(request->oid, name);
    }
    goto spawn_base;

request_index_path:
    request = yew_xcalloc(1U, sizeof(*request));
    request->buf_id = buf->id;
    request->snapshot_gen = snap->gen;
    request->by_snapshot = true;
    bytebuf_init(&input);
    bytebuf_push_u8(&input, ':');
    bytebuf_append(&input, path, strlen(path));

spawn_base:
    bytebuf_push_u8(&input, '\n');
    request->batch = true;
    if (yew_git_spawn_callback_input(ed, yew_git_verb("blob"), argv,
                                     input.data, (u64)input.len, request,
                                     &base_ops, err, sizeof(err)) == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        free(request);
    } else {
        state->base_pending = true;
    }
    bytebuf_free(&input);
}

static void rebuild(Ed *ed, Buffer *buf, GitBufState *state)
{
    Bytebuf live;
    YewDiffOutcome outcome;

    bytebuf_init(&live);
    text_bytes(buf->tb, &live);
    hunk_list_drop(&state->hunks);
    hunk_list_init(&state->hunks);
    state->hunks.buf_gen = buf->tb->gen;
    state->hunks.base_is_head = state->base_is_head;
    oid_copy(state->hunks.base_oid, state->base_oid);
    outcome = yew_diff_bytes(&state->hunks.a,
                             state->base.data, state->base.len,
                             live.data, live.len, YEW_DIFF_MAX_D,
                             &state->hunks.h);
    if (outcome == YEW_DIFF_TOO_LARGE) {
        yew_msg_hint(ed, YEW_MSG_WARN, "git: file too large for signs");
    } else if (outcome == YEW_DIFF_TRUNCATED) {
        state->hunks.truncated = true;
        yew_msg_hint(ed, YEW_MSG_WARN, "git: diff truncated");
    } else if (outcome == YEW_DIFF_INVALID) {
        state->hunks.h.len = 0U;
        yew_msg_hint(ed, YEW_MSG_ERROR, "git: cannot compute buffer diff");
    }
    state->hunk_revision++;
    if (state->hunk_revision == 0U)
        state->hunk_revision = 1U;
    bytebuf_free(&live);
    ed->full_damage = true;
}

void yew_git_editor_state_init(Ed *ed)
{
    if (ed != NULL && ed->git_editor == NULL) {
        ed->git_editor = yew_xcalloc(1U, sizeof(*ed->git_editor));
        ed->git_editor->next_scroll_link = 1U;
    }
}

void yew_git_editor_state_free(Ed *ed)
{
    size_t i;

    if (ed == NULL || ed->git_editor == NULL)
        return;
    for (i = 0U; i < ed->git_editor->len; i++) {
        bytebuf_free(&ed->git_editor->v[i].base);
        hunk_list_drop(&ed->git_editor->v[i].hunks);
        yew_blame_cache_free(ed->git_editor->v[i].blame);
    }
    free(ed->git_editor->v);
    free(ed->git_editor);
    ed->git_editor = NULL;
}

void yew_git_editor_prepare(Ed *ed, Win *w)
{
    GitBufState *state;
    u64 gen;
    size_t i;
    static const u8 add_glyph[] = "\342\226\216";
    static const u8 del_top_glyph[] = "\342\226\224";
    static const u8 del_end_glyph[] = "\342\226\201";
    static const u8 unknown_glyph[] = "~";

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->buf->path == NULL)
        return;
    state = state_for(ed, w->buf, true);
    gen = w->buf->tb->gen;
    if (gen != state->observed_gen) {
        state->observed_gen = gen;
        state->diff_due_ms = ed->now_ms + GIT_EDIT_DEBOUNCE_MS;
    }
    request_base(ed, w->buf, state);
    if (w->git_blame)
        yew_blame_cache_observe(state->blame, w->buf->id, w->buf->tb->gen,
                                w->vp.top, yew_vp_last_visible_line(w),
                                yew_textbuf_line_count(w->buf->tb),
                                ed->now_ms);
    if (w->git_sign_buf == w->buf->id &&
        w->git_sign_gen == state->hunk_revision)
        return;
    yew_gutter_sign_clear_kind(w, LINENO(0U),
        LINENO(yew_textbuf_line_count(w->buf->tb)), YEW_SIGN_GIT);
    for (i = 0U; i < state->hunks.h.len; i++) {
        const GitHunk *h = &state->hunks.h.data[i];
        GutterSign sign;
        u64 n = h->buf_n.v == 0U ? 1U : h->buf_n.v;
        u64 j;

        if (state->hunks.truncated) {
            sign = (GutterSign){unknown_glyph, 1U, "git.sign.unknown", 0U};
        } else if (h->kind == YEW_HUNK_DEL) {
            bool at_end = h->buf_lo.v >= yew_textbuf_line_count(w->buf->tb);
            sign = (GutterSign){at_end ? del_end_glyph : del_top_glyph, 3U,
                                "git.sign.del", 0U};
            if (at_end && h->buf_lo.v != 0U)
                n = 1U;
        } else {
            sign = (GutterSign){add_glyph, 3U,
                state->base_is_head ? "git.sign.conflict" :
                h->kind == YEW_HUNK_ADD ? "git.sign.add" : "git.sign.mod",
                0U};
        }
        for (j = 0U; j < n; j++) {
            u64 line = h->buf_lo.v + j;
            u64 count = yew_textbuf_line_count(w->buf->tb);
            if (line >= count && count != 0U)
                line = count - 1U;
            yew_gutter_sign_set(w, LINENO(line), YEW_SIGN_GIT, &sign);
        }
    }
    w->git_sign_buf = w->buf->id;
    w->git_sign_gen = state->hunk_revision;
}

void yew_git_editor_tick(Ed *ed, i64 now_ms)
{
    size_t i;

    if (ed == NULL || ed->git_editor == NULL)
        return;
    for (i = 0U; i < ed->git_editor->len; i++) {
        GitBufState *state = &ed->git_editor->v[i];
        Buffer *buf = yew_ws_buf_by_id(ed, state->buf_id);

        if (buf != NULL && buf->tb != NULL && state->base_ready &&
            state->diff_due_ms >= 0 && now_ms >= state->diff_due_ms) {
            rebuild(ed, buf, state);
            state->diff_due_ms = -1;
        }
        if (buf != NULL && buf->tb != NULL)
            request_blame(ed, buf, state, now_ms);
    }
}

i64 yew_git_editor_deadline(const Ed *ed, i64 now_ms)
{
    i64 best = -1;
    size_t i;

    if (ed == NULL || ed->git_editor == NULL)
        return -1;
    for (i = 0U; i < ed->git_editor->len; i++) {
        i64 at = ed->git_editor->v[i].diff_due_ms;
        i64 wait;

        if (at < 0)
            continue;
        wait = at <= now_ms ? 0 : at - now_ms;
        if (best < 0 || wait < best)
            best = wait;
    }
    return best;
}

static const GitHunk *current_hunk(Ed *ed, Win *w, GitBufState **out)
{
    GitBufState *state = state_for(ed, w->buf, false);
    LineNo line;
    size_t i;

    if (state == NULL || state->hunks.h.len == 0U || w->cs.curs.len == 0U)
        return NULL;
    line = yew_textbuf_line_of(w->buf->tb,
                               w->cs.curs.data[w->cs.primary].pos);
    for (i = 0U; i < state->hunks.h.len; i++) {
        const GitHunk *h = &state->hunks.h.data[i];
        u64 n = h->buf_n.v == 0U ? 1U : h->buf_n.v;
        if (line.v >= h->buf_lo.v && line.v < h->buf_lo.v + n) {
            if (out != NULL)
                *out = state;
            return h;
        }
    }
    return NULL;
}

typedef struct HunkSelection {
    LineNo first;
    LineNo last;
    bool active;
} HunkSelection;

static HunkSelection primary_hunk_selection(const Win *w)
{
    HunkSelection selection = {LINENO(0U), LINENO(0U), false};
    const Cursor *cursor;
    const TextBuf *tb;
    Span span;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->cs.curs.len == 0U || w->cs.primary >= w->cs.curs.len)
        return selection;
    cursor = &w->cs.curs.data[w->cs.primary];
    if (cursor->anchor.v == cursor->pos.v)
        return selection;
    tb = w->buf->tb;
    selection.active = true;
    if (w->h.kind == YEW_SEL_RECT) {
        LineNo pos = yew_textbuf_line_of(tb, cursor->pos);
        LineNo anchor = yew_textbuf_line_of(tb, cursor->anchor);

        selection.first = pos.v < anchor.v ? pos : anchor;
        selection.last = pos.v < anchor.v ? anchor : pos;
        return selection;
    }
    span = yew_sel_span(w, cursor);
    selection.first = yew_textbuf_line_of(tb, BYTEOFF(span.lo));
    selection.last = yew_textbuf_line_of(
        tb, BYTEOFF(span.hi > span.lo ? span.hi - 1U : span.hi));
    return selection;
}

static bool hunk_intersects_selection(const GitHunk *h,
                                      HunkSelection selection)
{
    u64 last;

    if (h == NULL || !selection.active)
        return false;
    last = h->buf_n.v == 0U ? h->buf_lo.v :
                              h->buf_lo.v + h->buf_n.v - 1U;
    return h->buf_lo.v <= selection.last.v && last >= selection.first.v;
}

static bool hunk_is_target(const GitHunk *candidate,
                           const GitHunk *cursor_hunk,
                           HunkSelection selection)
{
    return selection.active ?
        hunk_intersects_selection(candidate, selection) :
        candidate == cursor_hunk;
}

static CmdStatus move_hunk(CmdCtx *cx, int which)
{
    GitBufState *state;
    LineNo line;
    size_t at = 0U;
    bool wrapped = false;
    Cursor *cursor;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL ||
        cx->win->cs.curs.len == 0U)
        return YEW_CMD_ERR_STATE;
    yew_git_editor_prepare(cx->ed, cx->win);
    state = state_for(cx->ed, cx->win->buf, false);
    if (state == NULL || state->hunks.h.len == 0U) {
        yew_msg(cx->ed, YEW_MSG_INFO, "no changed hunks");
        return YEW_CMD_ERR_STATE;
    }
    cursor = &cx->win->cs.curs.data[cx->win->cs.primary];
    line = yew_textbuf_line_of(cx->win->buf->tb, cursor->pos);
    if (which == -2)
        at = 0U;
    else if (which == 2)
        at = state->hunks.h.len - 1U;
    else if (which > 0) {
        for (at = 0U; at < state->hunks.h.len; at++)
            if (state->hunks.h.data[at].buf_lo.v > line.v)
                break;
        if (at == state->hunks.h.len) { at = 0U; wrapped = true; }
    } else {
        at = state->hunks.h.len;
        while (at > 0U && state->hunks.h.data[at - 1U].buf_lo.v >= line.v)
            at--;
        if (at == 0U) { at = state->hunks.h.len; wrapped = true; }
        at--;
    }
    cursor->pos = yew_textbuf_line_start(cx->win->buf->tb,
                                         state->hunks.h.data[at].buf_lo);
    cursor->anchor = cursor->pos;
    yew_vp_follow(cx->win);
    if (wrapped)
        yew_msg(cx->ed, YEW_MSG_INFO, "hunk motion wrapped");
    return YEW_CMD_OK;
}

CmdStatus yew_git_cmd_hunk_next(CmdCtx *cx) { return move_hunk(cx, 1); }
CmdStatus yew_git_cmd_hunk_prev(CmdCtx *cx) { return move_hunk(cx, -1); }
CmdStatus yew_git_cmd_hunk_first(CmdCtx *cx) { return move_hunk(cx, -2); }
CmdStatus yew_git_cmd_hunk_last(CmdCtx *cx) { return move_hunk(cx, 2); }

static void stage_complete(void *owner, Ed *ed, const YewJob *job)
{
    StageJob *stage = owner;

    if (job->state == YEW_JOB_EXITED && job->exit_code == 0) {
        yew_git_invalidate(ed);
        (void)yew_git_refresh(ed, true);
        yew_msg(ed, YEW_MSG_INFO, "hunk staged%s",
                stage->dirty ? "; file on disk is still the old version" : "");
    } else {
        yew_msg(ed, YEW_MSG_ERROR, "%.*s", (int)job->collect_err.len,
                job->collect_err.data == NULL ? (const u8 *)"git apply failed" :
                                                job->collect_err.data);
    }
}

static const YewJobCallbackOps stage_ops = {stage_complete, owner_free};

CmdStatus yew_git_cmd_hunk_stage(CmdCtx *cx)
{
    GitBufState *state = NULL;
    const GitHunk *h;
    HunkSelection selection;
    Bytebuf live;
    Bytebuf patch;
    StageJob *owner;
    const char *path;
    char *argv[] = {(char *)"apply", (char *)"--cached", (char *)"-", NULL};
    char err[160];
    bool dirty;
    size_t i;
    size_t target_count = 0U;
    const GitHunk *first_target = NULL;
    const GitHunk *last_target = NULL;
    GitHunk combined;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    selection = primary_hunk_selection(cx->win);
    state = state_for(cx->ed, cx->win->buf, false);
    h = selection.active ? NULL : current_hunk(cx->ed, cx->win, &state);
    path = repo_path(cx->ed, cx->win->buf);
    if (state == NULL || path == NULL ||
        (!selection.active && h == NULL)) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                selection.active ? "selection does not intersect a changed hunk" :
                                   "cursor is not on a changed hunk");
        return YEW_CMD_ERR_STATE;
    }
    if (strchr(path, '\n') != NULL) {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "cannot stage hunks in a file whose name contains a newline; use F mode to stage the whole file");
        return YEW_CMD_ERR_ARG;
    }
    bytebuf_init(&live);
    bytebuf_init(&patch);
    text_bytes(cx->win->buf->tb, &live);
    for (i = 0U; i < state->hunks.h.len; i++) {
        if (!hunk_is_target(&state->hunks.h.data[i], h, selection))
            continue;
        if (first_target == NULL)
            first_target = &state->hunks.h.data[i];
        last_target = &state->hunks.h.data[i];
        target_count++;
    }
    if (target_count == 0U) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                "selection does not intersect a changed hunk");
        bytebuf_free(&patch); bytebuf_free(&live);
        return YEW_CMD_ERR_STATE;
    }
    combined = *first_target;
    if (last_target != first_target) {
        combined.base_n = LINENO(last_target->base_lo.v +
                                 last_target->base_n.v -
                                 first_target->base_lo.v);
        combined.buf_n = LINENO(last_target->buf_lo.v +
                                last_target->buf_n.v -
                                first_target->buf_lo.v);
        combined.kind = YEW_HUNK_MOD;
    }
    if (!yew_git_hunk_patch(&patch, path, state->base.data, state->base.len,
                            live.data, live.len, &combined)) {
        bytebuf_free(&patch); bytebuf_free(&live);
        return YEW_CMD_ERR_STATE;
    }
    dirty = !yew_undo_at_save_point(cx->win->buf->undo);
    owner = yew_xcalloc(1U, sizeof(*owner));
    owner->dirty = dirty;
    if (yew_git_spawn_callback_input(cx->ed, yew_git_verb("apply"), argv,
                                     patch.data, (u64)patch.len, owner,
                                     &stage_ops, err, sizeof(err)) == 0U) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s", err);
        free(owner); bytebuf_free(&patch); bytebuf_free(&live);
        return YEW_CMD_ERR_IO;
    }
    if (dirty && !cx->ed->git_editor->warned_dirty_stage) {
        cx->ed->git_editor->warned_dirty_stage = true;
        yew_msg(cx->ed, YEW_MSG_WARN,
                "staged from an unsaved buffer — the file on disk is still the old version");
    }
    bytebuf_free(&patch); bytebuf_free(&live);
    return YEW_CMD_OK;
}

CmdStatus yew_git_cmd_hunk_unstage(CmdCtx *cx)
{
    yew_msg(cx->ed, YEW_MSG_INFO,
            "hunk unstage is post-1.0; F mode can unstage the whole file");
    return YEW_CMD_ERR_STATE;
}

CmdStatus yew_git_cmd_hunk_discard(CmdCtx *cx)
{
    GitBufState *state = NULL;
    const GitHunk *h;
    HunkSelection selection;
    EditCtx edit;
    bool ok = true;
    TextBuf *base;
    size_t i;
    size_t target_count = 0U;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    selection = primary_hunk_selection(cx->win);
    state = state_for(cx->ed, cx->win->buf, false);
    h = selection.active ? NULL : current_hunk(cx->ed, cx->win, &state);
    if (state == NULL || (!selection.active && h == NULL))
        return YEW_CMD_ERR_STATE;
    for (i = 0U; i < state->hunks.h.len; i++)
        if (hunk_is_target(&state->hunks.h.data[i], h, selection))
            target_count++;
    if (target_count == 0U) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                "selection does not intersect a changed hunk");
        return YEW_CMD_ERR_STATE;
    }
    base = yew_textbuf_from_bytes(state->base.data, (u64)state->base.len);
    edit = yew_ed_edit_ctx_for(cx->ed, cx->win);
    yew_undo_begin(&edit, YEW_TXN_EXTERNAL);
    i = state->hunks.h.len;
    while (i > 0U && ok) {
        const GitHunk *target;
        Span removed;
        Span restored;

        i--;
        target = &state->hunks.h.data[i];
        if (!hunk_is_target(target, h, selection))
            continue;
        removed.lo = yew_textbuf_line_start(cx->win->buf->tb,
                                             target->buf_lo).v;
        removed.hi = target->buf_lo.v + target->buf_n.v >=
                     yew_textbuf_line_count(cx->win->buf->tb) ?
                     yew_textbuf_len(cx->win->buf->tb) :
                     yew_textbuf_line_start(cx->win->buf->tb,
                         LINENO(target->buf_lo.v + target->buf_n.v)).v;
        restored.lo = yew_textbuf_line_start(base, target->base_lo).v;
        restored.hi = target->base_lo.v + target->base_n.v >=
                      yew_textbuf_line_count(base) ? yew_textbuf_len(base) :
                      yew_textbuf_line_start(base,
                          LINENO(target->base_lo.v + target->base_n.v)).v;
        if (removed.lo != removed.hi)
            ok = yew_edit_delete(&edit, removed);
        if (ok && restored.lo != restored.hi)
            ok = yew_edit_insert(&edit, BYTEOFF(removed.lo),
                                 state->base.data + restored.lo,
                                 restored.hi - restored.lo);
    }
    if (ok) yew_undo_end(&edit); else yew_undo_abort(&edit);
    yew_ed_finish_edit(cx->ed, &edit);
    yew_textbuf_free(base);
    if (!ok)
        return YEW_CMD_ERR_IO;
    yew_msg(cx->ed, YEW_MSG_INFO, "hunk discarded (undo restores it)");
    return YEW_CMD_OK;
}

CmdStatus yew_git_cmd_blame_toggle(CmdCtx *cx)
{
    if (cx == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    cx->win->git_blame = !cx->win->git_blame;
    cx->ed->full_damage = true;
    yew_msg(cx->ed, YEW_MSG_INFO, "inline blame %s",
            cx->win->git_blame ? "on" : "off");
    return YEW_CMD_OK;
}

const BlameLine *yew_git_blame_at(Ed *ed, Win *w, LineNo line)
{
    GitBufState *state = state_for(ed, w == NULL ? NULL : w->buf, false);
    return state == NULL || w == NULL || !w->git_blame ? NULL :
        yew_blame_cache_at(state->blame, w->buf->id, w->buf->tb->gen, line);
}

void yew_git_blame_draw(Ed *ed, Win *w, u16 lo, u16 hi)
{
    i64 wall_now;
    u16 row;

    if (ed == NULL || w == NULL || !w->git_blame)
        return;
    wall_now = (i64)time(NULL);
    if (hi > w->rect.h)
        hi = w->rect.h;
    for (row = lo; row < hi; row++) {
        LineNo line;
        u32 sub;
        const BlameLine *blame;
        Span span;
        CCol end;
        u16 text_end;
        u16 col;
        u16 available;
        char text[320];
        size_t n;
        size_t clipped;
        TextIter it;
        const u8 *tail;
        u64 tail_len;
        const ThemeEnt *style;

        if (!yew_vp_line_of_row(w, row, &line, &sub))
            continue;
        if (w->vp.wrap && sub + 1U < yew_wrap_rows(w, line))
            continue;
        blame = yew_git_blame_at(ed, w, line);
        if (blame == NULL)
            continue;
        span = yew_textbuf_line_span(w->buf->tb, line);
        if (span.hi > span.lo &&
            yew_textiter_begin(&it, w->buf->tb, BYTEOFF(span.hi - 1U)) &&
            yew_textiter_chunk(&it, w->buf->tb, &tail, &tail_len) &&
            tail_len != 0U && tail[0] == '\n')
            span.hi--;
        end = yew_vp_display_col(w, BYTEOFF(span.hi));
        text_end = end.v > UINT16_MAX ? UINT16_MAX : (u16)end.v;
        if (w->vp.wrap && w->vp.cols != 0U)
            text_end = (u16)(text_end % w->vp.cols);
        if (!yew_blame_layout(w->rect.w, text_end, w->vp.wrap, true,
                              &col, &available))
            continue;
        n = yew_blame_format(text, sizeof(text), blame,
                             wall_now);
        if (n == 0U)
            continue;
        clipped = yew_str_clip((const u8 *)text, n, (int)available, NULL);
        if (clipped == 0U)
            continue;
        style = yew_theme_ui_tab(ed, blame->stale ? "git.blame.stale" :
                                                   "git.blame");
        if (style == NULL)
            style = yew_theme_ui_tab(ed, "muted");
        if (style != NULL)
            (void)yew_grid_puts(&ed->grid, (u16)(w->rect.y + row),
                                (u16)(w->rect.x + col), (const u8 *)text,
                                clipped,
                                style->fg, style->bg, style->attrs);
    }
}

CmdStatus yew_git_cmd_diff_view(CmdCtx *cx)
{
    GitBufState *state;
    Bytebuf live;
    DiffRowMap map;
    DiffScratchPair pair;
    Buffer *left;
    Buffer *right;
    Pane *split;
    Pane *right_leaf;
    TextBuf *base_tb;
    u32 base_lines;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    state = state_for(cx->ed, cx->win->buf, false);
    if (state == NULL || !state->base_ready)
        return YEW_CMD_ERR_STATE;
    bytebuf_init(&live); text_bytes(cx->win->buf->tb, &live);
    base_tb = yew_textbuf_from_bytes(state->base.data, state->base.len);
    base_lines = (u32)yew_textbuf_line_count(base_tb);
    yew_textbuf_free(base_tb);
    yew_diff_rowmap_init(&map);
    yew_diff_scratch_pair_init(&pair);
    if (!yew_diff_rowmap_build(&map, base_lines,
            (u32)yew_textbuf_line_count(cx->win->buf->tb),
            state->hunks.h.data, state->hunks.h.len) ||
        !yew_diff_scratch_pair_build(&pair, state->base.data, state->base.len,
                                     live.data, live.len, &map)) {
        yew_diff_scratch_pair_drop(&pair); yew_diff_rowmap_drop(&map);
        bytebuf_free(&live); return YEW_CMD_ERR_STATE;
    }
    split = cx->ed->focus;
    right_leaf = yew_pane_split(cx->ed, split, YEW_SPLIT_H);
    if (right_leaf == NULL) {
        yew_diff_scratch_pair_drop(&pair); yew_diff_rowmap_drop(&map);
        bytebuf_free(&live); return YEW_CMD_ERR_STATE;
    }
    left = yew_ws_scratch_new(cx->ed, "*git-index*",
                              YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    right = yew_ws_scratch_new(cx->ed, "*git-buffer*",
                               YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (left == NULL || right == NULL)
        YEW_BUG("diff scratch allocation failed after pane split");
    yew_textbuf_insert(left->tb, BYTEOFF(0U), pair.left.bytes.data,
                       (u64)pair.left.bytes.len);
    yew_textbuf_insert(right->tb, BYTEOFF(0U), pair.right.bytes.data,
                       (u64)pair.right.bytes.len);
    yew_ed_win_set_buffer(cx->ed, split->a->win, left);
    yew_ed_win_set_buffer(cx->ed, right_leaf->win, right);
    split->a->win->git_diff_intra_add = false;
    right_leaf->win->git_diff_intra_add = true;
    diff_intraline_spans(split->a->win, right_leaf->win, &map,
                         &state->hunks);
    split->a->win->scroll_link = cx->ed->git_editor->next_scroll_link;
    right_leaf->win->scroll_link = cx->ed->git_editor->next_scroll_link++;
    cx->ed->focus = right_leaf; cx->ed->win = right_leaf->win;
    cx->ed->layout_dirty = true; cx->ed->full_damage = true;
    yew_diff_scratch_pair_drop(&pair); yew_diff_rowmap_drop(&map);
    bytebuf_free(&live);
    return YEW_CMD_OK;
}

CmdStatus yew_git_cmd_conflict_scope(CmdCtx *cx)
{
    yew_msg(cx->ed, YEW_MSG_ERROR,
            "conflict resolution is not a 1.0 feature (F mode's diff and your editor are)");
    return YEW_CMD_ERR_STATE;
}
