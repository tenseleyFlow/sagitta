#include "fl/motion_tab.h"

#include "edit/cmd.h"
#include "edit/ed.h"
#include "fl/flruntime.h"
#include "fl/fltxn.h"
#include "util/intern.h"

enum { FL_MOTION_NEST_MAX = 256 };

static bool motion_fail_status(FlVm *vm, CmdStatus status,
                               const char *word, u32 word_len)
{
    const char *kind;

    switch (status) {
    case SAG_CMD_ERR_ARG:
        kind = "type";
        break;
    case SAG_CMD_ERR_STATE:
        kind = "user";
        break;
    case SAG_CMD_ERR_IO:
        kind = "io";
        break;
    case SAG_CMD_ERR_DEFERRED:
        kind = "name";
        break;
    case SAG_CMD_OK:
        return true;
    default:
        kind = "user";
        break;
    }
    return fl_raise(vm, kind, "command word '%.*s' failed",
                    (int)word_len, word);
}

static bool invoke_word(FlVm *vm, Ed *ed, Win *win,
                        const char *word, u32 word_len, u32 count,
                        bool count_given, const char *sarg, u32 sarg_len)
{
    CmdId id = sag_cmd_by_word(word, word_len);
    const CmdDesc *desc;
    CmdCtx cx = {0};
    CmdStatus status;
    bool changes;
    EditCtx ec;

    if (id.v == 0U)
        return fl_raise(vm, "name", "no command has word '%.*s'",
                        (int)word_len, word);
    desc = sag_cmd_desc(id);
    changes = desc != NULL &&
              (desc->flags & SAG_CMD_CHANGES_BUFFER) != 0U;
    if (changes && ed->fl_model_teardown)
        return fl_raise(vm, "user",
                        "editor mutation during model teardown");
    cx.ed = ed;
    cx.win = win;
    cx.count = count == 0U ? 1U : count;
    cx.count_given = count_given;
    cx.sarg = sarg;
    cx.sarg_len = sarg_len;
    cx.source = fl_runtime_cmd_source(vm);
    if (changes) {
        ec = sag_ed_edit_ctx_for(ed, win);
        if (!fl_txn_enlist(vm, &ec))
            return false;
    }
    status = sag_cmd_invoke(id, &cx);
    if (changes) {
        ec = sag_ed_edit_ctx_for(ed, win);
        if (!fl_txn_enlist(vm, &ec))
            return false;
    }
    return motion_fail_status(vm, status, word, word_len);
}

static bool invoke_mode(FlVm *vm, Ed *ed, Win *win, char mode,
                        u32 count, bool count_given)
{
    const char arg[1] = {mode};

    return invoke_word(vm, ed, win, "mode", 4U, count, count_given,
                       arg, 1U);
}

static bool invoke_arrow(FlVm *vm, Ed *ed, Win *win,
                         const FlMotionOp *op)
{
    const char *word;
    u32 len;

    switch (op->ch) {
    case '>':
        word = (op->flags & FL_MOTION_F_ALT) != 0U
                   ? "unit_next_alt" : "unit_next";
        len = (op->flags & FL_MOTION_F_ALT) != 0U ? 13U : 9U;
        break;
    case '<':
        word = (op->flags & FL_MOTION_F_ALT) != 0U
                   ? "unit_prev_alt" : "unit_prev";
        len = (op->flags & FL_MOTION_F_ALT) != 0U ? 13U : 9U;
        break;
    case '^':
        word = (op->flags & FL_MOTION_F_ALT) != 0U
                   ? "unit_up_alt" : "unit_up";
        len = (op->flags & FL_MOTION_F_ALT) != 0U ? 11U : 7U;
        break;
    case 'v':
        word = (op->flags & FL_MOTION_F_ALT) != 0U
                   ? "unit_down_alt" : "unit_down";
        len = (op->flags & FL_MOTION_F_ALT) != 0U ? 13U : 9U;
        break;
    default:
        return fl_raise(vm, "motion", "invalid motion arrow");
    }
    return invoke_word(vm, ed, win, word, len, op->count,
                       (op->flags & FL_MOTION_F_COUNT_GIVEN) != 0U,
                       NULL, 0U);
}

static bool invoke_unit(FlVm *vm, Ed *ed, Win *win,
                        const FlMotionOp *op)
{
    char mode;

    switch (op->ch) {
    case 'l':
        mode = 'L';
        break;
    case 'w':
        mode = 'W';
        break;
    case 'b':
        mode = 'B';
        break;
    case 'c':
        mode = 'C';
        break;
    default:
        return fl_raise(vm, "motion", "invalid motion unit");
    }
    return invoke_mode(vm, ed, win, mode, op->count,
                       (op->flags & FL_MOTION_F_COUNT_GIVEN) != 0U);
}

