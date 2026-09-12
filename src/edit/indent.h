#ifndef YEW_EDIT_INDENT_H
#define YEW_EDIT_INDENT_H

/*
 * Sprint 57.16 §1: the one place that answers "what is this line's
 * indentation".
 *
 * Four private near-duplicates of this question existed before this module
 * (src/text/register.c, src/edit/block.c, src/edit/motion.c,
 * src/edit/sel_actions.c).  block.c's `line_info` now delegates here.  The
 * other two are deliberately NOT folded in: motion.c's `line_home` walks
 * grapheme clusters to find a cursor landing site and treats '\n'/'\r' as
 * non-blank, and register.c's `first_nonblank` is a byte scan from an
 * arbitrary offset that skips only ' ' and '\t'.  Both answer different
 * questions; collapsing them would change behaviour, not remove a duplicate.
 *
 * `expandtab` acquires its meaning in yew_indent_unit and nowhere else.
 */

#include "edit/buf.h"
#include "text/coords.h"
#include "text/piece.h"
#include "unicode/coords.h"
#include "util/base.h"
#include "util/buf.h"

/* The option table caps `tabwidth` at 16, so one indent level never emits
 * more bytes than this.  Callers size stack buffers with it. */
enum { YEW_INDENT_UNIT_MAX = 16 };

typedef struct IndentInfo {
    ByteOff first;   /* first non-blank byte; content end when blank */
    CCol    width;   /* indent WIDTH in cells, tab-expanded */
    bool    blank;   /* line is entirely whitespace */
    bool    tabs;    /* leading whitespace begins with '\t' */
} IndentInfo;

/*
 * Classifies `line` (a span from yew_textbuf_line_span, EOL included).
 * Takes the ASCII fast path first and falls back to grapheme iteration for
 * lines whose leading run is not ASCII.  Returns false only on a NULL
 * argument; every real line has an answer.
 */
bool yew_indent_info(const TextBuf *tb, Span line, u32 tabwidth,
                     IndentInfo *out);

/*
 * Bytes one indent level emits, honouring `expandtab` and `tabwidth`:
 * `tabwidth` spaces when expandtab is set, one '\t' when it is clear.
 * Writes nothing and returns 0 when cap is too small.
 */
u32 yew_indent_unit(const Buffer *b, u8 *out, u32 cap);
void yew_indent_unit_append(const Buffer *b, Bytebuf *out);

/* Appends `line`'s leading whitespace to `out` verbatim. */
void yew_indent_lead_append(const TextBuf *tb, Span line,
                            const IndentInfo *info, Bytebuf *out);

/*
 * The line's last byte that is not ASCII whitespace.  False when the line
 * has none.  Enter's "did the line I am leaving end in an opener" question
 * and the block unit's continuation heuristic are the same lookup.
 */
bool yew_indent_last_nonwhite(const TextBuf *tb, Span line, u8 *out);

/*
 * Where one Backspace inside leading whitespace should land: the previous
 * tab stop, snapping a ragged indent back to the stop below it.  Outside
 * the leading whitespace this is the previous grapheme boundary, so the
 * caller can use it unconditionally.
 */
ByteOff yew_indent_back(const TextBuf *tb, Span line, u32 tabwidth,
                        ByteOff from);

#endif /* YEW_EDIT_INDENT_H */
