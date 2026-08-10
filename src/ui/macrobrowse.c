#include "ui/macrobrowse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "edit/ed.h"
#include "edit/flapi_cmds.h"
#include "fl/flruntime.h"
#include "fl/macrolib.h"
#include "fl/record.h"
#include "term/grid.h"
#include "text/register.h"
#include "text/file.h"
#include "text/undo.h"
#include "ui/cmdline.h"
#include "ui/message.h"
#include "ui/picker.h"
#include "ui/tabs.h"
#include "ui/viewport.h"
#include "unicode/width.h"
#include "util/buf.h"
#include "util/xdg.h"

enum {
    MACRO_ROWS_MAX = 256,
    /* Picker labels are both display and fuzzy-match storage.  Search spans
     * the macro name/metadata plus as much normalized source as fits here;
     * preview still reads the complete source without copying it. */
    MACRO_MATCH_MAX = 1024
};

typedef struct MacroBrowse {
    PickItem rows[MACRO_ROWS_MAX];
    char labels[MACRO_ROWS_MAX][MACRO_MATCH_MAX];
    u32 nrows;
    i32 confirm_delete;
} MacroBrowse;

static MacroBrowse mb;

static bool named_reg(u8 reg)
{
    return reg >= (u8)'a' && reg <= (u8)'z';
}

static void text_bytes(const TextBuf *tb, Bytebuf *out)
{
    TextIter it;
    u64 left;

    bytebuf_init(out);
    if (tb == NULL)
        return;
    left = sag_textbuf_len(tb);
    if (left == 0U || !sag_textiter_begin(&it, tb, BYTEOFF(0U)))
        return;
    while (left != 0U) {
        const u8 *p;
        u64 n;
        size_t take;

        if (!sag_textiter_chunk(&it, tb, &p, &n))
            break;
        if (n > left)
            n = left;
        take = n > SIZE_MAX ? SIZE_MAX : (size_t)n;
        bytebuf_append(out, p, take);
        left -= n;
        if (left != 0U && !sag_textiter_advance(&it, tb))
            break;
    }
}

static u32 source_lines(const u8 *source, size_t len)
{
    u32 lines = 1U;
    size_t i;

    for (i = 0U; i < len; i++)
        if (source[i] == (u8)'\n' && i + 1U < len)
            lines++;
    return lines;
}

CmdStatus sag_macro_edit(Ed *ed, u8 reg)
{
    const RegVal *value;
    Buffer *scratch;
    char name[32];

    if (ed == NULL || !named_reg(reg))
        return SAG_CMD_ERR_ARG;
    value = sag_reg_get(&ed->regs, reg);
    if (value == NULL || value->bytes.len == 0U) {
        sag_msg(ed, SAG_MSG_ERROR, "macro @%c is empty", (int)reg);
        return SAG_CMD_ERR_ARG;
    }
    (void)snprintf(name, sizeof(name), "*macro %c*", (int)reg);
    scratch = sag_ws_scratch_new(ed, name, 0U);
    if (scratch == NULL)
        return SAG_CMD_ERR_IO;
    if (value->bytes.len != 0U)
        sag_textbuf_insert(scratch->tb, BYTEOFF(0U), value->bytes.data,
                           value->bytes.len);
    sag_undo_mark_saved(scratch->undo);
    scratch->lang = "fletch";
    scratch->macro_reg = reg;
    if (!sag_ed_show_buffer(ed, scratch)) {
        sag_ws_scratch_drop(ed, scratch);
        return SAG_CMD_ERR_STATE;
    }
    return SAG_CMD_OK;
}

