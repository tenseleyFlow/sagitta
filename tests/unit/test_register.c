#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "text/register.h"

static void reg_test_value(RegVal *v, RegType type, const u8 *bytes,
                           size_t len)
{
    sag_regval_init(v);
    v->type = (u8)type;
    bytebuf_append(&v->bytes, bytes, len);
}

static void reg_assert_value(const RegVal *v, RegType type,
                             const u8 *bytes, size_t len)
{
    SAG_ASSERT_EQ_U64(v->type, type);
    SAG_ASSERT_EQ_U64(v->bytes.len, len);
    SAG_ASSERT_EQ_MEM(v->bytes.data, bytes, len);
}

void test_register_defaults_and_empty_deferred_slots(void)
{
    Registers r;

    sag_reg_init(&r);
    SAG_ASSERT_EQ_U64(r.ring_depth, 32U);
    SAG_ASSERT_EQ_U64(r.ring_bytes_max, UINT64_C(8) * 1024U * 1024U);
    SAG_ASSERT_EQ_U64(r.clipboard_sync, SAG_CLIP_SYNC_YANK);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, '/')->bytes.len, 0U);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, ':')->bytes.len, 0U);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, '#')->bytes.len, 0U);
    SAG_ASSERT_NULL(sag_reg_get(&r, '_'));
    SAG_ASSERT_NULL(sag_reg_get(&r, '?'));
    SAG_ASSERT(sag_reg_get(&r, '*') == sag_reg_get(&r, '+'));
    sag_reg_free(&r);
}

