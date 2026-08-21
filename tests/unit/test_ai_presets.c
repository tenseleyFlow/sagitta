#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/option.h"
#include "mod/ai/ai.h"
#include "mod/ai/preset.h"
#include "mod/ai/registry.h"
#include "text/piece.h"

static const char *preset_selected(Ed *ed)
{
    OptVal value;

    YEW_ASSERT(yew_opt_get(ed, NULL, NULL, "ai.backend", 10U, &value));
    YEW_ASSERT_EQ_U64(value.type, YEW_OPT_STR);
    return value.as.str.s;
}

void test_ai_presets_parse_execute_and_replace_selection(void)
{
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ai_preset_load(&ed, "local"));
    YEW_ASSERT_EQ_STR(preset_selected(&ed), "local");
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    YEW_ASSERT_NOT_NULL(yew_ai_backend_selected(&ed));
    YEW_ASSERT(yew_ai_backend_selected(&ed)->backend.url.loopback);
    YEW_ASSERT(yew_ai_preset_load(&ed, "cloud"));
    YEW_ASSERT_EQ_STR(preset_selected(&ed), "work");
    YEW_ASSERT_EQ_U64(yew_ai_backend_count(&ed), 1U);
    YEW_ASSERT_NOT_NULL(yew_ai_backend_selected(&ed));
    YEW_ASSERT(!yew_ai_backend_selected(&ed)->backend.url.loopback);
    YEW_ASSERT(!yew_ai_preset_load(&ed, "unknown"));
    yew_ed_free(&ed);
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
