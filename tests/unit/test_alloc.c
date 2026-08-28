#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/buf.h"

void test_alloc_release_contract(void)
{
    max_align_t *plain = yew_xmalloc(sizeof(*plain));
    u8 *zeroed = yew_xcalloc(8U, sizeof(*zeroed));
    u8 *grown = yew_xreallocarray(NULL, 4U, sizeof(*grown));
    char *copy = yew_xstrdup("tracked");
    char *resolved = yew_xrealpath(".");
    char *cwd = yew_xgetcwd();
    size_t i;

    YEW_ASSERT_EQ_U64((uintptr_t)plain % _Alignof(max_align_t), 0U);
    for (i = 0U; i < 8U; i++)
        YEW_ASSERT_EQ_U64(zeroed[i], 0U);
    grown[0] = 0xa5U;
    grown = yew_xrealloc(grown, 16U);
    YEW_ASSERT_EQ_U64(grown[0], 0xa5U);
    YEW_ASSERT_EQ_STR(copy, "tracked");
    YEW_ASSERT_NOT_NULL(resolved);
    YEW_ASSERT_NOT_NULL(cwd);
    yew_xfree(plain);
    yew_xfree(zeroed);
    yew_xfree(grown);
    yew_xfree(copy);
    yew_xfree(resolved);
    yew_xfree(cwd);
    yew_xfree(NULL);
}

void test_alloc_debug_counts_and_report_order(void)
{
#if YEW_ALLOC_DEBUG
    void *b1;
    void *b2;
    void *a;
    void *c;
    Bytebuf out;
    const char *b_row;
    const char *a_row;
    const char *c_row;

    yew_alloc_reset();
    b1 = yew_xmalloc_at(5U, "b.c", 10);
    b2 = yew_xmalloc_at(7U, "b.c", 10);
    a = yew_xcalloc_at(2U, 3U, "a.c", 20);
    a = yew_xrealloc_at(a, 9U, "realloc.c", 99);
    c = yew_xmalloc_at(4U, "c.c", 3);
    YEW_ASSERT_EQ_U64(yew_alloc_calls(), 5U);

    bytebuf_init(&out);
    yew_alloc_report(&out);
    bytebuf_push_u8(&out, 0U);
    b_row = strstr((const char *)out.data, "2 12 12 12 b.c:10\n");
    a_row = strstr((const char *)out.data, "2 15 9 9 a.c:20\n");
    c_row = strstr((const char *)out.data, "1 4 4 4 c.c:3\n");
    YEW_ASSERT_NOT_NULL(b_row);
    YEW_ASSERT_NOT_NULL(a_row);
    YEW_ASSERT_NOT_NULL(c_row);
    YEW_ASSERT(a_row < b_row);
    YEW_ASSERT(b_row < c_row);
    YEW_ASSERT_NULL(strstr((const char *)out.data, "realloc.c:99"));

    yew_xfree(out.data);
    yew_xfree(b1);
    yew_xfree(b2);
    yew_xfree(a);
    yew_xfree(c);
#else
    yew_alloc_reset();
    YEW_ASSERT_EQ_U64(yew_alloc_calls(), 0U);
#endif
}

void test_alloc_debug_reset_and_fixed_overflow_site(void)
{
#if YEW_ALLOC_DEBUG
    void *old;
    void *blocks[4097];
    Bytebuf out;
    size_t i;

    yew_alloc_reset();
    old = yew_xmalloc_at(19U, "old.c", 1);
    yew_alloc_reset();
    yew_xfree_at(old, "free.c", 2);
    YEW_ASSERT_EQ_U64(yew_alloc_calls(), 0U);

    for (i = 0U; i < YEW_ARRAY_LEN(blocks); i++)
        blocks[i] = yew_xmalloc_at(1U, "sites.c", (int)i + 1);
    YEW_ASSERT_EQ_U64(yew_alloc_calls(), YEW_ARRAY_LEN(blocks));
    bytebuf_init(&out);
    yew_alloc_report(&out);
    bytebuf_push_u8(&out, 0U);
    YEW_ASSERT_NOT_NULL(strstr((const char *)out.data,
                               "1 1 1 1 <overflow>:0\n"));
    yew_xfree(out.data);
    for (i = 0U; i < YEW_ARRAY_LEN(blocks); i++)
        yew_xfree(blocks[i]);
#else
    YEW_ASSERT_EQ_U64(yew_alloc_calls(), 0U);
#endif
}

void test_alloc_debug_exit_report(void)
{
#if YEW_ALLOC_DEBUG
    char path[] = "build/tmp/alloc-report-XXXXXX";
    char bytes[1024];
    pid_t child;
    int fd;
    int status;
    ssize_t n;

    fd = mkstemp(path);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    child = fork();
    YEW_ASSERT(child >= 0);
    if (child == 0) {
        void *p;

        if (setenv("YEW_ALLOC_OUT", path, 1) != 0)
            _exit(101);
        yew_alloc_reset();
        p = yew_xmalloc_at(23U, "exit.c", 9);
        yew_xfree_at(p, "exit.c", 10);
        exit(0);
    }
    YEW_ASSERT_EQ_I64(waitpid(child, &status, 0), child);
    YEW_ASSERT(WIFEXITED(status));
    YEW_ASSERT_EQ_I64(WEXITSTATUS(status), 0);
    fd = open(path, O_RDONLY);
    YEW_ASSERT(fd >= 0);
    n = read(fd, bytes, sizeof(bytes) - 1U);
    YEW_ASSERT(n > 0);
    bytes[n] = '\0';
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_NOT_NULL(strstr(bytes, "# yew allocation report v1\n"));
    YEW_ASSERT_NOT_NULL(strstr(bytes, "1 23 0 23 exit.c:9\n"));
    YEW_ASSERT_EQ_I64(unlink(path), 0);
#else
    YEW_ASSERT_EQ_U64(yew_alloc_calls(), 0U);
#endif
}
