#include "mod/ai/optin.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flconf.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/preset.h"
#include "term/tty.h"
#include "text/file.h"
#include "ui/cmdline.h"
#include "ui/message.h"
#include "util/buf.h"
#include "util/xdg.h"

static const char choose_prompt[] =
    "Turn on AI completions?\n\n"
    "  [1] Local model (ollama on this machine)\n"
    "      Your code is sent to a program running on this computer.\n"
    "      Nothing leaves the machine. No account, no API key, no network.\n\n"
    "  [2] Cloud model (Anthropic)\n"
    "      Parts of the file you are editing are sent over the internet to "
    "a company's servers under that company's policy, not yew's.\n\n"
    "  [3] Read the full privacy page first  (:ai privacy)\n\n"
    "  [n] Not now\n\nChoice [1/2/3/n]:";

static const char local_prompt[] =
    "Local model — what leaves this editor:\n\n"
    "  * Up to 4096 bytes of the file you are editing: the text "
    "immediately before your cursor and immediately after it.\n"
    "  * The file's path, relative to this workspace (never an absolute "
    "path, so never your home directory or your username).\n"
    "  * The file's language name (\"c\", \"rust\", ...).\n\n"
    "Where it goes: http://127.0.0.1:11434 — a process on this machine.\n"
    "Over the network: nothing. The socket is loopback; there is no wire.\n"
    "Retained by yew: nothing but counters you can read with :ai stats "
    "and delete at any time.\n\n"
    "When: 350 ms after you stop typing, in insert mode, at a line end.\n"
    "Never while a selection is active, during a macro, in --batch, or "
    "for an excluded path (:ai privacy).\n\n"
    "Enable local AI completions? [y/N]:";

static const char cloud_prompt[] =
    "Cloud model — the same file text and path leave this machine.\n\n"
    "Destination: https://api.anthropic.com (backend \"work\")\n"
    "Credential: read from $ANTHROPIC_API_KEY at request time\n"
    "Transport: the curl program on this system\n\n"
    "Once the text is sent, yew cannot recall it, cannot delete it, and "
    "does not know how long it is kept or who can read it. That is between "
    "you and Anthropic, under their terms — which yew neither controls nor "
    "recites here, because they change and this text does not.\n\n"
    "yew scans every request for secrets first. If it finds one, the "
    "request is BLOCKED and names the matching line. It does not scrub and "
    "send: a scrubber that is 99% right leaks on the hundredth file.\n\n"
    "Type the word 'send' to confirm you want file contents sent to "
    "api.anthropic.com, or press Esc to cancel:";

static void optin_done(Ed *ed, bool accepted, const u8 *text, size_t len,
                       void *ctx);

static void optin_open(YewAiOptin *optin, const char *prompt)
{
    yew_cmdline_open_input(optin->ed, "", optin_done, optin);
    if (optin->ed->cmdline.active)
        yew_msg(optin->ed, YEW_MSG_INFO, "%s", prompt);
}

static void optin_open_scope(YewAiOptin *optin)
{
    yew_cmdline_open_input(optin->ed, "", optin_done, optin);
    if (optin->ed->cmdline.active)
        yew_msg(optin->ed, YEW_MSG_INFO,
                "Enable AI for:\n"
                "  [w] this workspace only  (%s)  <- recommended\n"
                "  [a] every workspace\n"
                "  [o] this session only\n"
                "Choice [w/a/o]:",
                yew_ws_root(optin->ed));
}

static void optin_open_session_fallback(YewAiOptin *optin)
{
    yew_cmdline_open_input(optin->ed, "", optin_done, optin);
    if (optin->ed->cmdline.active)
        yew_msg(optin->ed, YEW_MSG_WARN,
                "AI settings could not be saved.\n"
                "Continue with AI enabled for this session only? [y/N]:");
}

static bool answer_is(const u8 *text, size_t len, const char *expected)
{
    size_t expected_len = strlen(expected);

    return text != NULL && len == expected_len &&
           memcmp(text, expected, expected_len) == 0;
}

const char *yew_ai_optin_no_tty_message(void)
{
    return "AI cannot be enabled non-interactively; set ai.enable in init.fl if you have read :ai privacy";
}

