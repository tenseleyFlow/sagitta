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

#include "args.h"
#include "edit/cmd.h"
#include "edit/ed.h"
#include "fl/diag.h"
#include "fl/flconf.h"
#include "fl/flruntime.h"
#include "fl/flruntime_int.h"
#include "fl/gc.h"
#include "fl/record.h"
#include "fl/trace.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "mod/mods.h"
#include "mod/plug/plug.h"
#include "text/file.h"
#include "text/journal.h"
#include "text/piece.h"
#include "text/undo.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"
#include "util/log.h"
#include "util/xdg.h"

typedef struct BatchLogCtx {
    bool quiet;
    YewBatchTestState *test;
} BatchLogCtx;

typedef struct InteractiveRow {
    const char *name;
    const char *alternative;
} InteractiveRow;

static void batch_prof_statement(void *ctx, u16 line)
{
    Ed *ed = ctx;

    (void)line;
    yew_prof_frame_end(&ed->prof, 0U, 0U, 0U);
    yew_prof_frame_begin(&ed->prof);
    yew_prof_phase(&ed->prof, YEW_PH_DISPATCH);
}

static char *batch_prof_path(void)
{
    const char *configured = getenv("YEW_PROF_OUT");
    char *dir;
    char *path;
    int need;

    if (configured != NULL && configured[0] != '\0')
        return yew_xstrdup(configured);
    dir = yew_xdg_state_dir();
    if (dir == NULL || !yew_mkdirs(dir, 0700U)) {
        yew_xfree(dir);
        return NULL;
    }
    need = snprintf(NULL, 0, "%s/prof-%ld.txt", dir, (long)getpid());
    if (need < 0)
        YEW_BUG("cannot format batch profiler path");
    path = yew_xmalloc((size_t)need + 1U);
    (void)snprintf(path, (size_t)need + 1U, "%s/prof-%ld.txt", dir,
                   (long)getpid());
    yew_xfree(dir);
    return path;
}

static bool batch_prof_dump(Ed *ed)
{
    Bytebuf out;
    char *path;
    YewSaveErr saved;

    if (!ed->prof.on)
        return true;
    ed->prof.batch = true;
    path = batch_prof_path();
    if (path == NULL) {
        yew_log(YEW_LOG_ERROR, "cannot create batch profiler path");
        return false;
    }
    bytebuf_init(&out);
    yew_prof_write(&ed->prof, &out);
    saved = yew_file_write_atomic(path, out.data, out.len, 0600U);
    bytebuf_free(&out);
    if (saved != YEW_SAVE_OK) {
        yew_log(YEW_LOG_ERROR, "cannot write profiler dump: %s", path);
        yew_xfree(path);
        return false;
    }
    yew_log(YEW_LOG_INFO, "profiler dump: %s", path);
    yew_xfree(path);
    return true;
}

