#define _XOPEN_SOURCE 700

#include "mod/lsp/rename.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/motion.h"
#include "mod/lsp/client.h"
#include "mod/lsp/sync.h"
#include "term/input.h"
#include "text/edit.h"
#include "text/piece.h"
#include "text/undo.h"
#include "ui/cmdline.h"
#include "ui/message.h"
#include "ui/panel.h"
#include "ui/tabs.h"
#include "ui/viewport.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "util/sort.h"

typedef struct RenameDraftEdit {
    u32 start_line;
    u32 start_chr;
    u32 end_line;
    u32 end_chr;
    const u8 *text;
    u32 len;
} RenameDraftEdit;

VEC_DECL(Vec_RenameDraftEdit, RenameDraftEdit);

typedef struct RenameDraftFile {
    char *path;
    Vec_RenameDraftEdit edits;
    i64 version;
    bool has_version;
} RenameDraftFile;

VEC_DECL(Vec_RenameDraftFile, RenameDraftFile);

typedef enum RenamePhase {
    RENAME_ASK = 1,
    RENAME_REQUEST,
    RENAME_CONFIRM,
    RENAME_DIFF
} RenamePhase;

struct LspRenameState {
    RenamePlan plan;
    LspGen ask_gen;
    LspGen request_gen;
    ByteOff cursor;
    char *old_name;
    char *new_name;
    u32 old_len;
    u32 new_len;
    u32 win_id;
    u32 buf_id;
    u32 server_id;
    u32 diff_buf_id;
    u64 request;
    u8 pos_enc;
    u8 phase;
};

#define YEW_RENAME_DIFF_NAME "[LSP Rename Diff]"

static bool rename_error(char err[YEW_RENAME_ERROR_MAX], const char *fmt,
                         ...)
{
    va_list ap;

    if (err != NULL) {
        va_start(ap, fmt);
        (void)vsnprintf(err, YEW_RENAME_ERROR_MAX, fmt, ap);
        va_end(ap);
    }
    return false;
}

static void *rename_arena_copy(Arena *arena, const void *bytes, size_t len)
{
    u8 *copy = arena_alloc(arena, len == 0U ? 1U : len, _Alignof(u8));

    if (len != 0U)
        (void)memcpy(copy, bytes, len);
    return copy;
}

void yew_lsp_rename_plan_init(RenamePlan *plan)
{
    if (plan == NULL)
        return;
    (void)memset(plan, 0, sizeof(*plan));
    arena_init(&plan->arena);
}

void yew_lsp_rename_plan_free(RenamePlan *plan)
{
    size_t i;

    if (plan == NULL)
        return;
    for (i = 0U; i < plan->files.len; i++)
        Vec_RenameEdit_free(&plan->files.data[i].edits);
    Vec_RenameFile_free(&plan->files);
    arena_free_all(&plan->arena);
    (void)memset(plan, 0, sizeof(*plan));
}

void yew_lsp_rename_plan_test_fail_at(RenamePlan *plan, size_t file_index,
                                      size_t edit_index)
{
    if (plan == NULL)
        return;
    plan->test_fail_file = file_index;
    plan->test_fail_edit = edit_index;
    plan->test_fail_enabled = true;
}

static void rename_drafts_free(Vec_RenameDraftFile *drafts)
{
    size_t i;

    for (i = 0U; i < drafts->len; i++)
        Vec_RenameDraftEdit_free(&drafts->data[i].edits);
    Vec_RenameDraftFile_free(drafts);
}

static bool rename_path_inside(const char *root, const char *path)
{
    size_t n;

    if (root == NULL || path == NULL)
        return false;
    n = strlen(root);
    if (n == 1U && root[0] == '/')
        return path[0] == '/';
    return strncmp(root, path, n) == 0 &&
           (path[n] == '\0' || path[n] == '/');
}

static char *rename_canonical_path(Ed *ed, const u8 *uri, u32 uri_len,
                                   char err[YEW_RENAME_ERROR_MAX])
{
    Bytebuf decoded;
    char *root;
    char *path;
    const char *decoded_path;
    struct stat st;
    int saved_errno;

    bytebuf_init(&decoded);
    if (!yew_lsp_path_of_uri(&decoded, uri, uri_len) || decoded.len == 0U ||
        memchr(decoded.data, 0, decoded.len) != NULL) {
        bytebuf_free(&decoded);
        (void)rename_error(err, "rename touches a non-file URI; refusing");
        return NULL;
    }
    bytebuf_push_u8(&decoded, 0U);
    decoded_path = (const char *)decoded.data;
    root = realpath(yew_ws_root(ed), NULL);
    path = realpath(decoded_path, NULL);
    saved_errno = errno;
    if (path == NULL) {
        (void)rename_error(err, "cannot edit %s: %s", decoded_path,
                           strerror(saved_errno));
        bytebuf_free(&decoded);
        free(root);
        return NULL;
    }
    bytebuf_free(&decoded);
    if (root == NULL || !rename_path_inside(root, path)) {
        (void)rename_error(err, "rename would edit outside the workspace: %s",
                           path);
        free(root);
        free(path);
        return NULL;
    }
    free(root);
    if (stat(path, &st) != 0) {
        saved_errno = errno;
        (void)rename_error(err, "cannot edit %s: %s", path,
                           strerror(saved_errno));
        free(path);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        (void)rename_error(err, "cannot edit %s: %s", path,
                           strerror(EINVAL));
        free(path);
        return NULL;
    }
    if (access(path, R_OK) != 0) {
        saved_errno = errno;
        (void)rename_error(err, "cannot edit %s: %s", path,
                           strerror(saved_errno));
        free(path);
        return NULL;
    }
    return path;
}

