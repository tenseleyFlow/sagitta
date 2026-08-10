#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "term/osc52.h"
#include "util/buf.h"

typedef struct OscEnvEntry {
    const char *name;
    const char *value;
} OscEnvEntry;

static const OscEnvEntry *osc_env_entries;
static size_t osc_env_len;

static const char *osc_test_env(const char *name)
{
    size_t i;

    for (i = 0u; i < osc_env_len; i++) {
        if (strcmp(osc_env_entries[i].name, name) == 0)
            return osc_env_entries[i].value;
    }
    return NULL;
}

static void osc_set_env(const OscEnvEntry *entries, size_t len)
{
    osc_env_entries = entries;
    osc_env_len = len;
}

static size_t osc_count(const Bytebuf *out, const u8 *needle, size_t n)
{
    size_t count = 0u;
    size_t i;

    if (n > out->len)
        return 0u;
    for (i = 0u; i + n <= out->len; i++) {
        if (memcmp(out->data + i, needle, n) == 0)
            count++;
    }
    return count;
}

void test_osc52_plain_targets_and_limit(void)
{
    static const u8 c_expected[] = "\033]52;c;Zm9v\033\\";
    static const u8 p_expected[] = "\033]52;p;Zm9v\033\\";
    static const u8 cp_expected[] = "\033]52;cp;Zm9v\033\\";
    static const u8 empty_expected[] = "\033]52;c;\033\\";
    static const char *const targets[] = {"c", "p", "cp"};
    static const u8 *const expected[] = {c_expected, p_expected, cp_expected};
    static const size_t expected_len[] = {
        sizeof(c_expected) - 1u, sizeof(p_expected) - 1u,
        sizeof(cp_expected) - 1u
    };
    Bytebuf out;
    size_t i;

    bytebuf_init(&out);
    for (i = 0u; i < YEW_ARRAY_LEN(targets); i++) {
        out.len = 0u;
        YEW_ASSERT(yew_osc52_build(&out, (const u8 *)"foo", 3u, targets[i],
                                   YEW_OSC52_PLAIN, 4u));
        YEW_ASSERT_EQ_U64(out.len, expected_len[i]);
        YEW_ASSERT_EQ_MEM(out.data, expected[i], expected_len[i]);
    }
    out.len = 0u;
    YEW_ASSERT(yew_osc52_build(&out, NULL, 0u, "c", YEW_OSC52_PLAIN, 0u));
    YEW_ASSERT_EQ_U64(out.len, sizeof(empty_expected) - 1u);
    YEW_ASSERT_EQ_MEM(out.data, empty_expected, sizeof(empty_expected) - 1u);

    out.len = 0u;
    bytebuf_append(&out, "sentinel", 8u);
    YEW_ASSERT(!yew_osc52_build(&out, (const u8 *)"foo", 3u, "c",
                                YEW_OSC52_PLAIN, 3u));
    YEW_ASSERT_EQ_U64(out.len, 8u);
    YEW_ASSERT_EQ_MEM(out.data, "sentinel", 8u);
    YEW_ASSERT(!yew_osc52_build(&out, (const u8 *)"foo", 3u, "c",
                                YEW_OSC52_OFF, 100u));
    YEW_ASSERT_EQ_U64(out.len, 8u);
    bytebuf_free(&out);
}

void test_osc52_tmux_escape_doubling_golden(void)
{
    static const u8 expected[] =
        "\033Ptmux;\033\033]52;c;G1g=\033\033\\\033\\";
    static const u8 payload[] = {0x1bu, (u8)'X'};
    Bytebuf out;

    bytebuf_init(&out);
    YEW_ASSERT(yew_osc52_build(&out, payload, sizeof(payload), "c",
                               YEW_OSC52_TMUX, 100u));
    YEW_ASSERT_EQ_U64(out.len, sizeof(expected) - 1u);
    YEW_ASSERT_EQ_MEM(out.data, expected, sizeof(expected) - 1u);
    YEW_ASSERT_EQ_U64(osc_count(&out, (const u8 *)"\033\033", 2u), 2u);
    bytebuf_free(&out);
}

