#ifndef SAG_FL_ORIGIN_H
#define SAG_FL_ORIGIN_H

/*
 * Sprint 34 deliverable 2: origins and capabilities, spec §13.
 *
 * SPEC §13, PINNED VERBATIM BECAUSE EVERY MISREADING OF IT IS A
 * PRIVILEGE ESCALATION:
 *
 *   The check reads the origin of the CALLING FUNCTION'S DEFINING
 *   MODULE, not the current stack top.
 *
 * fl_cap_origin walks down to the first frame whose function carries a
 * non-builtin origin, so host natives are transparent and a plugin
 * calling a user-config helper gains nothing: the helper's frame is
 * below the plugin's, the walk stops at the plugin, and the plugin's
 * grants are what is checked.  Reading the stack top would let any
 * stdlib function launder authority for its caller; reading frames[0]
 * would hand out whatever started the program.
 *
 * TWO DIFFERENT NUMBERS LIVE IN THIS HEADER AND CONFLATING THEM IS HOW
 * A PLUGIN GETS THE USER CONFIG'S GRANTS:
 *
 *   - FlOriginKind is an ENUM.  FL_ORIGIN_BUILTIN is 0 because
 *     fl_cap_origin skips builtins and therefore needs no particular
 *     value for them; the numbering carries no authority.
 *   - The REGISTRY ID is an index into the per-editor origin registry.
 *     Id 0 is reserved for the user config -- Sprint 54's capability
 *     prompts depend on that number -- so fl_origin_register NEVER
 *     returns 0 for anything else.  A unit test asserts it.
 *
 * Sprint 31 declared FlOriginKind and FlOrigin in value.h for the
 * module cache.  This sprint relocates that declaration here verbatim
 * (value.h includes this header) and builds the registry on top; there
 * is deliberately no parallel SAG_ORIGIN_* spelling.
 */

#include <stdbool.h>

#include "util/base.h"

typedef struct Ed Ed;
typedef struct FlVm FlVm;

/*
 * Capability origin, spec §13.  Every FlFn carries the origin of the
 * module that DEFINED it, and that is the only thing a capability check
 * ever reads.
 *
 * `kind` and `path_id` are separate on purpose.  The kind decides what
 * a grant means (a plugin's caps are prompted for, a config's are
 * implicit); the path is what an error message must name to be
 * actionable.  Collapsing them into one module index, as Sprint 30's
 * placeholder did, makes the cache key in §11 -- (realpath, origin
 * kind) -- inexpressible, and that key is what stops a plugin from
 * borrowing a config helper's authority.
 */
typedef enum {
    FL_ORIGIN_BUILTIN = 0,   /* the seven modules; transparent to §13    */
    FL_ORIGIN_CONFIG,
    FL_ORIGIN_WORKSPACE,
    FL_ORIGIN_PLUGIN,
    FL_ORIGIN_CLI,
    FL_ORIGIN_REPL
} FlOriginKind;

/* Capability bits (spec §13).  `shell` and `net` have no stdlib surface
 * in 1.0 -- they exist so Sprint 54 can prompt for them. */
enum {
    FL_CAP_FS_READ  = 1U << 0,
    FL_CAP_FS_WRITE = 1U << 1,
    FL_CAP_SHELL    = 1U << 2,
    FL_CAP_NET      = 1U << 3
};

typedef struct FlOrigin {
    u8 kind;         /* FlOriginKind                                     */
    u32 path_id;     /* interned REALPATH; 0 for builtins                */
    u32 caps;        /* FL_CAP_*                                         */
} FlOrigin;

/* The user config.  Sprint 54 depends on this number. */
#define FL_ORIGIN_ID_CONFIG 0U
/*
 * No registry to ask -- a headless VM with no editor host.  A distinct
 * value rather than 0, because "I could not find out" and "the user's
 * own config" must never be the same answer to a capability question.
 *
 * A macro, not an enumerator: C11 restricts enumerator values to the
 * range of int and -pedantic is a gate.
 */
#define FL_ORIGIN_ID_NONE 0xFFFFFFFFU

typedef struct FlOriginRec {
    u8 kind;
    u32 caps;
    char *label;     /* owned; the path when there is one                */
} FlOriginRec;

/*
 * The registry.  Slot 0 is seeded with the user config at init, so a
 * config origin registered later finds itself rather than taking a new
 * id.
 */
typedef struct FlOriginReg {
    FlOriginRec *v;
    u32 n;
    u32 cap;
    /*
     * Origins currently being torn down.  sag_hook_fire skips them
     * (§7): Sprint 36's config reload and Sprint 54's plugin disable
     * both need a window where a registration still exists -- so the
     * ledger can walk it -- but must not run.
     */
    u32 *masked;
    u32 nmasked;
    u32 maskcap;
} FlOriginReg;

void fl_origin_reg_init(FlOriginReg *r);
void fl_origin_reg_free(FlOriginReg *r);

/*
 * Registers `label` under `kind`, or returns the existing id when the
 * same (kind, label) pair is already known -- loading a config twice
 * must not give it two identities, because the ledger keys teardown on
 * the id.
 */
u32 fl_origin_register(Ed *ed, FlOriginKind k, const char *label, u32 caps);
const char *fl_origin_label(const Ed *ed, u32 origin_id);
u32 fl_origin_caps(const Ed *ed, u32 origin_id);

/* The registry id of the defining module of the CALLER, per §13's walk.
 * FL_ORIGIN_ID_NONE when there is no editor host to ask. */
u32 fl_origin_of_frame(FlVm *vm);

/* Teardown masking (§7).  Idempotent; unmask of an unmasked id is a
 * no-op, because a reload that fails halfway must still be able to
 * unwind without knowing how far it got. */
void fl_origin_mask(Ed *ed, u32 origin_id);
void fl_origin_unmask(Ed *ed, u32 origin_id);
bool fl_origin_masked(const Ed *ed, u32 origin_id);

/* ---------------------------------------------------------------- */
/* The check itself                                                 */
/* ---------------------------------------------------------------- */

/*
 * The grant is read from the DEFINING MODULE of the calling function --
 * never from the stack top, never from a VM-global "current caps".
 *
 * Builtin frames are transparent, so `list.map(f, io.read)` checks f's
 * origin rather than list's.  That transparency is the whole mechanism:
 * without it, any stdlib function would launder authority for whatever
 * called it, and §13's promise that "a plugin calling a user-config
 * helper gains nothing" would be false.
 */
FlOrigin fl_cap_origin(const FlVm *vm);
const char *fl_cap_name(u32 cap);
const char *fl_origin_name(const FlVm *vm, const FlOrigin *o);

/* True when the caller holds every bit in `need`; otherwise raises kind
 * "capability" and returns false. */
bool fl_cap_check(FlVm *vm, u32 need);

#endif /* SAG_FL_ORIGIN_H */