static RenameDraftFile *rename_draft_file(Ed *ed, RenamePlan *plan,
                                           Vec_RenameDraftFile *drafts,
                                           const u8 *uri, u32 uri_len,
                                           char err[YEW_RENAME_ERROR_MAX])
{
    RenameDraftFile file = {0};
    char *canonical;
    size_t i;

    canonical = rename_canonical_path(ed, uri, uri_len, err);
    if (canonical == NULL)
        return NULL;
    for (i = 0U; i < drafts->len; i++) {
        if (strcmp(drafts->data[i].path, canonical) == 0) {
            free(canonical);
            return &drafts->data[i];
        }
    }
    if (drafts->len >= YEW_RENAME_MAX_FILES) {
        (void)rename_error(err,
                           "rename spans %llu files; refusing (limit %u)",
                           (unsigned long long)(drafts->len + 1U),
                           (unsigned)YEW_RENAME_MAX_FILES);
        free(canonical);
        return NULL;
    }
    file.path = arena_strdup(&plan->arena, canonical);
    free(canonical);
    Vec_RenameDraftFile_push(drafts, file);
    return &drafts->data[drafts->len - 1U];
}

static bool rename_position(const JsonValue *value, u32 *line, u32 *chr)
{
    const JsonValue *linev;
    const JsonValue *chrv;

    if (value == NULL || value->kind != YEW_JS_OBJ)
        return false;
    linev = yew_json_get(value, "line");
    chrv = yew_json_get(value, "character");
    if (linev == NULL || chrv == NULL || linev->kind != YEW_JS_INT ||
        chrv->kind != YEW_JS_INT || linev->i < 0 || chrv->i < 0 ||
        (u64)linev->i > UINT32_MAX || (u64)chrv->i > UINT32_MAX)
        return false;
    *line = (u32)linev->i;
    *chr = (u32)chrv->i;
    return true;
}

static bool rename_draft_edit(const JsonValue *value, RenameDraftEdit *out)
{
    const JsonValue *range;
    const JsonValue *new_text;

    if (value == NULL || value->kind != YEW_JS_OBJ || out == NULL)
        return false;
    range = yew_json_get(value, "range");
    new_text = yew_json_get(value, "newText");
    if (range == NULL || range->kind != YEW_JS_OBJ || new_text == NULL ||
        new_text->kind != YEW_JS_STR ||
        !rename_position(yew_json_get(range, "start"), &out->start_line,
                         &out->start_chr) ||
        !rename_position(yew_json_get(range, "end"), &out->end_line,
                         &out->end_chr))
        return false;
    out->text = new_text->s.p;
    out->len = new_text->s.len;
    return true;
}

static bool rename_add_edits(RenameDraftFile *file, const JsonValue *edits,
                             u32 *total, char err[YEW_RENAME_ERROR_MAX])
{
    u32 i;

    if (file == NULL || edits == NULL || edits->kind != YEW_JS_ARR)
        return rename_error(err, "server returned an invalid rename edit; refusing");
    for (i = 0U; i < edits->arr.n; i++) {
        RenameDraftEdit edit = {0};

        if (*total >= YEW_RENAME_MAX_EDITS)
            return rename_error(err,
                "rename spans %u edits; refusing (limit %u)",
                *total + 1U, (unsigned)YEW_RENAME_MAX_EDITS);
        if (!rename_draft_edit(edits->arr.v[i], &edit))
            return rename_error(err, "rename produced an invalid range in %s:1",
                                file->path);
        Vec_RenameDraftEdit_push(&file->edits, edit);
        (*total)++;
    }
    return true;
}

static bool rename_set_version(RenameDraftFile *file,
                               const JsonValue *version,
                               char err[YEW_RENAME_ERROR_MAX])
{
    if (version == NULL || version->kind == YEW_JS_NULL)
        return true;
    if (version->kind != YEW_JS_INT ||
        (file->has_version && file->version != version->i))
        return rename_error(err,
                            "rename is stale; the buffer changed. try again");
    file->version = version->i;
    file->has_version = true;
    return true;
}

static bool rename_parse_document_changes(Ed *ed, RenamePlan *plan,
                                          Vec_RenameDraftFile *drafts,
                                          const JsonValue *changes,
                                          u32 *total,
                                          char err[YEW_RENAME_ERROR_MAX])
{
    u32 i;

    if (changes == NULL || changes->kind != YEW_JS_ARR)
        return rename_error(err, "server returned an invalid rename edit; refusing");
    for (i = 0U; i < changes->arr.n; i++) {
        const JsonValue *item = changes->arr.v[i];
        const JsonValue *document;
        const JsonValue *uriv;
        const u8 *uri;
        u32 uri_len = 0U;
        RenameDraftFile *file;

        if (item == NULL || item->kind != YEW_JS_OBJ)
            return rename_error(err,
                                "server returned an invalid rename edit; refusing");
        if (yew_json_get(item, "kind") != NULL)
            return rename_error(err,
                "server asked to create or delete files; refusing (not supported in 1.0)");
        document = yew_json_get(item, "textDocument");
        if (document == NULL || document->kind != YEW_JS_OBJ)
            return rename_error(err,
                                "rename touches a non-file URI; refusing");
        uriv = yew_json_get(document, "uri");
        uri = yew_json_str(uriv, &uri_len);
        if (uri == NULL)
            return rename_error(err,
                                "rename touches a non-file URI; refusing");
        file = rename_draft_file(ed, plan, drafts, uri, uri_len, err);
        if (file == NULL ||
            !rename_set_version(file, yew_json_get(document, "version"), err) ||
            !rename_add_edits(file, yew_json_get(item, "edits"), total, err))
            return false;
    }
    return true;
}

static bool rename_parse_changes(Ed *ed, RenamePlan *plan,
                                 Vec_RenameDraftFile *drafts,
                                 const JsonValue *changes, u32 *total,
                                 char err[YEW_RENAME_ERROR_MAX])
{
    u32 i;

    if (changes == NULL || changes->kind != YEW_JS_OBJ)
        return rename_error(err, "server returned an invalid rename edit; refusing");
    for (i = 0U; i < changes->obj.n; i++) {
        const JsonMember *member = &changes->obj.m[i];
        RenameDraftFile *file = rename_draft_file(
            ed, plan, drafts, member->key, member->klen, err);

        if (file == NULL ||
            !rename_add_edits(file, member->val, total, err))
            return false;
    }
    return true;
}

