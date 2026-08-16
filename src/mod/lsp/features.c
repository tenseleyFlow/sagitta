/*
 * Sprint 47 snippet downgrade policy.
 *
 * yew 1.0 does not expand snippets: there is no tab-stop mode, placeholder
 * navigation, or mirrored-field machinery.  The client advertises
 * snippetSupport:false, but servers sometimes ignore it, so every
 * insertTextFormat==2 item is downgraded here to deterministic plain text.
 * Defaults are retained, choices select their first value, variables and
 * bare tab stops disappear, and $0 records the final cursor position.
 * Snippet expansion is a post-1.0 feature, not a stub or TODO.
 */
#include "mod/lsp/features.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "mod/lsp/client.h"
#include "mod/lsp/sync.h"
#include "text/edit.h"
#include "text/undo.h"
#include "ui/win.h"
#include "unicode/coords.h"
#include "unicode/utf8.h"
#include "util/sort.h"
#include "ws/finder.h"

enum { SNIPPET_DEPTH_MAX = 8 };

typedef struct SnippetStrip {
    const u8 *in;
    Bytebuf *out;
    size_t base;
    u32 cursor;
    bool have_cursor;
} SnippetStrip;

static bool snippet_ident_start(u8 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool snippet_ident_continue(u8 c)
{
    return snippet_ident_start(c) || (c >= '0' && c <= '9');
}

static bool snippet_escaped(u8 c)
{
    return c == '$' || c == '\\' || c == '}' || c == ',' || c == '|';
}

static void snippet_cursor(SnippetStrip *s)
{
    size_t offset;

    if (s->have_cursor)
        return;
    offset = s->out->len - s->base;
    s->cursor = offset > UINT32_MAX ? UINT32_MAX : (u32)offset;
    s->have_cursor = true;
}

static bool snippet_is_zero(const u8 *in, u32 lo, u32 hi)
{
    u32 at;

    if (lo == hi)
        return false;
    for (at = lo; at < hi; at++)
        if (in[at] != '0')
            return false;
    return true;
}

static bool snippet_brace_end(const u8 *in, u32 n, u32 open, u32 *close)
{
    u32 at = open + 2U;
    u32 depth = 1U;

    while (at < n) {
        if (in[at] == '\\' && at + 1U < n) {
            at += 2U;
            continue;
        }
        if (in[at] == '$' && at + 1U < n && in[at + 1U] == '{') {
            depth++;
            at += 2U;
            continue;
        }
        if (in[at] == '}') {
            depth--;
            if (depth == 0U) {
                *close = at;
                return true;
            }
        }
        at++;
    }
    return false;
}

static void snippet_range(SnippetStrip *s, u32 lo, u32 hi, u8 depth);

static bool snippet_braced(SnippetStrip *s, u32 at, u32 hi, u8 depth,
                           u32 *after)
{
    u32 close;
    u32 id_lo;
    u32 id_hi;
    bool numeric;

    if (!snippet_brace_end(s->in, hi, at, &close))
        return false;
    id_lo = at + 2U;
    id_hi = id_lo;
    numeric = id_hi < close && s->in[id_hi] >= '0' && s->in[id_hi] <= '9';
    if (numeric) {
        while (id_hi < close && s->in[id_hi] >= '0' &&
               s->in[id_hi] <= '9')
            id_hi++;
    } else if (id_hi < close && snippet_ident_start(s->in[id_hi])) {
        while (id_hi < close && snippet_ident_continue(s->in[id_hi]))
            id_hi++;
    } else {
        return false;
    }
    *after = close + 1U;
    if (numeric && snippet_is_zero(s->in, id_lo, id_hi))
        snippet_cursor(s);
    if (id_hi == close)
        return true;
    if (depth >= SNIPPET_DEPTH_MAX)
        return true;
    if (s->in[id_hi] == ':') {
        snippet_range(s, id_hi + 1U, close, (u8)(depth + 1U));
        return true;
    }
    if (s->in[id_hi] == '|' && close > id_hi + 1U &&
        s->in[close - 1U] == '|') {
        u32 choice_hi = id_hi + 1U;

        while (choice_hi < close - 1U) {
            if (s->in[choice_hi] == '\\' && choice_hi + 1U < close - 1U) {
                choice_hi += 2U;
                continue;
            }
            if (s->in[choice_hi] == ',')
                break;
            choice_hi++;
        }
        snippet_range(s, id_hi + 1U, choice_hi, (u8)(depth + 1U));
        return true;
    }
    return false;
}

static bool snippet_dollar(SnippetStrip *s, u32 at, u32 hi, u8 depth,
                           u32 *after)
{
    u32 end;

    if (at + 1U >= hi)
        return false;
    if (s->in[at + 1U] == '{')
        return snippet_braced(s, at, hi, depth, after);
    end = at + 1U;
    if (s->in[end] >= '0' && s->in[end] <= '9') {
        while (end < hi && s->in[end] >= '0' && s->in[end] <= '9')
            end++;
        if (snippet_is_zero(s->in, at + 1U, end))
            snippet_cursor(s);
        *after = end;
        return true;
    }
    if (snippet_ident_start(s->in[end])) {
        while (end < hi && snippet_ident_continue(s->in[end]))
            end++;
        *after = end;
        return true;
    }
    return false;
}

static void snippet_range(SnippetStrip *s, u32 lo, u32 hi, u8 depth)
{
    u32 at = lo;

    while (at < hi) {
        u32 after;

        if (s->in[at] == '\\' && at + 1U < hi &&
            snippet_escaped(s->in[at + 1U])) {
            bytebuf_push_u8(s->out, s->in[at + 1U]);
            at += 2U;
        } else if (s->in[at] == '$' &&
                   snippet_dollar(s, at, hi, depth, &after)) {
            at = after;
        } else {
            bytebuf_push_u8(s->out, s->in[at]);
            at++;
        }
    }
}

u32 yew_lsp_snippet_strip(const u8 *in, u32 n, Bytebuf *out)
{
    SnippetStrip strip;
    size_t emitted;

    if (out == NULL || (n != 0U && in == NULL))
        return 0U;
    strip = (SnippetStrip){in, out, out->len, 0U, false};
    snippet_range(&strip, 0U, n, 0U);
    emitted = out->len - strip.base;
    return strip.have_cursor ? strip.cursor :
           (emitted > UINT32_MAX ? UINT32_MAX : (u32)emitted);
}

enum {
    LSP_COMPLETION_MAX = 10000U,
    LSP_ADDITIONAL_EDIT_MAX = 256U
};

typedef struct LspCompletionEdit {
    Span span;
    u8 *text;
    u32 len;
} LspCompletionEdit;

VEC_DECL(Vec_LspCompletionEdit, LspCompletionEdit);

typedef struct LspCompletionOwned {
    u8 *label;
    u8 *insert;
    u8 *detail;
    u8 *doc;
    u8 *sort;
    u32 sort_len;
    Span replace;
    bool has_replace;
    bool preselect;
    bool resolved;
    u32 cursor;
    Vec_LspCompletionEdit additional;
    Bytebuf raw;
} LspCompletionOwned;

typedef struct LspApplyEdit {
    Span span;
    const u8 *text;
    u32 len;
    bool main;
} LspApplyEdit;

static u8 *completion_copy(const u8 *bytes, u32 len)
{
    u8 *copy;

    if (bytes == NULL && len != 0U)
        return NULL;
    copy = yew_xmalloc(len == 0U ? 1U : (size_t)len);
    if (len != 0U)
        (void)memcpy(copy, bytes, len);
    return copy;
}

static void completion_owned_free(LspCompletionOwned *owned)
{
    size_t i;

    if (owned == NULL)
        return;
    free(owned->label);
    free(owned->insert);
    free(owned->detail);
    free(owned->doc);
    free(owned->sort);
    for (i = 0U; i < owned->additional.len; i++)
        free(owned->additional.data[i].text);
    Vec_LspCompletionEdit_free(&owned->additional);
    bytebuf_free(&owned->raw);
    free(owned);
}

void yew_lsp_completion_discard(ComplItem *item)
{
    if (item == NULL)
        return;
    completion_owned_free(item->user);
    (void)memset(item, 0, sizeof(*item));
}

static bool lsp_position_exact(const JsonValue *value, const TextBuf *tb,
                               u8 pos_enc, ByteOff *out)
{
    const JsonValue *linev;
    const JsonValue *charv;
    i64 line;
    i64 character;
    i64 check_line;
    i64 check_character;
    ByteOff off;

    if (value == NULL || value->kind != YEW_JS_OBJ || tb == NULL)
        return false;
    linev = yew_json_get(value, "line");
    charv = yew_json_get(value, "character");
    if (linev == NULL || charv == NULL || linev->kind != YEW_JS_INT ||
        charv->kind != YEW_JS_INT)
        return false;
    line = linev->i;
    character = charv->i;
    if (line < 0 || character < 0 ||
        (u64)line >= yew_textbuf_line_count(tb))
        return false;
    off = yew_lsp_off_of_pos(pos_enc, tb, LINENO((u64)line),
                             (u64)character);
    yew_lsp_pos_of_off(pos_enc, tb, off, &check_line, &check_character);
    if (check_line != line || check_character != character ||
        !yew_is_grapheme_boundary(tb, off))
        return false;
    *out = off;
    return true;
}

static bool lsp_range_exact(const JsonValue *value, const TextBuf *tb,
                            u8 pos_enc, Span *out)
{
    ByteOff lo;
    ByteOff hi;

    if (value == NULL || value->kind != YEW_JS_OBJ ||
        !lsp_position_exact(yew_json_get(value, "start"), tb, pos_enc,
                            &lo) ||
        !lsp_position_exact(yew_json_get(value, "end"), tb, pos_enc,
                            &hi) || lo.v > hi.v)
        return false;
    *out = (Span){lo.v, hi.v};
    return true;
}

static bool append_hover_piece(Bytebuf *body, const JsonValue *piece)
{
    const u8 *text;
    u32 len = 0U;

    text = yew_json_str(piece, &len);
    if (text == NULL && piece != NULL && piece->kind == YEW_JS_OBJ)
        text = yew_json_str(yew_json_get(piece, "value"), &len);
    if (text == NULL || len == 0U)
        return false;
    if (body->len != 0U)
        bytebuf_push_u8(body, (u8)'\n');
    bytebuf_append(body, text, len);
    return true;
}

bool yew_lsp_hover_parse(const JsonValue *result, const TextBuf *tb,
                         u8 pos_enc, Bytebuf *body, Span *range,
                         bool *has_range)
{
    const JsonValue *contents;
    const JsonValue *server_range;
    bool any = false;
    u32 i;

    if (body == NULL || range == NULL || has_range == NULL ||
        result == NULL || result->kind != YEW_JS_OBJ)
        return false;
    *has_range = false;
    contents = yew_json_get(result, "contents");
    if (contents != NULL && contents->kind == YEW_JS_ARR) {
        for (i = 0U; i < contents->arr.n; i++)
            if (append_hover_piece(body, contents->arr.v[i]))
                any = true;
    } else {
        any = append_hover_piece(body, contents);
    }
    server_range = yew_json_get(result, "range");
    if (server_range != NULL &&
        lsp_range_exact(server_range, tb, pos_enc, range))
        *has_range = range->lo != range->hi;
    return any;
}

static bool raw_u16_byte(const u8 *text, u32 len, u64 want, u32 *out)
{
    u32 at = 0U;
    u64 units = 0U;

    while (at < len) {
        u32 cp;
        size_t n;
        u64 next;

        if (units == want) {
            *out = at;
            return true;
        }
        n = yew_utf8_decode(text + at, len - at, &cp);
        if (n == 0U || n > len - at)
            return false;
        next = units + (cp > 0xFFFFU ? 2U : 1U);
        if (want < next)
            return false;
        units = next;
        at += (u32)n;
    }
    if (units != want)
        return false;
    *out = len;
    return true;
}

static bool raw_find(const u8 *hay, u32 hay_len, const u8 *needle,
                     u32 needle_len, Span *out)
{
    u32 at;

    if (needle_len == 0U || needle_len > hay_len)
        return false;
    for (at = 0U; at <= hay_len - needle_len; at++) {
        if (memcmp(hay + at, needle, needle_len) == 0) {
            *out = (Span){at, at + needle_len};
            return true;
        }
    }
    return false;
}

static bool signature_parameter_span(const JsonValue *parameter,
                                     const u8 *label, u32 label_len,
                                     Span *out)
{
    const JsonValue *value;
    const u8 *text;
    u32 len = 0U;

    if (parameter == NULL || parameter->kind != YEW_JS_OBJ)
        return false;
    value = yew_json_get(parameter, "label");
    text = yew_json_str(value, &len);
    if (text != NULL)
        return raw_find(label, label_len, text, len, out);
    if (value != NULL && value->kind == YEW_JS_ARR && value->arr.n == 2U) {
        const JsonValue *lov = value->arr.v[0];
        const JsonValue *hiv = value->arr.v[1];
        u32 lo;
        u32 hi;

        if (lov == NULL || hiv == NULL || lov->kind != YEW_JS_INT ||
            hiv->kind != YEW_JS_INT || lov->i < 0 || hiv->i < lov->i ||
            !raw_u16_byte(label, label_len, (u64)lov->i, &lo) ||
            !raw_u16_byte(label, label_len, (u64)hiv->i, &hi) || lo == hi)
            return false;
        *out = (Span){lo, hi};
        return true;
    }
    return false;
}

bool yew_lsp_signature_parse(const JsonValue *result, Bytebuf *body,
                             Span *emph, bool *has_emph)
{
    const JsonValue *signatures;
    const JsonValue *signature;
    const JsonValue *parameters;
    const JsonValue *active;
    const u8 *label;
    u32 label_len = 0U;
    i64 sig_index;
    i64 param_index;

    if (body == NULL || emph == NULL || has_emph == NULL ||
        result == NULL || result->kind != YEW_JS_OBJ)
        return false;
    *has_emph = false;
    signatures = yew_json_get(result, "signatures");
    if (signatures == NULL || signatures->kind != YEW_JS_ARR ||
        signatures->arr.n == 0U)
        return false;
    sig_index = yew_json_int(yew_json_get(result, "activeSignature"), 0);
    if (sig_index < 0 || (u64)sig_index >= signatures->arr.n)
        sig_index = 0;
    signature = signatures->arr.v[(u32)sig_index];
    if (signature == NULL || signature->kind != YEW_JS_OBJ)
        return false;
    label = yew_json_str(yew_json_get(signature, "label"), &label_len);
    if (label == NULL || label_len == 0U)
        return false;
    bytebuf_append(body, label, label_len);

    active = yew_json_get(signature, "activeParameter");
    if (active == NULL || active->kind != YEW_JS_INT)
        active = yew_json_get(result, "activeParameter");
    if (active == NULL || active->kind != YEW_JS_INT || active->i < 0)
        return true;
    param_index = active->i;
    parameters = yew_json_get(signature, "parameters");
    if (parameters == NULL || parameters->kind != YEW_JS_ARR ||
        (u64)param_index >= parameters->arr.n)
        return true;
    *has_emph = signature_parameter_span(
        parameters->arr.v[(u32)param_index], label, label_len, emph);
    return true;
}

static const u8 *completion_doc(const JsonValue *value, u32 *len)
{
    const u8 *text;

    text = yew_json_str(value, len);
    if (text != NULL)
        return text;
    if (value != NULL && value->kind == YEW_JS_OBJ)
        return yew_json_str(yew_json_get(value, "value"), len);
    *len = 0U;
    return NULL;
}

static u8 completion_kind(i64 kind)
{
    switch (kind) {
    case 2:
    case 3: return (u8)YEW_COMPLK_FUNC;
    case 6: return (u8)YEW_COMPLK_VARIABLE;
    case 7:
    case 8:
    case 22:
    case 25: return (u8)YEW_COMPLK_TYPE;
    case 20:
    case 21: return (u8)YEW_COMPLK_CONSTANT;
    case 5:
    case 10: return (u8)YEW_COMPLK_FIELD;
    case 13: return (u8)YEW_COMPLK_ENUM;
    case 9:
    case 18:
    case 19: return (u8)YEW_COMPLK_MODULE;
    case 14: return (u8)YEW_COMPLK_KEYWORD;
    case 15: return (u8)YEW_COMPLK_SNIPPET;
    default: return (u8)YEW_COMPLK_WORD;
    }
}

static bool completion_text_edit(LspCompletionOwned *owned,
                                 const JsonValue *edit, const TextBuf *tb,
                                 u8 pos_enc, const u8 **text, u32 *len)
{
    const JsonValue *range;

    if (edit == NULL)
        return true;
    if (edit->kind != YEW_JS_OBJ)
        return false;
    *text = yew_json_str(yew_json_get(edit, "newText"), len);
    if (*text == NULL)
        return false;
    range = yew_json_get(edit, "range");
    if (range == NULL)
        range = yew_json_get(edit, "replace");
    if (!lsp_range_exact(range, tb, pos_enc, &owned->replace))
        return false;
    owned->has_replace = true;
    return true;
}

static bool completion_additional(LspCompletionOwned *owned,
                                  const JsonValue *edits,
                                  const TextBuf *tb, u8 pos_enc)
{
    u32 i;

    if (edits == NULL)
        return true;
    if (edits->kind != YEW_JS_ARR ||
        edits->arr.n > LSP_ADDITIONAL_EDIT_MAX)
        return false;
    for (i = 0U; i < edits->arr.n; i++) {
        const JsonValue *edit = edits->arr.v[i];
        const u8 *text;
        u32 len;
        LspCompletionEdit copy;

        if (edit == NULL || edit->kind != YEW_JS_OBJ ||
            !lsp_range_exact(yew_json_get(edit, "range"), tb, pos_enc,
                             &copy.span))
            return false;
        text = yew_json_str(yew_json_get(edit, "newText"), &len);
        if (text == NULL)
            return false;
        copy.text = completion_copy(text, len);
        copy.len = len;
        Vec_LspCompletionEdit_push(&owned->additional, copy);
    }
    return true;
}

static bool completion_parse_item(const JsonValue *value, const TextBuf *tb,
                                  u8 pos_enc, const u8 *stem, u32 stem_len,
                                  ComplItem *out)
{
    static const u8 empty_stem[] = "";
    const JsonValue *text_edit;
    const u8 *label;
    const u8 *insert;
    const u8 *detail;
    const u8 *doc;
    const u8 *sort;
    const u8 *filter;
    u32 label_len;
    u32 insert_len = 0U;
    u32 detail_len = 0U;
    u32 doc_len = 0U;
    u32 sort_len = 0U;
    u32 filter_len = 0U;
    i64 format;
    i32 score;
    LspCompletionOwned *owned;
    Bytebuf stripped;
    JsonW writer;

    if (value == NULL || value->kind != YEW_JS_OBJ)
        return false;
    label = yew_json_str(yew_json_get(value, "label"), &label_len);
    if (label == NULL)
        return false;
    filter = yew_json_str(yew_json_get(value, "filterText"), &filter_len);
    if (filter == NULL) {
        filter = label;
        filter_len = label_len;
    }
    if (stem == NULL)
        stem = empty_stem;
    score = yew_fz_score((const char *)stem, stem_len,
                         (const char *)filter, filter_len, NULL);
    if (score == YEW_FZ_NO_MATCH)
        return false;
    owned = yew_xcalloc(1U, sizeof(*owned));
    bytebuf_init(&owned->raw);
    text_edit = yew_json_get(value, "textEdit");
    insert = NULL;
    if (!completion_text_edit(owned, text_edit, tb, pos_enc, &insert,
                              &insert_len))
        goto invalid;
    if (insert == NULL)
        insert = yew_json_str(yew_json_get(value, "insertText"),
                              &insert_len);
    if (insert == NULL) {
        insert = label;
        insert_len = label_len;
    }
    format = yew_json_int(yew_json_get(value, "insertTextFormat"), 1);
    if (format != 1 && format != 2)
        goto invalid;
    owned->label = completion_copy(label, label_len);
    if (format == 2) {
        bytebuf_init(&stripped);
        owned->cursor = yew_lsp_snippet_strip(insert, insert_len, &stripped);
        if (stripped.len > UINT32_MAX) {
            bytebuf_free(&stripped);
            goto invalid;
        }
        owned->insert = completion_copy(stripped.data, (u32)stripped.len);
        insert_len = (u32)stripped.len;
        bytebuf_free(&stripped);
    } else {
        owned->insert = completion_copy(insert, insert_len);
        owned->cursor = insert_len;
    }
    detail = yew_json_str(yew_json_get(value, "detail"), &detail_len);
    doc = completion_doc(yew_json_get(value, "documentation"), &doc_len);
    owned->detail = detail == NULL ? NULL : completion_copy(detail, detail_len);
    owned->doc = doc == NULL ? NULL : completion_copy(doc, doc_len);
    sort = yew_json_str(yew_json_get(value, "sortText"), &sort_len);
    if (sort == NULL) {
        sort = filter;
        sort_len = filter_len;
    }
    owned->sort = completion_copy(sort, sort_len);
    owned->sort_len = sort_len;
    owned->preselect = yew_json_bool(yew_json_get(value, "preselect"),
                                     false);
    if (!completion_additional(owned,
            yew_json_get(value, "additionalTextEdits"), tb, pos_enc))
        goto invalid;
    yew_jsonw_init(&writer, &owned->raw);
    yew_jsonw_value(&writer, value);
    out->label = owned->label;
    out->label_len = label_len;
    out->insert = owned->insert;
    out->insert_len = insert_len;
    out->detail = owned->detail;
    out->detail_len = detail_len;
    out->doc = owned->doc;
    out->doc_len = doc_len;
    out->kind = completion_kind(yew_json_int(yew_json_get(value, "kind"),
                                             0));
    out->score = score;
    (void)yew_fz_score((const char *)stem, stem_len, (const char *)label,
                       label_len, &out->m);
    out->user = owned;
    return true;

invalid:
    completion_owned_free(owned);
    return false;
}

static int completion_cmp(const void *a, const void *b, void *ctx)
{
    const ComplItem *left = a;
    const ComplItem *right = b;
    const LspCompletionOwned *lo = left->user;
    const LspCompletionOwned *ro = right->user;
    u32 n;
    int cmp;

    (void)ctx;
    n = lo->sort_len < ro->sort_len ? lo->sort_len : ro->sort_len;
    cmp = n == 0U ? 0 : memcmp(lo->sort, ro->sort, n);
    if (cmp != 0)
        return cmp;
    if (lo->sort_len != ro->sort_len)
        return lo->sort_len < ro->sort_len ? -1 : 1;
    if (left->score != right->score)
        return left->score > right->score ? -1 : 1;
    return 0;
}

u32 yew_lsp_completion_parse(const JsonValue *result, const TextBuf *tb,
                             u8 pos_enc, const u8 *stem, u32 stem_len,
                             Vec_ComplItem *out, i32 *preselect)
{
    const JsonValue *items = result;
    u32 n;
    u32 i;
    size_t base;

    if (preselect != NULL)
        *preselect = -1;
    if (result == NULL || tb == NULL || out == NULL ||
        (stem == NULL && stem_len != 0U))
        return 0U;
    if (result->kind == YEW_JS_OBJ)
        items = yew_json_get(result, "items");
    if (items == NULL || items->kind != YEW_JS_ARR)
        return 0U;
    n = items->arr.n < LSP_COMPLETION_MAX ? items->arr.n :
                                           LSP_COMPLETION_MAX;
    base = out->len;
    for (i = 0U; i < n; i++) {
        ComplItem item = {0};

        if (completion_parse_item(items->arr.v[i], tb, pos_enc, stem,
                                  stem_len, &item))
            Vec_ComplItem_push(out, item);
    }
    if (out->len - base > 1U)
        yew_sort_stable(out->data + base, out->len - base,
                        sizeof(out->data[0]), completion_cmp, NULL);
    if (preselect != NULL) {
        size_t at;

        for (at = base; at < out->len; at++) {
            const LspCompletionOwned *owned = out->data[at].user;

            if (owned->preselect) {
                *preselect = (i32)(at - base);
                break;
            }
        }
    }
    return (u32)(out->len - base);
}

bool yew_lsp_completion_resolve_apply(ComplItem *item,
                                      const JsonValue *result)
{
    LspCompletionOwned *owned;
    const u8 *detail;
    const u8 *doc;
    u8 *detail_copy = NULL;
    u8 *doc_copy = NULL;
    u32 detail_len = 0U;
    u32 doc_len = 0U;

    if (item == NULL || item->user == NULL || result == NULL ||
        result->kind != YEW_JS_OBJ)
        return false;
    owned = item->user;
    detail = yew_json_str(yew_json_get(result, "detail"), &detail_len);
    doc = completion_doc(yew_json_get(result, "documentation"), &doc_len);
    if (detail != NULL)
        detail_copy = completion_copy(detail, detail_len);
    if (doc != NULL)
        doc_copy = completion_copy(doc, doc_len);
    if (detail != NULL) {
        free(owned->detail);
        owned->detail = detail_copy;
        item->detail = owned->detail;
        item->detail_len = detail_len;
    }
    if (doc != NULL) {
        free(owned->doc);
        owned->doc = doc_copy;
        item->doc = owned->doc;
        item->doc_len = doc_len;
    }
    owned->resolved = true;
    return true;
}

static int apply_edit_cmp(const void *a, const void *b, void *ctx)
{
    const LspApplyEdit *left = a;
    const LspApplyEdit *right = b;

    (void)ctx;
    if (left->span.lo != right->span.lo)
        return left->span.lo < right->span.lo ? -1 : 1;
    if (left->span.hi != right->span.hi)
        return left->span.hi < right->span.hi ? -1 : 1;
    return 0;
}

static bool completion_final_start(const LspApplyEdit *edits, size_t n,
                                   Span main, u64 *out)
{
    u64 at = main.lo;
    size_t i;

    for (i = 0U; i < n; i++) {
        u64 removed;

        if (edits[i].main || edits[i].span.lo >= main.lo)
            continue;
        removed = edits[i].span.hi - edits[i].span.lo;
        if ((u64)edits[i].len >= removed) {
            u64 add = (u64)edits[i].len - removed;

            if (at > UINT64_MAX - add)
                return false;
            at += add;
        } else {
            u64 sub = removed - (u64)edits[i].len;

            if (at < sub)
                return false;
            at -= sub;
        }
    }
    *out = at;
    return true;
}

bool yew_lsp_completion_accept(Ed *ed, Win *w, Span replace,
                               const ComplItem *item)
{
    const LspCompletionOwned *owned;
    LspApplyEdit edits[LSP_ADDITIONAL_EDIT_MAX + 1U];
    Span main;
    size_t n;
    size_t i;
    u64 final_start;
    u64 intended;
    EditCtx edit;
    bool own_txn;
    bool ok = true;
    Cursor *cursor;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        item == NULL || item->user == NULL || item->insert == NULL ||
        w->cs.curs.len != 1U || w->cs.primary >= w->cs.curs.len)
        return false;
    owned = item->user;
    main = owned->has_replace ? owned->replace : replace;
    edits[0] = (LspApplyEdit){main, item->insert, item->insert_len, true};
    n = 1U;
    for (i = 0U; i < owned->additional.len; i++) {
        const LspCompletionEdit *extra = &owned->additional.data[i];

        edits[n++] = (LspApplyEdit){extra->span, extra->text, extra->len,
                                    false};
    }
    for (i = 0U; i < n; i++)
        if (edits[i].span.lo > edits[i].span.hi ||
            edits[i].span.hi > yew_textbuf_len(w->buf->tb) ||
            !yew_is_grapheme_boundary(w->buf->tb,
                                      BYTEOFF(edits[i].span.lo)) ||
            !yew_is_grapheme_boundary(w->buf->tb,
                                      BYTEOFF(edits[i].span.hi)))
            return false;
    yew_sort_stable(edits, n, sizeof(edits[0]), apply_edit_cmp, NULL);
    for (i = 1U; i < n; i++)
        if (edits[i - 1U].span.hi > edits[i].span.lo)
            return false;
    if (!completion_final_start(edits, n, main, &final_start) ||
        final_start > UINT64_MAX - owned->cursor)
        return false;
    edit = yew_ed_edit_ctx_for(ed, w);
    own_txn = edit.undo != NULL && edit.undo->depth == 0U;
    if (own_txn)
        yew_undo_begin(&edit, YEW_TXN_PASTE);
    for (i = n; i-- > 0U;) {
        if (edits[i].span.lo != edits[i].span.hi)
            ok = yew_edit_delete(&edit, edits[i].span);
        if (ok && edits[i].len != 0U)
            ok = yew_edit_insert(&edit, BYTEOFF(edits[i].span.lo),
                                 edits[i].text, edits[i].len);
        if (!ok)
            break;
    }
    if (own_txn) {
        if (ok)
            yew_undo_end(&edit);
        else
            yew_undo_abort(&edit);
    }
    if (!ok) {
        if (own_txn)
            yew_ed_finish_edit(ed, &edit);
        return false;
    }
    intended = final_start + owned->cursor;
    if (intended > yew_textbuf_len(w->buf->tb))
        intended = yew_textbuf_len(w->buf->tb);
    if (!yew_is_grapheme_boundary(w->buf->tb, BYTEOFF(intended)))
        intended = yew_grapheme_next(w->buf->tb, BYTEOFF(intended)).v;
    cursor = &w->cs.curs.data[w->cs.primary];
    cursor->pos = BYTEOFF(intended);
    cursor->anchor = cursor->pos;
    if (own_txn)
        yew_ed_finish_edit(ed, &edit);
    return true;
}

