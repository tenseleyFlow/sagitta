#ifndef YEW_UI_COMPHELP_H
#define YEW_UI_COMPHELP_H

/*
 * Sprint 57.25: learning a command's subcommands and flags from its
 * `--help`.
 *
 * A spec (57.24) covers the commands someone wrote one for; nearly every
 * other modern CLI prints a two-column `--help` table.  This layer runs
 * `<resolved executable> [listed subcommands] --help` ASYNCHRONOUSLY and
 * only on the idle path, parses the output into the SAME in-memory spec
 * tree compspec resolves against, and caches it on disk keyed by the
 * executable's identity, so a reinstalled tool re-learns.
 *
 * Running a program to ask how it works is not free of risk.  The rules
 * (§1), each tested:
 *   - `shell.complete_help`: native (default) runs only ELF / Mach-O
 *     executables, never a script; all runs any executable; off, nothing;
 *   - never a name on the denylist (typed, resolved or real basename);
 *   - never a command that has a spec;
 *   - never `<cmd> <word> --help` for a word the parent's help did not
 *     list (§4 pitfall);
 *   - argv[0] is the RESOLVED absolute path, never the bare name; no
 *     stdin; a neutral cwd; a 1 s timeout; 256 KiB of output at most.
 *
 * Nothing here spawns on the keystroke path: a lookup that misses only
 * QUEUES a request, and yew_comphelp_idle -- called by the loop on a turn
 * with no input -- spawns it.  A lookup never writes the message line or
 * the log; reports are queued for yew_comphelp_notice.
 */

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "ui/compspec.h"
#include "util/base.h"

typedef struct Ed Ed;

enum {
    YEW_COMPHELP_TIMEOUT_MS = 1000,
    /* §5: at most this many cache files; a write past it prunes the
     * oldest (by mtime) down to YEW_COMPHELP_CACHE_KEEP. */
    YEW_COMPHELP_CACHE_MAX = 2000,
    YEW_COMPHELP_CACHE_KEEP = 1800,
    /* §3: a description keeps its first 60 bytes. */
    YEW_COMPHELP_DESC_MAX = 60,
    /* Subcommand depth a lazy request may reach (`a b c d …`). */
    YEW_COMPHELP_DEPTH_MAX = 8
};

#define YEW_COMPHELP_COLLECT_MAX (256U * 1024U)

typedef enum YewHelpPolicy {
    YEW_HELP_POLICY_NATIVE,
    YEW_HELP_POLICY_ALL,
    YEW_HELP_POLICY_OFF
} YewHelpPolicy;

/* What the first bytes of a file say it is (§1's magic table). */
typedef enum YewHelpExeKind {
    YEW_HELP_EXE_NONE,   /* not an executable regular file         */
    YEW_HELP_EXE_NATIVE, /* ELF or Mach-O (thin or universal)      */
    YEW_HELP_EXE_SCRIPT, /* `#!`                                   */
    YEW_HELP_EXE_OTHER   /* executable, but neither                */
} YewHelpExeKind;

/* ------------------------------------------------------------------ */
/* §3: the parser                                                      */
/* ------------------------------------------------------------------ */

/*
 * Parse help text into a spec whose ROOT is the command at `words`
 * (`words[0]` the program's basename, then the subcommand path the help
 * was asked for -- usage rows spelled with that prefix are recognised).
 * Returns NULL for a NEGATIVE result: no subcommand and no flag row.
 * Input past YEW_COMPHELP_COLLECT_MAX is ignored.
 */
YewCompSpec *yew_comphelp_parse(const char *origin, const char *const *words,
                                u32 n_words, const char *text, size_t len);

/* ------------------------------------------------------------------ */
/* §1: policy                                                          */
/* ------------------------------------------------------------------ */

/* The option, read as a global; an Ed without options reads the default
 * (native), a NULL Ed reads off. */
YewHelpPolicy yew_comphelp_policy(Ed *ed);
/* The denylist, on a basename; `mkfs.*` included. */
bool yew_comphelp_denied(const char *name);
YewHelpExeKind yew_comphelp_exe_kind(const char *path);
/* Whether `policy` lets a file of `kind` run. */
bool yew_comphelp_kind_allowed(YewHelpPolicy policy, YewHelpExeKind kind);

/* ------------------------------------------------------------------ */
/* Lookups (keystroke path: never spawn)                               */
/* ------------------------------------------------------------------ */