CmdStatus sag_macro_store(Ed *ed, Buffer *scratch)
{
    Bytebuf source;
    FlFn *compiled;
    FlSpan span;
    const char *diag;
    CmdStatus status;
    u32 lines;

    if (ed == NULL || scratch == NULL || !named_reg(scratch->macro_reg) ||
        scratch->tb == NULL)
        return SAG_CMD_ERR_ARG;
    text_bytes(scratch->tb, &source);
    compiled = fl_compile_str(ed->fl, source.data, source.len,
                              sag_buf_label(scratch));
    if (compiled == NULL) {
        diag = fl_runtime_last_diag(ed->fl, &span);
        if (ed->win != NULL && ed->win->buf == scratch &&
            ed->win->cs.curs.len != 0U && span.line != 0U &&
            (u64)span.line <= sag_textbuf_line_count(scratch->tb)) {
            LineNo line = {span.line - 1U};
            Span line_span = sag_textbuf_line_span(scratch->tb, line);
            u64 col = span.col == 0U ? 0U : (u64)span.col - 1U;
            ByteOff pos;

            if (col > line_span.hi - line_span.lo)
                col = line_span.hi - line_span.lo;
            pos = BYTEOFF(line_span.lo + col);
            ed->win->cs.curs.data[ed->win->cs.primary].pos = pos;
            ed->win->cs.curs.data[ed->win->cs.primary].anchor = pos;
            sag_win_follow_cursor(ed->win);
        }
        if (diag != NULL)
            sag_msg(ed, SAG_MSG_ERROR, "%s", diag);
        else
            sag_msg(ed, SAG_MSG_ERROR, "macro does not compile");
        bytebuf_free(&source);
        return SAG_CMD_ERR_ARG;
    }
    (void)compiled;
    status = sag_flapi_reg_write(ed, scratch->macro_reg, source.data,
                                 (u32)source.len, false);
    if (status != SAG_CMD_OK) {
        bytebuf_free(&source);
        return status;
    }
    sag_undo_boundary(scratch->undo);
    sag_undo_mark_saved(scratch->undo);
    lines = source_lines(source.data, source.len);
    sag_msg(ed, SAG_MSG_INFO, "stored macro @%c (%u lines)",
            (int)scratch->macro_reg, (unsigned)lines);
    bytebuf_free(&source);
    return SAG_CMD_OK;
}

static const PickItem *browser_items(void *ctx, u32 *n)
{
    MacroBrowse *browse = (MacroBrowse *)ctx;

    *n = browse->nrows;
    return browse->rows;
}

static const RegVal *payload_reg(Ed *ed, i32 payload)
{
    return payload >= 'a' && payload <= 'z'
               ? sag_reg_get(&ed->regs, (u8)payload)
               : NULL;
}

static bool payload_library(Ed *ed, i32 payload, SagMacroEntryView *out)
{
    u32 index;

    if (payload >= 0)
        return false;
    index = (u32)(-payload - 1);
    return sag_macrolib_at(ed, index, out);
}

static void browser_preview(Ed *ed, void *ctx, i32 payload, Rect r)
{
    const RegVal *value = payload_reg(ed, payload);
    SagMacroEntryView library;
    const u8 *source;
    size_t source_len;
    SagColor fg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    SagColor bg = {SAG_COLOR_DEFAULT, 0U, 0U, 0U};
    size_t at = 0U;
    u16 row = 0U;

    (void)ctx;
    if (value != NULL) {
        source = value->bytes.data;
        source_len = value->bytes.len;
    } else if (payload_library(ed, payload, &library)) {
        /* The library owns these bytes for the picker's lifetime. */
        source = (const u8 *)library.source;
        source_len = library.source_len;
    } else {
        return;
    }
    while (at < source_len && row < r.h) {
        size_t end = at;
        int cells = 0;
        size_t fit;

        while (end < source_len && source[end] != (u8)'\n')
            end++;
        fit = sag_str_clip(source + at, end - at, (int)r.w, &cells);
        (void)sag_grid_puts(&ed->grid, (u16)(r.y + row), r.x,
                            source + at, fit, fg, bg, 0U);
        row++;
        at = end < source_len ? end + 1U : end;
    }
}

static bool browser_accept(Ed *ed, void *ctx, i32 payload, u8 how)
{
    (void)ctx;
    (void)how;
    if (payload >= 'a' && payload <= 'z')
        return sag_macro_replay(ed, (u8)payload, 1U) == SAG_CMD_OK;
    {
        SagMacroEntryView entry;

        return payload_library(ed, payload, &entry) && entry.replayable &&
               sag_macrolib_call(ed, entry.name) == SAG_CMD_OK;
    }
}

