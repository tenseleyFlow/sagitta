#ifndef YEW_MOD_LSP_LSP_H
#define YEW_MOD_LSP_LSP_H

#include <stdbool.h>

typedef struct Ed Ed;
typedef struct Buffer Buffer;

/* Sprint 45's editor-facing module boundary.  The disabled-module shim
 * implements this same surface, so editor commands never depend on module
 * internals or disappear from the registry in stripped builds. */
bool yew_lsp_require(Ed *ed);
bool yew_lsp_info(Ed *ed);
bool yew_lsp_log(Ed *ed);
bool yew_lsp_start(Ed *ed, Buffer *b);

#endif