static bool rename_offset(const TextBuf *tb, u8 pos_enc, u32 line, u32 chr,
                          ByteOff *out)
{
    i64 exact_line;
    i64 exact_chr;

    if (tb == NULL || out == NULL ||
        (u64)line >= yew_textbuf_line_count(tb))
        return false;
    *out = yew_lsp_off_of_pos(pos_enc, tb, LINENO(line), chr);
    yew_lsp_pos_of_off(pos_enc, tb, *out, &exact_line, &exact_chr);
    return exact_line == (i64)line && exact_chr == (i64)chr &&
           yew_is_grapheme_boundary(tb, *out);
}

static int rename_edit_cmp(const void *av, const void *bv, void *ctx)
{
    const RenameEdit *a = av;
    const RenameEdit *b = bv;

    (void)ctx;
    if (a->lo.v != b->lo.v)
        return a->lo.v > b->lo.v ? -1 : 1;
    if (a->hi.v != b->hi.v)
        return a->hi.v > b->hi.v ? -1 : 1;
    return 0;
}

static int rename_file_cmp(const void *av, const void *bv, void *ctx)
{
    const RenameFile *a = av;
    const RenameFile *b = bv;

    (void)ctx;
    return strcmp(a->path, b->path);
}

static bool rename_span_matches(const TextBuf *tb, Span span,
                                const u8 *text, u32 len)
{
    TextIter it;
    u64 matched = 0U;

    if (tb == NULL || span.lo > span.hi ||
        span.hi > yew_textbuf_len(tb) || span.hi - span.lo != len)
        return false;
    if (len == 0U)
        return true;
    if (text == NULL || !yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        return false;
    while (matched < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &available) ||
            available == 0U)
            return false;
        take = available < (u64)len - matched
            ? available : (u64)len - matched;
        if (memcmp(bytes, text + (size_t)matched, (size_t)take) != 0)
            return false;
        matched += take;
        if (matched < len && !yew_textiter_advance(&it, tb))
            return false;
    }
    return true;
}

static bool rename_file_convert(Ed *ed, RenamePlan *plan,
                                const RenameDraftFile *draft, u8 pos_enc,
                                char err[YEW_RENAME_ERROR_MAX])
{
    RenameFile file = {0};
    Buffer *buffer;
    LspDoc *doc;
    int tab;
    size_t i;

    if (draft->edits.len == 0U)
        return true;
    tab = yew_tab_find_by_path(ed, draft->path);
    file.was_open = tab >= 0;
    if (tab < 0)
        tab = yew_tab_open(ed, draft->path);
    if (tab < 0)
        return rename_error(err, "cannot edit %s: too many tabs", draft->path);
    buffer = yew_tab_buffer(ed, tab);
    errno = 0;
    if (buffer == NULL || yew_buf_hydrate(ed, buffer) != 0)
        return rename_error(err, "cannot edit %s: %s", draft->path,
                            strerror(errno == 0 ? EIO : errno));
    file.path = draft->path;
    file.buf_id = buffer->id;
    file.tab_id = yew_tab_at(ed, tab)->tab_id;
    file.undo_before = yew_undo_current(buffer->undo);
    file.buf_gen = buffer->tb->gen;
    file.was_dirty = yew_buf_dirty(buffer);
    file.has_version = draft->has_version;
    file.version = draft->version;
    doc = yew_lsp_doc_for_buffer(ed, buffer);
    if (file.has_version && (doc == NULL || doc->version != file.version))
        return rename_error(err,
                            "rename is stale; the buffer changed. try again");
    for (i = 0U; i < draft->edits.len; i++) {
        const RenameDraftEdit *source = &draft->edits.data[i];
        RenameEdit edit = {0};

        if (!rename_offset(buffer->tb, pos_enc, source->start_line,
                           source->start_chr, &edit.lo) ||
            !rename_offset(buffer->tb, pos_enc, source->end_line,
                           source->end_chr, &edit.hi) ||
            edit.lo.v > edit.hi.v) {
            Vec_RenameEdit_free(&file.edits);
            return rename_error(err,
                "rename produced an invalid range in %s:%llu", draft->path,
                (unsigned long long)source->start_line + 1U);
        }
        edit.len = source->len;
        edit.text = rename_arena_copy(&plan->arena, source->text, source->len);
        Vec_RenameEdit_push(&file.edits, edit);
    }
    if (file.edits.len > 1U)
        yew_sort_stable(file.edits.data, file.edits.len,
                        sizeof(file.edits.data[0]), rename_edit_cmp, NULL);
    for (i = 1U; i < file.edits.len; i++) {
        const RenameEdit *previous = &file.edits.data[i - 1U];
        const RenameEdit *current = &file.edits.data[i];

        if (current->lo.v == previous->lo.v || current->hi.v > previous->lo.v) {
            Vec_RenameEdit_free(&file.edits);
            return rename_error(err,
                "server sent overlapping edits for %s; refusing", draft->path);
        }
    }
    {
        size_t keep = 0U;

        for (i = 0U; i < file.edits.len; i++) {
            RenameEdit edit = file.edits.data[i];

            if (!rename_span_matches(buffer->tb,
                                     (Span){edit.lo.v, edit.hi.v},
                                     edit.text, edit.len))
                file.edits.data[keep++] = edit;
        }
        file.edits.len = keep;
    }
    if (file.edits.len == 0U) {
        Vec_RenameEdit_free(&file.edits);
        return true;
    }
    Vec_RenameFile_push(&plan->files, file);
    return true;
}