static bool browser_action(Ed *ed, void *ctx, i32 payload, const Key *key)
{
    MacroBrowse *browse = (MacroBrowse *)ctx;
    const RegVal *value = payload_reg(ed, payload);
    SagMacroEntryView library;
    u8 ch;

    if (key->ntext != 1U)
        return false;
    ch = key->text[0];
    if (value == NULL && payload_library(ed, payload, &library)) {
        if (ch == (u8)'e') {
            sag_picker_close(ed, false);
            return sag_tab_open(ed, library.path) >= 0;
        }
        if (ch == (u8)'y') {
            RegVal copy;

            sag_regval_init(&copy);
            copy.type = SAG_REG_CHARWISE;
            bytebuf_append(&copy.bytes, library.source, library.source_len);
            sag_reg_yank(&ed->regs, 0U, &copy);
            sag_regval_free(&copy);
            sag_msg(ed, SAG_MSG_INFO, "yanked macro %s", library.name);
            return true;
        }
        return ch == (u8)'d' || ch == (u8)'n';
    }
    if (value == NULL)
        return false;
    if (ch == (u8)'e') {
        sag_picker_close(ed, false);
        (void)sag_macro_edit(ed, (u8)payload);
        return true;
    }
    if (ch == (u8)'y') {
        RegVal copy;

        sag_regval_init(&copy);
        sag_regval_copy(&copy, value);
        sag_reg_yank(&ed->regs, 0U, &copy);
        sag_regval_free(&copy);
        sag_msg(ed, SAG_MSG_INFO, "yanked macro @%c", (int)payload);
        return true;
    }
    if (ch == (u8)'d') {
        if (browse->confirm_delete != payload) {
            browse->confirm_delete = payload;
            sag_msg(ed, SAG_MSG_WARN, "press d again to clear macro @%c",
                    (int)payload);
            return true;
        }
        (void)sag_flapi_reg_write(ed, (u8)payload, NULL, 0U, false);
        browse->confirm_delete = 0;
        sag_picker_close(ed, true);
        sag_msg(ed, SAG_MSG_INFO, "cleared macro @%c", (int)payload);
        return true;
    }
    if (ch == (u8)'n') {
        char seed[32];

        (void)snprintf(seed, sizeof(seed), "macro name %c ", (int)payload);
        sag_picker_close(ed, false);
        sag_cmdline_open(ed, SAG_PROMPT_CMD, seed);
        return true;
    }
    browse->confirm_delete = 0;
    return false;
}

static void build_register_rows(Ed *ed)
{
    RecStatus status = {0};
    u8 reg;

    (void)memset(&mb, 0, sizeof(mb));
    (void)sag_record_status(ed, &status);
    for (reg = (u8)'a'; reg <= (u8)'z'; reg++) {
        const RegVal *value = sag_reg_get(&ed->regs, reg);
        char *label;
        size_t at;
        size_t out;
        u32 events;

        if (value == NULL || value->bytes.len == 0U ||
            mb.nrows >= MACRO_ROWS_MAX)
            continue;
        label = mb.labels[mb.nrows];
        events = sag_macro_event_count(value->bytes.data, value->bytes.len);
        out = (size_t)snprintf(label, MACRO_MATCH_MAX,
                              "%s %c   %s%u ev   %llu B   ",
                              status.active && status.reg == reg ? "\xE2\x97\x8F" : " ",
                              (int)reg, events == 0U ? "" : "",
                              (unsigned)events,
                              (unsigned long long)value->bytes.len);
        if (out >= MACRO_MATCH_MAX)
            out = MACRO_MATCH_MAX - 1U;
        for (at = 0U; at < value->bytes.len && out + 1U < MACRO_MATCH_MAX;
             at++) {
            u8 ch = value->bytes.data[at];

            label[out++] = ch == (u8)'\n' || ch == (u8)'\r' ||
                                   ch == (u8)'\t' ? ' ' : (char)ch;
        }
        label[out] = '\0';
        mb.rows[mb.nrows] = (PickItem){label, NULL, (i32)reg, 0U};
        mb.nrows++;
    }
    {
        u32 count = sag_macrolib_count(ed);
        u32 i;

        for (i = 0U; i < count && mb.nrows < MACRO_ROWS_MAX; i++) {
            SagMacroEntryView entry;
            char *label;
            size_t out;
            size_t at;

            if (!sag_macrolib_at(ed, i, &entry) || !entry.replayable)
                continue;
            label = mb.labels[mb.nrows];
            out = (size_t)snprintf(label, MACRO_MATCH_MAX,
                                   "  %s   %s   %llu B   ", entry.name,
                                   entry.events == 0U ? "\xE2\x80\x94 ev" : "events",
                                   (unsigned long long)entry.source_len);
            if (out >= MACRO_MATCH_MAX)
                out = MACRO_MATCH_MAX - 1U;
            for (at = 0U; at < entry.source_len &&
                         out + 1U < MACRO_MATCH_MAX; at++) {
                u8 ch = (u8)entry.source[at];

                label[out++] = ch == (u8)'\n' || ch == (u8)'\r' ||
                                       ch == (u8)'\t' ? ' ' : (char)ch;
            }
            label[out] = '\0';
            mb.rows[mb.nrows] = (PickItem){label, entry.path,
                                           -(i32)i - 1, 0U};
            mb.nrows++;
        }
    }
}

