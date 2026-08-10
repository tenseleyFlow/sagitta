#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit/ed.h"
#include "edit/opt.h"
#include "fl/flapi.h"
#include "fl/flruntime.h"
#include "fl/gc.h"
#include "fl/handle.h"
#include "fl/vm.h"
#include "ui/cmdline.h"

static u32 runtime_calls;
static u32 runtime_nargs;
static bool runtime_payload_ok;
static u32 runtime_buffer_ids[8];
static u32 runtime_buffer_ids_len;
static Buffer *runtime_reentrant_buffer;
static char runtime_mode_payloads[8][4];
static u32 runtime_mode_payloads_len;
static bool runtime_reentrant_event;

static bool runtime_probe(FlVm *vm, FlValue *args, u32 nargs, FlValue *out)
{
    runtime_calls++;
    runtime_nargs = nargs;
    if (nargs == 2U) {
        Buffer *buffer = fl_h_buf(vm, args[0]);
        Buffer *span_buffer = NULL;
        Span span = {0U, 0U};

        runtime_payload_ok = buffer != NULL &&
                             fl_h_span(vm, args[1], &span_buffer, &span) &&
                             span_buffer == buffer && span.lo <= span.hi;
        if (buffer != NULL && runtime_buffer_ids_len <
                              SAG_ARRAY_LEN(runtime_buffer_ids))
            runtime_buffer_ids[runtime_buffer_ids_len++] = buffer->id;
        if (runtime_reentrant_buffer != NULL) {
            sag_fl_hook_note_change(vm->ed, runtime_reentrant_buffer, 0U);
            runtime_reentrant_buffer = NULL;
        }
    } else if (nargs == 1U && args[0].t == (u8)FL_STR) {
        runtime_payload_ok = true;
        if (runtime_mode_payloads_len <
                                  SAG_ARRAY_LEN(runtime_mode_payloads)) {
            FlStr *s = (FlStr *)args[0].as.o;
            u32 n = s->len < 3U ? s->len : 3U;

            (void)memcpy(runtime_mode_payloads[runtime_mode_payloads_len],
                         s->b, n);
            runtime_mode_payloads[runtime_mode_payloads_len][n] = '\0';
            runtime_mode_payloads_len++;
        }
    } else if (nargs == 1U && args[0].t == (u8)FL_WIN) {
        runtime_payload_ok = fl_h_win(vm, args[0]) != NULL;
    } else {
        runtime_payload_ok = false;
    }
    if (runtime_reentrant_event)
        sag_fl_hook_fire(vm->ed, FL_EV_BUF_CHANGE, NULL, 0U);
    *out = FL_NIL_V;
    return true;
}

