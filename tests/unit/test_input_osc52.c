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

    sag_input_init(&in, &caps);
    sag_test_capture_log();
    while (off < len) {
        size_t n = len - off;

        if (n > chunk)
            n = chunk;
        sag_input_feed(&in, reply + off, n);
        off += n;
        SAG_ASSERT(!sag_input_next(&in, 0, &key));
        SAG_ASSERT_EQ_U64(key.kind, SAG_EV_NONE);
    }
    SAG_ASSERT_EQ_U64(in.dropped, 1U);
    SAG_ASSERT(sag_test_log_contains(
        SAG_LOG_WARN, "unsolicited OSC 52 reply discarded"));

    sag_input_feed(&in, visible, sizeof(visible) - 1U);
    SAG_ASSERT(sag_input_next(&in, 0, &key));
    SAG_ASSERT_EQ_U64(key.kind, SAG_EV_KEY);
    SAG_ASSERT_EQ_U64(key.code, (u32)'x');
    SAG_ASSERT_EQ_U64(key.ntext, 1U);
    SAG_ASSERT_EQ_U64(key.text[0], (u8)'x');
    SAG_ASSERT(!sag_input_next(&in, 0, &key));
    sag_input_free(&in);
    sag_test_teardown();
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
    const size_t body_len = SAG_IN_STRING_MAX + 1024U;
    const size_t len = sizeof(prefix) - 1U + body_len + sizeof(suffix) - 1U;
    TtyCaps caps = {0};
    u8 *reply = sag_xmalloc(len);
    In in;
    Key key;
    bool saw_visible = false;

    (void)memcpy(reply, prefix, sizeof(prefix) - 1U);
    (void)memset(reply + sizeof(prefix) - 1U, 'A', body_len);
    (void)memcpy(reply + sizeof(prefix) - 1U + body_len, suffix,
                 sizeof(suffix) - 1U);

    sag_input_init(&in, &caps);
    sag_test_capture_log();
    sag_input_feed(&in, reply, len);
    while (sag_input_next(&in, 0, &key)) {
        if (key.kind == SAG_EV_KEY) {
            SAG_ASSERT_EQ_U64(key.code, (u32)'z');
            saw_visible = true;
        } else {
            SAG_ASSERT_EQ_U64(key.kind, SAG_EV_NONE);
        }
    }
    SAG_ASSERT(saw_visible);
    SAG_ASSERT_EQ_U64(in.dropped, 1U);
    SAG_ASSERT(sag_test_log_contains(
        SAG_LOG_WARN, "unsolicited OSC 52 reply discarded"));
    SAG_ASSERT(!sag_test_log_contains(SAG_LOG_WARN, "terminal string cap"));

    sag_input_free(&in);
    sag_test_teardown();
    free(reply);
}
