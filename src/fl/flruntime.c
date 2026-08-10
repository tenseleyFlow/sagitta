#include "fl/flruntime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "fl/diag.h"
#include "fl/flapi.h"
#include "fl/flruntime_int.h"
#include "fl/fltxn.h"
#include "fl/std.h"
#include "fl/trace.h"
#include "fl/vm.h"
#include "ui/message.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"

static void runtime_diag(void *ctx, FlDiagLevel level, FlSpan span,
                         const char *msg, const char *rendered)
{
    FlRuntime *rt = (FlRuntime *)ctx;
    (void)span;
    (void)rendered;
    sag_log(level == FL_DIAG_ERROR ? SAG_LOG_ERROR :
            level == FL_DIAG_WARNING ? SAG_LOG_WARN : SAG_LOG_INFO,
            "Fletch: %s", msg == NULL ? "diagnostic" : msg);
    if (rt != NULL && level == FL_DIAG_ERROR) {
        rt->diag_error = true;
        (void)snprintf(rt->diag_message, sizeof(rt->diag_message), "%s",
                       msg == NULL ? "Fletch diagnostic" : msg);
    }
}

static bool hook_call(void *ctx, FlVm *vm, FlValue fn,
                      const FlValue *args, u8 nargs, FlValue *err)
{
    FlValue ignored = FL_NIL_V;
    bool nested;
    bool ok;

    (void)ctx;
    nested = vm->txn.entry_active;
    if (nested) {
        if (!vm->host->edit_begin(vm))
            return false;
    } else if (!vm->host->run_begin(vm)) {
        return false;
    }
    ok = fl_call(vm, fn, args, (u32)nargs, &ignored);
    if (!ok && err != NULL)
        *err = vm->err;
    if (nested)
        (void)vm->host->edit_end(vm, ok);
    else
        (void)vm->host->run_end(vm, ok);
    return ok;
}

static bool hook_masked(void *ctx, u32 origin)
{
    return fl_origin_masked((const Ed *)ctx, origin);
}

static const char *first_line(const Bytebuf *text, char *out, size_t cap)
{
    size_t n = 0U;

    if (cap == 0U)
        return "hook error";
    while (n < text->len && text->data[n] != '\n' && n + 1U < cap) {
        out[n] = (char)text->data[n];
        n++;
    }
    out[n] = '\0';
    return n == 0U ? "hook error" : out;
}

static void hook_notice(void *ctx, FlHookNotice what, u32 event,
                        u32 ledger_id, u32 errs, FlValue err)
{
    Ed *ed = (Ed *)ctx;
    const char *name = fl_event_name(event);
    Bytebuf trace;
    char line[256];

    (void)ledger_id;
    if (name == NULL)
        name = "?";
    if (what == FL_HOOK_NOTICE_REENTRANT) {
        sag_log(SAG_LOG_WARN, "hook \"%s\" reentrant fire dropped", name);
        return;
    }
    if (what == FL_HOOK_NOTICE_DEPTH) {
        sag_log(SAG_LOG_ERROR, "hook \"%s\" exceeded nesting depth", name);
        sag_msg(ed, SAG_MSG_WARN,
                "hook \"%s\" error (%lu/%lu): nesting depth exceeded",
                name, (unsigned long)errs,
                (unsigned long)ed->hooks.error_limit);
        return;
    }
    if (what == FL_HOOK_NOTICE_DISABLED) {
        sag_log(SAG_LOG_ERROR, "hook \"%s\" disabled after %lu errors",
                name, (unsigned long)errs);
        sag_msg(ed, SAG_MSG_WARN,
                "hook \"%s\" disabled after %lu errors", name,
                (unsigned long)errs);
        return;
    }
    bytebuf_init(&trace);
    fl_trace_render(sag_fl_vm(ed), err, &trace);
    sag_log(SAG_LOG_ERROR, "hook \"%s\" error (%lu/%lu): %.*s", name,
            (unsigned long)errs,
            (unsigned long)ed->hooks.error_limit, (int)trace.len,
            trace.data == NULL ? "" : (const char *)trace.data);
    sag_msg(ed, SAG_MSG_WARN, "hook \"%s\" error (%lu/%lu): %s", name,
            (unsigned long)errs,
            (unsigned long)ed->hooks.error_limit,
            first_line(&trace, line, sizeof(line)));
    bytebuf_free(&trace);
}

