#define _POSIX_C_SOURCE 200809L

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

enum {
    GIT_EDIT_DEBOUNCE_MS = 150,
    /* Leave room for the scheduler's final clock read and publication
     * bookkeeping while keeping the whole idle callback inside 4 ms. */
    GIT_DIFF_CLOCK_RESERVE_US = 500
};

typedef struct GitBufState {
    u32 buf_id;
    Bytebuf base;
    Bytebuf live;
    char base_oid[65];
    HunkList hunks;
    BlameCache *blame;
    YewDiffWork *diff_work;
    TextSnap diff_snap;
    u64 observed_gen;
    u64 diff_gen;
    u64 base_revision;
    u64 diff_base_revision;
    u64 hunk_revision;
    u64 diff_copy_at;
    u32 base_snapshot_gen;
    i64 diff_due_ms;
    bool base_ready;
    bool base_pending;
    bool base_too_large;
    bool base_is_head;
    bool base_by_snapshot;
} GitBufState;

struct GitEditorState {
    GitBufState *v;
    size_t len;
    size_t cap;
    u32 next_scroll_link;
    i64 clock_mono_ms;
    i64 clock_wall_secs;
    YewDiffNowUsFn diff_now_us;
    void *diff_clock_ctx;
    YewGitEditorStats diff_stats;
    bool clock_anchored;
    bool warned_dirty_stage;
};

