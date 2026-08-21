#define _POSIX_C_SOURCE 200809L

#include "mod/ai/preset.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/buf.h"
#include "edit/ed.h"
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
        free(bytes);
        bytes = NULL;
    } else {
        bytes[(size_t)size] = '\0';
        *len = (u32)size;
    }
    if (fclose(file) != 0) {
        free(bytes);
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
    free(path);
    return ai_join_path(YEW_RUNTIME_DIR_DEFAULT, name);
}

static char *ai_privacy_path(void)
{
    const char *runtime = getenv("YEW_RUNTIME_DIR");
    char *path = ai_join_path("docs", "ai-privacy.md");
    size_t len;

    if (access(path, R_OK) == 0)
        return path;
    free(path);
    if (runtime == NULL || runtime[0] == '\0')
        runtime = YEW_RUNTIME_DIR_DEFAULT;
    len = strlen(runtime) + sizeof("/../docs/ai-privacy.md");
    path = yew_xmalloc(len);
    (void)snprintf(path, len, "%s/../docs/ai-privacy.md", runtime);
    return path;
}

bool yew_ai_preset_load(Ed *ed, const char *name)
{
    const char *file;
    char *path;
    char *source;
    u32 len = 0U;
    CmdStatus status;

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
    free(path);
    if (source == NULL)
        return false;
    status = yew_fl_eval(ed, source, len);
    free(source);
    if (status != YEW_CMD_OK || ed->ai == NULL)
        return false;
    return yew_ai_registry_keep(&ed->ai->backends,
                                strcmp(name, "local") == 0 ? "local" :
                                                               "work");
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
    free(path);
    if (source == NULL)
        return false;
    buffer = yew_ws_scratch_find(ed, name);
    if (buffer == NULL)
        buffer = yew_ws_scratch_new(ed, name,
                                    YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (buffer == NULL) {
        free(source);
        return false;
    }
    yew_textbuf_delete(buffer->tb,
                       (Span){0U, yew_textbuf_len(buffer->tb)});
    yew_textbuf_insert(buffer->tb, BYTEOFF(0U), (const u8 *)source, len);
    yew_undo_mark_saved(buffer->undo);
    free(source);
    return yew_ed_show_buffer(ed, buffer);
}