bool sag_fl_runtime_init(Ed *ed)
{
    FlRuntime *rt;
    FlHookOps ops = {hook_call, hook_masked, hook_notice};
    u32 i;

    if (ed == NULL)
        return false;
    if (ed->fl != NULL)
        return true;
    rt = sag_xcalloc(1U, sizeof(*rt));
    rt->ed = ed;
    arena_init(&rt->arena);
    interner_init(&rt->interner, &rt->arena);
    fl_diag_init(&rt->diag, &rt->arena);
    fl_diag_set_sink(&rt->diag, runtime_diag, rt);
    if (!fl_vm_init(&rt->vm, &rt->arena, &rt->interner, &rt->diag)) {
        interner_free(&rt->interner);
        arena_free_all(&rt->arena);
        free(rt);
        return false;
    }
    fl_std_register(&rt->vm);
    fl_api_init();
    fl_ed_attach(&rt->vm, ed, &fl_host_editor);
    rt->command_source = SAG_SRC_FLETCH;
    for (i = 0U; i < (u32)SAG_ARRAY_LEN(rt->macro_cache); i++) {
        rt->macro_cache[i].fn = FL_NIL_V;
        fl_gc_host_root_add(&rt->vm, &rt->macro_cache[i].fn);
    }
    fl_hook_table_init(&ed->hooks, &ops, ed);
    fl_gc_root_provider(&rt->vm, fl_hook_mark, &ed->hooks);
    rt->ready = true;
    ed->fl = rt;
    ed->fl_idle_since_ms = -1;
    return true;
}

void sag_fl_runtime_free(Ed *ed)
{
    FlRuntime *rt;

    if (ed == NULL || ed->fl == NULL)
        return;
    rt = ed->fl;
    ed->fl = NULL;
    free(ed->fl_changes);
    ed->fl_changes = NULL;
    ed->fl_changes_len = 0U;
    ed->fl_changes_cap = 0U;
    fl_hook_table_free(&ed->hooks);
    fl_ed_detach(&rt->vm);
    fl_vm_free(&rt->vm);
    interner_free(&rt->interner);
    arena_free_all(&rt->arena);
    free(rt);
}

FlVm *sag_fl_vm(Ed *ed)
{
    return ed == NULL || ed->fl == NULL || !ed->fl->ready
               ? NULL : &ed->fl->vm;
}

CmdSource fl_runtime_cmd_source(const FlVm *vm)
{
    const Ed *ed;

    if (vm == NULL || (ed = vm->ed) == NULL || ed->fl == NULL)
        return SAG_SRC_FLETCH;
    return ed->fl->command_source;
}

CmdStatus sag_fl_eval(Ed *ed, const char *source, u32 len)
{
    if (ed == NULL || source == NULL || ed->fl == NULL)
        return SAG_CMD_ERR_ARG;
    return fl_runtime_eval(ed->fl, source, len);
}

bool fl_runtime_on(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    FlStr *event;
    u32 id;
    u32 origin;

    if (vm == NULL || vm->ed == NULL)
        return vm == NULL ? false :
               fl_raise(vm, "handle", "on: no editor is attached");
    if (nargs != 2U || args[0].t != (u8)FL_STR ||
        (args[1].t != (u8)FL_CLOSURE && args[1].t != (u8)FL_NATIVE))
        return fl_raise(vm, "type", "on expects (event string, function)");
    event = (FlStr *)args[0].as.o;
    if (!fl_event_parse(event->b, event->len, &id))
        return fl_raise(vm, "name", "unknown editor event '%.*s'",
                        (int)event->len, event->b);
    origin = fl_origin_of_frame(vm);
    if (origin == FL_ORIGIN_ID_NONE)
        return fl_raise(vm, "handle", "on: callback has no editor origin");
    *out = FL_INT_V((i64)fl_hook_add(&vm->ed->hooks, origin, id, args[1]));
    return true;
}

void sag_fl_hook_fire(Ed *ed, FlEvent event,
                      const FlValue *args, u8 nargs)
{
    FlVm *vm = sag_fl_vm(ed);

    if (vm != NULL)
        fl_hook_fire(&ed->hooks, vm, (u32)event, args, nargs);
}

void sag_fl_hook_buffer(Ed *ed, FlEvent event, Buffer *buffer)
{
    FlValue arg;

    if (ed == NULL || buffer == NULL || sag_fl_vm(ed) == NULL)
        return;
    arg = fl_h_buf_make(ed, buffer);
    sag_fl_hook_fire(ed, event, &arg, 1U);
}

void sag_fl_hook_mode(Ed *ed, FlEvent event, const char *mode)
{
    FlVm *vm = sag_fl_vm(ed);
    FlValue arg;

    if (vm == NULL || mode == NULL)
        return;
    arg = FL_OBJ_V(FL_STR, fl_str_new(vm, mode, (u32)strlen(mode)));
    sag_fl_hook_fire(ed, event, &arg, 1U);
}

void sag_fl_hook_window(Ed *ed, FlEvent event, Win *win)
{
    FlValue arg;

    if (ed == NULL || win == NULL || sag_fl_vm(ed) == NULL)
        return;
    arg = fl_h_win_make(ed, win);
    sag_fl_hook_fire(ed, event, &arg, 1U);
}