static const char ai_enable_no_tty[] =
    "AI cannot be enabled non-interactively; set ai.enable in init.fl "
    "if you have read :ai privacy";

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
    {"ed.find.symbol", "no batch alternative"},
    {"ed.find.command", "no batch alternative"},
    {"ed.undo.branches", "use ed.run(\"ed.edit.undo\") or redo"},
    {"ed.search.open", "use b.find(re)"},
    {"ed.search.open_back", "use b.find(re)"},
    {"ed.compl.open", "no batch alternative"},
    {"ed.lsp.complete", "no batch alternative"},
    {"ed.lsp.hover", "no batch alternative"},
    {"ed.lsp.references", "no batch alternative"},
    {"ed.lsp.rename", "no batch alternative"},
    {"ed.lsp.symbols", "no batch alternative"},
    {"ed.lsp.signature", "no batch alternative"},
    {"ed.macro.record", "recording needs keys; none exist"},
    {"ed.ai.enable", ai_enable_no_tty},
    {"ed.ai.open", "no batch alternative"},
    {"ed.plug.list", "use `yew plug list`"},
    /* Sprint 51 registers the complete Git command vocabulary so scripts,
     * completion, and stripped builds agree before the Sprint 52/53 UI
     * lands.  Prompt-backed Git actions remain terminal-only. */
    {"ed.git.commit", "no batch alternative"},
    {"ed.git.commit.amend", "no batch alternative"},
    {"ed.git.push", "no batch alternative"},
    {"ed.git.push.force", "no batch alternative"},
    {"ed.git.branch.switch", "no batch alternative"},
    {"ed.git.branch.create", "no batch alternative"},
    {"ed.git.branch.delete", "no batch alternative"},
    {"ed.git.merge", "no batch alternative"},
    {"ed.git.reset", "no batch alternative"},
    {"ed.git.rebase.interactive", "no batch alternative"},
    {"ed.git.cherry_pick", "no batch alternative"},
    {"ed.git.revert", "no batch alternative"},
    {"ed.git.stash.push", "no batch alternative"},
    {"ed.git.stash.pop", "no batch alternative"},
    {"ed.git.tag", "no batch alternative"},
    {"ed.git.discard", "no batch alternative"},
    {"ed.git.file.delete", "no batch alternative"},
    {"ed.git.file.rename", "no batch alternative"},
    /* Hunk discard is undoable and therefore does not prompt in the editor,
     * but it still needs an editor buffer and has no headless equivalent. */
    {"ed.git.hunk.discard", "no batch alternative"},
    {"ed.mode.enter", "use ed.run(name, args)"},
};

static void flush_stdout(void)
{
    (void)fflush(stdout);
}

static void batch_log_write(void *user, YewLogLevel level, const char *msg)
{
    const BatchLogCtx *ctx = (const BatchLogCtx *)user;

    if (ctx != NULL && ctx->test != NULL)
        yew_batch_test_note_log(ctx->test, level, msg == NULL ? "" : msg);
    if (level < YEW_LOG_WARN)
        return;
    if (ctx != NULL && ctx->quiet && level < YEW_LOG_ERROR)
        return;
    (void)fprintf(stderr, "yew: %s\n", msg == NULL ? "" : msg);
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
    desc = binding == NULL ? NULL : yew_cmd_desc(binding->cmd);
    if (desc != NULL && (desc->flags & YEW_CMD_INTERACTIVE) != 0U)
        *ok = false;
    return *ok;
}

bool yew_batch_selfcheck_ok(const Ed *ed, const char **why)
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
            !yew_keymap_visit(map, binding_is_headless, &ok) && !ok) {
            if (why != NULL) *why = "interactive command bound";
            return false;
        }
    }
    return true;
}

void yew_batch_selfcheck(Ed *ed)
{
    const char *why;

    if (!yew_batch_selfcheck_ok(ed, &why))
        YEW_BUG("batch startup self-check failed: %s", why);
}

const char *yew_batch_command_alternative(const char *name,
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
    for (i = 0U; i < YEW_ARRAY_LEN(interactive_rows); i++)
        if (strcmp(interactive_rows[i].name, name) == 0)
            return interactive_rows[i].alternative;
    return NULL;
}

static FlValue batch_string(FlVm *vm, const char *s)
{
    size_t len = s == NULL ? 0U : strlen(s);

    if (len > UINT32_MAX)
        YEW_BUG("batch global string is too large");
    return FL_OBJ_V(FL_STR, fl_str_new(vm, s == NULL ? "" : s, (u32)len));
}

static void set_global(FlVm *vm, const char *name, FlValue value)
{
    u32 id = yew_intern(vm->in, name, strlen(name));

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
    FlVm *vm = yew_fl_vm(ed);

    if (vm == NULL)
        YEW_BUG("batch runtime has no VM");
    set_global(vm, "args", string_list(vm, opts->args, opts->nargs));
    set_global(vm, "script_path", batch_string(vm, script_path));
    set_global(vm, "files", string_list(vm, opts->files, opts->nfiles));
    set_global(vm, "batch", FL_BOOL_V(true));
}