static void append_first_line(Bytebuf *out, const char *source, size_t len)
{
    size_t i;

    for (i = 0U; i < len && source[i] != '\n' && source[i] != '\r'; i++) {
        char ch = source[i] == '\t' ? ' ' : source[i];

        bytebuf_append(out, &ch, 1U);
    }
}

static CmdStatus macro_porcelain(CmdCtx *cx)
{
    Bytebuf out;
    u8 reg;
    u32 i;

    bytebuf_init(&out);
    for (reg = (u8)'a'; reg <= (u8)'z'; reg++) {
        const RegVal *value = sag_reg_get(&cx->ed->regs, reg);
        u32 events;

        if (value == NULL || value->bytes.len == 0U)
            continue;
        events = sag_macro_event_count(value->bytes.data, value->bytes.len);
        bytebuf_printf(&out, "%c\t", (int)reg);
        if (events == 0U)
            bytebuf_append(&out, "-", 1U);
        else
            bytebuf_printf(&out, "%u", (unsigned)events);
        bytebuf_printf(&out, "\t%llu\t", (unsigned long long)value->bytes.len);
        append_first_line(&out, (const char *)value->bytes.data,
                          value->bytes.len);
        bytebuf_push_u8(&out, (u8)'\n');
    }
    for (i = 0U; i < sag_macrolib_count(cx->ed); i++) {
        SagMacroEntryView entry;

        if (!sag_macrolib_at(cx->ed, i, &entry) || !entry.replayable)
            continue;
        bytebuf_printf(&out, "%s\t", entry.name);
        if (entry.events == 0U)
            bytebuf_append(&out, "-", 1U);
        else
            bytebuf_printf(&out, "%u", (unsigned)entry.events);
        bytebuf_printf(&out, "\t%llu\t",
                       (unsigned long long)entry.source_len);
        append_first_line(&out, entry.source, entry.source_len);
        bytebuf_push_u8(&out, (u8)'\n');
    }
    if (out.len != 0U)
        out.len--;
    bytebuf_push_u8(&out, 0U);
    sag_msg(cx->ed, SAG_MSG_INFO, "%s",
            out.len <= 1U ? "" : (const char *)out.data);
    bytebuf_free(&out);
    return SAG_CMD_OK;
}

CmdStatus sag_macro_cmd_list(CmdCtx *cx)
{
    PickerSpec spec;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->source != SAG_SRC_KEY && cx->source != SAG_SRC_MOUSE)
        return macro_porcelain(cx);
    build_register_rows(cx->ed);
    (void)memset(&spec, 0, sizeof(spec));
    spec.title = "Macros";
    spec.items = browser_items;
    spec.preview = browser_preview;
    spec.accept = browser_accept;
    spec.action = browser_action;
    spec.footer = "enter replay . e edit . y yank . d clear . n name . / filter . esc";
    spec.filter_requires_slash = true;
    spec.ctx = &mb;
    sag_picker_open(cx->ed, &spec);
    return sag_picker_active(cx->ed) ? SAG_CMD_OK : SAG_CMD_ERR_STATE;
}

