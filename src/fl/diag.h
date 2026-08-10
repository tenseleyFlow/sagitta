#ifndef SAG_FL_DIAG_H
#define SAG_FL_DIAG_H

/*
 * Sprint 29: source spans and caret diagnostics for Fletch.
 *
 * WHY THIS LIVES IN src/fl/ RATHER THAN src/util/
 *
 * Sprint 29's prerequisites expect a `DiagCtx` from Sprint 0, whose entry
 * says "diag/log module" without ever pinning an API.  What actually
 * landed is util/log.h -- sag_log(level, fmt, ...) -- which has no notion
 * of a file, a span or a caret, and is a process-wide sink rather than a
 * per-compilation context.  The sprint anticipates the gap and says the
 * RENDERING CONTRACT is what binds, so the contract is implemented here,
 * inside the one directory Sprint 29's DoD 1 permits it to touch.
 *
 * If a general diag module ever lands in util/, this is the shape to
 * lift: nothing here is Fletch-specific except the FL_ prefix.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"

/*
 * A half-open source range, 1-based for humans.
 *
 * `col` is in BYTES, not codepoints or columns: it indexes the source
 * line directly, which is what caret rendering needs, and it is the only
 * measure that stays correct when the line contains invalid UTF-8 -- the
 * case a lexer diagnostic most needs to point at.  Display width is a
 * rendering concern and is handled where the caret is drawn.
 */
typedef struct FlSpan {
    u32 file_id;
    u32 line;
    u32 col;
    u32 len;
} FlSpan;

typedef enum FlDiagLevel {
    FL_DIAG_ERROR,
    FL_DIAG_WARNING,
    FL_DIAG_NOTE
} FlDiagLevel;

/*
 * Where a diagnostic goes.
 *
 * `msg` is the bare message and `rendered` the full caret block, so a
 * test can assert on the SENTENCE without depending on how the caret is
 * drawn, and a golden can pin the whole block when that is the point.
 * Sprint 29's testing strategy requires the former explicitly: message
 * text is asserted "via a capturing diag sink -- structural, not
 * stderr-scraped".
 */
typedef void (*FlDiagSink)(void *ctx, FlDiagLevel level, FlSpan sp,
                           const char *msg, const char *rendered);

enum { FL_DIAG_INITIAL_FILES = 16 };

typedef struct FlDiagFile {
    const char *path;
    const char *src;
    size_t len;
} FlDiagFile;

typedef struct DiagCtx {
    Arena *arena;
    /* Arena-grown.  File ids are indices and therefore remain stable when
     * the backing array moves; old arrays die with the same arena. */
    FlDiagFile *files;
    u32 nfiles;
    u32 capfiles;
    u32 nerrors;
    u32 nwarnings;
    FlDiagSink sink;
    void *sink_ctx;
    /*
     * Emit nothing more.
     *
     * The parser's error cap cannot be enforced by the parser alone:
     * the LEXER reports straight through this context, and one
     * advance() drains a whole run of bad bytes, so a burst emits
     * several more diagnostics after the parser has decided to stop.
     * Muting the sink is what makes the cap a bound rather than an
     * intention.  Scoped to one parse -- each entry point clears it.
     */
    bool muted;
} DiagCtx;

void fl_diag_init(DiagCtx *dc, Arena *arena);

/*
 * Registers a source and returns its stable id.  The text is BORROWED: the
 * caller keeps it alive for as long as diagnostics may be rendered
 * against it, because the caret block quotes the offending line out of
 * it rather than copying every line up front.
 */
u32 fl_diag_add_file(DiagCtx *dc, const char *path, const char *src,
                     size_t len);

void fl_diag_set_sink(DiagCtx *dc, FlDiagSink sink, void *ctx);

void fl_diag_emit(DiagCtx *dc, FlDiagLevel level, FlSpan sp,
                  const char *fmt, ...);
void fl_diag_vemit(DiagCtx *dc, FlDiagLevel level, FlSpan sp,
                   const char *fmt, va_list ap);

/* Renders `path:line:col: error: msg`, the source line, and the caret
 * run, into `out`.  Exposed so goldens can pin the block without going
 * anywhere near stderr. */
void fl_diag_render(Bytebuf *out, const DiagCtx *dc, FlDiagLevel level,
                    FlSpan sp, const char *msg);

u32 fl_diag_errors(const DiagCtx *dc);

#endif /* SAG_FL_DIAG_H */