void test_osc52_screen_chunking_golden(void)
{
    static const u8 dcs[] = {0x1bu, (u8)'P'};
    static const u8 osc[] = {0x1bu, (u8)']', (u8)'5', (u8)'2', (u8)';'};
    u8 payload[1350];
    Bytebuf out;
    size_t chunk_sizes[] = {768u, 768u, 264u};
    size_t offset = 0u;
    size_t i;

    memset(payload, 0u, sizeof(payload));
    bytebuf_init(&out);
    YEW_ASSERT(yew_osc52_build(&out, payload, sizeof(payload), "cp",
                               YEW_OSC52_SCREEN, 1800u));
    YEW_ASSERT_EQ_U64(out.len, 1822u);
    YEW_ASSERT_EQ_U64(osc_count(&out, dcs, sizeof(dcs)), 3u);
    YEW_ASSERT_EQ_U64(osc_count(&out, osc, sizeof(osc)), 1u);
    for (i = 0u; i < YEW_ARRAY_LEN(chunk_sizes); i++) {
        size_t j;

        if (i == 0u) {
            YEW_ASSERT_EQ_MEM(out.data + offset, "\033P\033]52;cp;", 10u);
            offset += 10u;
        } else {
            YEW_ASSERT_EQ_MEM(out.data + offset, "\033P", 2u);
            offset += 2u;
        }
        for (j = 0u; j < chunk_sizes[i]; j++)
            YEW_ASSERT(out.data[offset + j] == (u8)'A');
        offset += chunk_sizes[i];
        if (i + 1u == YEW_ARRAY_LEN(chunk_sizes)) {
            YEW_ASSERT_EQ_MEM(out.data + offset, "\033\\\033\\", 4u);
            offset += 4u;
        } else {
            YEW_ASSERT_EQ_MEM(out.data + offset, "\033\\", 2u);
            offset += 2u;
        }
    }
    YEW_ASSERT_EQ_U64(offset, out.len);
    bytebuf_free(&out);
}

void test_osc52_environment_selection(void)
{
    static const OscEnvEntry forced_off[] = {{"YEW_OSC52", "off"}};
    static const OscEnvEntry forced_plain[] = {
        {"YEW_OSC52", "plain"}, {"TMUX", "/tmp/tmux"}
    };
    static const OscEnvEntry tmux[] = {{"TMUX", "/tmp/tmux"}};
    static const OscEnvEntry sty[] = {{"STY", "123.session"}};
    static const OscEnvEntry screen_term[] = {{"TERM", "screen-256color"}};
    static const OscEnvEntry target_limit[] = {
        {"YEW_CLIPBOARD_TARGET", "cp"}, {"YEW_OSC52_MAX", "4096"}
    };
    static const OscEnvEntry invalid[] = {
        {"YEW_OSC52", "wat"}, {"YEW_CLIPBOARD_TARGET", "x"},
        {"YEW_OSC52_MAX", "12x"}
    };
    Bytebuf out;

    osc_set_env(forced_off, YEW_ARRAY_LEN(forced_off));
    YEW_ASSERT_EQ_U64(yew_osc52_mode(osc_test_env), YEW_OSC52_OFF);
    osc_set_env(forced_plain, YEW_ARRAY_LEN(forced_plain));
    YEW_ASSERT_EQ_U64(yew_osc52_mode(osc_test_env), YEW_OSC52_PLAIN);
    osc_set_env(tmux, YEW_ARRAY_LEN(tmux));
    YEW_ASSERT_EQ_U64(yew_osc52_mode(osc_test_env), YEW_OSC52_TMUX);
    osc_set_env(sty, YEW_ARRAY_LEN(sty));
    YEW_ASSERT_EQ_U64(yew_osc52_mode(osc_test_env), YEW_OSC52_SCREEN);
    osc_set_env(screen_term, YEW_ARRAY_LEN(screen_term));
    YEW_ASSERT_EQ_U64(yew_osc52_mode(osc_test_env), YEW_OSC52_SCREEN);
    osc_set_env(NULL, 0u);
    YEW_ASSERT_EQ_U64(yew_osc52_mode(osc_test_env), YEW_OSC52_PLAIN);

    osc_set_env(target_limit, YEW_ARRAY_LEN(target_limit));
    YEW_ASSERT_EQ_STR(yew_osc52_target(osc_test_env), "cp");
    YEW_ASSERT_EQ_U64(yew_osc52_max(osc_test_env), 4096u);
    bytebuf_init(&out);
    YEW_ASSERT(yew_osc52_build_env(&out, (const u8 *)"f", 1u,
                                   osc_test_env));
    YEW_ASSERT_EQ_U64(out.len, 14u);
    YEW_ASSERT_EQ_MEM(out.data, "\033]52;cp;Zg==\033\\", out.len);
    bytebuf_free(&out);

    yew_test_capture_log();
    osc_set_env(invalid, YEW_ARRAY_LEN(invalid));
    YEW_ASSERT_EQ_U64(yew_osc52_mode(osc_test_env), YEW_OSC52_PLAIN);
    YEW_ASSERT_EQ_STR(yew_osc52_target(osc_test_env), "c");
    YEW_ASSERT_EQ_U64(yew_osc52_max(osc_test_env), YEW_OSC52_DEFAULT_MAX);
    YEW_ASSERT(yew_test_log_count() >= 3u);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN, "invalid YEW_OSC52"));
    osc_set_env(NULL, 0u);
}
