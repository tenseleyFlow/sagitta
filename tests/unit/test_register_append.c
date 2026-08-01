#include "harness.h"

#include "text/register.h"

static void append_value(RegVal *v, RegType type, const char *bytes)
{
    sag_regval_init(v);
    v->type = (u8)type;
    bytebuf_append(&v->bytes, bytes, strlen(bytes));
}

static void append_assert(const RegVal *v, RegType type, const char *bytes)
{
    size_t len = strlen(bytes);
    SAG_ASSERT_EQ_U64(v->type, type);
    SAG_ASSERT_EQ_U64(v->bytes.len, len);
    SAG_ASSERT_EQ_MEM(v->bytes.data, bytes, len);
}

void test_register_append_char_char(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    sag_reg_init(&r);
    append_value(&a, SAG_REG_CHARWISE, "left");
    append_value(&b, SAG_REG_CHARWISE, "right");
    sag_reg_set(&r, 'a', &a);
    sag_reg_append(&r, 'A', &b);
    append_assert(&r.named[0], SAG_REG_CHARWISE, "leftright");
    sag_regval_free(&b); sag_regval_free(&a); sag_reg_free(&r);
}

void test_register_append_char_line_uses_line_eol(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    sag_reg_init(&r);
    append_value(&a, SAG_REG_CHARWISE, "left");
    append_value(&b, SAG_REG_LINEWISE, "right\r\n");
    sag_reg_set(&r, 'a', &a);
    sag_reg_append(&r, 'A', &b);
    append_assert(&r.named[0], SAG_REG_LINEWISE, "left\r\nright\r\n");
    sag_regval_free(&b); sag_regval_free(&a); sag_reg_free(&r);
}

void test_register_append_line_char_preserves_destination_eol(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    sag_reg_init(&r);
    append_value(&a, SAG_REG_LINEWISE, "left\r\n");
    append_value(&b, SAG_REG_CHARWISE, "right");
    sag_reg_set(&r, 'a', &a);
    sag_reg_append(&r, 'A', &b);
    append_assert(&r.named[0], SAG_REG_LINEWISE, "left\r\nright\r\n");
    sag_regval_free(&b); sag_regval_free(&a); sag_reg_free(&r);
}

void test_register_append_line_line_is_byte_exact(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    sag_reg_init(&r);
    append_value(&a, SAG_REG_LINEWISE, "left\n");
    append_value(&b, SAG_REG_LINEWISE, "right\r\n");
    sag_reg_set(&r, 'a', &a);
    sag_reg_append(&r, 'A', &b);
    append_assert(&r.named[0], SAG_REG_LINEWISE, "left\nright\r\n");
    sag_regval_free(&b); sag_regval_free(&a); sag_reg_free(&r);
}

void test_register_append_refuses_mixed_block_without_mutation(void)
{
    Registers r;
    RegVal chars;
    RegVal block;
    sag_reg_init(&r);
    append_value(&chars, SAG_REG_CHARWISE, "keep");
    append_value(&block, SAG_REG_BLOCKWISE, "xx");
    block.width = 2U;
    SagRegRowVec_push(&block.rows, ((Span){0U, 2U}));
    sag_reg_set(&r, 'a', &chars);
    sag_test_capture_log();
    sag_reg_append(&r, 'A', &block);
    append_assert(&r.named[0], SAG_REG_CHARWISE, "keep");
    SAG_ASSERT(sag_test_log_contains(SAG_LOG_WARN, "refused"));
    sag_regval_free(&block); sag_regval_free(&chars); sag_reg_free(&r);
}

void test_register_append_block_block_concatenates_and_repads(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    sag_reg_init(&r);
    append_value(&a, SAG_REG_BLOCKWISE, "a");
    a.width = 1U;
    SagRegRowVec_push(&a.rows, ((Span){0U, 1U}));
    append_value(&b, SAG_REG_BLOCKWISE, "bbb");
    b.width = 3U;
    SagRegRowVec_push(&b.rows, ((Span){0U, 3U}));
    sag_reg_append(&r, 'A', &a);
    sag_reg_append(&r, 'A', &b);
    SAG_ASSERT_EQ_U64(r.named[0].type, SAG_REG_BLOCKWISE);
    SAG_ASSERT_EQ_U64(r.named[0].width, 3U);
    SAG_ASSERT_EQ_U64(r.named[0].rows.len, 2U);
    SAG_ASSERT_EQ_U64(r.named[0].rows.data[0].hi -
                      r.named[0].rows.data[0].lo, 3U);
    SAG_ASSERT_EQ_MEM(r.named[0].bytes.data, "a  bbb", 6U);
    sag_regval_free(&b); sag_regval_free(&a); sag_reg_free(&r);
}

void test_register_append_ragged_rows_never_pad_nonragged_rows_do(void)
{
    Registers r;
    RegVal a;
    RegVal b;
    sag_reg_init(&r);
    append_value(&a, SAG_REG_BLOCKWISE, "a");
    a.width = 1U;
    SagRegRowVec_push(&a.rows, ((Span){0U, 1U}));
    append_value(&b, SAG_REG_BLOCKWISE, "bbb");
    b.width = 3U;
    b.ragged = true;
    SagRegRowVec_push(&b.rows, ((Span){0U, 3U}));
    sag_reg_append(&r, 'A', &a);
    sag_reg_append(&r, 'A', &b);
    SAG_ASSERT(r.named[0].ragged);
    SAG_ASSERT_EQ_U64(r.named[0].bytes.len, 6U);
    SAG_ASSERT_EQ_MEM(r.named[0].bytes.data, "a  bbb", 6U);
    sag_regval_free(&b); sag_regval_free(&a); sag_reg_free(&r);
}
