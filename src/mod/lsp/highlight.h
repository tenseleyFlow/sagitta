#ifndef YEW_MOD_LSP_HIGHLIGHT_H
#define YEW_MOD_LSP_HIGHLIGHT_H

#include "mod/lsp/json.h"
#include "text/coords.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;
typedef struct TextBuf TextBuf;
typedef struct Win Win;

enum {
    YEW_LSP_HIGHLIGHT_TEXT = 1,
    YEW_LSP_HIGHLIGHT_READ = 2,
    YEW_LSP_HIGHLIGHT_WRITE = 3,
    YEW_LSP_HIGHLIGHT_MAX = 20000
};

typedef struct LspHighlight {
    Span span;
    u8 kind;
} LspHighlight;

VEC_DECL(Vec_LspHighlight, LspHighlight);

u32 yew_lsp_highlights_parse(const JsonValue *result, const TextBuf *tb,
                             u8 pos_enc, Vec_LspHighlight *out);
void yew_lsp_highlights_free(Vec_LspHighlight *out);

void yew_lsp_highlight_cursor(Ed *ed, Win *w);
void yew_lsp_highlight_clear(Ed *ed, Win *w);
void yew_lsp_highlight_buffer_clear(Ed *ed, u32 buf_id);
void yew_lsp_highlight_shutdown(Ed *ed);

#endif
