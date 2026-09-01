#ifndef YEW_MOD_GIT_FUSSTREE_H
#define YEW_MOD_GIT_FUSSTREE_H

#include <stdbool.h>
#include <stddef.h>

#include "mod/git/git.h"
#include "util/arena.h"
#include "util/base.h"
#include "ws/walk.h"

/* A conflicted node renders only the conflict marker; staged and unstaged
 * remain aggregated here so descendants retain their exact status. */
typedef struct FussNode {
    char *name;
    u32 name_len;
    char *path; /* canonical workspace-relative path, tree-arena-owned */
    u32 path_len;
    u32 first_child;
    u32 next_sibling;
    u32 parent;
    bool is_file;
    bool expanded;
    bool staged;
    bool unstaged;
    bool untracked;
    bool incoming;
    bool conflicted;
    bool ignored;
    bool untracked_dir;
    bool untracked_loaded; /* one-level walk result has been cached */
} FussNode;

typedef enum FussMarkerKind {
    YEW_FUSS_MARK_STAGED,
    YEW_FUSS_MARK_UNSTAGED,
    YEW_FUSS_MARK_UNTRACKED,
    YEW_FUSS_MARK_INCOMING,
    YEW_FUSS_MARK_CONFLICT
} FussMarkerKind;

/* Return the locked marker order.  Conflict replaces all ordinary markers. */
u8 yew_fuss_marker_kinds(const FussNode *node, FussMarkerKind out[4]);

typedef struct FussItem {
    char *path;
    u32 path_len;
    u32 node;
    u16 depth;
    bool is_file;
} FussItem;

typedef struct FussNodeList {
    FussNode *data;
    size_t len;
    size_t cap;
} FussNodeList;

typedef struct FussItemList {
    FussItem *data;
    size_t len;
    size_t cap;
} FussItemList;

typedef struct FussOpenPath {
    char *path;
    u32 path_len;
} FussOpenPath;

typedef struct FussOpenMemory {
    FussOpenPath *data;
    u32 len;
    u32 cap;
} FussOpenMemory;

typedef struct FussPathRef {
    const char *path;
    u32 path_len;
} FussPathRef;

typedef struct FussTree {
    Arena a;
    FussNodeList nodes; /* nodes[0] is the unshown root. */
    FussItemList items;
    char *repo_prefix; /* repo-relative path of the workspace root */
    u32 repo_prefix_len;
    u32 snap_gen;
    bool opts_valid;
    bool scope_valid;
    bool all_files;
    bool show_hidden;
    bool files_merged; /* current tree includes one completed FileList */
} FussTree;

typedef struct FussOpts {
    bool all_files;
    bool show_hidden;
} FussOpts;

/* Selection owns its path independently of a tree arena, so it remains valid
 * while the tree is rebuilt beneath it. */
typedef struct FussSel {
    char *path;
    u32 len;
} FussSel;

void yew_fuss_tree_init(FussTree *t);
void yew_fuss_tree_drop(FussTree *t);
/* Git porcelain -z paths are repository-root-relative even when Git runs
 * with the workspace as cwd.  Set the two canonical roots before building;
 * entries outside the workspace are then filtered and in-scope entries are
 * stripped to workspace-relative coordinates. */
bool yew_fuss_tree_scope_roots(FussTree *t, const char *repo_root,
                               const char *workspace_root);
void yew_fuss_build(FussTree *t, const GitSnapshot *s, const FussOpts *o);
/* Build the status tree and, when all-files is enabled, merge a completed
 * workspace walk.  Clean walk paths never replace snapshot status flags. */
bool yew_fuss_merge_files(FussTree *t, const FileList *files,
                          const GitSnapshot *s, const FussOpts *o);
void yew_fuss_flatten(FussTree *t);

void yew_fuss_open_memory_init(FussOpenMemory *m);
void yew_fuss_open_memory_drop(FussOpenMemory *m);
bool yew_fuss_open_memory_has(const FussOpenMemory *m,
                              const char *path, u32 path_len);
bool yew_fuss_open_memory_set(FussOpenMemory *m,
                              const char *path, u32 path_len,
                              bool expanded);
void yew_fuss_apply_expansion(FussTree *t,
                              const FussOpenMemory *manual_open,
                              const FussPathRef *open_files,
                              u32 nopen);

/* Navigation helpers return the selected row.  Invalid/empty selections are
 * normalized to row zero when rows exist and to -1 for an empty tree. */
i32 yew_fuss_nav_step(const FussTree *t, i32 row, i32 dir);
i32 yew_fuss_nav_raw(const FussTree *t, i32 row, i32 dir);
i32 yew_fuss_nav_parent(const FussTree *t, i32 row);
i32 yew_fuss_nav_enter(FussTree *t, i32 row);
bool yew_fuss_nav_toggle(FussTree *t, i32 row);

/* One-level untracked expansion seam.  `children` contains workspace-relative
 * paths beneath `node`; the model splices them as untracked and re-flattens.
 * The caller owns directory walking and gitignore filtering and may call this
 * with an empty list; the model independently enforces the dotfile option. */
bool yew_fuss_expand_untracked(FussTree *t, u32 node,
                               const GitPathList *children);

void yew_fuss_sel_clear(FussSel *s);
void yew_fuss_sel_set(FussSel *s, const char *path, u32 len);
void yew_fuss_sel_from_row(FussSel *s, const FussTree *t, i32 row);
i32 yew_fuss_row_of(const FussTree *t, const FussSel *s);

#endif
