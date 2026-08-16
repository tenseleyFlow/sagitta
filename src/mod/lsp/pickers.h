#ifndef YEW_MOD_LSP_PICKERS_H
#define YEW_MOD_LSP_PICKERS_H

#include "mod/lsp/features.h"

typedef struct Ed Ed;
typedef struct Win Win;

/* Obeys the global lsp.open_in option and pushes one jump only after the
 * target file has been hydrated and the jump can succeed. */
bool yew_lsp_location_jump(Ed *ed, Win *w, const LspLoc *loc, u8 pos_enc);

/* Consumes `locs` and opens the shared location picker. */
void yew_lsp_location_picker_open(Ed *ed, Win *w, Vec_LspLoc *locs,
                                  u8 pos_enc, const char *title);
void yew_lsp_pickers_free(void);

#endif /* YEW_MOD_LSP_PICKERS_H */