typedef struct LspCompletionRequest {
    u32 win_id;
    u32 server_id;
    u64 seq;
    u64 request;
    u8 *stem;
    u32 stem_len;
} LspCompletionRequest;

typedef struct LspCompletionResolve {
    u32 win_id;
    u32 server_id;
    u64 seq;
    u64 request;
    i32 selection;
    void *owned;
} LspCompletionResolve;

typedef struct LspShadowRequest {
    ShadowReq shadow;
    u32 server_id;
    u64 request;
    u8 *stem;
    u32 stem_len;
} LspShadowRequest;

typedef enum LspPanelKind {
    LSP_PANEL_HOVER = 0,
    LSP_PANEL_SIGNATURE
} LspPanelKind;

typedef struct LspPanelRequest {
    u32 win_id;
    u32 server_id;
    u64 seq;
    u64 request;
    u8 kind;
} LspPanelRequest;

static void completion_request_free(void *ctx)
{
    LspCompletionRequest *request = ctx;

    if (request == NULL)
        return;
    free(request->stem);
    free(request);
}

static void completion_resolve_free(void *ctx)
{
    free(ctx);
}

static void completion_shadow_free(void *ctx)
{
    LspShadowRequest *request = ctx;

    if (request == NULL)
        return;
    free(request->stem);
    free(request);
}

