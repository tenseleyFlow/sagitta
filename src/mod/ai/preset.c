#define _POSIX_C_SOURCE 200809L

#include "mod/ai/preset.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/registry.h"
#include "text/piece.h"
#include "text/undo.h"

#ifndef YEW_RUNTIME_DIR_DEFAULT
#define YEW_RUNTIME_DIR_DEFAULT "/usr/local/share/yew/runtime"
#endif

static char *ai_read_file(const char *path, u32 *len)
{
    FILE *file;
    long size;
    char *bytes;

    if (path == NULL || len == NULL || (file = fopen(path, "rb")) == NULL)
        return NULL;
    if (fseek(file, 0L, SEEK_END) != 0 ||
        (size = ftell(file)) < 0L || (unsigned long)size > UINT32_MAX ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    bytes = yew_xmalloc((size_t)size + 1U);
    if (fread(bytes, 1U, (size_t)size, file) != (size_t)size) {
        yew_xfree(bytes);
        bytes = NULL;
    } else {
        bytes[(size_t)size] = '\0';
        *len = (u32)size;
    }
    if (fclose(file) != 0) {
        yew_xfree(bytes);
        return NULL;
    }
    return bytes;
}

static char *ai_join_path(const char *root, const char *name)
{
    size_t root_len = strlen(root);
    size_t name_len = strlen(name);
    char *path = yew_xmalloc(root_len + name_len + 2U);

    (void)snprintf(path, root_len + name_len + 2U, "%s/%s", root, name);
    return path;
}

static char *ai_runtime_path(const char *name)
{
    const char *root = getenv("YEW_RUNTIME_DIR");
    char *path;

    if (root != NULL && root[0] != '\0')
        return ai_join_path(root, name);
    path = ai_join_path("runtime", name);
    if (access(path, R_OK) == 0)
        return path;
    yew_xfree(path);
    return ai_join_path(YEW_RUNTIME_DIR_DEFAULT, name);
}

static char *ai_privacy_path(void)
{
    const char *runtime = getenv("YEW_RUNTIME_DIR");
    char *path = ai_join_path("docs", "ai-privacy.md");
    size_t len;

    if (access(path, R_OK) == 0)
        return path;
    yew_xfree(path);
    if (runtime == NULL || runtime[0] == '\0')
        runtime = YEW_RUNTIME_DIR_DEFAULT;
    len = strlen(runtime) + sizeof("/../docs/ai-privacy.md");
    path = yew_xmalloc(len);
    (void)snprintf(path, len, "%s/../docs/ai-privacy.md", runtime);
    return path;
}

typedef struct AiPresetTxn {
    AiBackendRegistry backends;
    u32 checkpoints[10];
    u32 ncheckpoints;
    bool registry_saved;
} AiPresetTxn;

static const char *const ai_preset_options[] = {
    "ai.enable",
    "ai.backend",
    "shadow.ai_debounce_ms",
    "ai.context_bytes",
    "ai.max_tokens",
    "ai.max_lines",
    "ai.temperature",
    "ai.fim",
    "ai.on_redact",
    "ai.default_workspace"
};

_Static_assert(YEW_ARRAY_LEN(ai_preset_options) ==
                   YEW_ARRAY_LEN(((AiPresetTxn *)0)->checkpoints),
               "preset transaction checkpoint table mismatch");

static bool ai_preset_txn_begin(Ed *ed, AiPresetTxn *txn)
{
    u32 i;

    (void)memset(txn, 0, sizeof(*txn));
    for (i = 0U; i < YEW_ARRAY_LEN(ai_preset_options); i++) {
        const char *error = NULL;
        const char *name = ai_preset_options[i];
        u32 checkpoint = yew_opt_checkpoint(
            ed, name, (u32)strlen(name), &error);

        (void)error;
        if (checkpoint == 0U) {
            while (txn->ncheckpoints != 0U)
                yew_opt_discard(ed,
                                txn->checkpoints[--txn->ncheckpoints]);
            return false;
        }
        txn->checkpoints[txn->ncheckpoints++] = checkpoint;
    }
    txn->backends = ed->ai->backends;
    yew_ai_registry_init(&ed->ai->backends, txn->backends.prepare,
                         txn->backends.release,
                         txn->backends.prepare_ctx);
    txn->registry_saved = true;
    return true;
}

static void ai_preset_txn_finish(Ed *ed, AiPresetTxn *txn, bool commit)
{
    if (txn->registry_saved) {
        if (commit) {
            yew_ai_registry_drop(&txn->backends);
        } else {
            yew_ai_registry_drop(&ed->ai->backends);
            ed->ai->backends = txn->backends;
        }
        txn->registry_saved = false;
    }
    while (txn->ncheckpoints != 0U) {
        u32 checkpoint = txn->checkpoints[--txn->ncheckpoints];

        if (commit)
            yew_opt_discard(ed, checkpoint);
        else if (!yew_opt_rollback(ed, checkpoint))
            YEW_BUG("AI preset option rollback failed");
    }
}

bool yew_ai_preset_load(Ed *ed, const char *name)
{
    const char *file;
    char *path;
    char *source;
    u32 len = 0U;
    CmdStatus status;
    AiPresetTxn txn;
    bool ok;

    if (ed == NULL || name == NULL)
        return false;
    if (strcmp(name, "local") == 0)
        file = "preset.ai-local.fl";
    else if (strcmp(name, "cloud") == 0)
        file = "preset.ai-cloud.fl";
    else
        return false;
    path = ai_runtime_path(file);
    source = ai_read_file(path, &len);
    yew_xfree(path);
    if (source == NULL)
        return false;
    if (ed->ai == NULL || !ai_preset_txn_begin(ed, &txn)) {
        yew_xfree(source);
        return false;
    }
    status = yew_fl_eval(ed, source, len);
    yew_xfree(source);
    ok = status == YEW_CMD_OK &&
         yew_ai_registry_keep(&ed->ai->backends,
                              strcmp(name, "local") == 0 ? "local" :
                                                             "work");
    ai_preset_txn_finish(ed, &txn, ok);
    return ok;
}

bool yew_ai_privacy_open(Ed *ed)
{
    static const char name[] = "[AI Privacy]";
    char *path;
    char *source;
    u32 len = 0U;
    Buffer *buffer;

    if (ed == NULL || !ed->model_ready)
        return false;
    path = ai_privacy_path();
    source = ai_read_file(path, &len);
    yew_xfree(path);
    if (source == NULL)
        return false;
    buffer = yew_ws_scratch_find(ed, name);
    if (buffer == NULL)
        buffer = yew_ws_scratch_new(ed, name,
                                    YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (buffer == NULL) {
        yew_xfree(source);
        return false;
    }
    yew_textbuf_delete(buffer->tb,
                       (Span){0U, yew_textbuf_len(buffer->tb)});
    yew_textbuf_insert(buffer->tb, BYTEOFF(0U), (const u8 *)source, len);
    yew_undo_mark_saved(buffer->undo);
    yew_xfree(source);
    return yew_ed_show_buffer(ed, buffer);
}