void yew_ai_optin_cancel(YewAiOptin *optin)
{
    if (optin == NULL)
        return;
    if (optin->active && optin->ed != NULL && optin->ed->cmdline.active &&
        optin->ed->cmdline.input_ctx == optin)
        yew_cmdline_close(optin->ed, false);
    optin->active = false;
    optin->phase = 0U;
}

bool yew_ai_optin_begin_checked(YewAiOptin *optin, Ed *ed,
                                YewAiOptinCommit commit, void *ctx,
                                bool has_tty)
{
    if (optin == NULL || ed == NULL || commit == NULL)
        return false;
    (void)memset(optin, 0, sizeof(*optin));
    optin->ed = ed;
    optin->commit = commit;
    optin->ctx = ctx;
    if (!has_tty) {
        yew_msg(ed, YEW_MSG_ERROR, "%s", yew_ai_optin_no_tty_message());
        return false;
    }
    optin->active = true;
    optin->phase = 1U;
    optin_open(optin, choose_prompt);
    if (!ed->cmdline.active || ed->cmdline.input_ctx != optin) {
        optin->active = false;
        optin->phase = 0U;
        return false;
    }
    return true;
}

bool yew_ai_optin_begin(YewAiOptin *optin, Ed *ed,
                        YewAiOptinCommit commit, void *ctx)
{
    bool has_tty = ed != NULL && yew_tty_fd_is_terminal(ed->tty.rfd) &&
                   yew_tty_fd_is_terminal(ed->tty.wfd);

    return yew_ai_optin_begin_checked(optin, ed, commit, ctx, has_tty);
}

static void optin_abort(YewAiOptin *optin)
{
    optin->active = false;
    optin->phase = 0U;
}

static void optin_done(Ed *ed, bool accepted, const u8 *text, size_t len,
                       void *ctx)
{
    YewAiOptin *optin = ctx;

    if (optin == NULL || ed == NULL || !optin->active)
        return;
    if (!accepted) {
        optin_abort(optin);
        return;
    }
    if (optin->phase == 1U) {
        if (answer_is(text, len, "1"))
            optin->backend = YEW_AI_OPTIN_LOCAL;
        else if (answer_is(text, len, "2"))
            optin->backend = YEW_AI_OPTIN_CLOUD;
        else if (answer_is(text, len, "3")) {
            optin_abort(optin);
            if (!yew_ai_privacy_open(ed))
                yew_msg(ed, YEW_MSG_ERROR, "could not open AI privacy page");
            return;
        } else {
            optin_abort(optin);
            return;
        }
        optin->phase = 2U;
        optin_open(optin, optin->backend == YEW_AI_OPTIN_LOCAL ?
                               local_prompt : cloud_prompt);
        return;
    }
    if (optin->phase == 2U) {
        if ((optin->backend == YEW_AI_OPTIN_LOCAL &&
             !answer_is(text, len, "y") && !answer_is(text, len, "Y")) ||
            (optin->backend == YEW_AI_OPTIN_CLOUD &&
             !answer_is(text, len, "send"))) {
            optin_abort(optin);
            return;
        }
        optin->phase = 3U;
        optin_open_scope(optin);
        return;
    }
    if (optin->phase == 3U) {
        if (len != 1U || text == NULL ||
            (text[0] != (u8)'w' && text[0] != (u8)'a' &&
             text[0] != (u8)'o')) {
            optin_abort(optin);
            return;
        }
        optin->scope = (char)text[0];
        if (!optin->commit(ed, optin->backend, optin->scope, optin->ctx)) {
            if (optin->scope == 'o') {
                optin_abort(optin);
                return;
            }
            optin->phase = 4U;
            optin_open_session_fallback(optin);
            return;
        }
        optin_abort(optin);
        return;
    }
    if (optin->phase == 4U) {
        if (answer_is(text, len, "y") || answer_is(text, len, "Y"))
            (void)optin->commit(ed, optin->backend, 'o', optin->ctx);
        optin_abort(optin);
    }
}

static const char local_config[] =
    "# yew AI opt-in (managed by :ai enable)\n"
    "import ai\n"
    "ai.backend(\"local\", {kind: \"ollama\", "
    "url: \"http://127.0.0.1:11434\", "
    "model: \"qwen2.5-coder:7b\", stream: true})\n"
    "set({\"ai.enable\": true, \"ai.backend\": \"local\", "
    "\"shadow.ai_debounce_ms\": 250, \"ai.context_bytes\": 4096, "
    "\"ai.max_tokens\": 256, \"ai.max_lines\": 8, "
    "\"ai.temperature\": 10, \"ai.fim\": \"auto\"})\n";

