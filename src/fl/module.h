#ifndef YEW_FL_MODULE_H
#define YEW_FL_MODULE_H

/*
 * Sprint 31 deliverable 9: `import`.
 *
 * THE CACHE KEY IS (realpath, origin.kind), NOT realpath.
 *
 * That is what makes spec §13's sentence -- "a plugin calling a
 * user-config helper gains nothing" -- literally true.  A helper file
 * imported by a plugin is a SEPARATE module instance carrying the
 * plugin's origin, so when the helper calls io.read, fl_cap_origin
 * finds the plugin's grants and not the config's.  Key by realpath
 * alone and the first importer wins: grants then leak in whichever
 * direction the load order happened to run, which is an ambient-
 * authority hole that no test notices and every reviewer should.
 *
 * The cost is that a file imported from two origins executes twice.
 * Documented, tested, and cheap -- helpers are small.
 *
 * REALPATH, NOT THE LITERAL STRING.  `./a.fl` and `a.fl` name one file,
 * and caching the spelling loads it twice inside ONE origin: two copies
 * of the module's globals, and a registration list that silently
 * doubles.  That is the classic double-init bug and realpath(3) is the
 * whole fix.
 */

#include "fl/value.h"

typedef struct FlVm FlVm;

enum { FL_MOD_LOADING = 0, FL_MOD_READY = 1 };

/* How deep imports may nest.  A module body's globals map is held on
 * the GC's temp-root stack while a nested import runs, and that stack
 * is 32 deep; this keeps the two facts from meeting. */
enum { FL_MOD_MAX_DEPTH = 16 };

typedef struct FlModule {
    u32 path_id;         /* interned REALPATH                           */
    FlOrigin origin;     /* the IMPORTER's kind and caps, this path     */
    FlMap *exports;      /* frozen on completion; NULL while loading    */
    u8 state;            /* FL_MOD_LOADING | FL_MOD_READY               */
    u32 importer;        /* index + 1 of the module that imported this  */
} FlModule;

typedef struct FlModTab {
    FlModule *v;
    u32 n;
    u32 cap;
} FlModTab;

/*
 * Resolves and, if needed, runs the module, leaving its exports map in
 * `out`.  `id` is the interned bare name for `import str`, or the
 * interned path text for `import "lib/x.fl" as x`.
 *
 * Returns false having raised: kind "import" for a module that is not
 * found (listing every path tried) and for a cycle (naming the whole
 * chain in load order), or whatever the module body raised.
 */
bool fl_import(FlVm *vm, u32 id, bool is_path, FlValue *out);

/*
 * Evaluates one exact file as an isolated module without consulting or
 * changing the import cache.  Macro-library rescans use this entry: a reload
 * must produce fresh closures even when the same real path was loaded by the
 * preceding scan.  On success `out` is the frozen exports map; the caller
 * must root it before performing another allocation.
 */
bool fl_module_eval_path(FlVm *vm, const char *path, FlOrigin origin,
                         FlValue *out);
bool fl_module_eval_source(FlVm *vm, const char *path,
                           const char *source, size_t len,
                           FlOrigin origin, FlValue *out);

/* Releases the table itself.  The exports maps are the collector's. */
void fl_mod_free(FlVm *vm);

#endif /* YEW_FL_MODULE_H */
