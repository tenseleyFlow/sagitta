#ifndef YEW_MOD_LSP_LSP_H
#define YEW_MOD_LSP_LSP_H

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Buffer Buffer;
typedef struct EditCtx EditCtx;
typedef struct Key Key;
typedef struct Win Win;

/* Sprint 45's editor-facing module boundary.  The disabled-module shim
 * implements this same surface, so editor commands never depend on module
 * internals or disappear from the registry in stripped builds. */
bool yew_lsp_require(Ed *ed);
bool yew_lsp_info(Ed *ed);
bool yew_lsp_log(Ed *ed);
bool yew_lsp_start(Ed *ed, Buffer *b);
bool yew_lsp_stop(Ed *ed, Buffer *b);
bool yew_lsp_diagnostics(Ed *ed);
bool yew_lsp_diag_step(Ed *ed, Win *w, bool forward);
bool yew_lsp_complete(Ed *ed, Win *w);
bool yew_lsp_hover(Ed *ed, Win *w);
bool yew_lsp_signature(Ed *ed, Win *w);
bool yew_lsp_goto_definition(Ed *ed, Win *w);
bool yew_lsp_goto_declaration(Ed *ed, Win *w);
bool yew_lsp_goto_type_definition(Ed *ed, Win *w);
bool yew_lsp_goto_implementation(Ed *ed, Win *w);
bool yew_lsp_references(Ed *ed, Win *w);
bool yew_lsp_rename(Ed *ed, Win *w);
bool yew_lsp_symbols(Ed *ed, Win *w);
bool yew_lsp_rename_key(Ed *ed, const Key *key);

/*
 * Sprint 57.13 Deliverable 4: the rename confirmation's three answers,
 * as something other than a keystroke.
 *
 * The confirmation is a RAW KEY HANDLER — Enter applies, `d` shows the
 * diff, Esc cancels — so its rows had no registry command to carry and
 * shipped as nothing.  `yew_lsp_rename_answer` is now the one
 * implementation of all three, and `yew_lsp_rename_key` is a second
 * ROUTE to it rather than a second copy of the phase logic: a menu row
 * and a keystroke that disagreed about what `d` does would be two
 * rename dialogs wearing one panel.
 *
 * Returns false when the confirm phase is not up; the commands turn
 * that into a message rather than a silent no-op.
 */
typedef enum LspRenameAnswer {
    YEW_LSP_RENAME_APPLY = 0,
    YEW_LSP_RENAME_DIFF,
    YEW_LSP_RENAME_CANCEL
} LspRenameAnswer;

bool yew_lsp_rename_answer(Ed *ed, LspRenameAnswer answer);
/*
 * Is the panel the FOCUSED window is showing the rename confirmation?
 *
 * The panel slot also hosts hover and signature help, which keep the
 * bare `Close` row, so the PANEL menu builder has to be able to tell
 * them apart (57.13 §4).  False in the diff phase: the summary panel is
 * closed before the diff buffer is shown, so there is no panel to
 * right-click.
 */
bool yew_lsp_rename_confirm_active(const Ed *ed);
void yew_lsp_signature_maybe_auto_trigger(Ed *ed, Win *w,
                                          const u8 *text, u32 len);
bool yew_lsp_status_badge(const Ed *ed, const Buffer *b,
                          char *out, size_t cap);
/*
 * Sprint 57.13 §4: is a server attached to THIS buffer?
 *
 * The document menu's LSP section is omitted whole when the answer is
 * no (the section-omission rule), so the question has to be answerable
 * without starting anything, without allocating and without a Win — a
 * menu is built for the pane that was pointed at, which is not
 * necessarily the focused one.  The shim answers false, which is what
 * makes a MODULES="" build simply have no LSP section rather than a
 * section of dead rows.
 */
bool yew_lsp_attached(const Ed *ed, const Buffer *b);
void yew_lsp_shadow_install(void);

/* Module-neutral editor lifecycle.  The stripped shim implements the same
 * surface so the core never needs feature-conditionals. */
void yew_lsp_pump(Ed *ed);
void yew_lsp_free(Ed *ed);
void yew_lsp_buffer_open(Ed *ed, Buffer *b);
void yew_lsp_buffer_save(Ed *ed, Buffer *b);
void yew_lsp_buffer_close(Ed *ed, Buffer *b);
void yew_lsp_note_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len);
void yew_lsp_note_edit_post(EditCtx *ec, u8 kind, ByteOff at, u64 len);
void yew_lsp_highlight_cursor(Ed *ed, Win *w);
void yew_lsp_highlight_clear(Ed *ed, Win *w);

#endif
