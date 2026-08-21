#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai.h"

typedef struct AiOptionDefault {
    const char *name;
    u8 type;
    i64 integer;
    bool boolean;
    i64 min;
    i64 max;
} AiOptionDefault;

static OptVal ai_option_get(Ed *ed, const char *name)
{
    OptVal value = {0};

    YEW_ASSERT(yew_opt_get(ed, NULL, NULL, name, (u32)strlen(name),
                           &value));
    return value;
}

void test_ai_state_lifecycle_is_owned_by_editor(void)
{
    Ed ed;
    AiState *first;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ai_state_ready(&ed));
    YEW_ASSERT_NOT_NULL(ed.ai);
    first = ed.ai;
    yew_ai_state_init(&ed);
    YEW_ASSERT(yew_ai_state_ready(&ed));
    YEW_ASSERT(ed.ai == first);
    yew_ai_state_free(&ed);
    YEW_ASSERT(!yew_ai_state_ready(&ed));
    YEW_ASSERT_NULL(ed.ai);
    yew_ai_state_free(&ed);
    yew_ed_free(&ed);
}

void test_ai_options_have_pinned_defaults_and_bounds(void)
{
    static const AiOptionDefault rows[] = {
        {"ai.enable", YEW_OPT_BOOL, 0, false, 0, 0},
        {"ai.deny_replace", YEW_OPT_BOOL, 0, false, 0, 0},
        {"ai.exclude_replace", YEW_OPT_BOOL, 0, false, 0, 0},
        {"ai.badge_host_max", YEW_OPT_INT, 20, false, 8, 255},
        {"ai.debug_bodies", YEW_OPT_BOOL, 0, false, 0, 0},
        {"ai.allow_plain_remote", YEW_OPT_BOOL, 0, false, 0, 0},
        {"ai.key_cache", YEW_OPT_BOOL, 0, true, 0, 0},
        {"ai.connect_timeout_ms", YEW_OPT_INT, 2000, false, 1, 600000},
        {"ai.first_byte_timeout_ms", YEW_OPT_INT, 10000, false, 1, 600000},
        {"ai.stream_idle_timeout_ms", YEW_OPT_INT, 20000, false, 1,
         600000},
        {"ai.total_timeout_ms", YEW_OPT_INT, 120000, false, 1, 3600000},
        {"ai.keepalive_ms", YEW_OPT_INT, 30000, false, 0, 600000},
        {"ai.backoff_max_ms", YEW_OPT_INT, 60000, false, 1000, 3600000}
    };
    Ed ed;
    u32 i;

    yew_ed_init(&ed);
    for (i = 0U; i < YEW_ARRAY_LEN(rows); i++) {
        const AiOptionDefault *row = &rows[i];
        const OptDesc *desc = yew_opt_desc(row->name,
                                           (u32)strlen(row->name));
        OptVal value = ai_option_get(&ed, row->name);

        YEW_ASSERT_NOT_NULL(desc);
        YEW_ASSERT_EQ_U64(desc->scope, YEW_OPT_GLOBAL);
        YEW_ASSERT_EQ_U64(desc->type, row->type);
        YEW_ASSERT_EQ_U64(value.type, row->type);
        YEW_ASSERT_EQ_I64(desc->imin, row->min);
        YEW_ASSERT_EQ_I64(desc->imax, row->max);
        if (row->type == (u8)YEW_OPT_BOOL)
            YEW_ASSERT_EQ_U64(value.as.b, row->boolean);
        else {
            const char *err = NULL;
            OptVal edge = {YEW_OPT_INT, {.i = row->min}};

            YEW_ASSERT_EQ_I64(value.as.i, row->integer);
            YEW_ASSERT(yew_opt_validate(&ed, YEW_OPT_GLOBAL, row->name,
                                        (u32)strlen(row->name), &edge,
                                        &err));
            edge.as.i = row->max;
            YEW_ASSERT(yew_opt_validate(&ed, YEW_OPT_GLOBAL, row->name,
                                        (u32)strlen(row->name), &edge,
                                        &err));
            edge.as.i = row->min - 1;
            YEW_ASSERT(!yew_opt_validate(&ed, YEW_OPT_GLOBAL, row->name,
                                         (u32)strlen(row->name), &edge,
                                         &err));
            edge.as.i = row->max + 1;
            YEW_ASSERT(!yew_opt_validate(&ed, YEW_OPT_GLOBAL, row->name,
                                         (u32)strlen(row->name), &edge,
                                         &err));
        }
    }
    {
        static const struct {
            const char *name;
            const char *value;
        } enums[] = {
            {"ai.default_workspace", "ask"},
            {"ai.on_redact", "block"},
            {"ai.badge", "on"}
        };

        for (i = 0U; i < YEW_ARRAY_LEN(enums); i++) {
            const OptDesc *desc = yew_opt_desc(
                enums[i].name, (u32)strlen(enums[i].name));
            OptVal value = ai_option_get(&ed, enums[i].name);

            YEW_ASSERT_NOT_NULL(desc);
            YEW_ASSERT_EQ_U64(desc->scope, YEW_OPT_GLOBAL);
            YEW_ASSERT_EQ_U64(desc->type, YEW_OPT_ENUM);
            YEW_ASSERT_EQ_U64(value.type, YEW_OPT_ENUM);
            YEW_ASSERT_EQ_U64(value.as.str.len, strlen(enums[i].value));
            YEW_ASSERT_EQ_MEM(value.as.str.s, enums[i].value,
                              value.as.str.len);
        }
    }
    YEW_ASSERT(yew_ai_state_key_cache_enabled(&ed));
    {
        const char *err = NULL;
        OptVal off = {YEW_OPT_BOOL, {.b = false}};

        YEW_ASSERT(yew_opt_set(&ed, YEW_OPT_GLOBAL, "ai.key_cache",
                               sizeof("ai.key_cache") - 1U, &off, &err));
        YEW_ASSERT(!yew_ai_state_key_cache_enabled(&ed));
    }
    yew_ed_free(&ed);
}