static const char cloud_config[] =
    "# yew AI opt-in (managed by :ai enable)\n"
    "import ai\n"
    "ai.backend(\"work\", {kind: \"anthropic\", "
    "url: \"https://api.anthropic.com\", "
    "model: \"claude-sonnet-4-6\", transport: \"curl\", "
    "key_env: \"ANTHROPIC_API_KEY\", max_tokens: 256, stream: true})\n"
    "set({\"ai.enable\": true, \"ai.backend\": \"work\", "
    "\"shadow.ai_debounce_ms\": 600, \"ai.context_bytes\": 2048, "
    "\"ai.max_tokens\": 192, \"ai.max_lines\": 4, "
    "\"ai.temperature\": 10, \"ai.on_redact\": \"block\", "
    "\"ai.default_workspace\": \"ask\"})\n";

static const char disable_block[] =
    "# yew AI disable (managed by :ai disable)\n"
    "set({\"ai.enable\": false})\n"
    "# end yew AI disable\n";

static void optin_strip_disable_blocks(Bytebuf *out, const char *old,
                                       size_t old_len)
{
    const char *at = old;
    const char *limit = old + old_len;

    while (at < limit) {
        const char *begin = strstr(at, disable_block);

        if (begin == NULL)
            break;
        bytebuf_append(out, at, (size_t)(begin - at));
        at = begin + sizeof(disable_block) - 1U;
    }
    bytebuf_append(out, at, (size_t)(limit - at));
}

bool yew_ai_optin_config_merge(Bytebuf *out, const char *old,
                               size_t old_len,
                               YewAiOptinBackend backend, bool allow_all)
{
    static const char start[] =
        "# yew AI opt-in (managed by :ai enable)\n";
    static const char finish[] = "# end yew AI opt-in\n";
    static const char allow[] =
        "set({\"ai.default_workspace\": \"allow\"})\n";
    Bytebuf clean;
    const char *source;
    size_t source_len;
    const char *block;
    const char *begin;
    const char *end;

    if (out == NULL || old == NULL || strlen(old) != old_len ||
        (backend != YEW_AI_OPTIN_LOCAL &&
         backend != YEW_AI_OPTIN_CLOUD))
        return false;
    block = backend == YEW_AI_OPTIN_LOCAL ? local_config : cloud_config;
    out->len = 0U;
    bytebuf_init(&clean);
    optin_strip_disable_blocks(&clean, old, old_len);
    bytebuf_push_u8(&clean, 0U);
    source = (const char *)clean.data;
    source_len = clean.len - 1U;
    begin = strstr(source, start);
    end = begin == NULL ? NULL : strstr(begin, finish);
    if (begin != NULL && end != NULL) {
        end += sizeof(finish) - 1U;
        bytebuf_append(out, source, (size_t)(begin - source));
        bytebuf_append(out, block, strlen(block));
        if (allow_all)
            bytebuf_append(out, allow, sizeof(allow) - 1U);
        bytebuf_append(out, finish, sizeof(finish) - 1U);
        bytebuf_append(out, end,
                       source_len - (size_t)(end - source));
    } else {
        bytebuf_append(out, source, source_len);
        if (source_len != 0U && source[source_len - 1U] != '\n')
            bytebuf_push_u8(out, (u8)'\n');
        if (source_len != 0U)
            bytebuf_push_u8(out, (u8)'\n');
        bytebuf_append(out, block, strlen(block));
        if (allow_all)
            bytebuf_append(out, allow, sizeof(allow) - 1U);
        bytebuf_append(out, finish, sizeof(finish) - 1U);
    }
    bytebuf_free(&clean);
    return true;
}

static bool optin_config_disable_merge(Bytebuf *out, const char *old,
                                       size_t old_len)
{
    Bytebuf clean;

    if (out == NULL || old == NULL || strlen(old) != old_len)
        return false;
    out->len = 0U;
    bytebuf_init(&clean);
    optin_strip_disable_blocks(&clean, old, old_len);
    bytebuf_append(out, clean.data, clean.len);
    if (out->len != 0U && out->data[out->len - 1U] != '\n')
        bytebuf_push_u8(out, (u8)'\n');
    bytebuf_append(out, disable_block, sizeof(disable_block) - 1U);
    bytebuf_free(&clean);
    return true;
}