bool yew_lsp_rename_preflight(Ed *ed, const JsonValue *workspace_edit,
                              u8 pos_enc, const char *old_name,
                              const char *new_name, RenamePlan *plan,
                              char err[YEW_RENAME_ERROR_MAX])
{
    Vec_RenameDraftFile drafts = {0};
    const JsonValue *document_changes;
    const JsonValue *changes;
    u32 total = 0U;
    size_t i;
    bool ok = false;

    if (err != NULL)
        err[0] = '\0';
    if (ed == NULL || workspace_edit == NULL || plan == NULL ||
        old_name == NULL || new_name == NULL ||
        workspace_edit->kind != YEW_JS_OBJ) {
        (void)rename_error(err,
                          "server returned an invalid rename edit; refusing");
        return false;
    }
    yew_lsp_rename_plan_free(plan);
    yew_lsp_rename_plan_init(plan);
    plan->old_name = arena_strdup(&plan->arena, old_name);
    plan->new_name = arena_strdup(&plan->arena, new_name);
    document_changes = yew_json_get(workspace_edit, "documentChanges");
    changes = yew_json_get(workspace_edit, "changes");
    if (document_changes != NULL) {
        if (!rename_parse_document_changes(ed, plan, &drafts,
                                           document_changes, &total, err))
            goto done;
    } else if (changes != NULL) {
        if (!rename_parse_changes(ed, plan, &drafts, changes, &total, err))
            goto done;
    } else {
        (void)rename_error(err,
                          "server returned an invalid rename edit; refusing");
        goto done;
    }
    for (i = 0U; i < drafts.len; i++) {
        if (!rename_file_convert(ed, plan, &drafts.data[i], pos_enc, err))
            goto done;
    }
    if (plan->files.len > 1U)
        yew_sort_stable(plan->files.data, plan->files.len,
                        sizeof(plan->files.data[0]), rename_file_cmp, NULL);
    plan->nedits = 0U;
    for (i = 0U; i < plan->files.len; i++)
        plan->nedits += (u32)plan->files.data[i].edits.len;
    ok = true;
done:
    rename_drafts_free(&drafts);
    if (!ok) {
        yew_lsp_rename_plan_free(plan);
        yew_lsp_rename_plan_init(plan);
    }
    return ok;
}

static bool rename_file_current(Ed *ed, const RenameFile *file,
                                Buffer **out,
                                char err[YEW_RENAME_ERROR_MAX])
{
    Buffer *buffer;
    LspDoc *doc;

    buffer = yew_ws_buf_by_id(ed, file->buf_id);
    if (buffer == NULL || !yew_buf_resident(buffer) ||
        buffer->path == NULL || strcmp(buffer->path, file->path) != 0 ||
        buffer->tb->gen != file->buf_gen ||
        yew_undo_current(buffer->undo) != file->undo_before)
        return rename_error(err,
                            "rename is stale; the buffer changed. try again");
    doc = yew_lsp_doc_for_buffer(ed, buffer);
    if (file->has_version && (doc == NULL || doc->version != file->version))
        return rename_error(err,
                            "rename is stale; the buffer changed. try again");
    if (out != NULL)
        *out = buffer;
    return true;
}

typedef struct RenameCommit {
    u32 node;
    u32 parent;
} RenameCommit;

static const UndoNode *rename_undo_node(const UndoTree *undo, u32 id)
{
    const UndoNode *node;

    if (undo == NULL || id == 0U || (size_t)id > undo->nodes.len)
        return NULL;
    node = &undo->nodes.data[id - 1U];
    return node->id == id ? node : NULL;
}

static void rename_rollback(Ed *ed, const RenamePlan *plan,
                            const RenameCommit commits[YEW_RENAME_MAX_FILES],
                            size_t committed, YewTxnReason txn_reason)
{
    while (committed != 0U) {
        const RenameFile *file;
        const RenameCommit *commit;
        Buffer *buffer;
        const UndoNode *node;
        EditCtx ec;

        committed--;
        file = &plan->files.data[committed];
        commit = &commits[committed];
        buffer = yew_ws_buf_by_id(ed, file->buf_id);

        if (buffer == NULL)
            YEW_BUG("rename rollback lost buffer %u", file->buf_id);
        node = rename_undo_node(buffer->undo, commit->node);
        if (node == NULL || yew_undo_current(buffer->undo) != commit->node)
            YEW_BUG("rename rollback lost undo node for %s", file->path);
        if (node->parent != commit->parent ||
            node->reason != (u8)txn_reason || node->n_ops == 0U)
            YEW_BUG("rename rollback found wrong transaction for %s",
                    file->path);
        ec = yew_ed_edit_ctx_buffer(ed, buffer);
        if (!yew_undo(&ec))
            YEW_BUG("rename rollback lost transaction for %s", file->path);
        if (!yew_undo_prune_redo(buffer->undo, txn_reason))
            YEW_BUG("rename rollback could not prune transaction for %s",
                    file->path);
        yew_ed_finish_edit(ed, &ec);
        if (yew_undo_current(buffer->undo) != commit->parent)
            YEW_BUG("rename rollback did not restore undo cursor for %s",
                    file->path);
    }
}

