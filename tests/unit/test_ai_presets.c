#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/key.h"
#include "mod/ai/preset.h"
#include "mod/ai/registry.h"
#include "text/piece.h"

static char *preset_source(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;
    char *source;

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_END), 0);
    size = ftell(file);
    YEW_ASSERT(size >= 0L);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_SET), 0);
    source = yew_xmalloc((size_t)size + 1U);
    YEW_ASSERT_EQ_U64(fread(source, 1U, (size_t)size, file), (u64)size);
    source[(size_t)size] = '\0';
    YEW_ASSERT_EQ_I64(fclose(file), 0);
    return source;
}

static void preset_write_source(const char *path, const char *source)
{
    FILE *file = fopen(path, "wb");
    size_t len = strlen(source);

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_U64(fwrite(source, 1U, len, file), len);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static const OptDesc *preset_option_at(const char *line, const char *end,
                                       const char **after)
{
    const char *name;
    const char *name_end;

    while (line < end && (*line == ' ' || *line == '\t'))
        line++;
    if (line == end || *line == '#' || *line == '}')
        return NULL;
    YEW_ASSERT(*line == '"');
    name = ++line;
    name_end = memchr(name, '"', (size_t)(end - name));
    YEW_ASSERT_NOT_NULL(name_end);
    line = name_end + 1;
    while (line < end && (*line == ' ' || *line == '\t'))
        line++;
    YEW_ASSERT(line < end && *line == ':');
    if (after != NULL)
        *after = line + 1;
    return yew_opt_desc(name, (u32)(name_end - name));
}

static bool preset_enum_contains(const OptDesc *desc, const OptVal *value)
{
    u32 i;

    for (i = 0U; desc->enums != NULL && desc->enums[i] != NULL; i++) {
        if (strlen(desc->enums[i]) == (size_t)value->as.str.len &&
            memcmp(desc->enums[i], value->as.str.s,
                   value->as.str.len) == 0)
            return true;
    }
    return false;
}

static void preset_assert_option_value(Ed *ed, const OptDesc *desc)
{
    OptVal value;
    const char *error = NULL;

    YEW_ASSERT_NOT_NULL(desc);
    YEW_ASSERT(yew_opt_get(ed, NULL, NULL, desc->name,
                           (u32)strlen(desc->name), &value));
    YEW_ASSERT_EQ_U64(value.type, desc->type);
    YEW_ASSERT(yew_opt_validate(ed, desc->scope, desc->name,
                                (u32)strlen(desc->name), &value, &error));
    YEW_ASSERT_NULL(error);
    if (desc->type == (u8)YEW_OPT_INT) {
        YEW_ASSERT(value.as.i >= desc->imin);
        YEW_ASSERT(value.as.i <= desc->imax);
    } else if (desc->type == (u8)YEW_OPT_ENUM) {
        YEW_ASSERT(preset_enum_contains(desc, &value));
    }
}

static const OptDesc *preset_next_option(const char *line, const char *end)
{
    while (line < end) {
        const char *line_end = memchr(line, '\n', (size_t)(end - line));
        const OptDesc *desc;

        if (line_end == NULL)
            line_end = end;
        desc = preset_option_at(line, line_end, NULL);
        if (desc != NULL)
            return desc;
        line = line_end < end ? line_end + 1 : end;
    }
    return NULL;
}

static bool preset_default_integer(const char *comment, const char *word,
                                   i64 *value)
{
    const char *p = word;
    const char *digits;
    char *parsed_end;

    while (p > comment && (p[-1] < '0' || p[-1] > '9'))
        p--;
    if (p == comment)
        return false;
    digits = p - 1;
    while (digits > comment && digits[-1] >= '0' && digits[-1] <= '9')
        digits--;
    if (digits > comment && digits[-1] == '-')
        digits--;
    *value = (i64)strtoll(digits, &parsed_end, 10);
    return parsed_end == p;
}

static u32 preset_assert_source_options(Ed *ed, const char *source,
                                        u32 *default_count)
{
    const char *body = strstr(source, "set({");
    const char *end;
    const char *line;
    u32 option_count = 0U;

    YEW_ASSERT_NOT_NULL(body);
    body += strlen("set({");
    end = strstr(body, "})");
    YEW_ASSERT_NOT_NULL(end);
    line = body;
    while (line < end) {
        const char *line_end = memchr(line, '\n', (size_t)(end - line));
        const char *comment;
        const char *default_word;
        const OptDesc *desc;

        if (line_end == NULL)
            line_end = end;
        desc = preset_option_at(line, line_end, NULL);
        if (desc != NULL) {
            preset_assert_option_value(ed, desc);
            option_count++;
        }
        comment = memchr(line, '#', (size_t)(line_end - line));
        default_word = comment == NULL ? NULL : strstr(comment, "default");
        if (default_word != NULL && default_word < line_end) {
            i64 claimed;

            if (desc == NULL)
                desc = preset_next_option(line_end, end);
            YEW_ASSERT_NOT_NULL(desc);
            YEW_ASSERT_EQ_U64(desc->type, YEW_OPT_INT);
            YEW_ASSERT(preset_default_integer(comment, default_word,
                                              &claimed));
            YEW_ASSERT_EQ_I64(claimed, desc->dflt.as.i);
            (*default_count)++;
        }
        line = line_end < end ? line_end + 1 : end;
    }
    YEW_ASSERT(option_count > 0U);
    return option_count;
}

static const char *preset_selected(Ed *ed)
{
    OptVal value;

    YEW_ASSERT(yew_opt_get(ed, NULL, NULL, "ai.backend", 10U, &value));
    YEW_ASSERT_EQ_U64(value.type, YEW_OPT_STR);
    return value.as.str.s;
}

static void preset_assert_load(Ed *ed, const char *name)
{
    const char *diag;

    if (yew_ai_preset_load(ed, name))
        return;
    diag = fl_runtime_last_diag(ed->fl, NULL);
    yew_test_fail(__FILE__, __LINE__,
                  diag == NULL ? "AI preset failed without a diagnostic" :
                                 diag);
}

static void preset_assert_annotations(const char *local, const char *cloud)
{
    YEW_ASSERT_NOT_NULL(strstr(local, "# Model sizing, honestly:"));
    YEW_ASSERT_NOT_NULL(strstr(local, "~60-120 tok/s"));
    YEW_ASSERT_NOT_NULL(strstr(local, "350 ms default"));
    YEW_ASSERT_NOT_NULL(strstr(local, "\"ai.fim\":           \"auto\""));
    YEW_ASSERT_NOT_NULL(strstr(cloud, "key_cmd: [\"pass\", \"show\""));
    YEW_ASSERT_NOT_NULL(strstr(cloud, "find-generic-password"));
    YEW_ASSERT_NOT_NULL(strstr(cloud, "# An OpenAI-compatible endpoint"));
    YEW_ASSERT_NOT_NULL(strstr(cloud, "gpt-4.1-mini"));
    YEW_ASSERT_NOT_NULL(strstr(cloud, "# BLOCK, not elide."));
}

void test_ai_presets_parse_execute_and_replace_selection(void)
{
    Ed ed;
    const AiBackendEntry *selected;
    const char *old_key = getenv("ANTHROPIC_API_KEY");
    char *saved_key = NULL;
    char secret[64];
    AiErr error;
    bool key_ok;
    int restore_rc;
    const char *old_runtime = getenv("YEW_RUNTIME_DIR");
    char *saved_runtime = old_runtime == NULL ? NULL : strdup(old_runtime);
    char bad_root[] = "/tmp/yew-ai-preset-XXXXXX";
    char bad_local[sizeof(bad_root) + sizeof("/preset.ai-local.fl")];
    OptVal value;
    u32 default_count = 0U;
    char *local_source = preset_source("runtime/preset.ai-local.fl");
    char *cloud_source = preset_source("runtime/preset.ai-cloud.fl");

    if (old_key != NULL) {
        saved_key = yew_xmalloc(strlen(old_key) + 1U);
        (void)strcpy(saved_key, old_key);
    }

    preset_assert_annotations(local_source, cloud_source);
    yew_ed_init(&ed);
    preset_assert_load(&ed, "local");
    YEW_ASSERT_NULL(fl_runtime_last_diag(ed.fl, NULL));
    YEW_ASSERT(preset_assert_source_options(&ed, local_source,
                                            &default_count) >= 1U);
    YEW_ASSERT_EQ_STR(preset_selected(&ed), "local");
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    selected = yew_ai_backend_selected(&ed);
    YEW_ASSERT_NOT_NULL(selected);
    YEW_ASSERT(selected->backend.url.loopback);
    YEW_ASSERT_NOT_NULL(yew_ai_registry_find(&ed.ai->backends, "local"));
    preset_assert_load(&ed, "cloud");
    YEW_ASSERT_NULL(fl_runtime_last_diag(ed.fl, NULL));
    YEW_ASSERT(preset_assert_source_options(&ed, cloud_source,
                                            &default_count) >= 1U);
    YEW_ASSERT(default_count > 0U);
    YEW_ASSERT_EQ_STR(preset_selected(&ed), "work");
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    selected = yew_ai_backend_selected(&ed);
    YEW_ASSERT_NOT_NULL(selected);
    YEW_ASSERT(!selected->backend.url.loopback);
    YEW_ASSERT_NULL(yew_ai_registry_find(&ed.ai->backends, "local"));
    YEW_ASSERT_NOT_NULL(yew_ai_registry_find(&ed.ai->backends, "work"));

    YEW_ASSERT_NOT_NULL(mkdtemp(bad_root));
    (void)snprintf(bad_local, sizeof(bad_local),
                   "%s/preset.ai-local.fl", bad_root);
    preset_write_source(
        bad_local,
        "import ai\n"
        "ai.backend(\"local\", {kind: \"ollama\", "
        "url: \"http://127.0.0.1:11434\", model: \"bad\"})\n"
        "set({\"ai.enable\": false, \"ai.backend\": \"local\", "
        "\"ai.context_bytes\": 1024})\n"
        "error(\"preset failed after mutation\")\n");
    YEW_ASSERT_EQ_I64(setenv("YEW_RUNTIME_DIR", bad_root, 1), 0);
    YEW_ASSERT(!yew_ai_preset_load(&ed, "local"));
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    YEW_ASSERT_NOT_NULL(yew_ai_registry_find(&ed.ai->backends, "work"));
    YEW_ASSERT_NULL(yew_ai_registry_find(&ed.ai->backends, "local"));
    YEW_ASSERT_EQ_STR(preset_selected(&ed), "work");
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "ai.enable", 9U, &value));
    YEW_ASSERT(value.as.b);
    YEW_ASSERT(yew_opt_get(&ed, NULL, NULL, "ai.context_bytes", 16U,
                           &value));
    YEW_ASSERT_EQ_I64(value.as.i, 2048);
    restore_rc = saved_runtime == NULL ? unsetenv("YEW_RUNTIME_DIR") :
                 setenv("YEW_RUNTIME_DIR", saved_runtime, 1);
    free(saved_runtime);
    YEW_ASSERT_EQ_I64(restore_rc, 0);
    YEW_ASSERT_EQ_I64(unlink(bad_local), 0);
    YEW_ASSERT_EQ_I64(rmdir(bad_root), 0);

    YEW_ASSERT_EQ_I64(unsetenv("ANTHROPIC_API_KEY"), 0);
    key_ok = yew_ai_key_get(&ed, &selected->backend, secret,
                            sizeof(secret), &error);
    restore_rc = saved_key == NULL ? unsetenv("ANTHROPIC_API_KEY") :
                                    setenv("ANTHROPIC_API_KEY", saved_key, 1);
    free(saved_key);
    YEW_ASSERT_EQ_I64(restore_rc, 0);
    YEW_ASSERT(!key_ok);
    YEW_ASSERT_EQ_U64(error.kind, YEW_AI_ERR_AUTH);
    YEW_ASSERT_NOT_NULL(strstr(error.msg, "$ANTHROPIC_API_KEY"));
    YEW_ASSERT_EQ_STR(preset_selected(&ed), "work");
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    YEW_ASSERT(!yew_ai_preset_load(&ed, "unknown"));
    yew_ed_free(&ed);
    free(cloud_source);
    free(local_source);
}

void test_ai_privacy_page_opens_readonly_scratch(void)
{
    Ed ed;
    u64 len;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, NULL, 0U, "privacy-origin"));
    YEW_ASSERT(yew_ai_privacy_open(&ed));
    YEW_ASSERT_EQ_STR(yew_buf_label(ed.win->buf), "[AI Privacy]");
    YEW_ASSERT(yew_buf_readonly(ed.win->buf));
    YEW_ASSERT(yew_textbuf_len(ed.win->buf->tb) > 1000U);
    len = yew_textbuf_len(ed.win->buf->tb);
    YEW_ASSERT(yew_ai_privacy_open(&ed));
    YEW_ASSERT_EQ_U64(yew_textbuf_len(ed.win->buf->tb), len);
    yew_ed_free(&ed);
}