static bool runtime_error_field_contains(FlVm *vm, const char *field,
                                         const char *needle)
{
    FlValue key;
    FlValue got = FL_NIL_V;
    FlStr *s;
    size_t needle_len = strlen(needle);
    u32 i;

    if (vm->err.t != (u8)FL_MAP)
        return false;
    key = FL_OBJ_V(FL_STR, fl_str_new(vm, field, (u32)strlen(field)));
    if (!fl_map_get((FlMap *)vm->err.as.o, key, &got) ||
        got.t != (u8)FL_STR)
        return false;
    s = (FlStr *)got.as.o;
    if (s->len < needle_len)
        return false;
    for (i = 0U; i <= s->len - needle_len; i++) {
        if (memcmp(s->b + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static FlValue runtime_native_fn(FlVm *vm, const char *name,
                                 FlNativeFn native_fn)
{
    FlNative *native = fl_gc_alloc(vm, sizeof(*native), FL_NATIVE);

    native->fn = native_fn;
    native->name_id = sag_intern(vm->in, name, strlen(name));
    native->min_ar = 0U;
    native->max_ar = UINT8_MAX;
    native->caps = 0U;
    return FL_OBJ_V(FL_NATIVE, native);
}

static FlValue runtime_native(FlVm *vm, const char *name)
{
    return runtime_native_fn(vm, name, runtime_probe);
}

static bool runtime_mutate_then_fail(FlVm *vm, FlValue *args, u32 nargs,
                                     FlValue *out)
{
    FlValue call[3];
    FlValue ignored = FL_NIL_V;
    Buffer *buffer;
    const FlBindDesc *insert = fl_api_find("buf.insert", 10U);

    (void)args;
    (void)nargs;
    (void)out;
    buffer = sag_ed_doc(vm->ed);
    if (buffer == NULL || insert == NULL)
        return fl_raise(vm, "user", "hook test has no buffer");
    call[0] = fl_h_buf_make(vm->ed, buffer);
    call[1] = FL_INT_V((i64)sag_buf_len(buffer));
    call[2] = FL_OBJ_V(FL_STR, fl_str_new(vm, "x", 1U));
    fl_gc_protect(vm, call[2]);
    if (!fl_api_invoke(vm, insert, call, 3U, &ignored)) {
        fl_gc_release(vm, 1U);
        return false;
    }
    fl_gc_release(vm, 1U);
    return fl_raise(vm, "user", "hook failure");
}

void test_fl_runtime_is_persistent_and_on_registers(void)
{
    Ed ed;
    FlVm *vm;
    FlValue args[2];
    FlValue out = FL_NIL_V;

    runtime_calls = 0U;
    runtime_nargs = 0U;
    runtime_payload_ok = false;
    runtime_buffer_ids_len = 0U;
    runtime_reentrant_buffer = NULL;
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    SAG_ASSERT(vm == sag_fl_vm(&ed));
    vm->root_origin.kind = (u8)FL_ORIGIN_CONFIG;
    args[0] = FL_OBJ_V(FL_STR, fl_str_new(vm, "ws.open", 7U));
    args[1] = runtime_native(vm, "runtime.ws.open");
    SAG_ASSERT(fl_runtime_on(vm, args, 2U, &out));
    SAG_ASSERT_EQ_U64(out.t, FL_INT);
    SAG_ASSERT(out.as.i > 0);
    sag_fl_hook_workspace(&ed, FL_EV_WS_OPEN);
    SAG_ASSERT_EQ_U64(runtime_calls, 1U);
    SAG_ASSERT_EQ_U64(runtime_nargs, 1U);
    SAG_ASSERT(runtime_payload_ok);
    sag_ed_free(&ed);
}

void test_fl_runtime_coalesces_buffer_changes(void)
{
    Ed ed;
    FlVm *vm;
    EditCtx ec;
    u64 cursor_before;
    u32 i;

    runtime_calls = 0U;
    runtime_nargs = 0U;
    runtime_payload_ok = false;
    runtime_buffer_ids_len = 0U;
    runtime_reentrant_buffer = NULL;
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    (void)fl_hook_add(&ed.hooks, FL_ORIGIN_ID_CONFIG, FL_EV_BUF_CHANGE,
                      runtime_native(vm, "runtime.buf.change"));
    ec = sag_ed_edit_ctx(&ed);
    for (i = 0U; i < 4000U; i++)
        SAG_ASSERT(sag_edit_insert(&ec, BYTEOFF(i), (const u8 *)"x", 1U));
    sag_ed_finish_edit(&ed, &ec);
    SAG_ASSERT_EQ_U64(runtime_calls, 0U);
    sag_fl_hook_flush_change(&ed);
    SAG_ASSERT_EQ_U64(runtime_calls, 1U);
    SAG_ASSERT_EQ_U64(runtime_nargs, 2U);
    SAG_ASSERT(runtime_payload_ok);
    sag_fl_hook_flush_change(&ed);
    SAG_ASSERT_EQ_U64(runtime_calls, 1U);

    runtime_calls = 0U;
    runtime_payload_ok = false;
    (void)fl_hook_add(&ed.hooks, FL_ORIGIN_ID_CONFIG, FL_EV_CURSOR_MOVE,
                      runtime_native(vm, "runtime.cursor.move"));
    cursor_before = sag_fl_cursor_burst_state(ed.win);
    for (i = 0U; i < 4000U; i++)
        ed.win->cs.curs.data[ed.win->cs.primary].pos =
            BYTEOFF(3999U - i);
    SAG_ASSERT_EQ_U64(runtime_calls, 0U);
    sag_fl_hook_flush_cursor(&ed, ed.win, cursor_before);
    SAG_ASSERT_EQ_U64(runtime_calls, 1U);
    SAG_ASSERT_EQ_U64(runtime_nargs, 1U);
    SAG_ASSERT(runtime_payload_ok);
    sag_fl_hook_flush_cursor(&ed, ed.win,
                             sag_fl_cursor_burst_state(ed.win));
    SAG_ASSERT_EQ_U64(runtime_calls, 1U);
    sag_ed_free(&ed);
}

void test_fl_runtime_coalesces_each_buffer_in_workspace_order(void)
{
    Ed ed;
    FlVm *vm;
    Buffer *second;

    runtime_calls = 0U;
    runtime_buffer_ids_len = 0U;
    runtime_reentrant_buffer = NULL;
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    second = sag_ws_scratch_new(&ed, "*second*", 0U);
    SAG_ASSERT_NOT_NULL(second);
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    (void)fl_hook_add(&ed.hooks, FL_ORIGIN_ID_CONFIG, FL_EV_BUF_CHANGE,
                      runtime_native(vm, "runtime.buf.order"));
    sag_fl_hook_note_change(&ed, second, 8U);
    sag_fl_hook_note_change(&ed, &ed.buffer, 2U);
    sag_fl_hook_note_change(&ed, second, 3U);
    sag_fl_hook_flush_change(&ed);
    SAG_ASSERT_EQ_U64(runtime_calls, 2U);
    SAG_ASSERT_EQ_U64(runtime_buffer_ids_len, 2U);
    SAG_ASSERT_EQ_U64(runtime_buffer_ids[0], ed.buffer.id);
    SAG_ASSERT_EQ_U64(runtime_buffer_ids[1], second->id);
    sag_ed_free(&ed);
}

void test_fl_runtime_drops_reentrant_buffer_change_notifications(void)
{
    Ed ed;
    FlVm *vm;
    Buffer *second;

    runtime_calls = 0U;
    runtime_buffer_ids_len = 0U;
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    second = sag_ws_scratch_new(&ed, "*second*", 0U);
    SAG_ASSERT_NOT_NULL(second);
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    (void)fl_hook_add(&ed.hooks, FL_ORIGIN_ID_CONFIG, FL_EV_BUF_CHANGE,
                      runtime_native(vm, "runtime.buf.reentrant"));
    runtime_reentrant_buffer = second;
    sag_fl_hook_note_change(&ed, &ed.buffer, 1U);
    sag_fl_hook_flush_change(&ed);
    SAG_ASSERT_EQ_U64(runtime_calls, 1U);
    sag_fl_hook_flush_change(&ed);
    SAG_ASSERT_EQ_U64(runtime_calls, 1U);
    runtime_reentrant_buffer = NULL;
    sag_ed_free(&ed);

    runtime_calls = 0U;
    runtime_reentrant_event = true;
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    (void)fl_hook_add(&ed.hooks, FL_ORIGIN_ID_CONFIG, FL_EV_BUF_CHANGE,
                      runtime_native(vm, "runtime.buf.logdrop"));
    sag_test_capture_log();
    sag_fl_hook_fire(&ed, FL_EV_BUF_CHANGE, NULL, 0U);
    sag_fl_hook_fire(&ed, FL_EV_BUF_CHANGE, NULL, 0U);
    SAG_ASSERT_EQ_U64(runtime_calls, 2U);
    SAG_ASSERT_EQ_U64(sag_test_log_count(), 1U);
    SAG_ASSERT(sag_test_log_contains(
        SAG_LOG_WARN, "hook \"buf.change\" reentrant fire dropped"));
    runtime_reentrant_event = false;
    sag_ed_free(&ed);
}

void test_fl_runtime_contains_failing_hook_mutations(void)
{
    char path[] = "/tmp/sag-fl-hook-save-XXXXXX";
    struct stat st;
    Ed ed;
    FlVm *vm;
    u32 event;
    int fd;

    fd = mkstemp(path);
    SAG_ASSERT(fd >= 0);
    SAG_ASSERT_EQ_I64(close(fd), 0);
    SAG_ASSERT_EQ_I64(unlink(path), 0);
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    for (event = 0U; event < (u32)FL_EV__N; event++) {
        u32 id = fl_hook_add(
            &ed.hooks, FL_ORIGIN_ID_CONFIG, event,
            runtime_native_fn(vm, "runtime.hook.failure",
                              runtime_mutate_then_fail));
        FlHook *hook;
        u32 fire;

        for (fire = 0U; fire < 6U; fire++)
            sag_fl_hook_fire(&ed, (FlEvent)event, NULL, 0U);
        hook = &ed.hooks.v[ed.hooks.ledger.v[id - 1U].handle - 1U];
        SAG_ASSERT(hook->disabled);
        SAG_ASSERT_EQ_U64(hook->errs, 5U);
        SAG_ASSERT_EQ_U64(sag_buf_len(sag_ed_doc(&ed)), 0U);
        SAG_ASSERT_EQ_U64(sag_ed_doc(&ed)->undo->depth, 0U);
    }

    /* A throwing pre-save hook is contained: its edit rolls back and the
     * write still completes with the pre-hook bytes. */
    (void)fl_hook_add(&ed.hooks, FL_ORIGIN_ID_CONFIG, FL_EV_BUF_SAVE,
                      runtime_native_fn(vm, "runtime.save.failure",
                                        runtime_mutate_then_fail));
    SAG_ASSERT_EQ_U64(sag_ed_file_write_to(&ed, path, false), SAG_CMD_OK);
    SAG_ASSERT_EQ_I64(stat(path, &st), 0);
    SAG_ASSERT_EQ_U64((u64)st.st_size, 0U);
    SAG_ASSERT_EQ_U64(sag_buf_len(sag_ed_doc(&ed)), 0U);
    SAG_ASSERT_EQ_U64(sag_fl_eval(&ed, "1 + 1", 5U), SAG_CMD_OK);
    SAG_ASSERT_EQ_STR(ed.msg.text, "2");
    sag_ed_free(&ed);
    SAG_ASSERT_EQ_I64(unlink(path), 0);
}

void test_fl_runtime_eval_keeps_globals_between_entries(void)
{
    Ed ed;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    SAG_ASSERT_EQ_U64(sag_fl_eval(&ed, "let answer = 41", 15U),
                      SAG_CMD_OK);
    SAG_ASSERT_EQ_U64(sag_fl_eval(&ed, "answer + 1", 10U), SAG_CMD_OK);
    SAG_ASSERT(ed.msg.active);
    SAG_ASSERT_EQ_STR(ed.msg.text, "42");
    sag_ed_free(&ed);
}

void test_fl_runtime_cmdline_mode_hooks_fire_once_per_transition(void)
{
    Ed ed;
    FlVm *vm;

    runtime_calls = 0U;
    runtime_mode_payloads_len = 0U;
    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    vm = sag_fl_vm(&ed);
    SAG_ASSERT_NOT_NULL(vm);
    (void)fl_hook_add(&ed.hooks, FL_ORIGIN_ID_CONFIG, FL_EV_MODE_LEAVE,
                      runtime_native(vm, "runtime.mode.leave"));
    (void)fl_hook_add(&ed.hooks, FL_ORIGIN_ID_CONFIG, FL_EV_MODE_ENTER,
                      runtime_native(vm, "runtime.mode.enter"));
    sag_cmdline_open(&ed, SAG_PROMPT_CMD, NULL);
    sag_cmdline_close(&ed, false);
    SAG_ASSERT_EQ_U64(runtime_calls, 4U);
    SAG_ASSERT_EQ_U64(runtime_mode_payloads_len, 4U);
    SAG_ASSERT_EQ_STR(runtime_mode_payloads[0], "L");
    SAG_ASSERT_EQ_STR(runtime_mode_payloads[1], "E");
    SAG_ASSERT_EQ_STR(runtime_mode_payloads[2], "E");
    SAG_ASSERT_EQ_STR(runtime_mode_payloads[3], "L");
    sag_ed_free(&ed);
}

void test_fl_options_cover_builtins_and_raise_name_suggestions(void)
{
    static const char *const names[] = {
        "tabwidth", "wrap", "chord_timeout_ms", "clipboard.sync",
        "undo.bytes_max", "undo.break_on_newline",
        "registers.ring_depth", "errorbells", "ambiguous_wide",
        "scrolloff", "hooks.error_limit"
    };
    Ed ed;
    FlVm *vm;
    const OptProvider *provider;
    const char *listed[16];
    FlValue args[2];
    FlValue out = FL_NIL_V;
    OptVal value;
    u32 i;

    sag_ed_init(&ed);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    provider = sag_opt_provider(&ed);
    SAG_ASSERT_EQ_U64(provider->list(&ed, listed, SAG_ARRAY_LEN(listed)),
                      SAG_ARRAY_LEN(names));
    for (i = 0U; i < SAG_ARRAY_LEN(names); i++) {
        SAG_ASSERT_EQ_STR(listed[i], names[i]);
        SAG_ASSERT(provider->get(&ed, names[i], (u32)strlen(names[i]),
                                 &value));
    }

    vm = sag_fl_vm(&ed);
    args[0] = FL_OBJ_V(FL_STR, fl_str_new(vm, "tabwidth", 8U));
    args[1] = FL_INT_V(7);
    SAG_ASSERT(fl_api_invoke(vm, fl_api_find("opt.set", 7U), args, 2U,
                             &out));
    SAG_ASSERT_EQ_U64(ed.buffer.tabwidth, 7U);
    SAG_ASSERT(fl_api_invoke(vm, fl_api_find("opt.get", 7U), args, 1U,
                             &out));
    SAG_ASSERT_EQ_U64(out.t, FL_INT);
    SAG_ASSERT_EQ_I64(out.as.i, 7);

    args[0] = FL_OBJ_V(FL_STR, fl_str_new(vm, "tabwith", 7U));
    vm->err = FL_NIL_V;
    SAG_ASSERT(!fl_api_invoke(vm, fl_api_find("opt.get", 7U), args, 1U,
                              &out));
    SAG_ASSERT(runtime_error_field_contains(vm, "kind", "name"));
    SAG_ASSERT(runtime_error_field_contains(vm, "msg", "tabwidth"));
    sag_ed_free(&ed);
}