static void panel_request_free(void *ctx)
{
    free(ctx);
}

static void completion_cancel_id(Ed *ed, u32 server_id, u64 request)
{
    LspServer *server;

    if (request == 0U)
        return;
    server = yew_lsp_server_by_id(ed, server_id);
    if (server == NULL)
        return;
    yew_rpc_cancel(&server->rpc, request);
    (void)yew_rpc_drop(&server->rpc, request);
}

static void completion_cancel_menu(Ed *ed, Win *w)
{
    if (w == NULL)
        return;
    completion_cancel_id(ed, w->compl.source_server,
                         w->compl.source_request);
    completion_cancel_id(ed, w->compl.source_server,
                         w->compl.source_resolve);
    w->compl.source_request = 0U;
    w->compl.source_resolve = 0U;
}

static void completion_done(Ed *ed, void *ctx, const JsonValue *result,
                            const JsonValue *error)
{
    LspCompletionRequest *request = ctx;
    Win *w;
    LspServer *server;
    Vec_ComplItem rows = {0};
    i32 preselect = -1;

    if (ed == NULL || request == NULL)
        return;
    w = yew_ed_win_by_id(ed, request->win_id);
    if (w == NULL || !w->compl.open ||
        w->compl.src != &yew_compl_src_lsp ||
        w->compl.source_server != request->server_id ||
        w->compl.source_seq != request->seq ||
        w->compl.source_request != request->request)
        return;
    w->compl.source_request = 0U;
    server = yew_lsp_server_by_id(ed, request->server_id);
    if (error != NULL) {
        if (server != NULL &&
            yew_json_int(yew_json_get(error, "code"), 0) == -32601)
            server->caps.bits &= ~YEW_LSPC_COMPLETION;
        yew_compl_close_result(ed, w, false);
        return;
    }
    if (result == NULL || w->buf == NULL ||
        w->buf->tb == NULL) {
        yew_compl_close_result(ed, w, false);
        return;
    }
    if (server == NULL) {
        yew_compl_close_result(ed, w, false);
        return;
    }
    (void)yew_lsp_completion_parse(result, w->buf->tb,
                                   server->pos_enc,
                                   request->stem, request->stem_len,
                                   &rows, &preselect);
    if (rows.len == 0U) {
        Vec_ComplItem_free(&rows);
        yew_compl_close_result(ed, w, false);
        return;
    }
    yew_compl_push(ed, w, rows.data, (u32)rows.len);
    rows.len = 0U;
    Vec_ComplItem_free(&rows);
    if (preselect >= 0)
        yew_compl_select(ed, w, preselect);
}

