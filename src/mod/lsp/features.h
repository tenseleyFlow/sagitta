#ifndef YEW_MOD_LSP_FEATURES_H
#define YEW_MOD_LSP_FEATURES_H

#include "util/base.h"
#include "util/buf.h"
#include "mod/lsp/json.h"
#include "edit/shadow.h"
#include "ui/complmenu.h"

typedef struct Ed Ed;
typedef struct TextBuf TextBuf;
typedef struct Win Win;

/* Downgrade LSP snippet syntax to insertion-ready plain text.  Returns the
 * byte offset where $0 occurred, or the emitted length when it was absent. */
u32 yew_lsp_snippet_strip(const u8 *in, u32 n, Bytebuf *out);

/* Parse one CompletionList or CompletionItem[] into fully owned menu rows.
 * The returned preselect is an index in the sorted output, or -1. */
u32 yew_lsp_completion_parse(const JsonValue *result, const TextBuf *tb,
                             u8 pos_enc, const u8 *stem, u32 stem_len,
                             Vec_ComplItem *out, i32 *preselect);
bool yew_lsp_completion_resolve_apply(ComplItem *item,
                                      const JsonValue *result);
void yew_lsp_completion_discard(ComplItem *item);
bool yew_lsp_completion_accept(Ed *ed, Win *w, Span replace,
                               const ComplItem *item);

/* Pure response decoders used by the panel request path and fuzz/unit
 * seams.  Outputs append to body and carry byte spans into that body. */
bool yew_lsp_hover_parse(const JsonValue *result, const TextBuf *tb,
                         u8 pos_enc, Bytebuf *body, Span *range,
                         bool *has_range);
bool yew_lsp_signature_parse(const JsonValue *result, Bytebuf *body,
                             Span *emph, bool *has_emph);
bool yew_lsp_hover_request(Ed *ed, Win *w);
bool yew_lsp_signature_request(Ed *ed, Win *w);

extern const ComplSource yew_compl_src_lsp;
const ShadowProvider *yew_lsp_shadow_provider(void);
void yew_lsp_shadow_install(void);

#endif /* YEW_MOD_LSP_FEATURES_H */