static bool append_stdin_buffer(Ed *ed, const Bytebuf *bytes, bool first)
{
    Buffer *buffer;

    if (first)
        return yew_ed_open_memory(ed, bytes->data, bytes->len, "[stdin]");
    buffer = yew_ws_scratch_new(ed, "[stdin]", 0U);
    if (buffer == NULL)
        return false;
    if (bytes->len != 0U)
        yew_textbuf_insert(buffer->tb, BYTEOFF(0U), bytes->data,
                           (u64)bytes->len);
    yew_undo_mark_saved(buffer->undo);
    yew_fl_hook_buffer(ed, FL_EV_BUF_OPEN, buffer);
    return true;
}

static const char *load_error_text(YewLoadErr error)
{
    switch (error) {
    case YEW_LOAD_EACCES: return "permission denied";
    case YEW_LOAD_EISDIR: return "is a directory";
    case YEW_LOAD_TOO_LARGE: return "file is too large";
    case YEW_LOAD_IO: return "input/output error";
    case YEW_LOAD_OK:
    case YEW_LOAD_ENOENT: break;
    }
    return "unknown load error";
}

static bool open_batch_files(Ed *ed, const BatchOpts *opts)
{
    size_t i;

    if (opts->nfiles == 0U)
        return yew_ed_open_scratch(ed);
    for (i = 0U; i < opts->nfiles; i++) {
        const char *path = opts->files[i];

        if (strcmp(path, "-") == 0) {
            Bytebuf bytes;

            if (ed->batch_stdin_claimed) {
                (void)fprintf(stderr,
                              "yew: error: stdin may be opened only once\n");
                return false;
            }
            ed->batch_stdin_claimed = true;
            if (!read_all_fd(STDIN_FILENO, &bytes)) {
                (void)fprintf(stderr,
                              "yew: error: cannot read stdin: %s\n",
                              strerror(errno));
                return false;
            }
            if (!append_stdin_buffer(ed, &bytes, i == 0U)) {
                bytebuf_free(&bytes);
                return false;
            }
            bytebuf_free(&bytes);
        } else if (i == 0U) {
            YewLoadErr load = yew_ed_open(ed, path);

            if (load != YEW_LOAD_OK) {
                (void)fprintf(stderr, "yew: error: cannot open %s: %s\n",
                              path, load_error_text(load));
                return false;
            }
        } else {
            Buffer *buffer = yew_ws_file_buf(ed, path);

            if (buffer == NULL || yew_buf_hydrate(ed, buffer) != 0) {
                (void)fprintf(stderr,
                              "yew: error: cannot open %s: input/output error\n",
                              path);
                return false;
            }
            yew_fl_hook_buffer(ed, FL_EV_BUF_OPEN, buffer);
        }
    }
    return true;
}

static const FlStr *batch_error_field(const FlVm *vm, const char *name)
{
    u32 cursor = 0U;
    FlValue key;
    FlValue value;
    size_t len = strlen(name);

    if (vm == NULL || vm->err.t != (u8)FL_MAP)
        return NULL;
    while (fl_map_iter((const FlMap *)vm->err.as.o, &cursor, &key, &value)) {
        const FlStr *field;

        if (key.t != (u8)FL_STR || value.t != (u8)FL_STR)
            continue;
        field = (const FlStr *)key.as.o;
        if ((size_t)field->len == len && memcmp(field->b, name, len) == 0)
            return (const FlStr *)value.as.o;
    }
    return NULL;
}

static bool batch_ai_enable_refused(const FlVm *vm)
{
    static const char prefix[] =
        "\"ed.ai.enable\" requires a terminal and is not available "
        "under --batch; ";
    const FlStr *kind = batch_error_field(vm, "kind");
    const FlStr *msg = batch_error_field(vm, "msg");
    size_t prefix_len = sizeof(prefix) - 1U;
    size_t refusal_len = sizeof(ai_enable_no_tty) - 1U;

    return kind != NULL && kind->len == sizeof("capability") - 1U &&
           memcmp(kind->b, "capability", sizeof("capability") - 1U) == 0 &&
           msg != NULL && (size_t)msg->len == prefix_len + refusal_len &&
           memcmp(msg->b, prefix, prefix_len) == 0 &&
           memcmp(msg->b + prefix_len, ai_enable_no_tty, refusal_len) == 0;
}