static void completion_resolve_done(Ed *ed, void *ctx,
                                    const JsonValue *result,
                                    const JsonValue *error)
{
    LspCompletionResolve *request = ctx;
    LspServer *server;
    ComplItem *item;
    Win *w;

    if (ed == NULL || request == NULL)
        return;
    w = yew_ed_win_by_id(ed, request->win_id);
    if (w == NULL || !w->compl.open ||
        w->compl.src != &yew_compl_src_lsp ||
        w->compl.source_server != request->server_id ||
        w->compl.source_seq != request->seq ||
        w->compl.source_resolve != request->request)
        return;
    w->compl.source_resolve = 0U;
    server = yew_lsp_server_by_id(ed, request->server_id);
    if (server == NULL)
        return;
    if (error != NULL) {
        if (yew_json_int(yew_json_get(error, "code"), 0) == -32601)
            server->caps.resolve_completion = false;
        return;
    }
    if (!w->compl.panel_open || w->compl.sel != request->selection ||
        request->selection < 0 ||
        (size_t)request->selection >= w->compl.items.len)
        return;
    item = &w->compl.items.data[request->selection];
    if (item->user != request->owned ||
        !yew_lsp_completion_resolve_apply(item, result))
        return;
    yew_compl_resize(ed, w);
    ed->full_damage = true;
}

