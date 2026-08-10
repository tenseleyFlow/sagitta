#include "term/osc52.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util/base64.h"
#include "util/log.h"

static const u8 yew_osc52_osc_prefix[] = {0x1bu, (u8)']', (u8)'5', (u8)'2',
                                          (u8)';'};
static const u8 yew_osc52_st[] = {0x1bu, (u8)'\\'};
static const u8 yew_osc52_dcs[] = {0x1bu, (u8)'P'};

static const char *osc52_env(YewOsc52EnvFn getv, const char *name)
{
    return getv != NULL ? getv(name) : getenv(name);
}

static bool osc52_nonempty(const char *s)
{
    return s != NULL && s[0] != '\0';
}

YewOsc52Mode yew_osc52_mode(YewOsc52EnvFn getv)
{
    const char *forced = osc52_env(getv, "YEW_OSC52");
    const char *term;

    if (forced != NULL) {
        if (strcmp(forced, "off") == 0)
            return YEW_OSC52_OFF;
        if (strcmp(forced, "plain") == 0)
            return YEW_OSC52_PLAIN;
        if (strcmp(forced, "tmux") == 0)
            return YEW_OSC52_TMUX;
        if (strcmp(forced, "screen") == 0)
            return YEW_OSC52_SCREEN;
        yew_log(YEW_LOG_WARN, "clipboard: ignoring invalid YEW_OSC52 mode");
    }
    if (osc52_nonempty(osc52_env(getv, "TMUX")))
        return YEW_OSC52_TMUX;
    if (osc52_nonempty(osc52_env(getv, "STY")))
        return YEW_OSC52_SCREEN;
    term = osc52_env(getv, "TERM");
    if (term != NULL && strncmp(term, "screen", 6u) == 0)
        return YEW_OSC52_SCREEN;
    return YEW_OSC52_PLAIN;
}

u64 yew_osc52_max(YewOsc52EnvFn getv)
{
    const char *value = osc52_env(getv, "YEW_OSC52_MAX");
    char *end;
    const char *p;
    unsigned long long parsed;

    if (!osc52_nonempty(value))
        return YEW_OSC52_DEFAULT_MAX;
    for (p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            yew_log(YEW_LOG_WARN,
                    "clipboard: ignoring invalid YEW_OSC52_MAX");
            return YEW_OSC52_DEFAULT_MAX;
        }
    }
    errno = 0;
    end = NULL;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        yew_log(YEW_LOG_WARN, "clipboard: ignoring invalid YEW_OSC52_MAX");
        return YEW_OSC52_DEFAULT_MAX;
    }
    return (u64)parsed;
}

const char *yew_osc52_target(YewOsc52EnvFn getv)
{
    const char *target = osc52_env(getv, "YEW_CLIPBOARD_TARGET");

    if (target == NULL || strcmp(target, "c") == 0)
        return "c";
    if (strcmp(target, "p") == 0 || strcmp(target, "cp") == 0)
        return target;
    yew_log(YEW_LOG_WARN,
            "clipboard: ignoring invalid YEW_CLIPBOARD_TARGET");
    return "c";
}

static void osc52_append_plain(Bytebuf *out, const u8 *encoded, u64 n,
                               const char *target)
{
    bytebuf_append(out, yew_osc52_osc_prefix, sizeof(yew_osc52_osc_prefix));
    bytebuf_append(out, target, strlen(target));
    bytebuf_push_u8(out, (u8)';');
    bytebuf_append(out, encoded, (size_t)n);
    bytebuf_append(out, yew_osc52_st, sizeof(yew_osc52_st));
}

static void osc52_append_tmux(Bytebuf *out, const u8 *plain, size_t n)
{
    static const u8 prefix[] = {0x1bu, (u8)'P', (u8)'t', (u8)'m', (u8)'u',
                                (u8)'x', (u8)';'};
    size_t i;

    bytebuf_append(out, prefix, sizeof(prefix));
    for (i = 0u; i < n; i++) {
        bytebuf_push_u8(out, plain[i]);
        if (plain[i] == 0x1bu)
            bytebuf_push_u8(out, 0x1bu);
    }
    bytebuf_append(out, yew_osc52_st, sizeof(yew_osc52_st));
}

static void osc52_append_screen(Bytebuf *out, const u8 *encoded, u64 n,
                                 const char *target)
{
    u64 offset = 0u;
    bool first = true;

    do {
        u64 chunk = n - offset;

        if (chunk > YEW_OSC52_SCREEN_CHUNK)
            chunk = YEW_OSC52_SCREEN_CHUNK;
        bytebuf_append(out, yew_osc52_dcs, sizeof(yew_osc52_dcs));
        if (first) {
            bytebuf_append(out, yew_osc52_osc_prefix,
                           sizeof(yew_osc52_osc_prefix));
            bytebuf_append(out, target, strlen(target));
            bytebuf_push_u8(out, (u8)';');
            first = false;
        }
        bytebuf_append(out, encoded + offset, (size_t)chunk);
        if (offset + chunk == n)
            bytebuf_append(out, yew_osc52_st, sizeof(yew_osc52_st));
        bytebuf_append(out, yew_osc52_st, sizeof(yew_osc52_st));
        offset += chunk;
    } while (offset < n);
}

bool yew_osc52_build(Bytebuf *out, const u8 *payload, u64 payload_len,
                     const char *target, YewOsc52Mode mode, u64 max_base64)
{
    Bytebuf encoded;
    Bytebuf plain;
    const u8 *encoded_data;
    u64 encoded_len;

    if (mode == YEW_OSC52_OFF)
        return false;
    if (target == NULL ||
        (strcmp(target, "c") != 0 && strcmp(target, "p") != 0 &&
         strcmp(target, "cp") != 0))
        target = "c";
    encoded_len = yew_base64_len(payload_len);
    if (encoded_len > max_base64)
        return false;
    if (encoded_len > SIZE_MAX)
        YEW_BUG("OSC 52 payload exceeds addressable memory");

    bytebuf_init(&encoded);
    bytebuf_reserve(&encoded, (size_t)encoded_len);
    yew_base64_encode(payload, payload_len, encoded.data);
    encoded.len = (size_t)encoded_len;
    encoded_data = encoded_len == 0u ? (const u8 *)"" : encoded.data;

    if (mode == YEW_OSC52_SCREEN) {
        osc52_append_screen(out, encoded_data, encoded_len, target);
    } else if (mode == YEW_OSC52_TMUX) {
        bytebuf_init(&plain);
        osc52_append_plain(&plain, encoded_data, encoded_len, target);
        osc52_append_tmux(out, plain.data, plain.len);
        bytebuf_free(&plain);
    } else {
        osc52_append_plain(out, encoded_data, encoded_len, target);
    }
    bytebuf_free(&encoded);
    return true;
}

bool yew_osc52_build_env(Bytebuf *out, const u8 *payload, u64 payload_len,
                         YewOsc52EnvFn getv)
{
    return yew_osc52_build(out, payload, payload_len, yew_osc52_target(getv),
                           yew_osc52_mode(getv), yew_osc52_max(getv));
}
