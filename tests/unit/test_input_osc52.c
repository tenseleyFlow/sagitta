#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "term/input.h"
#include "util/base.h"

static void assert_reply_is_quarantined(const u8 *reply, size_t len,
                                        size_t chunk)
{
    static const u8 visible[] = "x";
    TtyCaps caps = {0};
    In in;
    Key key;
    size_t off = 0U;

    yew_input_init(&in, &caps);
    yew_test_capture_log();
    while (off < len) {
        size_t n = len - off;

        if (n > chunk)
            n = chunk;
        yew_input_feed(&in, reply + off, n);
        off += n;
        YEW_ASSERT(!yew_input_next(&in, 0, &key));
        YEW_ASSERT_EQ_U64(key.kind, YEW_EV_NONE);
    }
    YEW_ASSERT_EQ_U64(in.dropped, 1U);
    YEW_ASSERT(yew_test_log_contains(
        YEW_LOG_WARN, "unsolicited OSC 52 reply discarded"));

    yew_input_feed(&in, visible, sizeof(visible) - 1U);
    YEW_ASSERT(yew_input_next(&in, 0, &key));
    YEW_ASSERT_EQ_U64(key.kind, YEW_EV_KEY);
    YEW_ASSERT_EQ_U64(key.code, (u32)'x');
    YEW_ASSERT_EQ_U64(key.ntext, 1U);
    YEW_ASSERT_EQ_U64(key.text[0], (u8)'x');
    YEW_ASSERT(!yew_input_next(&in, 0, &key));
    yew_input_free(&in);
    yew_test_teardown();
}

void test_input_osc52_reply_quarantined(void)
{
    static const u8 st_reply[] = "\x1b]52;c;c2VjcmV0AA==\x1b\\";
    static const u8 bel_reply[] = "\x1b]52;cp;bm90LXRleHQ=\x07";
    size_t chunk;

    for (chunk = 1U; chunk <= sizeof(st_reply) - 1U; chunk++)
        assert_reply_is_quarantined(st_reply, sizeof(st_reply) - 1U, chunk);
    assert_reply_is_quarantined(bel_reply, sizeof(bel_reply) - 1U, 2U);
}

void test_input_osc52_reply_over_cap_quarantined(void)
{
    static const u8 prefix[] = "\x1b]52;c;";
    static const u8 suffix[] = "\x1b\\z";
    const size_t body_len = YEW_IN_STRING_MAX + 1024U;
    const size_t len = sizeof(prefix) - 1U + body_len + sizeof(suffix) - 1U;
    TtyCaps caps = {0};
    u8 *reply = yew_xmalloc(len);
    In in;
    Key key;
    bool saw_visible = false;

    (void)memcpy(reply, prefix, sizeof(prefix) - 1U);
    (void)memset(reply + sizeof(prefix) - 1U, 'A', body_len);
    (void)memcpy(reply + sizeof(prefix) - 1U + body_len, suffix,
                 sizeof(suffix) - 1U);

    yew_input_init(&in, &caps);
    yew_test_capture_log();
    yew_input_feed(&in, reply, len);
    while (yew_input_next(&in, 0, &key)) {
        if (key.kind == YEW_EV_KEY) {
            YEW_ASSERT_EQ_U64(key.code, (u32)'z');
            saw_visible = true;
        } else {
            YEW_ASSERT_EQ_U64(key.kind, YEW_EV_NONE);
        }
    }
    YEW_ASSERT(saw_visible);
    YEW_ASSERT_EQ_U64(in.dropped, 1U);
    YEW_ASSERT(yew_test_log_contains(
        YEW_LOG_WARN, "unsolicited OSC 52 reply discarded"));
    YEW_ASSERT(!yew_test_log_contains(YEW_LOG_WARN, "terminal string cap"));

    yew_input_free(&in);
    yew_test_teardown();
    free(reply);
}