static void position_params(Bytebuf *out, const LspDoc *doc,
                            u8 pos_enc, const TextBuf *tb, ByteOff cursor,
                            bool completion)
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
    if (completion) {
        yew_jsonw_key(&writer, "context");
        yew_jsonw_obj(&writer);
        yew_jsonw_key(&writer, "triggerKind");
        yew_jsonw_int(&writer, 1);
        yew_jsonw_obj_end(&writer);
    }
    yew_jsonw_obj_end(&writer);
}

static bool panel_cursor_anchor(Win *w, u16 *x, u16 *y)
{
    const Cursor *cursor;
    TextBuf *tb;
    LineNo line;
    Span displayed;
    CCol col;
    u32 sub;
    u16 row;

    if (w == NULL || x == NULL || y == NULL || w->buf == NULL ||
        w->buf->tb == NULL || w->cs.curs.len == 0U ||
        w->cs.primary >= w->cs.curs.len)
        return false;
    cursor = &w->cs.curs.data[w->cs.primary];
    tb = w->buf->tb;
    line = yew_textbuf_line_of(tb, cursor->pos);
    sub = w->vp.wrap ? yew_vp_cursor_subrow(w) : 0U;
    if (!yew_vp_row_of_line(w, line, sub, &row) || row >= w->rect.h)
        return false;
    displayed = w->vp.wrap ? yew_wrap_row(w, line, sub) :
                              yew_textbuf_line_span(tb, line);
    col = yew_off_to_ccol(tb, displayed, cursor->pos,
                          w->buf->tabwidth == 0U ? YEW_VP_TABWIDTH :
                                                  w->buf->tabwidth);
    *x = yew_vp_gridx_of_ccol(w, col);
    *y = (u16)(w->rect.y + row);
    return *x >= w->rect.x &&
           (u32)*x < (u32)w->rect.x + w->rect.w;
}

