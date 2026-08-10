/* Sprint 29: caret diagnostics for Fletch.  See diag.h for why the
 * module lives under src/fl/ rather than src/util/. */
#include "fl/diag.h"

#include <stdio.h>
#include <string.h>

#include "util/log.h"

void fl_diag_init(DiagCtx *dc, Arena *arena)
{
    if (dc == NULL)
        return;
    (void)memset(dc, 0, sizeof(*dc));
    dc->arena = arena;
}

u32 fl_diag_add_file(DiagCtx *dc, const char *path, const char *src,
                     size_t len)
{
    FlDiagFile *grown;
    u32 id;

    if (dc == NULL || dc->arena == NULL)
        YEW_BUG("fletch diag: no context arena");
    if (dc->nfiles == UINT32_MAX)
        YEW_BUG("fletch diag: file id overflow");
    if (dc->nfiles == dc->capfiles) {
        u32 want = dc->capfiles == 0U ? (u32)FL_DIAG_INITIAL_FILES :
                   dc->capfiles * 2U;

        if (want < dc->capfiles)
            YEW_BUG("fletch diag: file table size overflow");
        grown = arena_alloc(dc->arena, (size_t)want * sizeof(*grown),
                            _Alignof(FlDiagFile));
        if (dc->nfiles != 0U)
            (void)memcpy(grown, dc->files,
                         (size_t)dc->nfiles * sizeof(*grown));
        dc->files = grown;
        dc->capfiles = want;
    }
    id = dc->nfiles++;
    dc->files[id].path = path == NULL ? "<input>" : path;
    dc->files[id].src = src;
    dc->files[id].len = src == NULL ? 0U : len;
    return id;
}

void fl_diag_set_sink(DiagCtx *dc, FlDiagSink sink, void *ctx)
{
    if (dc == NULL)
        return;
    dc->sink = sink;
    dc->sink_ctx = ctx;
}

u32 fl_diag_errors(const DiagCtx *dc)
{
    return dc == NULL ? 0U : dc->nerrors;
}

static const char *level_word(FlDiagLevel level)
{
    switch (level) {
    case FL_DIAG_WARNING:
        return "warning";
    case FL_DIAG_NOTE:
        return "note";
    default:
        return "error";
    }
}

/*
 * The bytes of `sp`'s line, without its terminator.
 *
 * Scans from the start of the file rather than keeping a line table:
 * diagnostics are rare (capped at 20 by the parser) and a table would
 * have to be rebuilt every time a caller re-registered a source.  A
 * pathological file makes this O(file) per diagnostic and 20 of those
 * is still nothing next to parsing it.
 */
static bool line_bytes(const FlDiagFile *f, u32 line, const char **out,
                       size_t *out_len)
{
    size_t at = 0U;
    u32 cur = 1U;

    if (f->src == NULL)
        return false;
    while (cur < line && at < f->len) {
        if (f->src[at] == '\n')
            cur++;
        at++;
    }
    if (cur != line || at > f->len)
        return false;
    {
        size_t end = at;

        while (end < f->len && f->src[end] != '\n')
            end++;
        /* A CRLF source must not put the caret under a stray '\r'. */
        if (end > at && f->src[end - 1U] == '\r')
            end--;
        *out = f->src + at;
        *out_len = end - at;
    }
    return true;
}

void fl_diag_render(Bytebuf *out, const DiagCtx *dc, FlDiagLevel level,
                    FlSpan sp, const char *msg)
{
    const FlDiagFile *f;
    const char *line = NULL;
    size_t line_len = 0U;
    size_t i;

    if (out == NULL || dc == NULL || sp.file_id >= dc->nfiles) {
        if (out != NULL)
            bytebuf_printf(out, "%s: %s\n", level_word(level),
                           msg == NULL ? "" : msg);
        return;
    }
    f = &dc->files[sp.file_id];
    bytebuf_printf(out, "%s:%u:%u: %s: %s\n", f->path, (unsigned)sp.line,
                   (unsigned)sp.col, level_word(level),
                   msg == NULL ? "" : msg);
    if (!line_bytes(f, sp.line, &line, &line_len))
        return;
    bytebuf_append(out, line, line_len);
    bytebuf_push_u8(out, (u8)'\n');
    /*
     * The caret run is built from the line's own bytes so a tab in the
     * source indents the caret by a tab: anything else slides the marker
     * off the token it is pointing at, which is the one job it has.
     */
    for (i = 0U; i + 1U < (size_t)sp.col && i < line_len; i++)
        bytebuf_push_u8(out, (u8)(line[i] == '\t' ? '\t' : ' '));
    bytebuf_push_u8(out, (u8)'^');
    {
        /* `len` counts the token; the caret already covers its first
         * byte, and a zero-length span (EOF) still gets its one mark. */
        u32 tildes = sp.len > 1U ? sp.len - 1U : 0U;
        u32 t;

        for (t = 0U; t < tildes; t++)
            bytebuf_push_u8(out, (u8)'~');
    }
    bytebuf_push_u8(out, (u8)'\n');
}

void fl_diag_vemit(DiagCtx *dc, FlDiagLevel level, FlSpan sp,
                   const char *fmt, va_list ap)
{
    char msg[512];
    Bytebuf rendered;

    if (dc == NULL || dc->muted)
        return;
    /* Not `fmt == NULL ? "" : fmt`: clang can see the empty literal
     * through the ternary and -Wformat-zero-length rejects it, so the
     * empty case never reaches vsnprintf at all. */
    if (fmt == NULL || fmt[0] == '\0')
        msg[0] = '\0';
    else
        (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    if (level == FL_DIAG_ERROR)
        dc->nerrors++;
    else if (level == FL_DIAG_WARNING)
        dc->nwarnings++;
    bytebuf_init(&rendered);
    fl_diag_render(&rendered, dc, level, sp, msg);
    bytebuf_push_u8(&rendered, (u8)'\0');
    if (dc->sink != NULL) {
        dc->sink(dc->sink_ctx, level, sp, msg, (const char *)rendered.data);
    } else {
        /*
         * No sink: through yew_log, never straight to stderr.  The pty
         * goldens are byte-exact, and a compiler that wrote to fd 2
         * behind the harness's back would corrupt whichever screen
         * happened to be under it.
         */
        yew_log(level == FL_DIAG_ERROR ? YEW_LOG_ERROR : YEW_LOG_WARN,
                "%s", (const char *)rendered.data);
    }
    bytebuf_free(&rendered);
}

void fl_diag_emit(DiagCtx *dc, FlDiagLevel level, FlSpan sp,
                  const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fl_diag_vemit(dc, level, sp, fmt, ap);
    va_end(ap);
}
