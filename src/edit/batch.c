#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "edit/batch.h"
#include "edit/batch_test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "fl/diag.h"
#include "fl/flconf.h"
#include "fl/flruntime.h"
#include "fl/flruntime_int.h"
#include "fl/gc.h"
#include "fl/trace.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "text/journal.h"
#include "text/piece.h"
#include "text/undo.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"

typedef struct BatchLogCtx {
    bool quiet;
    SagBatchTestState *test;
} BatchLogCtx;

typedef struct InteractiveRow {
    const char *name;
    const char *alternative;
} InteractiveRow;

static const InteractiveRow interactive_rows[] = {
    {"ed.ui.message_expand", "no batch alternative"},
    {"ed.cmdline.accept", "use ed.run(name, args)"},
    {"ed.cmdline.cancel", "use ed.run(name, args)"},
    {"ed.file.open", "use buf.open(path)"},
    {"ed.group.rename", "pass a name to a non-interactive group command"},
    {"ed.group.new", "construct the group with explicit arguments"},
    {"ed.group.edit", "no batch alternative"},
    {"ed.find.file", "use io.glob(pattern)"},
    {"ed.find.buffer", "use buf.list()"},
    {"ed.undo.branches", "use ed.run(\"ed.edit.undo\") or redo"},
    {"ed.search.open", "use b.find(re)"},
    {"ed.search.open_back", "use b.find(re)"},
    {"ed.macro.record", "recording needs keys; none exist"},
    {"ed.ai.open", "no batch alternative"},
    {"ed.mode.enter", "use ed.run(name, args)"},
};

static void flush_stdout(void)
{
    (void)fflush(stdout);
}

static void batch_log_write(void *user, SagLogLevel level, const char *msg)
{
    const BatchLogCtx *ctx = (const BatchLogCtx *)user;

    if (ctx != NULL && ctx->test != NULL)
        sag_batch_test_note_log(ctx->test, level, msg == NULL ? "" : msg);
    if (level < SAG_LOG_WARN)
        return;
    if (ctx != NULL && ctx->quiet && level < SAG_LOG_ERROR)
        return;
    (void)fprintf(stderr, "sagitta: %s\n", msg == NULL ? "" : msg);
}

static void batch_diag(void *user, FlDiagLevel level, FlSpan span,
                       const char *msg, const char *rendered)
{
    const BatchLogCtx *ctx = (const BatchLogCtx *)user;
    const char *text = rendered == NULL ? msg : rendered;

    (void)span;
    if (level != FL_DIAG_ERROR && ctx != NULL && ctx->quiet)
        return;
    if (text != NULL) {
        (void)fputs(text, stderr);
        if (text[0] != '\0' && text[strlen(text) - 1U] != '\n')
            (void)fputc('\n', stderr);
    }
}

static bool read_all_fd(int fd, Bytebuf *out)
{
    bytebuf_init(out);
    for (;;) {
        u8 chunk[65536];
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n > 0) {
            if (out->len > UINT32_MAX - (size_t)n) {
                errno = EFBIG;
                bytebuf_free(out);
                return false;
            }
            bytebuf_append(out, chunk, (size_t)n);
        } else if (n == 0) {
            return true;
        } else if (errno != EINTR) {
            bytebuf_free(out);
            return false;
        }
    }
}

static bool read_path(const char *path, Bytebuf *out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    bool ok;
    int saved;

    if (fd < 0)
        return false;
    ok = read_all_fd(fd, out);
    saved = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        saved = errno;
        bytebuf_free(out);
    }
    errno = saved;
    return ok;
}

static bool binding_is_headless(const KeyId *seq, u32 n,
                                const Binding *binding, void *ctx)
{
    const CmdDesc *desc;
    bool *ok = (bool *)ctx;

    (void)seq;
    (void)n;
    desc = binding == NULL ? NULL : sag_cmd_desc(binding->cmd);
    if (desc != NULL && (desc->flags & SAG_CMD_INTERACTIVE) != 0U)
        *ok = false;
    return *ok;
}

