#include "fl/flruntime_int.h"

#include "fl/compile.h"
#include "fl/parse.h"
#include "fl/std.h"
#include "fl/trace.h"
#include "ui/message.h"
#include "util/buf.h"
#include "util/log.h"

static const char *eval_first_line(const Bytebuf *text, char *out,
                                   size_t cap)
{
    size_t n = 0U;

    if (cap == 0U)
        return "Fletch error";
    while (n < text->len && text->data[n] != '\n' && n + 1U < cap) {
        out[n] = (char)text->data[n];
        n++;
    }
    out[n] = '\0';
    return n == 0U ? "Fletch error" : out;
}

CmdStatus fl_runtime_eval(FlRuntime *rt, const char *source, u32 len)
{
    const char *owned;
    u32 file_id;
    FlProgram program;
    FlFn *fn;
    FlValue result = FL_NIL_V;
    FlOrigin origin;
    Ed *ed;

    if (rt == NULL || source == NULL || rt->ed == NULL)
        return SAG_CMD_ERR_ARG;
    ed = rt->ed;
    if (rt->diag.nfiles >= FL_DIAG_MAX_FILES) {
        sag_msg(ed, SAG_MSG_ERROR,
                "Fletch evaluation source limit reached for this session");
        return SAG_CMD_ERR_STATE;
    }
    owned = arena_strndup(&rt->arena, source, len);
    rt->diag_error = false;
    rt->diag_message[0] = '\0';
    file_id = fl_diag_add_file(&rt->diag, "<E>", owned, len);
    program = fl_parse(&rt->arena, &rt->diag, &rt->interner, owned, len,
                       file_id);
    if (program.had_error) {
        sag_msg(ed, SAG_MSG_ERROR, "%s",
                rt->diag_message[0] == '\0' ? "Fletch parse failed" :
                                              rt->diag_message);
        return SAG_CMD_ERR_ARG;
    }
    origin = (FlOrigin){(u8)FL_ORIGIN_CONFIG, 0U,
                        (u32)FL_CAP_FS_READ | (u32)FL_CAP_FS_WRITE |
                            (u32)FL_CAP_SHELL | (u32)FL_CAP_NET};
    fn = fl_compile_repl(&rt->vm, &rt->diag, &program, file_id, origin);
    if (fn == NULL) {
        sag_msg(ed, SAG_MSG_ERROR, "%s",
                rt->diag_message[0] == '\0' ? "Fletch compile failed" :
                                              rt->diag_message);
        return SAG_CMD_ERR_ARG;
    }
    if (!fl_vm_run(&rt->vm, fn, &result)) {
        Bytebuf trace;
        char line[256];

        bytebuf_init(&trace);
        fl_trace_render(&rt->vm, result, &trace);
        sag_log(SAG_LOG_ERROR, "Fletch E-mode error: %.*s", (int)trace.len,
                trace.data == NULL ? "" : (const char *)trace.data);
        sag_msg(ed, SAG_MSG_ERROR, "%s",
                eval_first_line(&trace, line, sizeof(line)));
        bytebuf_free(&trace);
        return SAG_CMD_ERR_STATE;
    }
    if (result.t != (u8)FL_NIL) {
        Bytebuf rendered;

        bytebuf_init(&rendered);
        if (!fl_fmt_repl(&rt->vm, &rendered, result, 8U)) {
            bytebuf_free(&rendered);
            sag_msg(ed, SAG_MSG_ERROR, "Fletch result could not be rendered");
            return SAG_CMD_ERR_STATE;
        }
        sag_msg(ed, SAG_MSG_INFO, "%.*s", (int)rendered.len,
                rendered.data == NULL ? "" : (const char *)rendered.data);
        bytebuf_free(&rendered);
    }
    return SAG_CMD_OK;
}
