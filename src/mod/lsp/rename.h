#ifndef YEW_MOD_LSP_RENAME_H
#define YEW_MOD_LSP_RENAME_H

#include <stddef.h>

#include "mod/lsp/json.h"
#include "text/coords.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/vec.h"

typedef struct Ed Ed;
typedef struct Key Key;
typedef struct Win Win;

enum {
    YEW_RENAME_MAX_FILES = 200,
    YEW_RENAME_MAX_EDITS = 20000,
    YEW_RENAME_ERROR_MAX = 512
};

typedef struct RenameEdit {
    ByteOff lo;
    ByteOff hi;
    const u8 *text;
    u32 len;
} RenameEdit;

VEC_DECL(Vec_RenameEdit, RenameEdit);

typedef struct RenameFile {
    char *path;
    u32 buf_id;
    u32 tab_id;
    u32 undo_before;
    Vec_RenameEdit edits;
    u64 buf_gen;
    i64 version;
    bool has_version;
    bool was_open;
    bool was_dirty;
} RenameFile;

VEC_DECL(Vec_RenameFile, RenameFile);

typedef struct RenamePlan {
    Arena arena;
    Vec_RenameFile files;
    u32 nedits;
    char *old_name;
    char *new_name;
    size_t test_fail_file;
    size_t test_fail_edit;
    bool test_fail_enabled;
} RenamePlan;

void yew_lsp_rename_plan_init(RenamePlan *plan);
void yew_lsp_rename_plan_free(RenamePlan *plan);

/* Phase 2: validates and hydrates the complete WorkspaceEdit before any
 * source buffer bytes are changed.  On failure, err contains the exact
 * user-facing refusal. */
bool yew_lsp_rename_preflight(Ed *ed, const JsonValue *workspace_edit,
                              u8 pos_enc, const char *old_name,
                              const char *new_name, RenamePlan *plan,
                              char err[YEW_RENAME_ERROR_MAX]);

/* Phase 4: applies one atomic undo transaction per buffer and rolls every
 * committed buffer back if a later edit fails. */
bool yew_lsp_rename_apply(Ed *ed, RenamePlan *plan,
                          char err[YEW_RENAME_ERROR_MAX]);

/* Deterministic unit-test seam for the all-or-nothing rollback contract.
 * A production plan never enables it. */
void yew_lsp_rename_plan_test_fail_at(RenamePlan *plan, size_t file_index,
                                      size_t edit_index);

/*
 * Deterministic unit-test seam for the CONFIRM PHASE (Sprint 57.11
 * Deliverable 4).
 *
 * Reaching RENAME_CONFIRM in production needs a ready language server
 * and a `textDocument/rename` response, which no unit lane has — yet
 * the confirmation is exactly what the PANEL menu's `Apply` /
 * `Show Diff` / `Cancel` rows and the three `ed.lsp.rename.*` commands
 * act on, so it has to be reachable from a test or those rows ship
 * unchecked.  This runs the REAL preflight and the REAL summary-panel
 * open — the same two calls `rename_response_done` makes — and skips
 * only the server and generation checks that precede them.  It is not
 * a second implementation of the phase; it is the production one with
 * the transport removed.
 *
 * Returns false, having installed nothing, if a rename is already in
 * flight or either step refuses.
 */
bool yew_lsp_rename_test_confirm(Ed *ed, Win *w,
                                 const JsonValue *workspace_edit,
                                 u8 pos_enc, const char *old_name,
                                 const char *new_name);

bool yew_lsp_rename_request(Ed *ed, Win *w);
bool yew_lsp_rename_key(Ed *ed, const Key *key);
void yew_lsp_rename_shutdown(Ed *ed);

#endif