static bool render_script_error(FlVm *vm)
{
    Bytebuf trace;
    size_t first = 0U;

    if (batch_ai_enable_refused(vm)) {
        (void)fprintf(stderr, "%s\n", ai_enable_no_tty);
        return true;
    }

    bytebuf_init(&trace);
    fl_trace_render(vm, vm->err, &trace);
    if (trace.len >= 7U && memcmp(trace.data, "error: ", 7U) == 0)
        first = 7U;
    (void)fputs("yew: script failed: ", stderr);
    (void)fwrite(trace.data + first, 1U, trace.len - first, stderr);
    if (trace.len == first || trace.data[trace.len - 1U] != '\n')
        (void)fputc('\n', stderr);
    bytebuf_free(&trace);
    return false;
}

static void warn_dirty(Ed *ed, bool quiet)
{
    u32 i;
    u32 dirty = 0U;

    for (i = 0U; i < ed->ws.nbufs; i++)
        if (yew_buf_dirty(ed->ws.bufs[i]))
            dirty++;
    if (dirty != 0U && !quiet) {
        (void)fprintf(stderr, "warning: %u buffer%s modified and not saved: ",
                      (unsigned)dirty, dirty == 1U ? "" : "s");
        dirty = 0U;
        for (i = 0U; i < ed->ws.nbufs; i++) {
            if (!yew_buf_dirty(ed->ws.bufs[i]))
                continue;
            if (dirty++ != 0U)
                (void)fputs(", ", stderr);
            (void)fputs(yew_buf_label(ed->ws.bufs[i]), stderr);
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
            yew_journal_discard(buffer->jrn);
            buffer->jrn = NULL;
        }
    }
}

#if YEW_WITH_PLUGINS
static bool apply_plugin_grants(Ed *ed, const BatchOpts *opts)
{
    size_t i;

    for (i = 0U; i < opts->ngrants; i++) {
        const YewGrantArg *grant = &opts->grants[i];
        const char *cap = grant->text + grant->name_len + 1U;
        char *name = yew_xmalloc(grant->name_len + 1U);
        bool ok;

        (void)memcpy(name, grant->text, grant->name_len);
        name[grant->name_len] = '\0';
        ok = yew_plug_session_grant(ed, name, cap);
        if (!ok)
            (void)fprintf(stderr,
                          "yew: error: cannot grant %s to plugin %s\n",
                          cap, name);
        yew_xfree(name);
        if (!ok)
            return false;
    }
    return true;
}
#endif

