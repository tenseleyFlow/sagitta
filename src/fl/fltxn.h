#ifndef YEW_FL_FLTXN_H
#define YEW_FL_FLTXN_H

#include <stdbool.h>

#include "fl/vm.h"

typedef struct EditCtx EditCtx;
typedef struct UndoTree UndoTree;

/* Editor host transaction callbacks, including motion dispatch. */
extern const FlHost fl_host_editor;

/*
 * Enlist the buffer named by an edit context at its first mutation.
 * Repeated calls for the same undo tree are no-ops.  A headless VM, or a
 * VM not attached to fl_host_editor, deliberately treats enlistment as a
 * no-op so spec §10 edit blocks remain ordinary blocks outside the editor.
 * Mutating dispatchers call this both immediately before invocation (to
 * open the macro transaction) and immediately after it (to refresh journal
 * and cursor pointers acquired by the command).
 */
bool fl_txn_enlist(FlVm *vm, const EditCtx *ec);

/* Lifecycle/durability guards for editor commands that do not mutate via
 * yew_edit_*.  A buffer whose undo tree is enlisted cannot be destroyed;
 * a save must first commit any implicit transaction so the saved marker
 * describes the bytes actually written.  Explicit edit{} blocks remain
 * atomic and therefore refuse an in-block save rather than splitting it. */
bool fl_txn_enlisted(const FlVm *vm, const UndoTree *undo);
bool fl_txn_prepare_save(FlVm *vm, const UndoTree *undo);

#endif