bool yew_lsp_rename_apply(Ed *ed, RenamePlan *plan,
                          char err[YEW_RENAME_ERROR_MAX])
{
    size_t i;
    size_t committed = 0U;
    RenameCommit commits[YEW_RENAME_MAX_FILES] = {{0}};
    const YewTxnReason txn_reason = YEW_TXN_LSP;

    if (err != NULL)
        err[0] = '\0';
    if (ed == NULL || plan == NULL)
        return rename_error(err, "rename aborted: invalid plan");

    /* The event loop is single-threaded, but confirmation can sit open for
     * arbitrarily long.  Revalidate every participant before the first byte
     * changes so an intervening edit cannot turn this into a partial rename. */
    for (i = 0U; i < plan->files.len; i++) {
        if (!rename_file_current(ed, &plan->files.data[i], NULL, err))
            return false;
    }

    for (i = 0U; i < plan->files.len; i++) {
        const RenameFile *file = &plan->files.data[i];
        Buffer *buffer = yew_ws_buf_by_id(ed, file->buf_id);
        EditCtx ec = yew_ed_edit_ctx_buffer(ed, buffer);
        size_t j;
        char failure[160] = {0};

        yew_undo_begin(&ec, txn_reason);
        for (j = 0U; j < file->edits.len; j++) {
            const RenameEdit *edit = &file->edits.data[j];

            if (plan->test_fail_enabled && plan->test_fail_file == i &&
                plan->test_fail_edit == j) {
                (void)snprintf(failure, sizeof(failure),
                               "injected edit failure");
                break;
            }
            if (edit->lo.v != edit->hi.v &&
                !yew_edit_delete(&ec, (Span){edit->lo.v, edit->hi.v})) {
                (void)snprintf(failure, sizeof(failure),
                               "the crash journal rejected a deletion: %s",
                               strerror(errno == 0 ? EIO : errno));
                break;
            }
            if (edit->len != 0U &&
                !yew_edit_insert(&ec, edit->lo, edit->text, edit->len)) {
                (void)snprintf(failure, sizeof(failure),
                               "the crash journal rejected an insertion: %s",
                               strerror(errno == 0 ? EIO : errno));
                break;
            }
        }
        if (failure[0] != '\0') {
            yew_undo_abort(&ec);
            yew_ed_finish_edit(ed, &ec);
            rename_rollback(ed, plan, commits, committed, txn_reason);
            return rename_error(err,
                "rename aborted: %s in %s; %llu files rolled back",
                failure, file->path, (unsigned long long)committed);
        }
        yew_undo_end(&ec);
        yew_ed_finish_edit(ed, &ec);
        {
            u32 node_id = yew_undo_current(buffer->undo);
            const UndoNode *node = rename_undo_node(buffer->undo, node_id);

            if (node == NULL || node_id == file->undo_before ||
                node->reason != (u8)txn_reason || node->n_ops == 0U)
                YEW_BUG("rename did not commit one LSP transaction for %s",
                        file->path);
            commits[committed].node = node_id;
            commits[committed].parent = node->parent;
        }
        committed++;
    }
    ed->footer_dirty = true;
    return true;
}

static char *rename_span_copy(const TextBuf *tb, Span span, u32 *len_out)
{
    TextIter it;
    char *copy;
    u64 copied = 0U;
    u64 len;

    if (tb == NULL || span.lo > span.hi ||
        span.hi > yew_textbuf_len(tb) || span.hi - span.lo > UINT32_MAX)
        return NULL;
    len = span.hi - span.lo;
    copy = yew_xmalloc((size_t)len + 1U);
    if (len != 0U && !yew_textiter_begin(&it, tb, BYTEOFF(span.lo))) {
        free(copy);
        return NULL;
    }
    while (copied < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &available) ||
            available == 0U) {
            free(copy);
            return NULL;
        }
        take = available < len - copied ? available : len - copied;
        (void)memcpy(copy + (size_t)copied, bytes, (size_t)take);
        copied += take;
        if (copied < len && !yew_textiter_advance(&it, tb)) {
            free(copy);
            return NULL;
        }
    }
    copy[len] = '\0';
    if (len_out != NULL)
        *len_out = (u32)len;
    return copy;
}

static void rename_state_finish(Ed *ed, bool close_panel)
{
    LspRenameState *state;
    LspServer *server;
    Win *win;

    if (ed == NULL || ed->lsp_rename == NULL)
        return;
    state = ed->lsp_rename;
    if (ed->cmdline.active && ed->cmdline.input_ctx == state) {
        ed->cmdline.input_done = NULL;
        ed->cmdline.input_ctx = NULL;
    }
    if (state->request != 0U) {
        server = yew_lsp_server_by_id(ed, state->server_id);
        if (server != NULL) {
            yew_rpc_cancel(&server->rpc, state->request);
            (void)yew_rpc_drop(&server->rpc, state->request);
        }
    }
    win = yew_ed_win_by_id(ed, state->win_id);
    if (close_panel && win != NULL)
        yew_panel_close(ed, &win->panel);
    ed->lsp_rename = NULL;
    yew_lsp_rename_plan_free(&state->plan);
    free(state->old_name);
    free(state->new_name);
    free(state);
}

static bool rename_panel_anchor(Win *win, u16 *x, u16 *y)
{
    const Cursor *cursor;
    TextBuf *tb;
    LineNo line;
    Span displayed;
    CCol col;
    u32 sub;
    u16 row;

    if (win == NULL || x == NULL || y == NULL || win->buf == NULL ||
        win->buf->tb == NULL || win->cs.curs.len == 0U ||
        win->cs.primary >= win->cs.curs.len)
        return false;
    cursor = &win->cs.curs.data[win->cs.primary];
    tb = win->buf->tb;
    line = yew_textbuf_line_of(tb, cursor->pos);
    sub = win->vp.wrap ? yew_vp_cursor_subrow(win) : 0U;
    if (!yew_vp_row_of_line(win, line, sub, &row) || row >= win->rect.h)
        return false;
    displayed = win->vp.wrap ? yew_wrap_row(win, line, sub) :
                               yew_textbuf_line_span(tb, line);
    col = yew_off_to_ccol(tb, displayed, cursor->pos,
                          win->buf->tabwidth == 0U ? YEW_VP_TABWIDTH :
                                                    win->buf->tabwidth);
    *x = yew_vp_gridx_of_ccol(win, col);
    *y = (u16)(win->rect.y + row);
    return *x >= win->rect.x &&
           (u32)*x < (u32)win->rect.x + win->rect.w;
}

