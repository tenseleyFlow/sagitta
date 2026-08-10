#include "harness.h"

#include "util/buf.h"

void test_bytebuf(void)
{
    Bytebuf buf;

    bytebuf_init(&buf);
    bytebuf_printf(&buf, "%s:%d", "value", 42);
    YEW_ASSERT_EQ_U64(buf.len, 8U);
    YEW_ASSERT_EQ_MEM(buf.data, "value:42", 8U);
    YEW_ASSERT(buf.cap >= buf.len + 1U);
    bytebuf_free(&buf);
    YEW_ASSERT_NULL(buf.data);
}

void test_bytebuf_binary(void)
{
    Bytebuf buf;
    static const u8 expected[] = {'x', 0U, 0xffU};

    bytebuf_init(&buf);
    bytebuf_push_u8(&buf, (u8)'x');
    bytebuf_append(&buf, expected + 1U, 2U);
    YEW_ASSERT_EQ_U64(buf.len, sizeof(expected));
    YEW_ASSERT_EQ_MEM(buf.data, expected, sizeof(expected));
    bytebuf_append(&buf, NULL, 0U);
    YEW_ASSERT_EQ_U64(buf.len, sizeof(expected));
    bytebuf_free(&buf);
}
