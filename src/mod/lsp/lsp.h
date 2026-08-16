#ifndef YEW_MOD_LSP_LSP_H
#define YEW_MOD_LSP_LSP_H

#include <stdbool.h>
#include <stddef.h>

#include "text/coords.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct Buffer Buffer;
typedef struct EditCtx EditCtx;
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
bool yew_lsp_symbols(Ed *ed, Win *w);
void yew_lsp_signature_maybe_auto_trigger(Ed *ed, Win *w,
                                          const u8 *text, u32 len);
bool yew_lsp_status_badge(const Ed *ed, const Buffer *b,
                          char *out, size_t cap);
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