static void panel_request_done(Ed *ed, void *ctx,
                               const JsonValue *result,
                               const JsonValue *error)
{
    LspPanelRequest *request = ctx;
    LspServer *server;
    Win *w;
    Bytebuf body;
    Span span = {0};
    bool has_span = false;
    Vec_Span emph = {0};
    PanelSpec spec = {0};
    u16 x;
    u16 y;
    bool parsed;
    u32 cap;

    if (ed == NULL || request == NULL)
        return;
    w = yew_ed_win_by_id(ed, request->win_id);
    if (w == NULL || ed->win != w ||
        w->panel_source_server != request->server_id ||
        w->panel_source_seq != request->seq ||
        w->panel_source_request != request->request)
        return;
    w->panel_source_request = 0U;
    w->panel_source_server = 0U;
    server = yew_lsp_server_by_id(ed, request->server_id);
    cap = request->kind == LSP_PANEL_HOVER ? YEW_LSPC_HOVER :
                                             YEW_LSPC_SIGNATURE;
    if (error != NULL) {
        if (server != NULL &&
            yew_json_int(yew_json_get(error, "code"), 0) == -32601)
            server->caps.bits &= ~cap;
        return;
    }
    if (server == NULL || w->buf == NULL || w->buf->tb == NULL ||
        !panel_cursor_anchor(w, &x, &y))
        return;
    bytebuf_init(&body);
    if (request->kind == LSP_PANEL_HOVER) {
        parsed = yew_lsp_hover_parse(result, w->buf->tb, server->pos_enc,
                                     &body, &span, &has_span);
        if (!parsed) {
            yew_msg(ed, YEW_MSG_INFO, "no hover information here");
            bytebuf_free(&body);
            return;
        }
        spec.title = "hover";
        spec.place = (u8)YEW_PANEL_BELOW;
    } else {
        parsed = yew_lsp_signature_parse(result, &body, &span, &has_span);
        if (!parsed) {
            bytebuf_free(&body);
            return;
        }
        if (has_span)
            Vec_Span_push(&emph, span);
        spec.title = "signature";
        spec.place = (u8)YEW_PANEL_ABOVE;
        spec.emph = &emph;
    }
    if (body.len > UINT32_MAX) {
        Vec_Span_free(&emph);
        bytebuf_free(&body);
        return;
    }
    spec.body = body.data;
    spec.len = (u32)body.len;
    spec.x = x;
    spec.y = y;
    spec.role = "bg";
    if (yew_panel_open(ed, &w->panel, &spec) &&
        request->kind == LSP_PANEL_HOVER && has_span)
        (void)yew_panel_mark(&w->panel, w->buf->id, w->buf->tb->gen,
                             span, "lsp.hover_range");
    Vec_Span_free(&emph);
    bytebuf_free(&body);
}

static bool panel_request_start(Ed *ed, Win *w, u8 kind)
{
    LspDoc *doc;
    LspServer *server;
    LspPanelRequest *request;
    RpcPending pending = {0};
    Bytebuf params;
    const Cursor *cursor;
    const char *method;
    u32 cap;
    u64 id;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->cs.curs.len != 1U || w->cs.primary >= w->cs.curs.len)
        return false;
    doc = yew_lsp_doc_for_buffer(ed, w->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    cap = kind == LSP_PANEL_HOVER ? YEW_LSPC_HOVER : YEW_LSPC_SIGNATURE;
    if (doc == NULL || server == NULL || server->state != YEW_LSP_READY ||
        !yew_lsp_has(server, cap))
        return false;
    completion_cancel_id(ed, w->panel_source_server,
                         w->panel_source_request);
    w->panel_source_request = 0U;
    w->panel_source_server = 0U;
    yew_panel_close(ed, &w->panel);
    w->panel_source_seq++;
    if (w->panel_source_seq == 0U)
        w->panel_source_seq++;
    request = yew_xcalloc(1U, sizeof(*request));
    request->win_id = w->id;
    request->server_id = server->id;
    request->seq = w->panel_source_seq;
    request->kind = kind;
    pending.buf_id = w->buf->id;
    pending.gen = w->buf->tb->gen;
    pending.sent_ms = ed->now_ms;
    pending.cb = panel_request_done;
    pending.release = panel_request_free;
    pending.ctx = request;
    cursor = &w->cs.curs.data[w->cs.primary];
    method = kind == LSP_PANEL_HOVER ? "textDocument/hover" :
                                       "textDocument/signatureHelp";
    yew_lsp_sync_flush(ed);
    bytebuf_init(&params);
    position_params(&params, doc, server->pos_enc, w->buf->tb,
                    cursor->pos, false);
    id = params.len > UINT32_MAX ? 0U :
         yew_rpc_call(&server->rpc, method, params.data, (u32)params.len,
                      &pending);
    bytebuf_free(&params);
    if (id == 0U) {
        panel_request_free(request);
        return false;
    }
    request->request = id;
    w->panel_source_request = id;
    w->panel_source_server = server->id;
    return true;
}