typedef struct YewHelpLookup {
    const YewCompSpec *spec; /* the help tree, or NULL                */
    /* The request that would answer or is answering: 16 hex digits,
     * the disk key.  Empty when nothing was asked for. */
    char key[17];
    bool pending;            /* queued or in flight                   */
} YewHelpLookup;

/*
 * The help tree for command word `word`: memory, then the disk cache.
 * A miss QUEUES a root request (when the policy and the rules allow)
 * and reports it pending.  Returns whether `out->spec` is set.
 */
bool yew_comphelp_lookup(Ed *ed, const char *word, YewHelpLookup *out);
/* Sprint 57.32: the same, with a relative command word (`./tool`)
 * resolved against `cwd` -- the directory the command runs in; NULL
 * (unknown) answers nothing for one. */
bool yew_comphelp_lookup_in(Ed *ed, const char *word, const char *cwd,
                            YewHelpLookup *out);

typedef enum YewHelpDescend {
    YEW_HELP_DESCEND_READY,   /* the node's own help is in the tree     */
    YEW_HELP_DESCEND_CHANGED, /* just grafted from memory or disk: the
                               * caller re-resolves                     */
    YEW_HELP_DESCEND_PENDING, /* asked for (or not allowed): offers
                               * nothing yet                            */
    YEW_HELP_DESCEND_NONE     /* not a help tree                        */
} YewHelpDescend;

/*
 * §4: `node` is where resolution landed in a help tree.  A subcommand
 * learned from its parent's help has no children or flags of its own
 * until `<cmd> <path> --help` answers; this grafts that answer from
 * memory or disk, or queues the request.  Only nodes that ARE in the
 * tree can be asked about -- which is to say only names the parent's
 * help listed.
 */
YewHelpDescend yew_comphelp_descend(Ed *ed, const YewCompSpec *spec,
                                    const YewSpecNode *node, char key[17]);

/* Queued or in flight: the pager's `…` (57.24 §5.5). */
bool yew_comphelp_awaiting(const char *key);
u32 yew_comphelp_inflight(void);
u32 yew_comphelp_queued(void);

/* ------------------------------------------------------------------ */
/* The idle path                                                       */
/* ------------------------------------------------------------------ */

/* A request is queued AND could be spawned now -- the loop's "do not
 * sleep" predicate, shared with yew_comphelp_idle. */
bool yew_comphelp_idle_ready(void);
/* Spawn the next queued request, if the caps allow.  Returns how many
 * jobs it started (0 or 1).  Call only on a turn with no input. */
u32 yew_comphelp_idle(Ed *ed);
/* The prompt closed: unspawned requests and the per-prompt resolution
 * memo are dropped (in-flight answers still land in the cache). */
void yew_comphelp_prompt_closed(void);
/* Put one queued report on the message line.  The `:` prompt calls it. */
bool yew_comphelp_notice(Ed *ed);

/* ------------------------------------------------------------------ */
/* §5: the cache                                                       */
/* ------------------------------------------------------------------ */

/* $XDG_CACHE_HOME/yew/completions/help, heap-owned; NULL without one. */
char *yew_comphelp_cache_dir(void);
/*
 * Delete cached help: every entry when `name` is NULL or empty, else the
 * entries whose executable's basename is `name` or whose executable is
 * what `name` resolves to.  Memory too.  Returns the files removed, or
 * -1 with `err` set.
 */
i64 yew_comphelp_forget(Ed *ed, const char *name, char *err, size_t errsz);

/* `ed.shell.complete_forget [name]` */
CmdStatus yew_comphelp_run_forget(CmdCtx *cx);

/* ------------------------------------------------------------------ */
/* Test seams                                                          */
/* ------------------------------------------------------------------ */

/* Forget memory, queue, notices and flights' bookkeeping (not the disk). */
void yew_comphelp_reset(void);
/* The disk key for (real, mtime, size, subs). */
void yew_comphelp_key(const char *real, i64 mtime, i64 size,
                      const char *const *subs, u32 n_subs, char out[17]);
/* Spawns attempted since the last reset. */
u32 yew_comphelp_test_spawns(void);
/* Override the timeout (0 restores YEW_COMPHELP_TIMEOUT_MS). */
void yew_comphelp_test_set_timeout_ms(i64 ms);
/* The argv of the most recent spawn attempt, space-joined. */
const char *yew_comphelp_test_last_argv(void);

#endif
