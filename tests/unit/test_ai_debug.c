#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai_int.h"
#include "mod/ai/debug.h"

static bool log_contains(const Bytebuf *log, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0U)
        return true;
    if (nlen > log->len)
        return false;
    for (i = 0U; i <= log->len - nlen; i++) {
        if (memcmp(log->data + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

static u32 log_count(const Bytebuf *log, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;
    u32 count = 0U;

    if (nlen == 0U || nlen > log->len)
        return 0U;
    for (i = 0U; i <= log->len - nlen; i++) {
        if (memcmp(log->data + i, needle, nlen) == 0)
            count++;
    }
    return count;
}

static void set_debug_bodies(Ed *ed, bool enabled)
{
    const char *error = NULL;
    OptVal value = {YEW_OPT_BOOL, {.b = enabled}};

    YEW_ASSERT(yew_opt_set(ed, YEW_OPT_GLOBAL, "ai.debug_bodies", 15U,
                           &value, &error));
    YEW_ASSERT_NULL(error);
}

void test_ai_debug_bodies_require_both_privacy_gates(void)
{
    static const u8 request[] = "request-unique-marker-s50";
    static const u8 completion[] = "completion-unique-marker-s50";
    const char *old = getenv("YEW_AI_DEBUG");
    char *saved = old != NULL ? strdup(old) : NULL;
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", "1", 1), 0);
    yew_ai_debug_body(&ed, "request", request, sizeof(request) - 1U);
    YEW_ASSERT(!log_contains(&ed.ai->log, (const char *)request));

    set_debug_bodies(&ed, true);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_AI_DEBUG"), 0);
    yew_ai_debug_body(&ed, "request", request, sizeof(request) - 1U);
    YEW_ASSERT(!log_contains(&ed.ai->log, (const char *)request));

    YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", "true", 1), 0);
    yew_ai_debug_body(&ed, "request", request, sizeof(request) - 1U);
    YEW_ASSERT(!log_contains(&ed.ai->log, (const char *)request));

    YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", "1", 1), 0);
    yew_ai_debug_body(&ed, "request", request, sizeof(request) - 1U);
    yew_ai_debug_body(&ed, "completion", completion,
                      sizeof(completion) - 1U);
    YEW_ASSERT(log_contains(&ed.ai->log, (const char *)request));
    YEW_ASSERT(log_contains(&ed.ai->log, (const char *)completion));
    YEW_ASSERT_EQ_U64(log_count(&ed.ai->log,
                                "WARN: AI debug body logging is enabled"),
                      1U);

    if (saved != NULL) {
        YEW_ASSERT_EQ_I64(setenv("YEW_AI_DEBUG", saved, 1), 0);
        free(saved);
    } else {
        YEW_ASSERT_EQ_I64(unsetenv("YEW_AI_DEBUG"), 0);
    }
    yew_ed_free(&ed);
}