static char *optin_read_file(const char *path, size_t *len)
{
    FILE *file;
    long size;
    char *bytes;

    *len = 0U;
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            bytes = yew_xmalloc(1U);
            bytes[0] = '\0';
            return bytes;
        }
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) < 0L ||
        (unsigned long)size > SIZE_MAX - 1U ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    bytes = yew_xmalloc((size_t)size + 1U);
    if (fread(bytes, 1U, (size_t)size, file) != (size_t)size) {
        free(bytes);
        bytes = NULL;
    } else {
        bytes[(size_t)size] = '\0';
        *len = (size_t)size;
    }
    if (fclose(file) != 0) {
        free(bytes);
        return NULL;
    }
    return bytes;
}

static char *optin_parent(const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t len;
    char *parent;

    if (slash == NULL) {
        parent = yew_xmalloc(2U);
        parent[0] = '.';
        parent[1] = '\0';
        return parent;
    }
    len = slash == path ? 1U : (size_t)(slash - path);
    parent = yew_xmalloc(len + 1U);
    (void)memcpy(parent, path, len);
    parent[len] = '\0';
    return parent;
}

typedef struct OptinConfigBackup {
    const char *path;
    char *old;
    size_t old_len;
    bool existed;
    bool written;
} OptinConfigBackup;

static void optin_config_backup_free(OptinConfigBackup *backup)
{
    if (backup == NULL)
        return;
    free(backup->old);
    (void)memset(backup, 0, sizeof(*backup));
}

static bool optin_config_rollback(OptinConfigBackup *backup)
{
    char *parent;
    bool ok;

    if (backup == NULL || !backup->written || backup->path == NULL)
        return true;
    if (!backup->existed)
        return unlink(backup->path) == 0 || errno == ENOENT;
    parent = optin_parent(backup->path);
    ok = yew_mkdirs(parent, 0700U) &&
         yew_file_write_atomic(backup->path, (const u8 *)backup->old,
                               backup->old_len, 0600) == YEW_SAVE_OK;
    free(parent);
    return ok;
}

static bool optin_write_config(Ed *ed, YewAiOptinBackend backend,
                               bool allow_all, OptinConfigBackup *backup)
{
    const char *path = yew_config_user_path(ed);
    char *parent;
    char *old;
    size_t old_len;
    Bytebuf next;
    bool ok;

    if (path == NULL || backup == NULL)
        return false;
    (void)memset(backup, 0, sizeof(*backup));
    backup->path = path;
    backup->existed = access(path, F_OK) == 0;
    old = optin_read_file(path, &old_len);
    if (old == NULL)
        return false;
    bytebuf_init(&next);
    if (!yew_ai_optin_config_merge(&next, old, old_len, backend,
                                    allow_all)) {
        free(old);
        bytebuf_free(&next);
        return false;
    }
    parent = optin_parent(path);
    ok = yew_mkdirs(parent, 0700U) &&
         yew_file_write_atomic(path, next.data, next.len, 0600) ==
             YEW_SAVE_OK;
    free(parent);
    bytebuf_free(&next);
    if (!ok) {
        free(old);
        return false;
    }
    backup->old = old;
    backup->old_len = old_len;
    backup->written = true;
    return ok;
}

static bool optin_write_disabled_config(Ed *ed)
{
    const char *path = yew_config_user_path(ed);
    char *parent;
    char *old;
    size_t old_len;
    Bytebuf next;
    bool ok;

    if (path == NULL)
        return false;
    old = optin_read_file(path, &old_len);
    if (old == NULL)
        return false;
    bytebuf_init(&next);
    if (!optin_config_disable_merge(&next, old, old_len)) {
        free(old);
        bytebuf_free(&next);
        return false;
    }
    free(old);
    parent = optin_parent(path);
    ok = yew_mkdirs(parent, 0700U) &&
         yew_file_write_atomic(path, next.data, next.len, 0600) ==
             YEW_SAVE_OK;
    free(parent);
    bytebuf_free(&next);
    return ok;
}

static bool optin_set_value(Ed *ed, const char *name, const OptVal *value,
                            const char **error)
{
    CmdCtx cx = {0};

    cx.ed = ed;
    cx.sarg = name;
    cx.sarg_len = (u32)strlen(name);
    cx.opt_in = value;
    cx.source = YEW_SRC_CMDLINE;
    if (yew_opt_cmd_set(&cx) == YEW_CMD_OK)
        return true;
    if (error != NULL)
        *error = cx.opt_error_msg;
    return false;
}

