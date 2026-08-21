#ifndef YEW_MOD_AI_AI_H
#define YEW_MOD_AI_AI_H

#include <stdbool.h>
#include <stddef.h>
#include <poll.h>

#include "edit/cmd.h"
#include "mod/ai/backend.h"
#include "util/base.h"
#include "ws/trust.h"

typedef struct Ed Ed;
typedef struct FlMap FlMap;
typedef struct FlStr FlStr;
typedef struct AiBackendEntry AiBackendEntry;

/* Sprint 48's editor-facing module boundary.  The stripped-module shim
 * implements the same command surface, so AI commands remain discoverable
 * and explain how to obtain the module instead of disappearing. */
CmdStatus yew_ai_cmd_off(CmdCtx *cx);
CmdStatus yew_ai_cmd_require(CmdCtx *cx);
CmdStatus yew_ai_cmd_backends(CmdCtx *cx);
CmdStatus yew_ai_cmd_models(CmdCtx *cx);
CmdStatus yew_ai_cmd_ping(CmdCtx *cx);
CmdStatus yew_ai_cmd_log(CmdCtx *cx);
CmdStatus yew_ai_cmd_reload(CmdCtx *cx);
CmdStatus yew_ai_cmd_stats(CmdCtx *cx);
CmdStatus yew_ai_cmd_open(CmdCtx *cx);
CmdStatus yew_ai_cmd_enable(CmdCtx *cx);
CmdStatus yew_ai_cmd_disable(CmdCtx *cx);
CmdStatus yew_ai_cmd_forget(CmdCtx *cx);
CmdStatus yew_ai_cmd_privacy(CmdCtx *cx);
CmdStatus yew_ai_cmd_preset(CmdCtx *cx);
CmdStatus yew_ai_cmd_status(CmdCtx *cx);

/* Module-neutral, Ed-owned lifecycle.  Both the enabled implementation and
 * stripped shim provide it, keeping compile-time module checks out of core. */
void yew_ai_state_init(Ed *ed);
void yew_ai_state_free(Ed *ed);
bool yew_ai_state_ready(const Ed *ed);
void yew_ai_state_key_cache_enable(Ed *ed, bool enabled);
bool yew_ai_state_key_cache_enabled(const Ed *ed);

/* Render the privacy badge without exposing AiState internals. */
bool yew_ai_status_badge(const Ed *ed, char *out, size_t outsz,
                         u8 *priority);
void yew_ai_status_note(Ed *ed, AiErrKind kind);
void yew_ai_status_clear(Ed *ed);

/* Runtime backend definitions are copied immediately out of Fletch's GC. */
bool yew_ai_backend_define(Ed *ed, const FlStr *name, const FlMap *config,
                           char *err, size_t errsz);
u32 yew_ai_backend_count(const Ed *ed);
const AiBackendEntry *yew_ai_backend_at(const Ed *ed, u32 index);
const AiBackendEntry *yew_ai_backend_selected(const Ed *ed);
bool yew_ai_backend_name_is_remote(const Ed *ed, const char *name, u32 len);

/* Workspace disclosure is independent of workspace-config execution trust. */
AiWsGrant yew_ai_workspace_grant(Ed *ed);
bool yew_ai_workspace_allowed(Ed *ed);
void yew_ai_workspace_set(Ed *ed, AiWsGrant grant);
void yew_ai_workspace_session_set(Ed *ed, AiWsGrant grant);
/* Returns the borrowed shipped/user glob that excludes path, or NULL. */
const char *yew_ai_path_exclusion(Ed *ed, const char *path);

/* Module-neutral event-loop hooks. */
void yew_ai_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n);
void yew_ai_pump(Ed *ed, const struct pollfd *pfd, u32 n);
i64 yew_ai_deadline(const Ed *ed, i64 now_ms);

/* Sprint 49's passive completion provider.  Registration is process-global;
 * the Ed argument is accepted for the module-neutral startup surface. */
void yew_ai_shadow_init(Ed *ed);
void yew_ai_shadow_accept_note(Ed *ed, u32 seq, u8 kind, u64 bytes);
void yew_ai_shadow_dismiss_note(Ed *ed, u32 seq);

#endif