bool sag_batch_selfcheck_ok(const Ed *ed, const char **why)
{
    u32 i;

    if (why != NULL)
        *why = NULL;
    if (ed == NULL) {
        if (why != NULL) *why = "null editor";
        return false;
    }
    if (ed->grid.front != NULL || ed->grid.back != NULL ||
        ed->grid.dmg != NULL) {
        if (why != NULL) *why = "grid initialized";
        return false;
    }
    if (ed->in.buf.data != NULL || ed->in.buf.len != 0U ||
        ed->in.buf.cap != 0U) {
        if (why != NULL) *why = "input initialized";
        return false;
    }
    if (!ed->tty.poisoned || ed->tty.rfd >= 0) {
        if (why != NULL) *why = "terminal not poisoned";
        return false;
    }
    if (ed->timers.len != 0U) {
        if (why != NULL) *why = "timer installed";
        return false;
    }
    for (i = 0U; i < ed->keys.n; i++) {
        bool ok = true;
        const Keymap *map = ed->keys.l[i];

        if (map != NULL && map->nodes.len != 0U &&
            !sag_keymap_visit(map, binding_is_headless, &ok) && !ok) {
            if (why != NULL) *why = "interactive command bound";
            return false;
        }
    }
    return true;
}

void sag_batch_selfcheck(Ed *ed)
{
    const char *why;

    if (!sag_batch_selfcheck_ok(ed, &why))
        SAG_BUG("batch startup self-check failed: %s", why);
}

const char *sag_batch_command_alternative(const char *name,
                                          const CmdCtx *ctx)
{
    size_t i;

    if (name == NULL)
        return NULL;
    if (strcmp(name, "ed.mode.enter") == 0) {
        if (ctx == NULL || ctx->sarg == NULL || ctx->sarg_len != 1U ||
            ctx->sarg[0] != 'E')
            return NULL;
    }
    for (i = 0U; i < SAG_ARRAY_LEN(interactive_rows); i++)
        if (strcmp(interactive_rows[i].name, name) == 0)
            return interactive_rows[i].alternative;
    return NULL;
}

static FlValue batch_string(FlVm *vm, const char *s)
{
    size_t len = s == NULL ? 0U : strlen(s);

    if (len > UINT32_MAX)
        SAG_BUG("batch global string is too large");
    return FL_OBJ_V(FL_STR, fl_str_new(vm, s == NULL ? "" : s, (u32)len));
}

static void set_global(FlVm *vm, const char *name, FlValue value)
{
    u32 id = sag_intern(vm->in, name, strlen(name));

    fl_gc_protect(vm, value);
    (void)fl_map_set(vm, vm->globals, FL_INT_V((i64)id), value);
    fl_gc_release(vm, 1U);
}

static FlValue string_list(FlVm *vm, const char *const *v, size_t n)
{
    FlList *list = fl_list_new(vm);
    size_t i;

    fl_gc_protect(vm, FL_OBJ_V(FL_LIST, list));
    for (i = 0U; i < n; i++)
        (void)fl_list_push(vm, list, batch_string(vm, v[i]));
    fl_gc_release(vm, 1U);
    return FL_OBJ_V(FL_LIST, list);
}

static void install_globals(Ed *ed, const BatchOpts *opts,
                            const char *script_path)
{
    FlVm *vm = sag_fl_vm(ed);

    if (vm == NULL)
        SAG_BUG("batch runtime has no VM");
    set_global(vm, "args", string_list(vm, opts->args, opts->nargs));
    set_global(vm, "script_path", batch_string(vm, script_path));
    set_global(vm, "files", string_list(vm, opts->files, opts->nfiles));
    set_global(vm, "batch", FL_BOOL_V(true));
}

