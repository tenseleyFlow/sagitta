#include "mod/ai/prompt.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "util/buf.h"

static const char chat_system[] =
    "You are a code completion engine embedded in a text editor.\n"
    "You are given the text before the cursor and the text after it.\n"
    "Reply with ONLY the code that belongs at the cursor.\n"
    "\n"
    "Rules:\n"
    "- Output raw code. No prose, no explanation, no apology.\n"
    "- No Markdown. No ``` fences. No language tag.\n"
    "- Do not repeat any of the text before the cursor.\n"
    "- Do not repeat any of the text after the cursor.\n"
    "- Continue the file's existing indentation, brace style, and naming.\n"
    "- If nothing sensible belongs at the cursor, reply with an empty message.\n"
    "- Reply with at most 8 lines.\n";

static bool prefix_ci(const char *value, const char *prefix)
{
    size_t i;

    if (value == NULL)
        return false;
    for (i = 0U; prefix[i] != '\0'; i++) {
        unsigned char left = (unsigned char)value[i];
        unsigned char right = (unsigned char)prefix[i];

        if (left == '\0' || tolower(left) != tolower(right))
            return false;
    }
    return true;
}

AiTemplate yew_ai_template_of(const AiBackend *backend)
{
    static const char *const families[] = {
        "qwen2.5-coder", "qwen3-coder", "codellama", "codegemma",
        "starcoder2", "deepseek-coder", "codestral"
    };
    u32 i;

    if (backend == NULL || backend->kind != (u8)YEW_AI_OLLAMA)
        return YEW_AI_TPL_CHAT;
    if (backend->fim_policy == (u8)YEW_AI_FIM_OFF)
        return YEW_AI_TPL_CHAT;
    if (backend->fim || backend->fim_policy == (u8)YEW_AI_FIM_ON)
        return YEW_AI_TPL_FIM;
    for (i = 0U; i < YEW_ARRAY_LEN(families); i++)
        if (prefix_ci(backend->model, families[i]))
            return YEW_AI_TPL_FIM;
    return YEW_AI_TPL_CHAT;
}

const char *yew_ai_chat_system_prompt(void)
{
    return chat_system;
}

static void json_text(JsonW *writer, const u8 *bytes, u32 len)
{
    yew_jsonw_str(writer, bytes, len);
}

static void json_stop(JsonW *writer, const char *const *stops, u32 nstops)
{
    u32 i;

    yew_jsonw_arr(writer);
    for (i = 0U; i < nstops; i++)
        yew_jsonw_cstr(writer, stops[i]);
    yew_jsonw_arr_end(writer);
}

static void json_temperature(JsonW *writer, double temperature)
{
    char number[32];
    size_t len;

    (void)snprintf(number, sizeof(number), "%.3f", temperature);
    len = strlen(number);
    while (len > 2U && number[len - 1U] == '0')
        len--;
    yew_jsonw_raw(writer, (const u8 *)number, (u32)len);
}

static void chat_user(Bytebuf *out, const AiCtx *context)
{
    if (context->path != NULL && context->path[0] != '\0')
        bytebuf_printf(out, "File: %s\n", context->path);
    if (context->lang != NULL && context->lang[0] != '\0')
        bytebuf_printf(out, "Language: %s\n", context->lang);
    if (out->len != 0U)
        bytebuf_push_u8(out, (u8)'\n');
    bytebuf_append(out, "<|before|>\n", sizeof("<|before|>\n") - 1U);
    if (context->truncated_head)
        bytebuf_append(out, "[… earlier in the file elided …]\n",
                       sizeof("[… earlier in the file elided …]\n") - 1U);
    bytebuf_append(out, context->prefix, context->plen);
    bytebuf_append(out, "\n<|cursor|>\n", sizeof("\n<|cursor|>\n") - 1U);
    bytebuf_append(out, context->suffix, context->slen);
    bytebuf_append(out, "\n<|after|>", sizeof("\n<|after|>") - 1U);
}

static void json_message(JsonW *writer, const char *role,
                         const u8 *content, u32 len)
{
    yew_jsonw_obj(writer);
    yew_jsonw_key(writer, "role");
    yew_jsonw_cstr(writer, role);
    yew_jsonw_key(writer, "content");
    json_text(writer, content, len);
    yew_jsonw_obj_end(writer);
}

/* Never hand-write FIM sentinels.  Ollama's Modelfile template inserts the
 * family-specific sentinels around prompt/suffix; duplicating them makes the
 * model emit its own middle marker into the ghost. */
static void prompt_fim(JsonW *writer, const AiBackend *backend,
                       const AiCtx *context)
{
    static const char *const stops[] = {"\n\n", "\n}", "```"};

    yew_jsonw_obj(writer);
    yew_jsonw_key(writer, "model");
    yew_jsonw_cstr(writer, backend->model);
    yew_jsonw_key(writer, "prompt");
    json_text(writer, context->prefix, context->plen);
    yew_jsonw_key(writer, "suffix");
    json_text(writer, context->suffix, context->slen);
    yew_jsonw_key(writer, "stream");
    yew_jsonw_bool(writer, true);
    yew_jsonw_key(writer, "raw");
    yew_jsonw_bool(writer, false);
    yew_jsonw_key(writer, "options");
    yew_jsonw_obj(writer);
    yew_jsonw_key(writer, "num_predict");
    yew_jsonw_int(writer, backend->max_tokens);
    yew_jsonw_key(writer, "temperature");
    json_temperature(writer, backend->temperature);
    yew_jsonw_key(writer, "stop");
    json_stop(writer, stops, YEW_ARRAY_LEN(stops));
    yew_jsonw_obj_end(writer);
    yew_jsonw_obj_end(writer);
}