int yew_batch_run(const BatchOpts *opts)
{
    BatchLogCtx log_ctx;
    YewLogSink mirror;
    YewEdStartup startup;
    Bytebuf source;
    char *script_path = NULL;
    Ed ed;
    FlFn *script;
    YewBatchTestState test_state;
    int result = YEW_EXIT_ERR;
    bool ed_ready = false;
    bool test_ready = false;
    bool test_installed = false;
    bool workspace_hook = false;

    if (opts == NULL || opts->script == NULL)
        return YEW_EXIT_ERR;
    if (!read_path(opts->script, &source)) {
        (void)fprintf(stderr, "yew: error: cannot read script %s: %s\n",
                      opts->script, strerror(errno));
        flush_stdout();
        return YEW_EXIT_ERR;
    }
    script_path = yew_xrealpath(opts->script);
    if (script_path == NULL)
        script_path = yew_xstrdup(opts->script);
    if (script_path == NULL)
        YEW_BUG("cannot allocate batch script path");

    if (opts->test) {
        yew_batch_test_init(&test_state);
        test_ready = true;
    }
    log_ctx = (BatchLogCtx){opts->quiet,
                            test_ready ? &test_state : NULL};
    mirror = (YewLogSink){batch_log_write, &log_ctx};
    yew_log_set_mirror(&mirror);
    yew_bug_set_prehook(flush_stdout);

    yew_ed_init(&ed);
    ed_ready = true;
    ed.headless = true;
    yew_tty_poison(&ed.tty);
    yew_batch_selfcheck(&ed);
    ed.fl->diag.sink = batch_diag;
    ed.fl->diag.sink_ctx = &log_ctx;
    startup = (YewEdStartup){
        .config_path = opts->config_path,
        .clean = opts->clean,
        .no_workspace_config = opts->no_workspace_config,
        .trust_workspace = opts->trust_workspace
    };
    yew_config_init(&ed, &startup);
    (void)yew_config_load_all(&ed, NULL);
    if (!open_batch_files(&ed, opts)) {
        result = YEW_EXIT_IO;
        goto done;
    }
#if YEW_WITH_PLUGINS
    if (!yew_plug_discover(&ed, NULL)) {
        result = YEW_EXIT_IO;
        goto done;
    }
    if (!apply_plugin_grants(&ed, opts) ||
        !yew_plug_enable_desired(&ed, NULL)) {
        result = YEW_EXIT_ERR;
        goto done;
    }
#else
    if (opts->ngrants != 0U) {
        char err[160];

        (void)yew_mod_require(YEW_MOD_PLUGINS, err, sizeof(err));
        (void)fprintf(stderr, "%s\n", err);
        result = YEW_EXIT_ERR;
        goto done;
    }
#endif
    yew_fl_hook_workspace(&ed, FL_EV_WS_OPEN);
    workspace_hook = true;
    install_globals(&ed, opts, script_path);
    if (opts->test) {
        if (!yew_batch_test_install(&test_state, yew_fl_vm(&ed)))
            YEW_BUG("cannot install batch assertion host");
        test_installed = true;
    }
    script = ed.prof.on ?
        fl_compile_script_profiled(ed.fl, source.data, source.len,
                                   script_path) :
        fl_compile_script(ed.fl, source.data, source.len, script_path);
    if (script == NULL) {
        result = YEW_EXIT_BATCH;
        goto done;
    }
    if (ed.prof.on) {
        ed.prof.batch = true;
        fl_vm_set_line_observer(yew_fl_vm(&ed), batch_prof_statement, &ed);
    }
    if (!fl_call_chunk(ed.fl, script, YEW_SRC_FLETCH)) {
        fl_vm_set_line_observer(yew_fl_vm(&ed), NULL, NULL);
        yew_prof_frame_end(&ed.prof, 0U, 0U, 0U);
        result = render_script_error(yew_fl_vm(&ed)) ? YEW_EXIT_ERR :
                                                       YEW_EXIT_BATCH;
        goto done;
    }
    fl_vm_set_line_observer(yew_fl_vm(&ed), NULL, NULL);
    yew_prof_frame_end(&ed.prof, 0U, 0U, 0U);
    if (opts->replay_reg != 0U &&
        yew_macro_replay(&ed, opts->replay_reg, 1U) != YEW_CMD_OK) {
        FlVm *vm = yew_fl_vm(&ed);

        if (vm->err.t != (u8)FL_NIL)
            (void)render_script_error(vm);
        else
            (void)fprintf(stderr, "yew: replay @%c failed\n",
                          (int)opts->replay_reg);
        result = YEW_EXIT_BATCH;
        goto done;
    }
    /* Product-level guard drill: the smoke lane seeds the forbidden call
     * only after script output exists, proving both exit 4 and the stdout
     * flush contract.  This is intentionally an environment selftest, not
     * a user-facing batch option. */
    if (getenv("YEW_BATCH_SELFTEST_TTY") != NULL)
        (void)yew_tty_signal_fd(&ed.tty);
    result = YEW_EXIT_OK;

done:
    if (ed_ready && !batch_prof_dump(&ed) && result == YEW_EXIT_OK)
        result = YEW_EXIT_IO;
    if (test_installed && !yew_batch_test_finish(&test_state, -1))
        result = YEW_EXIT_BATCH;
    if (workspace_hook)
        yew_fl_hook_workspace(&ed, FL_EV_WS_CLOSE);
    warn_dirty(&ed, opts->quiet);
    discard_journals(&ed);
    flush_stdout();
    if (test_ready)
        yew_batch_test_free(&test_state);
    if (ed_ready)
        yew_ed_free(&ed);
    yew_bug_set_prehook(NULL);
    yew_log_set_mirror(NULL);
    yew_xfree(script_path);
    bytebuf_free(&source);
    return result;
}