CmdStatus sag_macro_cmd_edit(CmdCtx *cx)
{
    if (cx == NULL || cx->sarg == NULL || cx->sarg_len != 1U)
        return SAG_CMD_ERR_ARG;
    return sag_macro_edit(cx->ed, (u8)cx->sarg[0]);
}

static bool macro_name_valid(const char *name, size_t len)
{
    size_t i;

    if (name == NULL || len == 0U ||
        !((name[0] >= 'a' && name[0] <= 'z') ||
          (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_'))
        return false;
    for (i = 1U; i < len; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
            return false;
    return true;
}

static bool read_file(const char *path, Bytebuf *out)
{
    FILE *fp;
    u8 buf[4096];
    size_t n;

    bytebuf_init(out);
    fp = fopen(path, "rb");
    if (fp == NULL)
        return false;
    while ((n = fread(buf, 1U, sizeof(buf), fp)) != 0U)
        bytebuf_append(out, buf, n);
    if (ferror(fp)) {
        (void)fclose(fp);
        bytebuf_free(out);
        bytebuf_init(out);
        return false;
    }
    return fclose(fp) == 0;
}

static bool line_is_import_ed(const u8 *source, size_t lo, size_t hi)
{
    while (hi > lo && (source[hi - 1U] == (u8)'\r' ||
                       source[hi - 1U] == (u8)' ' ||
                       source[hi - 1U] == (u8)'\t'))
        hi--;
    while (lo < hi && (source[lo] == (u8)' ' || source[lo] == (u8)'\t'))
        lo++;
    return hi - lo == sizeof("import ed") - 1U &&
           memcmp(source + lo, "import ed", sizeof("import ed") - 1U) == 0;
}

static bool source_has_import_ed(const u8 *source, size_t len)
{
    size_t at = 0U;

    while (at < len) {
        size_t end = at;

        while (end < len && source[end] != (u8)'\n')
            end++;
        if (line_is_import_ed(source, at, end))
            return true;
        at = end < len ? end + 1U : end;
    }
    return false;
}

static void append_lifted(Bytebuf *out, const u8 *source, size_t len,
                          const char *name)
{
    size_t at = 0U;

    bytebuf_printf(out, "\nfn %s() {\n", name);
    while (at < len) {
        size_t end = at;

        while (end < len && source[end] != (u8)'\n')
            end++;
        if (!line_is_import_ed(source, at, end)) {
            bytebuf_append(out, "  ", 2U);
            bytebuf_append(out, source + at, end - at);
            bytebuf_push_u8(out, (u8)'\n');
        }
        at = end < len ? end + 1U : end;
    }
    bytebuf_append(out, "}\n", 2U);
}

static CmdStatus promote_macro(Ed *ed, u8 reg, const char *name,
                               size_t name_len)
{
    const RegVal *value;
    const char *dir;
    char *path;
    Bytebuf file;
    Bytebuf candidate;
    FlFn *compiled;
    const char *diag;
    FlSpan span;
    SagSaveErr saved;
    struct stat existing;
    size_t path_len;

    if (ed == NULL || !named_reg(reg) || !macro_name_valid(name, name_len)) {
        if (ed != NULL)
            sag_msg(ed, SAG_MSG_ERROR, "macro name must be a Fletch identifier");
        return SAG_CMD_ERR_ARG;
    }
    value = sag_reg_get(&ed->regs, reg);
    if (value == NULL || value->bytes.len == 0U)
        return SAG_CMD_ERR_ARG;
    dir = sag_macrolib_dir(ed);
    if (dir == NULL || dir[0] == '\0' || !sag_mkdirs(dir, 0700U)) {
        sag_msg(ed, SAG_MSG_ERROR, "cannot create macro library directory");
        return SAG_CMD_ERR_IO;
    }
    path_len = strlen(dir) + sizeof("/user.fl");
    path = sag_xmalloc(path_len);
    (void)snprintf(path, path_len, "%s/user.fl", dir);
    if (!read_file(path, &file) && stat(path, &existing) == 0) {
        sag_msg(ed, SAG_MSG_ERROR, "cannot read existing %s", path);
        bytebuf_free(&file);
        free(path);
        return SAG_CMD_ERR_IO;
    }
    bytebuf_init(&candidate);
    if (file.len == 0U) {
        bytebuf_append(&candidate,
                       "# sagitta-macro: 1\n# recorded-with: sagitta "
                       SAG_VERSION "\n# keymap: default\nimport ed\n",
                       sizeof("# sagitta-macro: 1\n# recorded-with: sagitta "
                              SAG_VERSION "\n# keymap: default\nimport ed\n") - 1U);
    } else {
        bytebuf_append(&candidate, file.data, file.len);
        if (candidate.data[candidate.len - 1U] != (u8)'\n')
            bytebuf_push_u8(&candidate, (u8)'\n');
        if (!source_has_import_ed(candidate.data, candidate.len))
            bytebuf_append(&candidate, "import ed\n", 10U);
    }
    append_lifted(&candidate, value->bytes.data, value->bytes.len, name);
    compiled = fl_compile_str(ed->fl, candidate.data, candidate.len, path);
    if (compiled == NULL) {
        diag = fl_runtime_last_diag(ed->fl, &span);
        sag_msg(ed, SAG_MSG_ERROR, "%s",
                diag == NULL ? "named macro does not compile" : diag);
        bytebuf_free(&candidate);
        bytebuf_free(&file);
        free(path);
        return SAG_CMD_ERR_ARG;
    }
    (void)compiled;
    saved = sag_file_write_atomic(path, candidate.data, candidate.len, 0600);
    bytebuf_free(&candidate);
    bytebuf_free(&file);
    if (saved != SAG_SAVE_OK) {
        sag_msg(ed, SAG_MSG_ERROR, "cannot write %s", path);
        free(path);
        return SAG_CMD_ERR_IO;
    }
    (void)sag_macrolib_scan(ed, NULL);
    {
        SagMacroEntryView loaded;
        char *canonical = sag_xmalloc(sizeof("user.") + name_len);

        (void)snprintf(canonical, sizeof("user.") + name_len,
                       "user.%.*s", (int)name_len, name);
        if (!sag_macrolib_find(ed, canonical, &loaded) ||
            !loaded.replayable) {
            sag_msg(ed, SAG_MSG_ERROR,
                    "wrote %s but macro %s did not load", path, canonical);
            free(canonical);
            free(path);
            return SAG_CMD_ERR_STATE;
        }
        free(canonical);
    }
    sag_msg(ed, SAG_MSG_INFO, "named macro @%c as %.*s", (int)reg,
            (int)name_len, name);
    free(path);
    return SAG_CMD_OK;
}

CmdStatus sag_macro_cmd_name(CmdCtx *cx)
{
    const char *reg_text;
    const char *name;
    size_t name_len;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    if (cx->argv.n == 3U) {
        reg_text = cx->argv.v[1];
        name = cx->argv.v[2];
        name_len = strlen(name);
    } else if (cx->sarg != NULL && cx->sarg_len >= 3U) {
        const char *sep = memchr(cx->sarg, ' ', cx->sarg_len);

        if (sep == NULL)
            return SAG_CMD_ERR_ARG;
        reg_text = cx->sarg;
        name = sep + 1;
        name_len = cx->sarg_len - (size_t)(name - cx->sarg);
    } else {
        return SAG_CMD_ERR_ARG;
    }
    if (reg_text[0] < 'a' || reg_text[0] > 'z' ||
        (cx->argv.n == 3U ? reg_text[1] != '\0' : reg_text[1] != ' '))
        return SAG_CMD_ERR_ARG;
    return promote_macro(cx->ed, (u8)reg_text[0], name, name_len);
}

CmdStatus sag_macro_cmd_reload(CmdCtx *cx)
{
    u32 loaded;

    if (cx == NULL || cx->ed == NULL)
        return SAG_CMD_ERR_ARG;
    loaded = sag_macrolib_scan(cx->ed, NULL);
    sag_msg(cx->ed, SAG_MSG_INFO, "reloaded %u macro%s", (unsigned)loaded,
            loaded == 1U ? "" : "s");
    return SAG_CMD_OK;
}