static bool append_stdin_buffer(Ed *ed, const Bytebuf *bytes, bool first)
{
    Buffer *buffer;

    if (first)
        return sag_ed_open_memory(ed, bytes->data, bytes->len, "[stdin]");
    buffer = sag_ws_scratch_new(ed, "[stdin]", 0U);
    if (buffer == NULL)
        return false;
    if (bytes->len != 0U)
        sag_textbuf_insert(buffer->tb, BYTEOFF(0U), bytes->data,
                           (u64)bytes->len);
    sag_undo_mark_saved(buffer->undo);
    sag_fl_hook_buffer(ed, FL_EV_BUF_OPEN, buffer);
    return true;
}

static const char *load_error_text(SagLoadErr error)
{
    switch (error) {
    case SAG_LOAD_EACCES: return "permission denied";
    case SAG_LOAD_EISDIR: return "is a directory";
    case SAG_LOAD_TOO_LARGE: return "file is too large";
    case SAG_LOAD_IO: return "input/output error";
    case SAG_LOAD_OK:
    case SAG_LOAD_ENOENT: break;
    }
    return "unknown load error";
}

static bool open_batch_files(Ed *ed, const BatchOpts *opts)
{
    size_t i;

    if (opts->nfiles == 0U)
        return sag_ed_open_scratch(ed);
    for (i = 0U; i < opts->nfiles; i++) {
        const char *path = opts->files[i];

        if (strcmp(path, "-") == 0) {
            Bytebuf bytes;

            if (ed->batch_stdin_claimed) {
                (void)fprintf(stderr,
                              "sagitta: error: stdin may be opened only once\n");
                return false;
            }
            ed->batch_stdin_claimed = true;
            if (!read_all_fd(STDIN_FILENO, &bytes)) {
                (void)fprintf(stderr,
                              "sagitta: error: cannot read stdin: %s\n",
                              strerror(errno));
                return false;
            }
            if (!append_stdin_buffer(ed, &bytes, i == 0U)) {
                bytebuf_free(&bytes);
                return false;
            }
            bytebuf_free(&bytes);
        } else if (i == 0U) {
            SagLoadErr load = sag_ed_open(ed, path);

            if (load != SAG_LOAD_OK) {
                (void)fprintf(stderr, "sagitta: error: cannot open %s: %s\n",
                              path, load_error_text(load));
                return false;
            }
        } else {
            Buffer *buffer = sag_ws_file_buf(ed, path);

            if (buffer == NULL || sag_buf_hydrate(ed, buffer) != 0) {
                (void)fprintf(stderr,
                              "sagitta: error: cannot open %s: input/output error\n",
                              path);
                return false;
            }
            sag_fl_hook_buffer(ed, FL_EV_BUF_OPEN, buffer);
        }
    }
    return true;
}

static void render_script_error(FlVm *vm)
{
    Bytebuf trace;
    size_t first = 0U;

    bytebuf_init(&trace);
    fl_trace_render(vm, vm->err, &trace);
    if (trace.len >= 7U && memcmp(trace.data, "error: ", 7U) == 0)
        first = 7U;
    (void)fputs("sagitta: script failed: ", stderr);
    (void)fwrite(trace.data + first, 1U, trace.len - first, stderr);
    if (trace.len == first || trace.data[trace.len - 1U] != '\n')
        (void)fputc('\n', stderr);
    bytebuf_free(&trace);
}

static void warn_dirty(Ed *ed, bool quiet)
{
    u32 i;
    u32 dirty = 0U;

    for (i = 0U; i < ed->ws.nbufs; i++)
        if (sag_buf_dirty(ed->ws.bufs[i]))
            dirty++;
    if (dirty != 0U && !quiet) {
        (void)fprintf(stderr, "warning: %u buffer%s modified and not saved: ",
                      (unsigned)dirty, dirty == 1U ? "" : "s");
        dirty = 0U;
        for (i = 0U; i < ed->ws.nbufs; i++) {
            if (!sag_buf_dirty(ed->ws.bufs[i]))
                continue;
            if (dirty++ != 0U)
                (void)fputs(", ", stderr);
            (void)fputs(sag_buf_label(ed->ws.bufs[i]), stderr);
        }
        (void)fputc('\n', stderr);
    }
}