static bool invoke_op(FlVm *vm, Ed *ed, Win *win,
                      const FlMotionOp *op)
{
    const char *arg;
    size_t arg_len;

    switch ((FlMotionExecKind)op->kind) {
    case FL_MOTION_UNIT:
        return invoke_unit(vm, ed, win, op);
    case FL_MOTION_ARROW:
        return invoke_arrow(vm, ed, win, op);
    case FL_MOTION_INSERT:
        arg = sag_intern_str(vm->in, op->arg);
        arg_len = sag_intern_len(vm->in, op->arg);
        if (arg == NULL)
            return fl_raise(vm, "motion", "invalid motion string");
        return invoke_word(vm, ed, win, "insert", 6U, op->count,
                           (op->flags & FL_MOTION_F_COUNT_GIVEN) != 0U,
                           arg, (u32)arg_len);
    case FL_MOTION_DEL:
        return invoke_word(vm, ed, win, "delete_unit", 11U,
                           op->count,
                           (op->flags & FL_MOTION_F_COUNT_GIVEN) != 0U,
                           NULL, 0U);
    case FL_MOTION_ESC:
        return invoke_word(vm, ed, win, "escape", 6U,
                           op->count,
                           (op->flags & FL_MOTION_F_COUNT_GIVEN) != 0U,
                           NULL, 0U);
    case FL_MOTION_WORD:
        arg = sag_intern_str(vm->in, op->arg);
        arg_len = sag_intern_len(vm->in, op->arg);
        if (arg == NULL)
            return fl_raise(vm, "motion", "invalid command word");
        return invoke_word(vm, ed, win, arg, (u32)arg_len,
                           op->count,
                           (op->flags & FL_MOTION_F_COUNT_GIVEN) != 0U,
                           NULL, 0U);
    case FL_MOTION_HIGHLIGHT:
        break;
    }
    return fl_raise(vm, "motion", "invalid motion operation");
}

static void close_highlights(FlVm *vm, Ed *ed, Win *win, u32 depth)
{
    while (depth-- != 0U) {
        CmdId id = sag_cmd_by_word("escape", 6U);
        CmdCtx cx = {0};

        if (id.v == 0U)
            return;
        cx.ed = ed;
        cx.win = win;
        cx.count = 1U;
        cx.source = fl_runtime_cmd_source(vm);
        (void)sag_cmd_invoke(id, &cx);
    }
}

bool fl_motion_exec(FlVm *vm, Ed *ed, Win *win,
                    const FlMotionProg *prog)
{
    u32 ends[FL_MOTION_NEST_MAX];
    u32 depth = 0U;
    u32 i = 0U;

    if (vm == NULL || ed == NULL || win == NULL || prog == NULL)
        return vm != NULL &&
               fl_raise(vm, "motion", "no editor host");
    while (i < prog->n) {
        const FlMotionOp *op;

        while (depth != 0U && i == ends[depth - 1U]) {
            if (!invoke_word(vm, ed, win, "escape", 6U, 1U, false,
                             NULL, 0U)) {
                close_highlights(vm, ed, win, depth - 1U);
                return false;
            }
            depth--;
        }
        op = &prog->op[i];
        if ((FlMotionExecKind)op->kind == FL_MOTION_HIGHLIGHT) {
            u32 parent_end = depth == 0U ? prog->n : ends[depth - 1U];

            if (depth == FL_MOTION_NEST_MAX) {
                close_highlights(vm, ed, win, depth);
                return fl_raise(vm, "limit", "motion nesting limit exceeded");
            }
            if (op->arg > parent_end - i - 1U) {
                close_highlights(vm, ed, win, depth);
                return fl_raise(vm, "motion", "invalid highlight extent");
            }
            if (!invoke_mode(
                    vm, ed, win, 'H', op->count,
                    (op->flags & FL_MOTION_F_COUNT_GIVEN) != 0U)) {
                close_highlights(vm, ed, win, depth);
                return false;
            }
            ends[depth++] = i + 1U + op->arg;
        } else if (!invoke_op(vm, ed, win, op)) {
            close_highlights(vm, ed, win, depth);
            return false;
        }
        i++;
    }
    while (depth != 0U) {
        if (ends[depth - 1U] != prog->n) {
            close_highlights(vm, ed, win, depth);
            return fl_raise(vm, "motion", "invalid highlight extent");
        }
        if (!invoke_word(vm, ed, win, "escape", 6U, 1U, false,
                         NULL, 0U)) {
            close_highlights(vm, ed, win, depth - 1U);
            return false;
        }
        depth--;
    }
    return true;
}

bool fl_motion_host_dispatch(FlVm *vm, const FlMotionProg *prog)
{
    if (vm == NULL)
        return false;
    if (vm->ed == NULL)
        return fl_raise(vm, "motion", "no editor host");
    return fl_motion_exec(vm, vm->ed, vm->ed->win, prog);
}