typedef struct BaseJob {
    Ed *ed;
    u32 buf_id;
    char oid[65];
    u32 snapshot_gen;
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

static i64 git_editor_now_us(void *ctx)
{
    struct timespec ts;

    (void)ctx;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        YEW_BUG("git editor: monotonic clock failed");
    if ((i64)ts.tv_sec > INT64_MAX / 1000000)
        return INT64_MAX;
    return (i64)ts.tv_sec * 1000000 + (i64)ts.tv_nsec / 1000;
}

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

static bool diff_active(const GitBufState *state)
{
    return state->diff_snap.active || state->diff_work != NULL;
}

static void diff_cancel(GitEditorState *editor, GitBufState *state,
                        bool counted)
{
    bool active = diff_active(state);

    if (state->diff_snap.active)
        yew_textsnap_release(NULL, &state->diff_snap);
    yew_diff_work_free(state->diff_work);
    state->diff_work = NULL;
    state->live.len = 0U;
    state->diff_copy_at = 0U;
    if (active && counted)
        editor->diff_stats.diff_cancelled++;
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
    bytebuf_init(&state->v[state->len].live);
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

static YewGitDiffRowStyle diff_row_style(HunkKind kind, i32 source_row)
{
    if (source_row == YEW_DIFF_FILLER_ROW)
        return YEW_GIT_DIFF_ROW_FILLER;
    switch (kind) {
    case YEW_HUNK_ADD: return YEW_GIT_DIFF_ROW_ADD;
    case YEW_HUNK_DEL: return YEW_GIT_DIFF_ROW_DEL;
    case YEW_HUNK_MOD: return YEW_GIT_DIFF_ROW_MOD;
    }
    return YEW_GIT_DIFF_ROW_NONE;
}

static void diff_row_styles(Win *left, Win *right, const DiffRowMap *map,
                            const HunkList *hunks)
{
    size_t display_at = 0U;
    u32 base_at = 0U;
    size_t h;

    for (h = 0U; h < hunks->h.len; h++) {
        const GitHunk *gh = &hunks->h.data[h];
        u64 unchanged = gh->base_lo.v - base_at;
        u64 changed = gh->base_n.v > gh->buf_n.v ? gh->base_n.v :
                                                       gh->buf_n.v;
        u64 i;

        for (i = 0U; i < unchanged; i++, display_at++) {
            YewGitDiffRowStyleVec_push(&left->git_diff_rows,
                                       YEW_GIT_DIFF_ROW_NONE);
            YewGitDiffRowStyleVec_push(&right->git_diff_rows,
                                       YEW_GIT_DIFF_ROW_NONE);
        }
        for (i = 0U; i < changed; i++, display_at++) {
            YewGitDiffRowStyleVec_push(
                &left->git_diff_rows,
                diff_row_style(gh->kind, map->left.data[display_at]));
            YewGitDiffRowStyleVec_push(
                &right->git_diff_rows,
                diff_row_style(gh->kind, map->right.data[display_at]));
        }
        base_at = (u32)(gh->base_lo.v + gh->base_n.v);
    }
    while (display_at < map->left.len) {
        YewGitDiffRowStyleVec_push(&left->git_diff_rows,
                                   YEW_GIT_DIFF_ROW_NONE);
        YewGitDiffRowStyleVec_push(&right->git_diff_rows,
                                   YEW_GIT_DIFF_ROW_NONE);
        display_at++;
    }
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

static void base_complete(void *owner, const YewGitBlobResult *result)
{
    BaseJob *request = owner;
    Ed *ed = request->ed;
    Buffer *buf = yew_ws_buf_by_id(ed, request->buf_id);
    GitBufState *state = state_for(ed, buf, false);
    const GitSnapshot *snapshot = yew_git_snapshot_cached(ed);

    if (state == NULL) {
        yew_xfree(request);
        return;
    }
    state->base_pending = false;
    if (snapshot == NULL || snapshot->gen != request->snapshot_gen) {
        yew_xfree(request);
        return;
    }
    if (result->state != YEW_GIT_BLOB_OK &&
        result->state != YEW_GIT_BLOB_MISSING &&
        result->state != YEW_GIT_BLOB_TOO_LARGE) {
        yew_msg(ed, YEW_MSG_ERROR, "git base unavailable");
        yew_xfree(request);
        return;
    }
    diff_cancel(ed->git_editor, state, true);
    state->base.len = 0U;
    if (result->state == YEW_GIT_BLOB_OK && result->len != 0U) {
        if (result->len > (u64)SIZE_MAX)
            YEW_BUG("git editor: blob exceeds address space");
        bytebuf_append(&state->base, result->bytes, (size_t)result->len);
    }
    (void)memcpy(state->base_oid, request->oid, sizeof(state->base_oid));
    state->base_snapshot_gen = request->snapshot_gen;
    state->base_by_snapshot = request->by_snapshot;
    state->base_is_head = request->base_is_head;
    state->base_too_large = result->state == YEW_GIT_BLOB_TOO_LARGE;
    state->base_ready = true;
    state->base_revision++;
    state->diff_due_ms = ed->now_ms;
    ed->full_damage = true;
    yew_xfree(owner);
}

static void owner_free(void *owner) { yew_xfree(owner); }

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
        yew_xfree(owner);
    }
    bytebuf_free(&input);
}

static void request_base(Ed *ed, Buffer *buf, GitBufState *state)
{
    const GitSnapshot *snap = yew_git_snapshot_cached(ed);
    const char *path = repo_path(ed, buf);
    const GitEntry *entry = snapshot_entry(snap, path);
    bool base_is_head;
    bool by_snapshot;
    BaseJob *request;
    char err[160];
    const char *oid;
    u32 id;

    if (path == NULL || snap == NULL || state->base_pending)
        return;
    if (entry != NULL && entry->untracked) {
        if (state->base_ready && state->base_by_snapshot &&
            state->base_snapshot_gen == snap->gen &&
            !state->base_is_head)
            return;
        diff_cancel(ed->git_editor, state, true);
        state->base.len = 0U;
        state->base_oid[0] = '\0';
        state->base_snapshot_gen = snap->gen;
        state->base_by_snapshot = true;
        state->base_is_head = false;
        state->base_too_large = false;
        state->base_ready = true;
        state->base_revision++;
        state->diff_due_ms = ed->now_ms;
        return;
    }
    base_is_head = entry != NULL && entry->conflicted;
    by_snapshot = !base_is_head &&
                  (entry == NULL || entry->index_oid[0] == '\0');
    oid = base_is_head ? snap->head_oid :
          by_snapshot ? snap->head_oid : entry->index_oid;
    if (oid == NULL)
        oid = "";
    if (by_snapshot && state->base_ready && state->base_by_snapshot &&
        state->base_snapshot_gen == snap->gen && !state->base_is_head)
        return;
    if (base_is_head && oid[0] == '\0') {
        if (state->base_ready && state->base_by_snapshot &&
            state->base_snapshot_gen == snap->gen &&
            state->base_is_head)
            return;
        diff_cancel(ed->git_editor, state, true);
        state->base.len = 0U;
        state->base_oid[0] = '\0';
        state->base_snapshot_gen = snap->gen;
        state->base_by_snapshot = true;
        state->base_ready = true;
        state->base_too_large = false;
        state->base_is_head = true;
        state->base_revision++;
        state->diff_due_ms = ed->now_ms;
        return;
    }
    if (state->base_ready && strcmp(state->base_oid, oid) == 0) {
        if (state->base_is_head != base_is_head) {
            diff_cancel(ed->git_editor, state, true);
            state->base_is_head = base_is_head;
            state->base_revision++;
            state->diff_due_ms = ed->now_ms;
        }
        return;
    }
    request = yew_xcalloc(1U, sizeof(*request));
    request->ed = ed;
    request->buf_id = buf->id;
    request->base_is_head = base_is_head;
    request->snapshot_gen = snap->gen;
    request->by_snapshot = by_snapshot;
    oid_copy(request->oid, oid);
    id = request->base_is_head ?
        yew_git_head_blob(ed, path, request, base_complete,
                          err, sizeof(err)) :
        yew_git_index_blob(ed, path, request, base_complete,
                           err, sizeof(err));
    if (id == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        yew_xfree(request);
    } else {
        state->base_pending = true;
    }
}

static void diff_publish(Ed *ed, GitBufState *state,
                         YewDiffOutcome outcome, GitHunkVec *hunks)
{
    HunkList next;

    hunk_list_init(&next);
    if (hunks != NULL) {
        next.h = *hunks;
        *hunks = (GitHunkVec){0};
    }
    next.buf_gen = state->diff_gen;
    next.base_is_head = state->base_is_head;
    oid_copy(next.base_oid, state->base_oid);
    if (outcome == YEW_DIFF_TOO_LARGE) {
        yew_msg_hint(ed, YEW_MSG_WARN, "git: file too large for signs");
    } else if (outcome == YEW_DIFF_TRUNCATED) {
        next.truncated = true;
        yew_msg_hint(ed, YEW_MSG_WARN, "git: diff truncated");
    } else if (outcome == YEW_DIFF_INVALID) {
        next.h.len = 0U;
        yew_msg_hint(ed, YEW_MSG_ERROR, "git: cannot compute buffer diff");
    }
    hunk_list_drop(&state->hunks);
    state->hunks = next;
    state->hunk_revision++;
    if (state->hunk_revision == 0U)
        state->hunk_revision = 1U;
    ed->git_editor->diff_stats.diff_published++;
    ed->full_damage = true;
}

static void diff_begin(Ed *ed, Buffer *buf, GitBufState *state)
{
    GitHunkVec empty = {0};

    state->diff_due_ms = -1;
    state->diff_gen = buf->tb->gen;
    state->diff_base_revision = state->base_revision;
    state->diff_copy_at = 0U;
    state->live.len = 0U;
    ed->git_editor->diff_stats.diff_started++;
    if (state->base_too_large ||
        yew_textbuf_len(buf->tb) > YEW_DIFF_MAX_BYTES) {
        diff_publish(ed, state, YEW_DIFF_TOO_LARGE, &empty);
        return;
    }
    state->diff_snap = yew_textbuf_snap(buf->tb);
    bytebuf_reserve(&state->live, (size_t)state->diff_snap.len);
}

static bool diff_copy_live(GitBufState *state, YewDiffNowUsFn now_us,
                           void *clock_ctx, i64 deadline)
{
    while (state->diff_copy_at < state->diff_snap.len) {
        TextIter it;
        const u8 *bytes;
        u64 len;
        size_t take;

        if (now_us(clock_ctx) >= deadline)
            return false;
        if (!yew_textsnap_iter(&it, &state->diff_snap,
                               BYTEOFF(state->diff_copy_at)) ||
            !yew_textiter_chunk(&it, NULL, &bytes, &len) || len == 0U)
            YEW_BUG("git editor: cannot copy diff snapshot");
        take = len > 65536U ? 65536U : (size_t)len;
        bytebuf_append(&state->live, bytes, take);
        state->diff_copy_at += (u64)take;
    }
    yew_textsnap_release(NULL, &state->diff_snap);
    state->diff_work = yew_diff_work_begin_bytes(
        state->base.data, state->base.len, state->live.data, state->live.len,
        YEW_DIFF_MAX_D);
    return true;
}

static void diff_pump(Ed *ed, Buffer *buf, GitBufState *state)
{
    GitEditorState *editor = ed->git_editor;
    YewDiffNowUsFn now_us = editor->diff_now_us;
    void *clock_ctx = editor->diff_clock_ctx;
    i64 started;
    i64 deadline;
    i64 ended;

    started = now_us(clock_ctx);
    editor->diff_stats.diff_slices++;
    if (!diff_active(state))
        diff_begin(ed, buf, state);
    if (!diff_active(state))
        goto measured;
    deadline = started > INT64_MAX -
        (i64)(YEW_DIFF_BUDGET_US - GIT_DIFF_CLOCK_RESERVE_US) ? INT64_MAX :
        started +
        (i64)(YEW_DIFF_BUDGET_US - GIT_DIFF_CLOCK_RESERVE_US);
    if (state->diff_snap.active &&
        !diff_copy_live(state, now_us, clock_ctx, deadline))
        goto measured;
    if (state->diff_work == NULL) {
        GitHunkVec empty = {0};

        diff_publish(ed, state, YEW_DIFF_INVALID, &empty);
        goto measured;
    }
    {
        i64 current = now_us(clock_ctx);
        u32 remaining = current >= deadline ? 0U :
            (u32)(deadline - current);

        if (remaining != 0U &&
            yew_diff_work_step(state->diff_work, remaining, now_us,
                               clock_ctx) == YEW_DIFF_DONE) {
            GitHunkVec hunks = {0};
            YewDiffOutcome outcome =
                yew_diff_work_take(state->diff_work, &hunks);

            diff_publish(ed, state, outcome, &hunks);
            GitHunkVec_free(&hunks);
            diff_cancel(editor, state, false);
        }
    }

measured:
    ended = now_us(clock_ctx);
    if (ended > started &&
        (u64)(ended - started) > editor->diff_stats.diff_max_slice_us)
        editor->diff_stats.diff_max_slice_us = (u64)(ended - started);
}

void yew_git_editor_state_init(Ed *ed)
{
    if (ed != NULL && ed->git_editor == NULL) {
        ed->git_editor = yew_xcalloc(1U, sizeof(*ed->git_editor));
        ed->git_editor->next_scroll_link = 1U;
        ed->git_editor->diff_now_us = git_editor_now_us;
    }
}

void yew_git_editor_state_free(Ed *ed)
{
    size_t i;

    if (ed == NULL || ed->git_editor == NULL)
        return;
    for (i = 0U; i < ed->git_editor->len; i++) {
        diff_cancel(ed->git_editor, &ed->git_editor->v[i], false);
        bytebuf_free(&ed->git_editor->v[i].base);
        bytebuf_free(&ed->git_editor->v[i].live);
        hunk_list_drop(&ed->git_editor->v[i].hunks);
        yew_blame_cache_free(ed->git_editor->v[i].blame);
    }
    yew_xfree(ed->git_editor->v);
    yew_xfree(ed->git_editor);
    ed->git_editor = NULL;
}

void yew_git_editor_clock_anchor(Ed *ed, i64 monotonic_ms, i64 wall_secs)
{
    if (ed == NULL || ed->git_editor == NULL || monotonic_ms < 0 ||
        wall_secs < 0)
        return;
    ed->git_editor->clock_mono_ms = monotonic_ms;
    ed->git_editor->clock_wall_secs = wall_secs;
    ed->git_editor->clock_anchored = true;
}

i64 yew_git_editor_wall_now(const Ed *ed)
{
    const GitEditorState *state;
    i64 elapsed_secs;

    if (ed == NULL || ed->git_editor == NULL)
        return 0;
    state = ed->git_editor;
    if (!state->clock_anchored)
        return 0;
    elapsed_secs = ed->now_ms <= state->clock_mono_ms ? 0 :
                   (ed->now_ms - state->clock_mono_ms) / 1000;
    if (elapsed_secs > INT64_MAX - state->clock_wall_secs)
        return INT64_MAX;
    return state->clock_wall_secs + elapsed_secs;
}

void yew_git_editor_stats(const Ed *ed, YewGitEditorStats *out)
{
    if (out == NULL)
        return;
    *out = ed == NULL || ed->git_editor == NULL ?
        (YewGitEditorStats){0} : ed->git_editor->diff_stats;
}

void yew_git_editor_test_clock(Ed *ed, YewDiffNowUsFn now_us, void *ctx)
{
    if (ed == NULL || ed->git_editor == NULL)
        return;
    ed->git_editor->diff_now_us = now_us == NULL ? git_editor_now_us : now_us;
    ed->git_editor->diff_clock_ctx = now_us == NULL ? NULL : ctx;
}

bool yew_git_editor_test_base(Ed *ed, Buffer *buf, const u8 *bytes,
                              size_t len, const char *oid,
                              bool base_is_head, i64 now_ms)
{
    GitBufState *state;

    if (ed == NULL || ed->git_editor == NULL || buf == NULL ||
        buf->tb == NULL || (bytes == NULL && len != 0U))
        return false;
    state = state_for(ed, buf, true);
    diff_cancel(ed->git_editor, state, true);
    state->base.len = 0U;
    bytebuf_append(&state->base, bytes, len);
    oid_copy(state->base_oid, oid);
    state->base_ready = true;
    state->base_pending = false;
    state->base_too_large = len > YEW_DIFF_MAX_BYTES;
    state->base_is_head = base_is_head;
    state->base_revision++;
    state->observed_gen = buf->tb->gen;
    state->diff_due_ms = now_ms;
    return true;
}

const HunkList *yew_git_editor_test_hunks(Ed *ed, Buffer *buf)
{
    GitBufState *state = state_for(ed, buf, false);

    return state == NULL ? NULL : &state->hunks;
}

static bool text_has_terminal_newline(const TextBuf *tb)
{
    u64 len = yew_textbuf_len(tb);
    TextIter it;
    const u8 *bytes;
    u64 n;

    return len != 0U &&
        yew_textiter_begin(&it, tb, BYTEOFF(len - 1U)) &&
        yew_textiter_chunk(&it, tb, &bytes, &n) && n != 0U &&
        bytes[0] == '\n';
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

    if (ed == NULL || ed->git_editor == NULL || w == NULL ||
        w->buf == NULL || w->buf->tb == NULL || w->buf->path == NULL)
        return;
    state = state_for(ed, w->buf, true);
    gen = w->buf->tb->gen;
    if (gen != state->observed_gen) {
        diff_cancel(ed->git_editor, state, true);
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
        LineNo sign_line;
        bool delete_below;
        u64 n = h->buf_n.v == 0U ? 1U : h->buf_n.v;
        u64 j;
        u64 count = yew_textbuf_line_count(w->buf->tb);

        if (!yew_git_hunk_sign_placement(
                h, count, text_has_terminal_newline(w->buf->tb),
                &sign_line, &delete_below))
            continue;

        if (state->hunks.truncated) {
            sign = (GutterSign){unknown_glyph, 1U, "git.sign.unknown", 0U};
        } else if (h->kind == YEW_HUNK_DEL) {
            sign = (GutterSign){delete_below ? del_end_glyph : del_top_glyph,
                                3U,
                                "git.sign.del", 0U};
            if (delete_below)
                n = 1U;
        } else {
            sign = (GutterSign){add_glyph, 3U,
                state->base_is_head ? "git.sign.conflict" :
                h->kind == YEW_HUNK_ADD ? "git.sign.add" : "git.sign.mod",
                0U};
        }
        for (j = 0U; j < n; j++) {
            u64 line = sign_line.v + j;
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

        if (buf != NULL && buf->tb != NULL && diff_active(state) &&
            (buf->tb->gen != state->diff_gen ||
             state->base_revision != state->diff_base_revision)) {
            diff_cancel(ed->git_editor, state, true);
            state->observed_gen = buf->tb->gen;
            state->diff_due_ms = now_ms + GIT_EDIT_DEBOUNCE_MS;
        }
        if (buf != NULL && buf->tb != NULL && state->base_ready &&
            (diff_active(state) ||
             (state->diff_due_ms >= 0 && now_ms >= state->diff_due_ms)))
            diff_pump(ed, buf, state);
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
        const GitBufState *state = &ed->git_editor->v[i];
        i64 at = state->diff_due_ms;
        i64 wait;

        if (diff_active(state))
            return 0;
        if (at < 0 || !state->base_ready)
            continue;
        wait = at <= now_ms ? 0 : at - now_ms;
        if (best < 0 || wait < best)
            best = wait;
    }
    return best;
}

typedef struct HunkSelection {
    LineNo first;
    LineNo last;
    bool active;
} HunkSelection;

static HunkSelection cursor_hunk_selection(const Win *w, const Cursor *cursor)
{
    HunkSelection selection = {LINENO(0U), LINENO(0U), false};
    const TextBuf *tb;
    Span span;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL || cursor == NULL)
        return selection;
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

static bool window_has_selection(const Win *w)
{
    size_t i;

    if (w == NULL)
        return false;
    for (i = 0U; i < w->cs.curs.len; i++)
        if (w->cs.curs.data[i].anchor.v != w->cs.curs.data[i].pos.v)
            return true;
    return false;
}

static bool hunk_contains_line(const GitHunk *h, LineNo line)
{
    u64 count;

    if (h == NULL)
        return false;
    count = h->buf_n.v == 0U ? 1U : h->buf_n.v;
    return line.v >= h->buf_lo.v && line.v < h->buf_lo.v + count;
}

static bool hunk_is_target(const Win *w, const GitHunk *candidate,
                           bool selections_active)
{
    size_t i;

    if (w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        candidate == NULL)
        return false;
    for (i = 0U; i < w->cs.curs.len; i++) {
        const Cursor *cursor = &w->cs.curs.data[i];

        if (selections_active) {
            HunkSelection selection = cursor_hunk_selection(w, cursor);

            if (hunk_intersects_selection(candidate, selection))
                return true;
        } else if (hunk_contains_line(
                       candidate, yew_textbuf_line_of(w->buf->tb,
                                                      cursor->pos))) {
            return true;
        }
    }
    return false;
}

static bool stage_patch_run(Bytebuf *patch, const char *path,
                            const GitBufState *state, const Bytebuf *live,
                            const GitHunk *first, const GitHunk *last)
{
    GitHunk combined = *first;
    Bytebuf section;

    if (last != first) {
        combined.base_n = LINENO(last->base_lo.v + last->base_n.v -
                                 first->base_lo.v);
        combined.buf_n = LINENO(last->buf_lo.v + last->buf_n.v -
                                first->buf_lo.v);
        combined.kind = YEW_HUNK_MOD;
    }
    if (!yew_git_hunk_patch(&section, path, state->base.data, state->base.len,
                            live->data, live->len, &combined))
        return false;
    bytebuf_append(patch, section.data, section.len);
    bytebuf_free(&section);
    return true;
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
    Bytebuf live;
    Bytebuf patch;
    StageJob *owner;
    const char *path;
    char *argv[] = {(char *)"apply", (char *)"--cached", (char *)"-", NULL};
    char err[160];
    bool dirty;
    bool selections_active;
    size_t i;
    size_t target_count = 0U;
    const GitHunk *run_first = NULL;
    const GitHunk *run_last = NULL;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    selections_active = window_has_selection(cx->win);
    state = state_for(cx->ed, cx->win->buf, false);
    path = repo_path(cx->ed, cx->win->buf);
    if (state == NULL || path == NULL) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                selections_active ?
                    "selection does not intersect a changed hunk" :
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
        const GitHunk *candidate = &state->hunks.h.data[i];

        if (!hunk_is_target(cx->win, candidate, selections_active)) {
            if (run_first != NULL &&
                !stage_patch_run(&patch, path, state, &live,
                                 run_first, run_last)) {
                bytebuf_free(&patch); bytebuf_free(&live);
                return YEW_CMD_ERR_STATE;
            }
            run_first = NULL;
            run_last = NULL;
            continue;
        }
        if (run_first == NULL)
            run_first = candidate;
        run_last = candidate;
        target_count++;
    }
    if (run_first != NULL &&
        !stage_patch_run(&patch, path, state, &live, run_first, run_last)) {
        bytebuf_free(&patch); bytebuf_free(&live);
        return YEW_CMD_ERR_STATE;
    }
    if (target_count == 0U) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                selections_active ?
                    "selection does not intersect a changed hunk" :
                    "cursor is not on a changed hunk");
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
        yew_xfree(owner); bytebuf_free(&patch); bytebuf_free(&live);
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
    EditCtx edit;
    bool ok = true;
    bool own_transaction;
    bool selections_active;
    bool *targeted;
    TextBuf *base;
    size_t i;
    size_t target_count = 0U;

    if (cx == NULL || cx->ed == NULL || cx->win == NULL)
        return YEW_CMD_ERR_STATE;
    selections_active = window_has_selection(cx->win);
    state = state_for(cx->ed, cx->win->buf, false);
    if (state == NULL)
        return YEW_CMD_ERR_STATE;
    targeted = yew_xcalloc(state->hunks.h.len == 0U ? 1U :
                           state->hunks.h.len, sizeof(*targeted));
    for (i = 0U; i < state->hunks.h.len; i++) {
        targeted[i] = hunk_is_target(cx->win, &state->hunks.h.data[i],
                                     selections_active);
        if (targeted[i])
            target_count++;
    }
    if (target_count == 0U) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                selections_active ?
                    "selection does not intersect a changed hunk" :
                    "cursor is not on a changed hunk");
        yew_xfree(targeted);
        return YEW_CMD_ERR_STATE;
    }
    base = yew_textbuf_from_bytes(state->base.data, (u64)state->base.len);
    edit = yew_ed_edit_ctx_for(cx->ed, cx->win);
    own_transaction = edit.undo->depth == 0U;
    if (own_transaction)
        yew_undo_begin(&edit, YEW_TXN_EXTERNAL);
    i = state->hunks.h.len;
    while (i > 0U && ok) {
        const GitHunk *target;
        Span removed;
        Span restored;

        i--;
        target = &state->hunks.h.data[i];
        if (!targeted[i])
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
    if (own_transaction) {
        if (ok)
            yew_undo_end(&edit);
        else
            yew_undo_abort(&edit);
    }
    yew_ed_finish_edit(cx->ed, &edit);
    yew_textbuf_free(base);
    yew_xfree(targeted);
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
    wall_now = yew_git_editor_wall_now(ed);
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
    /* These internal, read-only buffers are populated outside the edit
     * chokepoint.  Reattach after the direct insert so the syntax line-state
     * arrays describe the finished buffers before their first draw. */
    yew_syn_attach(&left->syn, YEW_LANG_NONE, left->tb);
    yew_syn_attach(&right->syn, YEW_LANG_NONE, right->tb);
    yew_ed_win_set_buffer(cx->ed, split->a->win, left);
    yew_ed_win_set_buffer(cx->ed, right_leaf->win, right);
    split->a->win->git_diff_intra_add = false;
    right_leaf->win->git_diff_intra_add = true;
    diff_row_styles(split->a->win, right_leaf->win, &map, &state->hunks);
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
