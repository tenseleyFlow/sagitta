#include "harness.h"

#include "text/register.h"

static void append_value(RegVal *v, RegType type, const char *bytes)
{
    yew_regval_init(v);
    v->type = (u8)type;
    bytebuf_append(&v->bytes, bytes, strlen(bytes));
}

static void append_assert(const RegVal *v, RegType type, const char *bytes)
{
    size_t len = strlen(bytes);
    YEW_ASSERT_EQ_U64(v->type, type);
    YEW_ASSERT_EQ_U64(v->bytes.len, len);
    YEW_ASSERT_EQ_MEM(v->bytes.data, bytes, len);
}

void test_register_append_char_char(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    yew_reg_init(&r);
    append_value(&a, YEW_REG_CHARWISE, "left");
    append_value(&b, YEW_REG_CHARWISE, "right");
    yew_reg_set(&r, 'a', &a);
    yew_reg_append(&r, 'A', &b);
    append_assert(&r.named[0], YEW_REG_CHARWISE, "leftright");
    yew_regval_free(&b); yew_regval_free(&a); yew_reg_free(&r);
}

void test_register_append_char_line_uses_line_eol(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    yew_reg_init(&r);
    append_value(&a, YEW_REG_CHARWISE, "left");
    append_value(&b, YEW_REG_LINEWISE, "right\r\n");
    yew_reg_set(&r, 'a', &a);
    yew_reg_append(&r, 'A', &b);
    append_assert(&r.named[0], YEW_REG_LINEWISE, "left\r\nright\r\n");
    yew_regval_free(&b); yew_regval_free(&a); yew_reg_free(&r);
}

void test_register_append_line_char_preserves_destination_eol(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    yew_reg_init(&r);
    append_value(&a, YEW_REG_LINEWISE, "left\r\n");
    append_value(&b, YEW_REG_CHARWISE, "right");
    yew_reg_set(&r, 'a', &a);
    yew_reg_append(&r, 'A', &b);
    append_assert(&r.named[0], YEW_REG_LINEWISE, "left\r\nright\r\n");
    yew_regval_free(&b); yew_regval_free(&a); yew_reg_free(&r);
}

void test_register_append_line_line_is_byte_exact(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    yew_reg_init(&r);
    append_value(&a, YEW_REG_LINEWISE, "left\n");
    append_value(&b, YEW_REG_LINEWISE, "right\r\n");
    yew_reg_set(&r, 'a', &a);
    yew_reg_append(&r, 'A', &b);
    append_assert(&r.named[0], YEW_REG_LINEWISE, "left\nright\r\n");
    yew_regval_free(&b); yew_regval_free(&a); yew_reg_free(&r);
}

void test_register_append_refuses_mixed_block_without_mutation(void)
{
    Registers r;
    RegVal chars;
    RegVal block;
    yew_reg_init(&r);
    append_value(&chars, YEW_REG_CHARWISE, "keep");
    append_value(&block, YEW_REG_BLOCKWISE, "xx");
    block.width = 2U;
    YewRegRowVec_push(&block.rows, ((Span){0U, 2U}));
    yew_reg_set(&r, 'a', &chars);
    yew_test_capture_log();
    yew_reg_append(&r, 'A', &block);
    append_assert(&r.named[0], YEW_REG_CHARWISE, "keep");
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN, "refused"));
    yew_regval_free(&block); yew_regval_free(&chars); yew_reg_free(&r);
}

void test_register_append_block_block_concatenates_and_repads(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    yew_reg_init(&r);
    append_value(&a, YEW_REG_BLOCKWISE, "a");
    a.width = 1U;
    YewRegRowVec_push(&a.rows, ((Span){0U, 1U}));
    append_value(&b, YEW_REG_BLOCKWISE, "bbb");
    b.width = 3U;
    YewRegRowVec_push(&b.rows, ((Span){0U, 3U}));
    yew_reg_append(&r, 'A', &a);
    yew_reg_append(&r, 'A', &b);
    YEW_ASSERT_EQ_U64(r.named[0].type, YEW_REG_BLOCKWISE);
    YEW_ASSERT_EQ_U64(r.named[0].width, 3U);
    YEW_ASSERT_EQ_U64(r.named[0].rows.len, 2U);
    YEW_ASSERT_EQ_U64(r.named[0].rows.data[0].hi -
                      r.named[0].rows.data[0].lo, 3U);
    YEW_ASSERT_EQ_MEM(r.named[0].bytes.data, "a  bbb", 6U);
    yew_regval_free(&b); yew_regval_free(&a); yew_reg_free(&r);
}

void test_register_append_ragged_rows_never_pad_nonragged_rows_do(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    yew_reg_init(&r);
    append_value(&a, YEW_REG_BLOCKWISE, "a");
    a.width = 1U;
    YewRegRowVec_push(&a.rows, ((Span){0U, 1U}));
    append_value(&b, YEW_REG_BLOCKWISE, "bbb");
    b.width = 3U;
    b.ragged = true;
    YewRegRowVec_push(&b.rows, ((Span){0U, 3U}));
    yew_reg_append(&r, 'A', &a);
    yew_reg_append(&r, 'A', &b);
    YEW_ASSERT(r.named[0].ragged);
    YEW_ASSERT_EQ_U64(r.named[0].bytes.len, 6U);
    YEW_ASSERT_EQ_MEM(r.named[0].bytes.data, "a  bbb", 6U);
    yew_regval_free(&b); yew_regval_free(&a); yew_reg_free(&r);
}
