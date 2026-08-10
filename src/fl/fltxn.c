#include "fl/fltxn.h"

#include <stdlib.h>

#include "fl/motion_tab.h"
#include "text/edit.h"
#include "text/undo.h"
#include "util/base.h"

typedef struct FlTxnEntry {
    EditCtx ec;
} FlTxnEntry;

static FlTxnEntry *entries(FlVm *vm)
{
    return (FlTxnEntry *)vm->txn.enlisted;
}

static const FlTxnEntry *const_entries(const FlVm *vm)
{
    return (const FlTxnEntry *)vm->txn.enlisted;
}

static void txn_finish(FlVm *vm, bool ok)
{
    FlTxnEntry *v = entries(vm);
    u32 i;

    if (ok) {
        for (i = 0U; i < vm->txn.n; i++)
            yew_undo_end(&v[i].ec);
    } else {
        i = vm->txn.n;
        while (i != 0U)
            yew_undo_abort(&v[--i].ec);
    }
    vm->txn.n = 0U;
}

static bool editor_run_begin(FlVm *vm)
{
    if (vm->txn.entry_active)
        return fl_raise(vm, "user", "nested editor entry transaction");
    if (vm->txn.n != 0U || vm->txn.depth != 0U)
        return fl_raise(vm, "user", "stale editor transaction state");
    vm->txn.entry_active = true;
    return true;
}

static bool editor_run_end(FlVm *vm, bool ok)
{
    if (!vm->txn.entry_active)
        return true;
    if (vm->txn.depth != 0U)
        ok = false;
    txn_finish(vm, ok);
    vm->txn.depth = 0U;
    vm->txn.entry_active = false;
    return true;
}

static bool editor_edit_begin(FlVm *vm)
{
    if (!vm->txn.entry_active)
        return fl_raise(vm, "user", "edit block outside Fletch entry");
    /* Finish preceding implicit edits before opening an explicit boundary. */
    if (vm->txn.depth == 0U && vm->txn.n != 0U)
        txn_finish(vm, true);
    if (vm->txn.depth == UINT32_MAX)
        return fl_raise(vm, "limit", "edit nesting exceeded");
    vm->txn.depth++;
    return true;
}

static bool editor_edit_end(FlVm *vm, bool ok)
{
    if (vm->txn.depth == 0U)
        return fl_raise(vm, "user", "unbalanced edit block");
    vm->txn.depth--;
    if (vm->txn.depth == 0U)
        txn_finish(vm, ok);
    return true;
}

const FlHost fl_host_editor = {
    editor_run_begin,
    editor_run_end,
    fl_motion_host_dispatch,
    editor_edit_begin,
    editor_edit_end
};

bool fl_txn_enlist(FlVm *vm, const EditCtx *ec)
{
    FlTxnEntry *v;
    u32 i;

    if (vm == NULL || ec == NULL)
        return false;
    if (vm->host != &fl_host_editor || vm->ed == NULL)
        return true;
    if (!vm->txn.entry_active)
        return fl_raise(vm, "user", "editor mutation outside Fletch entry");
    if (ec->tb == NULL || ec->undo == NULL)
        return fl_raise(vm, "user", "editor mutation has no undo context");
    v = entries(vm);
    for (i = 0U; i < vm->txn.n; i++) {
        if (v[i].ec.undo == ec->undo) {
            /*
             * The command may have opened the crash journal through its
             * own stack-local EditCtx.  A post-dispatch enlist refreshes
             * that pointer (and the current view's cursor set), so an
             * eventual inverse replay is journalled through the same live
             * context as the forward mutation.
             */
            v[i].ec = *ec;
            return true;
        }
    }
    if (vm->txn.n == vm->txn.cap) {
        u32 want = vm->txn.cap == 0U ? 4U : vm->txn.cap * 2U;

        if (want < vm->txn.cap)
            return fl_raise(vm, "limit", "too many transaction buffers");
        vm->txn.enlisted = yew_xreallocarray(vm->txn.enlisted, want,
                                             sizeof(*v));
        vm->txn.cap = want;
        v = entries(vm);
    }
    v[vm->txn.n].ec = *ec;
    yew_undo_begin(&v[vm->txn.n].ec, YEW_TXN_MACRO);
    vm->txn.n++;
    return true;
}

bool fl_txn_enlisted(const FlVm *vm, const UndoTree *undo)
{
    const FlTxnEntry *v;
    u32 i;

    if (vm == NULL || undo == NULL || vm->host != &fl_host_editor)
        return false;
    v = const_entries(vm);
    for (i = 0U; i < vm->txn.n; i++)
        if (v[i].ec.undo == undo)
            return true;
    return false;
}

bool fl_txn_prepare_save(FlVm *vm, const UndoTree *undo)
{
    if (vm == NULL || undo == NULL || vm->host != &fl_host_editor ||
        !vm->txn.entry_active)
        return true;
    if (vm->txn.depth != 0U)
        return fl_raise(vm, "user", "cannot save inside edit block");
    if (fl_txn_enlisted(vm, undo))
        txn_finish(vm, true);
    return true;
}