static void discard_journals(Ed *ed)
{
    u32 i;

    for (i = 0U; i < ed->ws.nbufs; i++) {
        Buffer *buffer = ed->ws.bufs[i];

        if (buffer->jrn != NULL) {
            sag_journal_discard(buffer->jrn);
            buffer->jrn = NULL;
        }
    }
}

int sag_batch_run(const BatchOpts *opts)
{
    BatchLogCtx log_ctx;
    SagLogSink mirror;
    SagEdStartup startup;
    Bytebuf source;
    char *script_path = NULL;
    Ed ed;
    FlFn *script;
    SagBatchTestState test_state;
    int result = SAG_EXIT_ERR;
    bool ed_ready = false;
    bool test_ready = false;
    bool test_installed = false;
    bool workspace_hook = false;

    if (opts == NULL || opts->script == NULL)
        return SAG_EXIT_ERR;
    if (!read_path(opts->script, &source)) {
        (void)fprintf(stderr, "sagitta: error: cannot read script %s: %s\n",
                      opts->script, strerror(errno));
        flush_stdout();
        return SAG_EXIT_ERR;
    }
    script_path = realpath(opts->script, NULL);
    if (script_path == NULL)
        script_path = strdup(opts->script);
    if (script_path == NULL)
        SAG_BUG("cannot allocate batch script path");

    if (opts->test) {
        sag_batch_test_init(&test_state);
        test_ready = true;
    }
    log_ctx = (BatchLogCtx){opts->quiet,
                            test_ready ? &test_state : NULL};
    mirror = (SagLogSink){batch_log_write, &log_ctx};
    sag_log_set_mirror(&mirror);
    sag_bug_set_prehook(flush_stdout);

    sag_ed_init(&ed);
    ed_ready = true;
    ed.headless = true;
    sag_tty_poison(&ed.tty);
    sag_batch_selfcheck(&ed);
    ed.fl->diag.sink = batch_diag;
    ed.fl->diag.sink_ctx = &log_ctx;
    startup = (SagEdStartup){opts->config_path, opts->clean,
                             opts->no_workspace_config,
                             opts->trust_workspace};
    sag_config_init(&ed, &startup);
    (void)sag_config_load_all(&ed, NULL);
    if (!open_batch_files(&ed, opts)) {
        result = SAG_EXIT_IO;
        goto done;
    }
    sag_fl_hook_workspace(&ed, FL_EV_WS_OPEN);
    workspace_hook = true;
    install_globals(&ed, opts, script_path);
    if (opts->test) {
        if (!sag_batch_test_install(&test_state, sag_fl_vm(&ed)))
            SAG_BUG("cannot install batch assertion host");
        test_installed = true;
    }
    script = fl_compile_script(ed.fl, source.data, source.len, script_path);
    if (script == NULL) {
        result = SAG_EXIT_BATCH;
        goto done;
    }
    if (!fl_call_chunk(ed.fl, script, SAG_SRC_FLETCH)) {
        render_script_error(sag_fl_vm(&ed));
        result = SAG_EXIT_BATCH;
        goto done;
    }
    /* Product-level guard drill: the smoke lane seeds the forbidden call
     * only after script output exists, proving both exit 4 and the stdout
     * flush contract.  This is intentionally an environment selftest, not
     * a user-facing batch option. */
    if (getenv("SAG_BATCH_SELFTEST_TTY") != NULL)
        (void)sag_tty_signal_fd(&ed.tty);
    result = SAG_EXIT_OK;

done:
    if (test_installed && !sag_batch_test_finish(&test_state, -1))
        result = SAG_EXIT_BATCH;
    if (workspace_hook)
        sag_fl_hook_workspace(&ed, FL_EV_WS_CLOSE);
    warn_dirty(&ed, opts->quiet);
    discard_journals(&ed);
    flush_stdout();
    if (test_ready)
        sag_batch_test_free(&test_state);
    if (ed_ready)
        sag_ed_free(&ed);
    sag_bug_set_prehook(NULL);
    sag_log_set_mirror(NULL);
    free(script_path);
    bytebuf_free(&source);
    return result;
}