bool yew_lsp_hover_request(Ed *ed, Win *w)
{
    return panel_request_start(ed, w, (u8)LSP_PANEL_HOVER);
}

bool yew_lsp_signature_request(Ed *ed, Win *w)
{
    return panel_request_start(ed, w, (u8)LSP_PANEL_SIGNATURE);
}

static void completion_rows_discard(Vec_ComplItem *rows)
{
    size_t i;

    if (rows == NULL)
        return;
    for (i = 0U; i < rows->len; i++)
        yew_lsp_completion_discard(&rows->data[i]);
    Vec_ComplItem_free(rows);
}

static bool completion_shadow_stem(Ed *ed, const ShadowReq *request,
                                   u8 **stem, u32 *stem_len)
{
    UnitCtx unit;
    TextIter iter;
    ByteOff home;
    TextBuf *tb;
    u64 copied = 0U;
    u64 len;

    if (ed == NULL || request == NULL || stem == NULL || stem_len == NULL ||
        ed->win == NULL || ed->win->buf == NULL ||
        ed->win->buf->id != request->buf_id || ed->win->buf->tb == NULL ||
        ed->win->buf->tb->gen != request->buf_gen ||
        request->pos.v > yew_textbuf_len(ed->win->buf->tb) ||
        request->pos.v < request->line.lo ||
        request->pos.v > request->line.hi)
        return false;
    tb = ed->win->buf->tb;
    unit = (UnitCtx){tb, ed->win->buf, ed->win};
    home = yew_unit_word.home(&unit, request->pos, false);
    if (home.v > request->pos.v)
        return false;
    len = request->pos.v - home.v;
    if (len > UINT32_MAX)
        return false;
    *stem = yew_xmalloc(len == 0U ? 1U : (size_t)len);
    *stem_len = (u32)len;
    if (len == 0U)
        return true;
    if (!yew_textiter_begin(&iter, tb, home))
        goto invalid;
    while (copied < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &available) ||
            available == 0U)
            goto invalid;
        take = available < len - copied ? available : len - copied;
        (void)memcpy(*stem + (size_t)copied, bytes, (size_t)take);
        copied += take;
        if (copied < len && !yew_textiter_advance(&iter, tb))
            goto invalid;
    }
    return true;

invalid:
    free(*stem);
    *stem = NULL;
    *stem_len = 0U;
    return false;
}

static const ComplItem *completion_shadow_item(const Vec_ComplItem *rows,
                                               const LspShadowRequest *request)
{
    size_t i;

    for (i = 0U; rows != NULL && i < rows->len; i++) {
        const ComplItem *item = &rows->data[i];
        const LspCompletionOwned *owned = item->user;
        u64 replace_lo;

        if (request->shadow.pos.v < request->stem_len)
            continue;
        replace_lo = request->shadow.pos.v - request->stem_len;
        if (owned == NULL || owned->additional.len != 0U ||
            owned->cursor != item->insert_len ||
            item->insert_len <= request->stem_len ||
            memcmp(item->insert, request->stem, request->stem_len) != 0)
            continue;
        if (owned->has_replace &&
            (owned->replace.lo != replace_lo ||
             owned->replace.hi != request->shadow.pos.v))
            continue;
        return item;
    }
    return NULL;
}

static void lsp_shadow_done(Ed *ed, void *ctx, const JsonValue *result,
                            const JsonValue *error)
{
    LspShadowRequest *request = ctx;
    LspServer *server;
    Buffer *buffer;
    Vec_ComplItem rows = {0};
    const ComplItem *item;
    ShadowSug suggestion = {0};

    if (ed == NULL || request == NULL)
        return;
    server = yew_lsp_server_by_id(ed, request->server_id);
    if (server == NULL || server->shadow_request != request->request ||
        server->shadow_buf_id != request->shadow.buf_id ||
        server->shadow_seq != request->shadow.seq)
        return;
    server->shadow_request = 0U;
    server->shadow_buf_id = 0U;
    server->shadow_seq = 0U;
    if (error != NULL) {
        if (yew_json_int(yew_json_get(error, "code"), 0) == -32601)
            server->caps.bits &= ~YEW_LSPC_COMPLETION;
        return;
    }
    buffer = yew_ws_buf_by_id(ed, request->shadow.buf_id);
    if (buffer == NULL || buffer->tb == NULL || result == NULL)
        return;
    (void)yew_lsp_completion_parse(result, buffer->tb, server->pos_enc,
                                   request->stem, request->stem_len,
                                   &rows, NULL);
    item = completion_shadow_item(&rows, request);
    if (item != NULL) {
        suggestion.seq = request->shadow.seq;
        suggestion.prov = (u8)YEW_SHADOW_LSP;
        suggestion.buf_id = request->shadow.buf_id;
        suggestion.buf_gen = request->shadow.buf_gen;
        suggestion.pos = request->shadow.pos;
        suggestion.text = item->insert + request->stem_len;
        suggestion.len = item->insert_len - request->stem_len;
        yew_shadow_deliver(ed, &suggestion);
    }
    completion_rows_discard(&rows);
}

static void lsp_shadow_cancel(Ed *ed, u32 buf_id, u32 up_to)
{
    Buffer *buffer;
    LspDoc *doc;
    LspServer *server;

    if (ed == NULL || buf_id == 0U)
        return;
    buffer = yew_ws_buf_by_id(ed, buf_id);
    doc = yew_lsp_doc_for_buffer(ed, buffer);
    server = yew_lsp_server_for_doc(ed, doc);
    if (server == NULL || server->shadow_buf_id != buf_id ||
        server->shadow_seq >= up_to)
        return;
    completion_cancel_id(ed, server->id, server->shadow_request);
    server->shadow_request = 0U;
    server->shadow_buf_id = 0U;
    server->shadow_seq = 0U;
}

