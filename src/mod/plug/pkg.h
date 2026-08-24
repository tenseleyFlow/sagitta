#ifndef YEW_MOD_PLUG_PKG_H
#define YEW_MOD_PLUG_PKG_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/job.h"
#include "fl/diag.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/base.h"
#include "util/buf.h"
#include "util/intern.h"

enum {
    YEW_PKG_REV_HEX = 40,
    YEW_PKG_TREE_HEX = 16,
    YEW_PKG_NET_TIMEOUT_MS = 60000
};

typedef struct GitRun {
    int status;
    Bytebuf out;
    Bytebuf err;
    bool timed_out;
    bool exec_failed;
} GitRun;

typedef struct PkgEntry {
    const char *name;
    const char *url;
    const char *shorthand;
    const char *pin;
    char rev[YEW_PKG_REV_HEX + 1];
    char tree[YEW_PKG_TREE_HEX + 1];
    i64 installed_at;
    i64 updated_at;
    FlValue extra;
} PkgEntry;

typedef struct PkgEntryVec {
    PkgEntry *data;
    size_t len;
    size_t cap;
} PkgEntryVec;

typedef struct PkgLock {
    Arena a;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    u32 schema;
    PkgEntryVec v;
    FlValue extra;
    bool initialized;
    bool corrupt;
} PkgLock;

void yew_pkg_git_run_init(GitRun *run);
void yew_pkg_git_run_free(GitRun *run);
bool yew_pkg_git(const char *const *argv, u32 nargv, i64 timeout_ms,
                 bool c_locale, GitRun *out);

bool yew_pkg_resolve_spec(const char *spec, Bytebuf *url, DiagCtx *dc);
bool yew_pkg_ref_valid(const char *ref);
bool yew_pkg_pin_valid(const char *pin);

void yew_pkg_lock_init(PkgLock *lock);
void yew_pkg_lock_free(PkgLock *lock);
bool yew_pkg_lock_load(PkgLock *lock, DiagCtx *dc);
bool yew_pkg_lock_save(const PkgLock *lock, DiagCtx *dc);
PkgEntry *yew_pkg_lock_find(PkgLock *lock, const char *name, u32 nlen);

/* FNV-1a-64 is a drift detector, not a cryptographic hash. */
bool yew_pkg_tree_hash(const char *dir, char out[17], DiagCtx *dc);

/* Pure-filesystem startup seam.  This never probes or runs git. */
bool yew_pkg_expected_tree(const char *name, char out[17], bool *managed,
                           DiagCtx *dc);

/* Refuses before the first unlink unless path resolves beneath root. */
bool yew_rmtree(const char *path, const char *must_contain, DiagCtx *dc);

/* argv[0] is "pkg", matching the other yew CLI subcommand conventions. */
int yew_pkg_main(int argc, char **argv);

#endif /* YEW_MOD_PLUG_PKG_H */