static void prompt_chat(JsonW *writer, const AiBackend *backend,
                        const AiCtx *context)
{
    static const char *const stops[] = {
        "```", "<|before|>", "<|after|>"
    };
    Bytebuf user;

    bytebuf_init(&user);
    chat_user(&user, context);
    yew_jsonw_obj(writer);
    yew_jsonw_key(writer, "model");
    yew_jsonw_cstr(writer, backend->model);
    if (backend->kind == (u8)YEW_AI_ANTHROPIC) {
        yew_jsonw_key(writer, "system");
        yew_jsonw_cstr(writer, chat_system);
        yew_jsonw_key(writer, "messages");
        yew_jsonw_arr(writer);
        json_message(writer, "user", user.data, (u32)user.len);
        yew_jsonw_arr_end(writer);
        yew_jsonw_key(writer, "max_tokens");
        yew_jsonw_int(writer, backend->max_tokens);
        yew_jsonw_key(writer, "stream");
        yew_jsonw_bool(writer, true);
        yew_jsonw_key(writer, "temperature");
        json_temperature(writer, backend->temperature);
        yew_jsonw_key(writer, "stop_sequences");
        json_stop(writer, stops, YEW_ARRAY_LEN(stops));
    } else {
        yew_jsonw_key(writer, "messages");
        yew_jsonw_arr(writer);
        json_message(writer, "system", (const u8 *)chat_system,
                     (u32)(sizeof(chat_system) - 1U));
        json_message(writer, "user", user.data, (u32)user.len);
        yew_jsonw_arr_end(writer);
        yew_jsonw_key(writer, "stream");
        yew_jsonw_bool(writer, true);
        if (backend->kind == (u8)YEW_AI_OPENAI) {
            yew_jsonw_key(writer, "max_tokens");
            yew_jsonw_int(writer, backend->max_tokens);
            yew_jsonw_key(writer, "temperature");
            json_temperature(writer, backend->temperature);
            yew_jsonw_key(writer, "stop");
            json_stop(writer, stops, YEW_ARRAY_LEN(stops));
        } else {
            yew_jsonw_key(writer, "options");
            yew_jsonw_obj(writer);
            yew_jsonw_key(writer, "num_predict");
            yew_jsonw_int(writer, backend->max_tokens);
            yew_jsonw_key(writer, "temperature");
            json_temperature(writer, backend->temperature);
            yew_jsonw_key(writer, "stop");
            json_stop(writer, stops, YEW_ARRAY_LEN(stops));
            yew_jsonw_obj_end(writer);
        }
    }
    yew_jsonw_obj_end(writer);
    bytebuf_free(&user);
}

void yew_ai_prompt_build(JsonW *writer, const AiBackend *backend,
                         const AiCtx *context)
{
    if (writer == NULL || backend == NULL || context == NULL)
        return;
    if (yew_ai_template_of(backend) == YEW_AI_TPL_FIM)
        prompt_fim(writer, backend, context);
    else
        prompt_chat(writer, backend, context);
}

static bool whole_fence(const u8 *bytes, u32 len, u32 *body_lo,
                        u32 *body_hi)
{
    u32 newline = 3U;
    u32 tail;

    if (len < 7U || memcmp(bytes, "```", 3U) != 0)
        return false;
    while (newline < len && bytes[newline] != (u8)'\n')
        newline++;
    if (newline == len)
        return false;
    tail = len;
    while (tail > newline + 1U &&
           (bytes[tail - 1U] == (u8)' ' || bytes[tail - 1U] == (u8)'\t' ||
            bytes[tail - 1U] == (u8)'\r' || bytes[tail - 1U] == (u8)'\n'))
        tail--;
    if (tail < newline + 4U || memcmp(bytes + tail - 3U, "```", 3U) != 0)
        return false;
    *body_lo = newline + 1U;
    *body_hi = tail - 3U;
    return true;
}

u32 yew_ai_response_trim(u8 *bytes, u32 len, const AiCtx *context,
                         u32 max_lines)
{
    u32 lo = 0U;
    u32 hi = len;
    u32 body_lo;
    u32 body_hi;
    u32 last_line = 0U;
    u32 prefix_line_len = 0U;
    u32 i;
    u32 lines = 1U;

    if (bytes == NULL || len == 0U || max_lines == 0U)
        return 0U;
    if (whole_fence(bytes, len, &body_lo, &body_hi)) {
        lo = body_lo;
        hi = body_hi;
    }
    if (lo < hi && bytes[lo] == (u8)'\n')
        lo++;
    if (context != NULL && context->prefix != NULL) {
        for (i = 0U; i < context->plen; i++)
            if (context->prefix[i] == (u8)'\n')
                last_line = i + 1U;
        prefix_line_len = context->plen - last_line;
        while (prefix_line_len != 0U && hi - lo >= prefix_line_len &&
               memcmp(bytes + lo, context->prefix + last_line,
                      prefix_line_len) == 0)
            lo += prefix_line_len;
    }
    for (i = lo; i < hi; i++) {
        if (bytes[i] == (u8)'\n' && ++lines > max_lines) {
            hi = i;
            break;
        }
    }
    while (hi > lo && (bytes[hi - 1U] == (u8)' ' ||
                       bytes[hi - 1U] == (u8)'\t' ||
                       bytes[hi - 1U] == (u8)'\r' ||
                       bytes[hi - 1U] == (u8)'\n'))
        hi--;
    if (lo != 0U && hi != lo)
        (void)memmove(bytes, bytes + lo, hi - lo);
    return hi - lo;
}