static bool lsp_shadow_request(Ed *ed, const ShadowReq *shadow)
{
    Buffer *buffer;
    LspDoc *doc;
    LspServer *server;
    LspShadowRequest *request;
    RpcPending pending = {0};
    Bytebuf params;
    u64 id;

    if (ed == NULL || shadow == NULL ||
        shadow->prov != (u8)YEW_SHADOW_LSP)
        return false;
    buffer = yew_ws_buf_by_id(ed, shadow->buf_id);
    doc = yew_lsp_doc_for_buffer(ed, buffer);
    server = yew_lsp_server_for_doc(ed, doc);
    if (buffer == NULL || buffer->tb == NULL || doc == NULL ||
        server == NULL || server->state != YEW_LSP_READY ||
        !yew_lsp_has(server, YEW_LSPC_COMPLETION))
        return false;
    request = yew_xcalloc(1U, sizeof(*request));
    request->shadow = *shadow;
    request->server_id = server->id;
    if (!completion_shadow_stem(ed, shadow, &request->stem,
                                &request->stem_len)) {
        completion_shadow_free(request);
        return false;
    }
    completion_cancel_id(ed, server->id, server->shadow_request);
    server->shadow_request = 0U;
    yew_lsp_sync_flush(ed);
    pending.buf_id = shadow->buf_id;
    pending.gen = shadow->buf_gen;
    pending.sent_ms = ed->now_ms;
    pending.cb = lsp_shadow_done;
    pending.release = completion_shadow_free;
    pending.ctx = request;
    bytebuf_init(&params);
    position_params(&params, doc, server->pos_enc, buffer->tb,
                    shadow->pos, true);
    id = params.len > UINT32_MAX ? 0U :
         yew_rpc_call(&server->rpc, "textDocument/completion", params.data,
                      (u32)params.len, &pending);
    bytebuf_free(&params);
    if (id == 0U) {
        completion_shadow_free(request);
        return false;
    }
    request->request = id;
    server->shadow_request = id;
    server->shadow_buf_id = shadow->buf_id;
    server->shadow_seq = shadow->seq;
    return true;
}

static const ShadowProvider yew_shadow_lsp = {
    "lsp", YEW_SHADOW_LSP, 120U, lsp_shadow_request, lsp_shadow_cancel,
};

const ShadowProvider *yew_lsp_shadow_provider(void)
{
    return &yew_shadow_lsp;
}

void yew_lsp_shadow_install(void)
{
    static bool installed;
    const char *test_provider = getenv("YEW_SHADOW_TEST");

    if (installed ||
        (test_provider != NULL && strcmp(test_provider, "1") == 0))
        return;
    yew_shadow_register(&yew_shadow_lsp);
    installed = true;
}

static u32 completion_enumerate(Ed *ed, Win *w, const u8 *stem, u32 stem_len,
                                Vec_ComplItem *out, void *ctx)
{
    LspDoc *doc;
    LspServer *server;
    LspCompletionRequest *request;
    RpcPending pending = {0};
    Bytebuf params;
    Cursor *cursor;
    u64 id;

    (void)out;
    (void)ctx;
    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL ||
        w->cs.curs.len != 1U || w->cs.primary >= w->cs.curs.len)
        return 0U;
    completion_cancel_menu(ed, w);
    doc = yew_lsp_doc_for_buffer(ed, w->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (doc == NULL || server == NULL || server->state != YEW_LSP_READY ||
        !yew_lsp_has(server, YEW_LSPC_COMPLETION))
        return 0U;
    yew_lsp_sync_flush(ed);
    cursor = &w->cs.curs.data[w->cs.primary];
    request = yew_xcalloc(1U, sizeof(*request));
    request->win_id = w->id;
    request->server_id = server->id;
    w->compl.source_seq++;
    if (w->compl.source_seq == 0U)
        w->compl.source_seq++;
    request->seq = w->compl.source_seq;
    request->stem_len = stem_len;
    request->stem = completion_copy(stem, stem_len);
    pending.buf_id = w->buf->id;
    pending.gen = w->buf->tb->gen;
    pending.sent_ms = ed->now_ms;
    pending.cb = completion_done;
    pending.release = completion_request_free;
    pending.ctx = request;
    bytebuf_init(&params);
    position_params(&params, doc, server->pos_enc, w->buf->tb,
                    cursor->pos, true);
    id = params.len > UINT32_MAX ? 0U :
         yew_rpc_call(&server->rpc, "textDocument/completion", params.data,
                      (u32)params.len, &pending);
    bytebuf_free(&params);
    if (id == 0U) {
        completion_request_free(request);
        return 0U;
    }
    request->request = id;
    w->compl.source_request = id;
    w->compl.source_server = server->id;
    return 0U;
}

static bool completion_accept_source(Ed *ed, Win *w, Span replace,
                                     const ComplItem *item, void *ctx)
{
    (void)ctx;
    return yew_lsp_completion_accept(ed, w, replace, item);
}

static void completion_resolve_source(Ed *ed, Win *w, ComplItem *item,
                                      void *ctx)
{
    LspCompletionOwned *owned;
    LspCompletionResolve *request;
    LspServer *server;
    RpcPending pending = {0};
    u64 id;

    (void)ctx;
    if (ed == NULL || w == NULL || item == NULL || item->user == NULL ||
        !w->compl.open || !w->compl.panel_open ||
        w->compl.src != &yew_compl_src_lsp || w->compl.sel < 0 ||
        (size_t)w->compl.sel >= w->compl.items.len ||
        &w->compl.items.data[w->compl.sel] != item || w->buf == NULL ||
        w->buf->tb == NULL)
        return;
    completion_cancel_id(ed, w->compl.source_server,
                         w->compl.source_resolve);
    w->compl.source_resolve = 0U;
    owned = item->user;
    server = yew_lsp_server_by_id(ed, w->compl.source_server);
    if (owned->resolved || server == NULL ||
        server->state != YEW_LSP_READY ||
        !server->caps.resolve_completion || owned->raw.len == 0U ||
        owned->raw.len > UINT32_MAX)
        return;
    request = yew_xcalloc(1U, sizeof(*request));
    request->win_id = w->id;
    request->server_id = server->id;
    request->seq = w->compl.source_seq;
    request->selection = w->compl.sel;
    request->owned = owned;
    pending.buf_id = w->buf->id;
    pending.gen = w->buf->tb->gen;
    pending.sent_ms = ed->now_ms;
    pending.cb = completion_resolve_done;
    pending.release = completion_resolve_free;
    pending.ctx = request;
    id = yew_rpc_call(&server->rpc, "completionItem/resolve",
                      owned->raw.data, (u32)owned->raw.len, &pending);
    if (id == 0U) {
        completion_resolve_free(request);
        return;
    }
    request->request = id;
    w->compl.source_resolve = id;
}

static void completion_discard_source(ComplItem *item, void *ctx)
{
    (void)ctx;
    yew_lsp_completion_discard(item);
}

static void completion_close_source(Ed *ed, Win *w, void *ctx)
{
    (void)ctx;
    completion_cancel_menu(ed, w);
    if (w != NULL) {
        w->compl.source_seq++;
        if (w->compl.source_seq == 0U)
            w->compl.source_seq++;
    }
}

const ComplSource yew_compl_src_lsp = {
    .name = "lsp",
    .flags = YEW_COMPL_SRC_ASYNC | YEW_COMPL_SRC_EMPTY_STEM,
    .enumerate = completion_enumerate,
    .resolve = completion_resolve_source,
    .accept = completion_accept_source,
    .discard = completion_discard_source,
    .close = completion_close_source,
};