static bool optin_set_enum(Ed *ed, const char *name, const char *text)
{
    OptVal value = {YEW_OPT_STR,
                    {.str = {text, (u32)strlen(text)}}};

    return optin_set_value(ed, name, &value, NULL);
}

static bool optin_commit(Ed *ed, YewAiOptinBackend backend, char scope,
                         void *ctx)
{
    const char *preset = backend == YEW_AI_OPTIN_LOCAL ? "local" : "cloud";
    OptinConfigBackup backup = {0};
    AiWsGrant old_grant;
    char *state_dir;
    char *trust_path = NULL;
    size_t trust_len;

    (void)ctx;
    if (scope == 'o') {
        if (!yew_ai_preset_load(ed, preset)) {
            yew_msg(ed, YEW_MSG_ERROR, "could not load the %s AI preset",
                    preset);
            return false;
        }
        yew_ai_workspace_session_set(ed, YEW_AI_WS_ALLOW);
        yew_msg(ed, YEW_MSG_INFO,
                "AI enabled for this session only.\n  wrote no files\n"
                "  undo: :ai disable");
        return true;
    }
    if (!optin_write_config(ed, backend, scope == 'a', &backup)) {
        yew_msg(ed, YEW_MSG_ERROR,
                "could not write %s", yew_config_user_path(ed));
        return false;
    }
    old_grant = yew_config_ai_workspace_grant(ed);
    if (scope == 'w' &&
        !yew_config_ai_workspace_set(ed, YEW_AI_WS_ALLOW)) {
        if (old_grant == YEW_AI_WS_UNSET)
            (void)yew_config_ai_workspace_forget(ed);
        else
            (void)yew_config_ai_workspace_set(ed, old_grant);
        if (!optin_config_rollback(&backup))
            yew_msg(ed, YEW_MSG_ERROR,
                    "workspace grant failed and %s could not be restored",
                    backup.path);
        else
            yew_msg(ed, YEW_MSG_ERROR,
                    "could not write the workspace AI grant");
        optin_config_backup_free(&backup);
        return false;
    }
    if (!yew_ai_preset_load(ed, preset)) {
        if (scope == 'w') {
            if (old_grant == YEW_AI_WS_UNSET)
                (void)yew_config_ai_workspace_forget(ed);
            else
                (void)yew_config_ai_workspace_set(ed, old_grant);
        }
        (void)optin_config_rollback(&backup);
        optin_config_backup_free(&backup);
        yew_msg(ed, YEW_MSG_ERROR, "could not load the %s AI preset", preset);
        return false;
    }
    if (scope == 'a') {
        if (!optin_set_enum(ed, "ai.default_workspace", "allow")) {
            (void)optin_config_rollback(&backup);
            optin_config_backup_free(&backup);
            return false;
        }
        yew_ai_workspace_session_set(ed, YEW_AI_WS_ALLOW);
        yew_msg(ed, YEW_MSG_INFO,
                "AI enabled.\n  wrote %s "
                "(ai.backend, ai.enable, every workspace)\n"
                "  undo: :ai disable", backup.path);
        optin_config_backup_free(&backup);
        return true;
    }
    yew_ai_workspace_session_set(ed, YEW_AI_WS_UNSET);
    state_dir = yew_xdg_state_dir();
    if (state_dir != NULL) {
        trust_len = strlen(state_dir) + sizeof("/trust.fl");
        trust_path = yew_xmalloc(trust_len);
        (void)snprintf(trust_path, trust_len, "%s/trust.fl", state_dir);
    }
    yew_msg(ed, YEW_MSG_INFO,
            "AI enabled.\n  wrote %s "
            "(ai.backend, ai.enable)\n"
            "  wrote %s "
            "(ai: allow for this workspace)\n"
            "  undo: :ai disable; :ai forget", backup.path,
            trust_path == NULL ? "trust.fl" : trust_path);
    free(trust_path);
    free(state_dir);
    optin_config_backup_free(&backup);
    return true;
}

bool yew_ai_optin_begin_default_checked(YewAiOptin *optin, Ed *ed,
                                        bool has_tty)
{
    return yew_ai_optin_begin_checked(optin, ed, optin_commit, NULL,
                                      has_tty);
}

