#ifndef YEW_MOD_AI_STATS_H
#define YEW_MOD_AI_STATS_H

#include "edit/cmd.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct AiStatsState AiStatsState;

AiStatsState *yew_ai_stats_new(void);
void yew_ai_stats_free(Ed *ed, AiStatsState *state);
void yew_ai_stats_pump(Ed *ed, i64 now_ms);

void yew_ai_stats_request(Ed *ed, const char *backend);
void yew_ai_stats_decline(Ed *ed, const char *backend);
void yew_ai_stats_delivery(Ed *ed, const char *backend, u64 bytes);
void yew_ai_stats_cancel(Ed *ed, const char *backend);
void yew_ai_stats_error(Ed *ed, const char *backend);
void yew_ai_stats_dismiss(Ed *ed, const char *backend);
void yew_ai_stats_accept(Ed *ed, const char *backend, u8 kind, u64 bytes);
void yew_ai_stats_finish(Ed *ed, const char *backend, i64 first_token_ms,
                         i64 tokens_in, i64 tokens_out);

CmdStatus yew_ai_cmd_stats(CmdCtx *cx);

#endif