void test_register_yank_routes_named_unnamed_zero_and_ring(void)
{
    Registers r;
    RegVal v;

    sag_reg_init(&r);
    reg_test_value(&v, SAG_REG_CHARWISE, (const u8 *)"yank\0", 5U);
    sag_reg_yank(&r, 'b', &v);
    reg_assert_value(sag_reg_get(&r, 'b'), SAG_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    reg_assert_value(sag_reg_get(&r, '"'), SAG_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    reg_assert_value(sag_reg_get(&r, '0'), SAG_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    reg_assert_value(&r.ring[r.ring_head], SAG_REG_CHARWISE,
                     (const u8 *)"yank\0", 5U);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, '1')->bytes.len, 0U);
    SAG_ASSERT_EQ_U64(sag_reg_get(&r, '-')->bytes.len, 0U);
    sag_regval_free(&v);
    sag_reg_free(&r);
}

void test_register_delete_shift_boundary(void)
{
    Registers r;
    RegVal small;
    RegVal lf;
    RegVal line;

    sag_reg_init(&r);
    reg_test_value(&small, SAG_REG_CHARWISE, (const u8 *)"x", 1U);
    reg_test_value(&lf, SAG_REG_CHARWISE, (const u8 *)"x\ny", 3U);
    reg_test_value(&line, SAG_REG_LINEWISE, (const u8 *)"line", 4U);
    sag_reg_delete(&r, 0U, &small);
    reg_assert_value(&r.small_del, SAG_REG_CHARWISE, (const u8 *)"x", 1U);
    SAG_ASSERT_EQ_U64(r.numbered[1].bytes.len, 0U);
    sag_reg_delete(&r, 0U, &lf);
    reg_assert_value(&r.numbered[1], SAG_REG_CHARWISE,
                     (const u8 *)"x\ny", 3U);
    sag_reg_delete(&r, 0U, &line);
    reg_assert_value(&r.numbered[1], SAG_REG_LINEWISE,
                     (const u8 *)"line", 4U);
    reg_assert_value(&r.numbered[2], SAG_REG_CHARWISE,
                     (const u8 *)"x\ny", 3U);
    reg_assert_value(&r.unnamed, SAG_REG_LINEWISE,
                     (const u8 *)"line", 4U);
    SAG_ASSERT_EQ_U64(r.numbered[0].bytes.len, 0U);
    SAG_ASSERT_EQ_U64(r.ring_len, 3U);
    sag_regval_free(&line);
    sag_regval_free(&lf);
    sag_regval_free(&small);
    sag_reg_free(&r);
}

void test_register_delete_shifts_nine_and_discards_oldest(void)
{
    Registers r;
    RegVal v;
    u8 byte = 0U;
    u32 i;

    sag_reg_init(&r);
    reg_test_value(&v, SAG_REG_LINEWISE, &byte, 0U);
    for (i = 1U; i <= 10U; i++) {
        byte = (u8)('a' + i - 1U);
        v.bytes.len = 0U;
        bytebuf_append(&v.bytes, &byte, 1U);
        sag_reg_delete(&r, 0U, &v);
    }
    SAG_ASSERT_EQ_U64(r.numbered[1].bytes.data[0], 'j');
    SAG_ASSERT_EQ_U64(r.numbered[2].bytes.data[0], 'i');
    SAG_ASSERT_EQ_U64(r.numbered[9].bytes.data[0], 'b');
    sag_regval_free(&v);
    sag_reg_free(&r);
}

void test_register_blackhole_is_total_discard(void)
{
    Registers r;
    RegVal keep;
    RegVal drop;

    sag_reg_init(&r);
    reg_test_value(&keep, SAG_REG_CHARWISE, (const u8 *)"keep", 4U);
    reg_test_value(&drop, SAG_REG_LINEWISE, (const u8 *)"drop\n", 5U);
    sag_reg_yank(&r, 0U, &keep);
    sag_reg_delete(&r, '_', &drop);
    reg_assert_value(&r.unnamed, SAG_REG_CHARWISE,
                     (const u8 *)"keep", 4U);
    reg_assert_value(&r.numbered[0], SAG_REG_CHARWISE,
                     (const u8 *)"keep", 4U);
    SAG_ASSERT_EQ_U64(r.numbered[1].bytes.len, 0U);
    SAG_ASSERT_EQ_U64(r.small_del.bytes.len, 0U);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    sag_reg_yank(&r, '_', &drop);
    SAG_ASSERT_EQ_U64(r.ring_len, 1U);
    sag_regval_free(&drop);
    sag_regval_free(&keep);
    sag_reg_free(&r);
}

void test_register_uppercase_routes_append_but_unnamed_is_new_value(void)
{
    Registers r;
    RegVal one;
    RegVal two;

    sag_reg_init(&r);
    reg_test_value(&one, SAG_REG_CHARWISE, (const u8 *)"one", 3U);
    reg_test_value(&two, SAG_REG_CHARWISE, (const u8 *)"two", 3U);
    sag_reg_yank(&r, 'a', &one);
    sag_reg_yank(&r, 'A', &two);
    reg_assert_value(&r.named[0], SAG_REG_CHARWISE,
                     (const u8 *)"onetwo", 6U);
    reg_assert_value(&r.unnamed, SAG_REG_CHARWISE,
                     (const u8 *)"two", 3U);
    reg_assert_value(&r.numbered[0], SAG_REG_CHARWISE,
                     (const u8 *)"two", 3U);
    SAG_ASSERT_EQ_U64(r.ring_len, 2U);
    sag_regval_free(&two);
    sag_regval_free(&one);
    sag_reg_free(&r);
}

void test_register_line_capture_synthesizes_only_missing_eol(void)
{
    TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abc", 3U);
    FileMeta meta;
    RegVal line;
    RegVal chars;

    sag_filemeta_init(&meta);
    meta.eol = SAG_EOL_CRLF;
    meta.dominant_eol = SAG_EOL_CRLF;
    sag_regval_init(&line);
    sag_regval_init(&chars);
    sag_regval_from_span(&line, tb, (Span){0U, 3U}, SAG_REG_LINEWISE,
                         &meta);
    sag_regval_from_span(&chars, tb, (Span){0U, 3U}, SAG_REG_CHARWISE,
                         &meta);
    reg_assert_value(&line, SAG_REG_LINEWISE,
                     (const u8 *)"abc\r\n", 5U);
    reg_assert_value(&chars, SAG_REG_CHARWISE, (const u8 *)"abc", 3U);
    sag_regval_free(&chars);
    sag_regval_free(&line);
    sag_filemeta_dispose(&meta);
    sag_textbuf_free(tb);
}

void test_register_block_producer_hard_errors_with_sprint17(void)
{
    int pipefd[2];
    pid_t child;
    pid_t waited;
    int status;
    Bytebuf output;

    bytebuf_init(&output);
    SAG_ASSERT_EQ_I64(pipe(pipefd), 0);
    (void)fflush(NULL);
    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        Registers r;
        RegVal block;
        (void)close(pipefd[0]);
        (void)dup2(pipefd[1], STDERR_FILENO);
        (void)close(pipefd[1]);
        sag_reg_init(&r);
        reg_test_value(&block, SAG_REG_BLOCKWISE, (const u8 *)"x", 1U);
        sag_reg_set(&r, 'a', &block);
        _exit(99);
    }
    (void)close(pipefd[1]);
    for (;;) {
        u8 chunk[128];
        ssize_t count = read(pipefd[0], chunk, sizeof(chunk));
        if (count > 0) {
            bytebuf_append(&output, chunk, (size_t)count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)close(pipefd[0]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    SAG_ASSERT_EQ_I64(WEXITSTATUS(status), SAG_EXIT_BUG);
    bytebuf_append(&output, "", 1U);
    SAG_ASSERT(strstr((const char *)output.data, "Sprint 17") != NULL);
    bytebuf_free(&output);
}
