#ifndef YEW_MOD_AI_CONFIG_H
#define YEW_MOD_AI_CONFIG_H

/*
 * Validate an ai.backend map while its Fletch source spans still exist.
 *
 * This boundary deliberately accepts the AST rather than an FlMap: map
 * conversion discards the key-token spans needed to point at a credential
 * field in init.fl.  Only direct entries in `backend` are configuration
 * fields; nested maps belong to their own schemas and are not scanned.
 */

#include <stdbool.h>

#include "fl/ast.h"
#include "fl/diag.h"
#include "mod/ai/http.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

void yew_ai_config_render_credential_diag(Bytebuf *out, const DiagCtx *dc,
                                          FlSpan span, const char *word);

bool yew_ai_config_validate_backend(Arena *arena, DiagCtx *dc,
                                    const Interner *in,
                                    const FlNode *backend,
                                    HttpUrl *parsed_url);

/* Walk a compiled config AST before FlMap conversion can discard key spans.
 * Every ai.backend(name, map) call is checked, including calls nested in
 * control flow or helper expressions. */
bool yew_ai_config_validate_program(Arena *arena, DiagCtx *dc,
                                    const Interner *in,
                                    const FlProgram *program);

#endif /* YEW_MOD_AI_CONFIG_H */
