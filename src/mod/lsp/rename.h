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

bool yew_lsp_rename_request(Ed *ed, Win *w);
bool yew_lsp_rename_key(Ed *ed, const Key *key);
void yew_lsp_rename_shutdown(Ed *ed);

#endif