static const char *rename_relative_path(const Ed *ed, const char *path)
{
    const char *root = yew_ws_root(ed);
    size_t n;

    if (root == NULL || path == NULL)
        return path == NULL ? "" : path;
    n = strlen(root);
    if (n != 0U && strncmp(root, path, n) == 0 && path[n] == '/')
        return path + n + 1U;
    return path;
}

static bool rename_summary_open(Ed *ed, LspRenameState *state)
{
    Bytebuf body;
    PanelSpec spec = {0};
    Win *win;
    u32 dirty = 0U;
    u16 x;
    u16 y;
    size_t i;

    win = yew_ed_win_by_id(ed, state->win_id);
    if (win == NULL || ed->win != win || win->buf == NULL ||
        win->buf->id != state->buf_id || !rename_panel_anchor(win, &x, &y))
        return false;
    bytebuf_init(&body);
    for (i = 0U; i < state->plan.files.len; i++)
        if (state->plan.files.data[i].was_dirty)
            dirty++;
    bytebuf_printf(&body, "rename '%s' \xE2\x86\x92 '%s'\n", state->old_name,
                   state->new_name);
    bytebuf_printf(&body, "%u edits in %llu files, %u already modified\n\n",
                   (unsigned)state->plan.nedits,
                   (unsigned long long)state->plan.files.len,
                   (unsigned)dirty);
    for (i = 0U; i < state->plan.files.len; i++) {
        const RenameFile *file = &state->plan.files.data[i];

        bytebuf_printf(&body, "  %-40s %5llu%s\n",
                       rename_relative_path(ed, file->path),
                       (unsigned long long)file->edits.len,
                       file->was_dirty ? " *" : "");
    }
    bytebuf_append(&body,
                   "\nenter apply \xC2\xB7 esc cancel \xC2\xB7 d show diff",
                   sizeof("\nenter apply \xC2\xB7 esc cancel \xC2\xB7 d show diff") - 1U);
    if (body.len > UINT32_MAX) {
        bytebuf_free(&body);
        return false;
    }
    spec.title = "rename";
    spec.body = body.data;
    spec.len = (u32)body.len;
    spec.x = x;
    spec.y = y;
    spec.place = (u8)YEW_PANEL_CURSOR;
    spec.role = "bg";
    state->phase = (u8)RENAME_CONFIRM;
    if (!yew_panel_open(ed, &win->panel, &spec)) {
        state->phase = (u8)RENAME_REQUEST;
        bytebuf_free(&body);
        return false;
    }
    bytebuf_free(&body);
    return true;
}

static void rename_request_params(Bytebuf *out, const LspDoc *doc,
                                  u8 pos_enc, const TextBuf *tb,
                                  ByteOff cursor, const u8 *new_name,
                                  u32 new_len)
{
    JsonW writer;
    i64 line;
    i64 character;

    yew_lsp_pos_of_off(pos_enc, tb, cursor, &line, &character);
    yew_jsonw_init(&writer, out);
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "textDocument");
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "uri");
    yew_jsonw_cstr(&writer, doc->uri);
    yew_jsonw_obj_end(&writer);
    yew_jsonw_key(&writer, "position");
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "line");
    yew_jsonw_int(&writer, line);
    yew_jsonw_key(&writer, "character");
    yew_jsonw_int(&writer, character);
    yew_jsonw_obj_end(&writer);
    yew_jsonw_key(&writer, "newName");
    yew_jsonw_str(&writer, new_name, new_len);
    yew_jsonw_obj_end(&writer);
}

static void rename_response_done(Ed *ed, void *ctx,
                                 const JsonValue *result,
                                 const JsonValue *error)
{
    LspRenameState *state = ctx;
    LspServer *server;
    char err[YEW_RENAME_ERROR_MAX];
    i64 code;

    if (ed == NULL || state == NULL || ed->lsp_rename != state ||
        state->phase != RENAME_REQUEST)
        return;
    state->request = 0U;
    server = yew_lsp_server_by_id(ed, state->server_id);
    if (error != NULL) {
        code = yew_json_int(yew_json_get(error, "code"), 0);
        if (server != NULL && code == -32601)
            server->caps.bits &= ~YEW_LSPC_RENAME;
        yew_msg(ed, YEW_MSG_ERROR, "rename request failed");
        rename_state_finish(ed, true);
        return;
    }
    if (server == NULL || server->state != YEW_LSP_READY ||
        !yew_lsp_gen_current(ed, &state->request_gen)) {
        rename_state_finish(ed, true);
        return;
    }
    if (result == NULL || result->kind == YEW_JS_NULL) {
        yew_msg(ed, YEW_MSG_INFO, "no rename available here");
        rename_state_finish(ed, true);
        return;
    }
    if (!yew_lsp_rename_preflight(ed, result, state->pos_enc,
                                  state->old_name, state->new_name,
                                  &state->plan, err)) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        rename_state_finish(ed, true);
        return;
    }
    if (!rename_summary_open(ed, state)) {
        yew_msg(ed, YEW_MSG_ERROR, "cannot show rename confirmation");
        rename_state_finish(ed, true);
    }
}

