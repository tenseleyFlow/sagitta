#include "fl/flruntime_int.h"

#include <stdio.h>
#include <string.h>

#include "fl/compile.h"
#include "fl/gc.h"
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

static FlOrigin runtime_origin(void)
{
    return (FlOrigin){(u8)FL_ORIGIN_CONFIG, 0U,
                      (u32)FL_CAP_FS_READ | (u32)FL_CAP_FS_WRITE |
                          (u32)FL_CAP_SHELL | (u32)FL_CAP_NET};
}

FlFn *fl_compile_str(FlRuntime *rt, const u8 *source, size_t len,
                     const char *label)
{
    const char *owned;
    const char *owned_label;
    u32 file_id;
    FlProgram program;

    if (rt == NULL || source == NULL || len > UINT32_MAX)
        return NULL;
    if (rt->diag.nfiles >= FL_DIAG_MAX_FILES) {
        if (rt->ed != NULL)
            sag_msg(rt->ed, SAG_MSG_ERROR,
                    "Fletch evaluation source limit reached for this session");
        return NULL;
    }
    owned = arena_strndup(&rt->arena, (const char *)source, len);
    owned_label = arena_strdup(&rt->arena,
                               label == NULL ? "<macro>" : label);
    rt->diag_error = false;
    rt->diag_message[0] = '\0';
    file_id = fl_diag_add_file(&rt->diag, owned_label, owned, len);
    program = fl_parse(&rt->arena, &rt->diag, &rt->interner, owned, len,
                       file_id);
    if (program.had_error || program.incomplete)
        return NULL;
    return fl_compile(&rt->vm, &rt->diag, &program, file_id,
                      runtime_origin());
}

static bool call_chunk_result(FlRuntime *rt, FlFn *fn, CmdSource source,
                              FlValue *result)
{
    CmdSource saved;
    bool ok;

    if (rt == NULL || fn == NULL || result == NULL || !rt->ready)
        return false;
    saved = rt->command_source;
    rt->command_source = source;
    if (rt->vm.nframes == 0U) {
        ok = fl_vm_run(&rt->vm, fn, result);
    } else {
        FlClosure *closure = fl_gc_alloc(&rt->vm, sizeof(*closure),
                                         FL_CLOSURE);

        closure->fn = fn;
        closure->up = NULL;
        closure->nup = 0U;
        closure->globals = rt->vm.globals;
        ok = fl_call(&rt->vm, FL_OBJ_V(FL_CLOSURE, closure), NULL, 0U,
                     result);
    }
    rt->command_source = saved;
    return ok;
}

bool fl_call_chunk(FlRuntime *rt, FlFn *fn, CmdSource source)
{
    FlValue result = FL_NIL_V;

    return call_chunk_result(rt, fn, source, &result);
}

bool fl_call_value(FlRuntime *rt, FlValue callable, CmdSource source)
{
    FlValue result = FL_NIL_V;
    FlOrigin saved_origin;
    CmdSource saved;
    bool ok;

    if (rt == NULL || !rt->ready ||
        (callable.t != (u8)FL_CLOSURE && callable.t != (u8)FL_NATIVE))
        return false;
    saved = rt->command_source;
    saved_origin = rt->vm.root_origin;
    rt->command_source = source;
    if (callable.t == (u8)FL_CLOSURE)
        rt->vm.root_origin = ((FlClosure *)callable.as.o)->fn->origin;
    ok = fl_call(&rt->vm, callable, NULL, 0U, &result);
    rt->vm.root_origin = saved_origin;
    rt->command_source = saved;
    return ok;
}

void fl_macro_cache_invalidate(FlRuntime *rt, u8 reg)
{
    FlMacroCache *entry;

    if (rt == NULL || reg < (u8)'a' || reg > (u8)'z')
        return;
    entry = &rt->macro_cache[reg - (u8)'a'];
    entry->fn = FL_NIL_V;
    entry->source = NULL;
    entry->len = 0U;
    entry->hash = 0U;
}

FlFn *fl_macro_compile_cached(FlRuntime *rt, u8 reg,
                              const u8 *source, size_t len)
{
    FlMacroCache *entry;
    FlFn *fn;
    u32 hash;
    char label[16];

    if (rt == NULL || source == NULL || len > UINT32_MAX ||
        reg < (u8)'a' || reg > (u8)'z')
        return NULL;
    entry = &rt->macro_cache[reg - (u8)'a'];
    hash = fl_hash_bytes((const char *)source, (u32)len);
    if (entry->fn.t == (u8)FL_FN && entry->hash == hash &&
        entry->len == len &&
        (len == 0U || memcmp(entry->source, source, len) == 0))
        return (FlFn *)entry->fn.as.o;
    (void)snprintf(label, sizeof(label), "<macro:%c>", (char)reg);
    fn = fl_compile_str(rt, source, len, label);
    if (fn == NULL)
        return NULL;
    entry->source = (const u8 *)rt->diag.files[rt->diag.nfiles - 1U].src;
    entry->len = len;
    entry->hash = hash;
    entry->fn = FL_OBJ_V(FL_FN, fn);
    return fn;
}

CmdStatus fl_runtime_eval(FlRuntime *rt, const char *source, u32 len)
{
    const char *owned;
    u32 file_id;
    FlProgram program;
    FlFn *fn;
    FlValue result = FL_NIL_V;
    FlOrigin origin;
    CmdSource source_kind;
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
    origin = runtime_origin();
    fn = fl_compile_repl(&rt->vm, &rt->diag, &program, file_id, origin);
    if (fn == NULL) {
        sag_msg(ed, SAG_MSG_ERROR, "%s",
                rt->diag_message[0] == '\0' ? "Fletch compile failed" :
                                              rt->diag_message);
        return SAG_CMD_ERR_ARG;
    }
    source_kind = rt->vm.nframes == 0U ? SAG_SRC_FLETCH : rt->command_source;
    if (!call_chunk_result(rt, fn, source_kind, &result)) {
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
