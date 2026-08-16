#ifndef YEW_MOD_LSP_FEATURES_H
#define YEW_MOD_LSP_FEATURES_H

#include "util/base.h"
#include "util/buf.h"

/* Downgrade LSP snippet syntax to insertion-ready plain text.  Returns the
 * byte offset where $0 occurred, or the emitted length when it was absent. */
u32 yew_lsp_snippet_strip(const u8 *in, u32 n, Bytebuf *out);

#endif /* YEW_MOD_LSP_FEATURES_H */