static void rename_prompt_done(Ed *ed, bool accepted, const u8 *text,
                               size_t len, void *ctx)
{
    LspRenameState *state = ctx;
    LspDoc *doc;
    LspServer *server;
    Win *win;
    RpcPending pending = {0};
    Bytebuf params;
    u64 request;

    if (ed == NULL || state == NULL || ed->lsp_rename != state ||
        state->phase != RENAME_ASK)
        return;
    if (!accepted || len == 0U ||
        (len == state->old_len &&
         memcmp(text, state->old_name, len) == 0)) {
        rename_state_finish(ed, true);
        return;
    }
    if (len > UINT32_MAX || memchr(text, 0, len) != NULL ||
        !yew_lsp_gen_current(ed, &state->ask_gen)) {
        rename_state_finish(ed, true);
        return;
    }
    win = yew_ed_win_by_id(ed, state->win_id);
    if (win == NULL || ed->win != win || win->buf == NULL ||
        win->buf->id != state->buf_id) {
        rename_state_finish(ed, true);
        return;
    }
    state->new_name = yew_xmalloc(len + 1U);
    (void)memcpy(state->new_name, text, len);
    state->new_name[len] = '\0';
    state->new_len = (u32)len;
    yew_lsp_sync_flush(ed);
    doc = yew_lsp_doc_for_buffer(ed, win->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (doc == NULL || server == NULL || server->id != state->server_id ||
        server->state != YEW_LSP_READY ||
        !yew_lsp_has(server, YEW_LSPC_RENAME)) {
        yew_msg(ed, YEW_MSG_INFO, "rename is no longer available");
        rename_state_finish(ed, true);
        return;
    }
    state->request_gen = yew_lsp_gen(doc, win->buf->tb);
    state->pos_enc = server->pos_enc;
    bytebuf_init(&params);
    rename_request_params(&params, doc, server->pos_enc, win->buf->tb,
                          state->cursor, text, (u32)len);
    pending.buf_id = state->buf_id;
    pending.gen = win->buf->tb->gen;
    pending.sent_ms = ed->now_ms;
    pending.cb = rename_response_done;
    pending.ctx = state;
    state->phase = (u8)RENAME_REQUEST;
    request = params.len > UINT32_MAX ? 0U :
              yew_rpc_call(&server->rpc, "textDocument/rename",
                           params.data, (u32)params.len, &pending);
    bytebuf_free(&params);
    if (request == 0U) {
        yew_msg(ed, YEW_MSG_ERROR, "could not send rename request");
        rename_state_finish(ed, true);
        return;
    }
    state->request = request;
}

bool yew_lsp_rename_request(Ed *ed, Win *win)
{
    LspRenameState *state;
    LspDoc *doc;
    LspServer *server;
    UnitCtx unit;
    Span word;
    const Cursor *cursor;

    if (ed == NULL || win == NULL || win->buf == NULL ||
        win->buf->tb == NULL)
        return false;
    if (win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len) {
        yew_msg(ed, YEW_MSG_INFO, "rename requires exactly one cursor");
        return true;
    }
    doc = yew_lsp_doc_for_buffer(ed, win->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (doc == NULL || server == NULL || server->state != YEW_LSP_READY ||
        !yew_lsp_has(server, YEW_LSPC_RENAME))
        return false;
    if (ed->lsp_rename != NULL) {
        if (ed->cmdline.active &&
            ed->cmdline.input_ctx == ed->lsp_rename)
            yew_cmdline_close(ed, false);
        else
            rename_state_finish(ed, true);
    }
    cursor = &win->cs.curs.data[win->cs.primary];
    unit = (UnitCtx){win->buf->tb, win->buf, win};
    word = yew_unit_word.span(&unit, cursor->pos, false);
    if (word.lo == word.hi) {
        yew_msg(ed, YEW_MSG_INFO, "no identifier under the cursor");
        return true;
    }
    state = yew_xcalloc(1U, sizeof(*state));
    yew_lsp_rename_plan_init(&state->plan);
    state->old_name = rename_span_copy(win->buf->tb, word,
                                       &state->old_len);
    if (state->old_name == NULL) {
        yew_lsp_rename_plan_free(&state->plan);
        free(state);
        return false;
    }
    state->ask_gen = yew_lsp_gen(doc, win->buf->tb);
    state->cursor = cursor->pos;
    state->win_id = win->id;
    state->buf_id = win->buf->id;
    state->server_id = server->id;
    state->pos_enc = server->pos_enc;
    state->phase = (u8)RENAME_ASK;
    ed->lsp_rename = state;
    yew_cmdline_open_input(ed, state->old_name, rename_prompt_done, state);
    if (!ed->cmdline.active || ed->cmdline.input_ctx != state) {
        rename_state_finish(ed, true);
        return false;
    }
    return true;
}

static void rename_text_append(Bytebuf *out, const TextBuf *tb)
{
    TextIter it;

    if (yew_textbuf_len(tb) == 0U)
        return;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(0U)))
        YEW_BUG("rename diff cannot iterate buffer");
    do {
        const u8 *bytes;
        u64 len;

        if (!yew_textiter_chunk(&it, tb, &bytes, &len))
            YEW_BUG("rename diff iterator truncated");
        bytebuf_append(out, bytes, (size_t)len);
    } while (yew_textiter_advance(&it, tb));
}

static u64 rename_diff_lines(const Bytebuf *text)
{
    u64 lines = 0U;
    size_t i;

    for (i = 0U; i < text->len; i++)
        if (text->data[i] == '\n')
            lines++;
    if (text->len != 0U && text->data[text->len - 1U] != '\n')
        lines++;
    return lines;
}

static void rename_diff_prefixed(Bytebuf *out, const Bytebuf *text, u8 prefix)
{
    size_t start = 0U;

    while (start < text->len) {
        const u8 *newline = memchr(text->data + start, '\n',
                                   text->len - start);
        size_t end = newline == NULL ? text->len :
                     (size_t)(newline - text->data) + 1U;

        bytebuf_push_u8(out, prefix);
        bytebuf_append(out, text->data + start, end - start);
        if (newline == NULL) {
            bytebuf_append(out, "\n\\ No newline at end of file\n",
                           sizeof("\n\\ No newline at end of file\n") - 1U);
        }
        start = end;
    }
}