static u64 cursor_hash_word(u64 hash, u64 word)
{
    u32 i;

    for (i = 0U; i < 8U; i++) {
        hash ^= (u8)(word >> (i * 8U));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

u64 sag_fl_cursor_burst_state(const Win *win)
{
    u64 hash = UINT64_C(14695981039346656037);
    size_t i;

    if (win == NULL)
        return 0U;
    hash = cursor_hash_word(hash, win->id);
    hash = cursor_hash_word(hash, win->cs.curs.len);
    hash = cursor_hash_word(hash, win->cs.primary);
    for (i = 0U; i < win->cs.curs.len; i++) {
        const Cursor *cursor = &win->cs.curs.data[i];

        hash = cursor_hash_word(hash, win->cs.stamps.data[i]);
        hash = cursor_hash_word(hash, cursor->pos.v);
        hash = cursor_hash_word(hash, cursor->anchor.v);
        hash = cursor_hash_word(hash, cursor->goal_col.v);
    }
    return hash;
}

void sag_fl_hook_flush_cursor(Ed *ed, Win *win, u64 before)
{
    if (ed != NULL && win != NULL && win == ed->win &&
        sag_fl_cursor_burst_state(win) != before)
        sag_fl_hook_window(ed, FL_EV_CURSOR_MOVE, win);
}

void sag_fl_hook_workspace(Ed *ed, FlEvent event)
{
    FlVm *vm = sag_fl_vm(ed);
    const char *root;
    FlValue arg;

    if (vm == NULL)
        return;
    root = sag_ws_root(ed);
    arg = FL_OBJ_V(FL_STR, fl_str_new(vm, root, (u32)strlen(root)));
    sag_fl_hook_fire(ed, event, &arg, 1U);
}

void sag_fl_hook_note_change(Ed *ed, Buffer *buffer, u64 at)
{
    FlPendingChange *pending = NULL;
    u64 len;
    u64 hi;
    u32 i;

    if (ed == NULL || buffer == NULL || buffer->tb == NULL ||
        ed->fl_flushing_change)
        return;
    len = sag_buf_len(buffer);
    if (at > len)
        at = len;
    hi = at < len ? at + 1U : at;
    for (i = 0U; i < ed->fl_changes_len; i++) {
        if (ed->fl_changes[i].buffer_id == buffer->id) {
            pending = &ed->fl_changes[i];
            break;
        }
    }
    if (pending == NULL) {
        if (ed->fl_changes_len == ed->fl_changes_cap) {
            u32 cap = ed->fl_changes_cap == 0U ? 4U :
                                                ed->fl_changes_cap * 2U;

            if (cap < ed->fl_changes_cap)
                SAG_BUG("Fletch change coalescer overflow");
            ed->fl_changes = sag_xreallocarray(ed->fl_changes, cap,
                                                sizeof(*ed->fl_changes));
            ed->fl_changes_cap = cap;
        }
        pending = &ed->fl_changes[ed->fl_changes_len++];
        *pending = (FlPendingChange){buffer->id, at, hi};
        return;
    }
    if (at < pending->lo)
        pending->lo = at;
    if (hi > pending->hi)
        pending->hi = hi;
}

void sag_fl_hook_flush_change(Ed *ed)
{
    FlPendingChange *pending;
    u32 npending;
    u32 wi;

    if (ed == NULL || ed->fl_changes_len == 0U || ed->fl_flushing_change)
        return;
    if (sag_fl_vm(ed) == NULL) {
        ed->fl_changes_len = 0U;
        return;
    }
    npending = ed->fl_changes_len;
    pending = sag_xreallocarray(NULL, npending, sizeof(*pending));
    (void)memcpy(pending, ed->fl_changes, npending * sizeof(*pending));
    ed->fl_changes_len = 0U;
    ed->fl_flushing_change = true;
    /* Workspace order, not edit order: stable across cursor/hook traversal
     * details and therefore byte-identical for the same model state. */
    for (wi = 0U; wi < ed->ws.nbufs; wi++) {
        Buffer *buffer = ed->ws.bufs[wi];
        u32 pi;

        for (pi = 0U; pi < npending; pi++) {
            FlValue args[2];
            u64 len;
            u64 lo;
            u64 hi;

            if (buffer == NULL || buffer->id != pending[pi].buffer_id ||
                buffer->tb == NULL)
                continue;
            len = sag_buf_len(buffer);
            lo = pending[pi].lo > len ? len : pending[pi].lo;
            hi = pending[pi].hi > len ? len : pending[pi].hi;
            args[0] = fl_h_buf_make(ed, buffer);
            args[1] = fl_h_span_make(ed, buffer, lo, hi);
            sag_fl_hook_fire(ed, FL_EV_BUF_CHANGE, args, 2U);
            break;
        }
    }
    ed->fl_flushing_change = false;
    free(pending);
}
