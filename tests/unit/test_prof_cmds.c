#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "text/piece.h"
#include "util/buf.h"

static CmdStatus invoke_prof(Ed *ed, const char *name, i64 iarg,
                             const char *sarg)
{
    CmdCtx cx = {0};
    CmdId id = yew_cmd_lookup(name, (u32)strlen(name));

    YEW_ASSERT(id.v != 0U);
    cx.win = ed->win;
    cx.count = 1U;
    cx.iarg = iarg;
    cx.sarg = sarg;
    cx.sarg_len = sarg == NULL ? 0U : (u32)strlen(sarg);
    cx.source = YEW_SRC_TEST;
    return yew_ed_invoke(ed, id, &cx);
}

static Bytebuf materialize(const TextBuf *tb)
{
    Bytebuf out;
    TextIter it;

    bytebuf_init(&out);
    if (yew_textiter_begin(&it, tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (yew_textiter_advance(&it, tb));
    }
    bytebuf_push_u8(&out, 0U);
    out.len--;
    return out;
}

static Bytebuf read_path(const char *path)
{
    Bytebuf out;
    u8 block[1024];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    YEW_ASSERT(fd >= 0);
    bytebuf_init(&out);
    while ((n = read(fd, block, sizeof(block))) > 0)
        bytebuf_append(&out, block, (size_t)n);
    YEW_ASSERT_EQ_I64(n, 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    bytebuf_push_u8(&out, 0U);
    out.len--;
    return out;
}

void test_prof_commands_refuse_when_disabled(void)
{
    static const struct {
        const char *name;
        i64 iarg;
        const char *sarg;
    } commands[] = {
        {"ed.prof.report", 0, NULL},
        {"ed.prof.reset", 0, NULL},
        {"ed.prof.dump", 0, "/tmp/yew-prof-disabled"},
        {"ed.prof.mark", 0, "disabled"},
        {"ed.prof.frames", 1, NULL}
    };
    Ed ed;
    size_t i;

    YEW_ASSERT_EQ_I64(unsetenv("YEW_PROF"), 0);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    for (i = 0U; i < YEW_ARRAY_LEN(commands); i++) {
        YEW_ASSERT_EQ_I64(invoke_prof(&ed, commands[i].name,
                                     commands[i].iarg, commands[i].sarg),
                          YEW_CMD_ERR_STATE);
        YEW_ASSERT_EQ_STR(ed.msg.text,
                          "profiling is off; restart with YEW_PROF=1");
    }
    yew_ed_free(&ed);
}

void test_prof_commands_report_frames_mark_reset_and_dump(void)
{
    Ed ed;
    Bytebuf report;
    Bytebuf frames;
    Bytebuf dumped;
    char path[] = "/tmp/yew-prof-dump-XXXXXX";
    int fd;

    YEW_ASSERT_EQ_I64(setenv("YEW_PROF", "1", 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_PROF"), 0);
    YEW_ASSERT(yew_ed_open_scratch(&ed));

    YEW_ASSERT_EQ_I64(invoke_prof(&ed, "ed.prof.mark", 0, "unit-frame"),
                      YEW_CMD_OK);
    YEW_ASSERT(ed.prof.mark_pending);
    yew_prof_frame_begin(&ed.prof);
    yew_prof_phase(&ed.prof, YEW_PH_DISPATCH);
    yew_prof_frame_end(&ed.prof, 2U, 17U, YEW_PF_FULL_DAMAGE);
    YEW_ASSERT(!ed.prof.mark_pending);
    YEW_ASSERT((ed.prof.ring[0].flags & YEW_PF_MARK) != 0U);
    YEW_ASSERT((ed.prof.ring[0].flags & YEW_PF_FULL_DAMAGE) != 0U);

    YEW_ASSERT_EQ_I64(invoke_prof(&ed, "ed.prof.report", 0, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_STR(ed.win->buf->name, "*prof*");
    YEW_ASSERT((ed.win->buf->flags & YEW_BUF_READONLY) != 0U);
    report = materialize(ed.win->buf->tb);
    YEW_ASSERT_NOT_NULL(strstr((const char *)report.data,
                               "# yew prof v1  frames=1"));
    YEW_ASSERT_NOT_NULL(strstr((const char *)report.data,
                               "mark=unit-frame"));
    bytebuf_free(&report);

    YEW_ASSERT_EQ_I64(invoke_prof(&ed, "ed.prof.frames", 1, NULL),
                      YEW_CMD_OK);
    frames = materialize(ed.win->buf->tb);
    YEW_ASSERT_NOT_NULL(strstr((const char *)frames.data,
                               "# yew prof frames v1  frames=1 shown=1"));
    YEW_ASSERT_NOT_NULL(strstr((const char *)frames.data,
                               "FULL,MARK"));
    bytebuf_free(&frames);

    fd = mkstemp(path);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_EQ_I64(invoke_prof(&ed, "ed.prof.dump", 0, path),
                      YEW_CMD_OK);
    dumped = read_path(path);
    YEW_ASSERT_NOT_NULL(strstr((const char *)dumped.data,
                               "# yew prof v1  frames=1"));
    bytebuf_free(&dumped);
    YEW_ASSERT_EQ_I64(unlink(path), 0);

    YEW_ASSERT_EQ_I64(invoke_prof(&ed, "ed.prof.reset", 0, NULL),
                      YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(ed.prof.n, 0U);
    YEW_ASSERT_EQ_STR(ed.prof.mark, "");
    yew_ed_free(&ed);
}

void test_prof_commands_validate_mark_and_frame_count(void)
{
    Ed ed;
    static const char long_mark[] = "12345678901234567890123456789012";

    YEW_ASSERT_EQ_I64(setenv("YEW_PROF", "1", 1), 0);
    yew_ed_init(&ed);
    YEW_ASSERT_EQ_I64(unsetenv("YEW_PROF"), 0);
    YEW_ASSERT(yew_ed_open_scratch(&ed));
    YEW_ASSERT_EQ_I64(invoke_prof(&ed, "ed.prof.mark", 0, long_mark),
                      YEW_CMD_ERR_ARG);
    YEW_ASSERT_EQ_I64(invoke_prof(&ed, "ed.prof.mark", 0, "bad\nmark"),
                      YEW_CMD_ERR_ARG);
    YEW_ASSERT_EQ_I64(invoke_prof(&ed, "ed.prof.frames", -1, NULL),
                      YEW_CMD_ERR_ARG);
    yew_ed_free(&ed);
}