static void rename_diff_file(Ed *ed, const RenameFile *file, Bytebuf *out)
{
    Buffer *buffer = yew_ws_buf_by_id(ed, file->buf_id);
    Bytebuf before;
    Bytebuf after;
    TextBuf *changed;
    const char *relative = rename_relative_path(ed, file->path);
    size_t i;

    if (buffer == NULL || buffer->tb == NULL)
        YEW_BUG("rename diff lost buffer %u", file->buf_id);
    bytebuf_init(&before);
    bytebuf_init(&after);
    rename_text_append(&before, buffer->tb);
    changed = yew_textbuf_from_bytes(before.data, before.len);
    for (i = 0U; i < file->edits.len; i++) {
        const RenameEdit *edit = &file->edits.data[i];

        if (edit->lo.v != edit->hi.v)
            yew_textbuf_delete(changed, (Span){edit->lo.v, edit->hi.v});
        if (edit->len != 0U)
            yew_textbuf_insert(changed, edit->lo, edit->text, edit->len);
    }
    rename_text_append(&after, changed);
    yew_textbuf_free(changed);
    bytebuf_printf(out, "--- a/%s\n+++ b/%s\n", relative, relative);
    bytebuf_printf(out, "@@ -1,%llu +1,%llu @@\n",
                   (unsigned long long)rename_diff_lines(&before),
                   (unsigned long long)rename_diff_lines(&after));
    rename_diff_prefixed(out, &before, '-');
    rename_diff_prefixed(out, &after, '+');
    bytebuf_free(&before);
    bytebuf_free(&after);
}

static bool rename_diff_open(Ed *ed, LspRenameState *state)
{
    Buffer *diff;
    Bytebuf body;
    EditCtx ec;
    u64 old_len;
    size_t i;

    bytebuf_init(&body);
    for (i = 0U; i < state->plan.files.len; i++) {
        if (i != 0U)
            bytebuf_push_u8(&body, '\n');
        rename_diff_file(ed, &state->plan.files.data[i], &body);
    }
    diff = yew_ws_scratch_find(ed, YEW_RENAME_DIFF_NAME);
    if (diff == NULL)
        diff = yew_ws_scratch_new(ed, YEW_RENAME_DIFF_NAME,
                                  YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (diff == NULL) {
        bytebuf_free(&body);
        return false;
    }
    /* This derived scratch text still goes through the edit choke point:
     * syntax line state, marks, and any other view of the buffer must see
     * the wholesale replacement.  It is deliberately outside undo. */
    ec = yew_ed_edit_ctx_buffer(ed, diff);
    ec.undo = NULL;
    old_len = yew_textbuf_len(diff->tb);
    if (old_len != 0U)
        (void)yew_edit_delete(&ec, (Span){0U, old_len});
    if (body.len != 0U)
        (void)yew_edit_insert(&ec, BYTEOFF(0U), body.data, body.len);
    yew_ed_finish_edit(ed, &ec);
    bytebuf_free(&body);
    if (!yew_ed_show_buffer(ed, diff))
        return false;
    state->diff_buf_id = diff->id;
    state->phase = (u8)RENAME_DIFF;
    yew_msg(ed, YEW_MSG_INFO,
            "rename diff: enter apply, esc cancel, d return to summary");
    return true;
}

static bool rename_diff_return(Ed *ed, LspRenameState *state)
{
    Win *win = yew_ed_win_by_id(ed, state->win_id);
    Buffer *origin = yew_ws_buf_by_id(ed, state->buf_id);

    if (win == NULL || origin == NULL || ed->win != win)
        return false;
    if (!yew_ed_show_buffer(ed, origin))
        return false;
    return rename_summary_open(ed, state);
}

static void rename_apply_confirmed(Ed *ed, LspRenameState *state)
{
    char err[YEW_RENAME_ERROR_MAX];
    u32 edits = state->plan.nedits;
    size_t files = state->plan.files.len;
    bool ok;

    ok = yew_lsp_rename_apply(ed, &state->plan, err);
    rename_state_finish(ed, true);
    if (!ok) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        return;
    }
    yew_msg(ed, YEW_MSG_INFO,
            "renamed %u occurrences in %llu files (unsaved \xE2\x80\x94 :wa to write)",
            (unsigned)edits, (unsigned long long)files);
}

bool yew_lsp_rename_key(Ed *ed, const Key *key)
{
    LspRenameState *state;

    if (ed == NULL || key == NULL || ed->lsp_rename == NULL)
        return false;
    state = ed->lsp_rename;
    if (state->phase != RENAME_CONFIRM && state->phase != RENAME_DIFF)
        return false;
    if (key->ev == YEW_KEY_RELEASE)
        return true;
    if (key->code == YEW_KEY_ESCAPE) {
        rename_state_finish(ed, true);
        return true;
    }
    if (key->code == YEW_KEY_ENTER || key->code == YEW_KEY_KP_ENTER) {
        rename_apply_confirmed(ed, state);
        return true;
    }
    if (key->mods == 0U && key->code == (u32)'d') {
        if (state->phase == RENAME_CONFIRM) {
            Win *win = yew_ed_win_by_id(ed, state->win_id);

            if (win != NULL)
                yew_panel_close(ed, &win->panel);
            if (!rename_diff_open(ed, state)) {
                yew_msg(ed, YEW_MSG_ERROR, "cannot show rename diff");
                rename_state_finish(ed, true);
            }
        } else if (!rename_diff_return(ed, state)) {
            yew_msg(ed, YEW_MSG_INFO,
                    "return to the rename window before reopening summary");
        }
        return true;
    }
    if (state->phase == RENAME_CONFIRM) {
        if (key->mods == 0U &&
            (key->code == YEW_KEY_UP || key->code == YEW_KEY_DOWN ||
             key->code == YEW_KEY_PAGE_UP ||
             key->code == YEW_KEY_PAGE_DOWN)) {
            Win *win = yew_ed_win_by_id(ed, state->win_id);

            if (win != NULL)
                (void)yew_panel_key(ed, &win->panel, key);
            return true;
        }
        rename_state_finish(ed, true);
        return false;
    }
    return false;
}

void yew_lsp_rename_shutdown(Ed *ed)
{
    rename_state_finish(ed, true);
}