CmdStatus yew_ai_cmd_enable(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL || cx->ed->ai == NULL)
        return YEW_CMD_ERR_STATE;
    if (cx->ed->ai->optin.active) {
        yew_msg(cx->ed, YEW_MSG_INFO, "AI enable disclosure is already open");
        return YEW_CMD_ERR_STATE;
    }
    return yew_ai_optin_begin(&cx->ed->ai->optin, cx->ed,
                              optin_commit, NULL) ? YEW_CMD_OK :
                                                   YEW_CMD_ERR_STATE;
}

CmdStatus yew_ai_cmd_disable(CmdCtx *cx)
{
    OptVal disabled = {YEW_OPT_BOOL, {.b = false}};
    const char *error = NULL;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (!optin_write_disabled_config(cx->ed)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "could not persist ai.enable=false");
        return YEW_CMD_ERR_IO;
    }
    if (!optin_set_value(cx->ed, "ai.enable", &disabled, &error)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "%s",
                error == NULL ? "could not disable AI" : error);
        return YEW_CMD_ERR_STATE;
    }
    yew_msg(cx->ed, YEW_MSG_INFO,
            "AI disabled; backend definitions and workspace grants remain");
    return YEW_CMD_OK;
}

CmdStatus yew_ai_cmd_forget(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    yew_ai_workspace_session_set(cx->ed, YEW_AI_WS_UNSET);
    if (!yew_config_ai_workspace_forget(cx->ed)) {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "could not remove the persistent AI workspace grant");
        return YEW_CMD_ERR_IO;
    }
    yew_msg(cx->ed, YEW_MSG_INFO, "AI workspace grant removed");
    return YEW_CMD_OK;
}

CmdStatus yew_ai_cmd_privacy(CmdCtx *cx)
{
    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    if (!yew_ai_privacy_open(cx->ed)) {
        yew_msg(cx->ed, YEW_MSG_ERROR, "could not open AI privacy page");
        return YEW_CMD_ERR_IO;
    }
    return YEW_CMD_OK;
}

CmdStatus yew_ai_cmd_preset(CmdCtx *cx)
{
    char name[16];

    if (cx == NULL || cx->ed == NULL || cx->sarg == NULL ||
        cx->sarg_len == 0U || cx->sarg_len >= sizeof(name))
        return YEW_CMD_ERR_ARG;
    (void)memcpy(name, cx->sarg, cx->sarg_len);
    name[cx->sarg_len] = '\0';
    if (!yew_ai_preset_load(cx->ed, name)) {
        yew_msg(cx->ed, YEW_MSG_ERROR,
                "unknown or invalid AI preset '%s' (use local or cloud)",
                name);
        return YEW_CMD_ERR_ARG;
    }
    yew_msg(cx->ed, YEW_MSG_INFO,
            "AI preset '%s' loaded for this session", name);
    return YEW_CMD_OK;
}

CmdStatus yew_ai_cmd_status(CmdCtx *cx)
{
    OptVal enabled;
    const AiBackendEntry *backend;
    const char *excluded;
    AiWsGrant grant;

    if (cx == NULL || cx->ed == NULL)
        return YEW_CMD_ERR_STATE;
    (void)yew_opt_get(cx->ed, NULL, NULL, "ai.enable", 9U, &enabled);
    backend = yew_ai_backend_selected(cx->ed);
    grant = yew_ai_workspace_grant(cx->ed);
    excluded = cx->ed->win == NULL || cx->ed->win->buf == NULL ? NULL :
        yew_ai_path_exclusion(cx->ed, cx->ed->win->buf->path);
    if (excluded != NULL) {
        yew_msg(cx->ed, YEW_MSG_INFO,
                "AI: excluded (path matches '%s')", excluded);
        return YEW_CMD_OK;
    }
    yew_msg(cx->ed, YEW_MSG_INFO, "AI: %s; backend=%s; workspace=%s",
            enabled.type == (u8)YEW_OPT_BOOL && enabled.as.b ? "enabled" :
                                                                    "disabled",
            backend == NULL ? "none" : backend->backend.name,
            grant == YEW_AI_WS_ALLOW ? "allow" :
            grant == YEW_AI_WS_DENY ? "deny" : "ask");
    return YEW_CMD_OK;
}
