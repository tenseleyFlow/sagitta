#ifndef YEW_UI_COMPSPEC_H
#define YEW_UI_COMPSPEC_H

/*
 * Sprint 57.24 §1-§3: completion specs.
 *
 * A spec is one Fletch DATA document (`runtime/completions/<cmd>.fl`)
 * describing what a command accepts: its subcommands, flags, positional
 * argument kinds and the generators that list their values.  It is data,
 * not code -- fl_data_read has no production for a call -- and the one
 * thing in it that runs a program is a generator's argv, which only ever
 * runs through the job layer (compgen.c), never a shell.
 *
 * Loaded lazily, validated once, cached for the process.  A rejected
 * file is reported ONCE (yew_msg naming the file and the key) and the
 * command then behaves as if it had no spec.
 *
 * Specs from plugins and workspaces are deferred past Sprint 57.26: a
 * spec's generators run programs, so loading one from a repository the
 * user merely opened would be code execution on open.
 */

#include <stdbool.h>
#include <stddef.h>

#include "fl/value.h"
#include "ui/shctx.h"
#include "util/arena.h"
#include "util/base.h"

typedef struct Ed Ed;

/* Opaque; owns its arena.  Valid until yew_compspec_invalidate_all. */
typedef struct YewCompSpec YewCompSpec;

typedef enum YewSpecArgKind {
    YEW_SPEC_ARG_PATH,
    YEW_SPEC_ARG_DIR,
    YEW_SPEC_ARG_EXEC,
    YEW_SPEC_ARG_COMMAND,
    YEW_SPEC_ARG_VAR,
    YEW_SPEC_ARG_USER,
    YEW_SPEC_ARG_HOST,
    YEW_SPEC_ARG_PID,
    YEW_SPEC_ARG_SIGNAL,
    YEW_SPEC_ARG_VALUES,
    YEW_SPEC_ARG_GENERATOR,
    YEW_SPEC_ARG_NONE,
    YEW_SPEC_ARG__N
} YewSpecArgKind;

typedef struct YewSpecValue {
    const char *value;
    const char *desc; /* NULL: none */
} YewSpecValue;

/* A spec generator (§5): a program whose output lines are candidates. */
typedef struct YewSpecGen {
    const char *name;
    const char *const *argv; /* NULL-terminated, argc entries */
    u32 argc;
    i64 cache_ms;
    const char *const *pass_flags;
    u32 n_pass;
} YewSpecGen;

typedef struct YewSpecArg {
    YewSpecArgKind kind;
    bool repeat;
    const char *const *ext; /* path: extensions without the dot */
    u32 n_ext;
    const YewSpecValue *values;
    u32 n_values;
    /*
     * The generator's name: a built-in (hosts, signals, users,
     * make_targets, pids) or a key of the spec's `generators`, in which
     * case `gen` points at it.  Kinds host/pid/signal/user name their
     * built-in here too, so every generator-backed arg reads one field.
     */
    const char *generator;
    const YewSpecGen *gen;
} YewSpecArg;

typedef struct YewSpecFlag {
    const char *lng;  /* without the dashes, or NULL */
    const char *shrt; /* one character, or NULL      */
    const char *desc;
    const YewSpecArg *arg; /* non-NULL <=> the flag takes a value */
    bool arg_optional;
    bool global;
} YewSpecFlag;

typedef struct YewSpecNode YewSpecNode;
struct YewSpecNode {
    const char *name; /* NULL at the root */
    const char *const *aliases;
    u32 n_aliases;
    const char *desc;
    const YewSpecFlag *flags;
    u32 n_flags;
    const YewSpecArg *args;
    u32 n_args;
    const YewSpecNode *subs;
    u32 n_subs;
    /*
     * Not in §1's table: values that are spelled as a FLAG, `-KILL` for
     * `kill`.  Offered with a leading `-` when the caret's stem starts
     * with one; a word of that shape in the walk is an unknown short
     * flag, which is exactly how the tool reads it.
     */
    const YewSpecArg *dash_values;
    /*
     * Also not in §1: what EVERY word after an unquoted `--` is, where it
     * differs from the slots (`git checkout <branch>` but
     * `git checkout -- <path>...`).  NULL: the slots apply as usual.
     */
    const YewSpecArg *after_dashdash;
    const YewSpecNode *parent;
};

/*
 * The spec for `name` -- the BASENAME of the command word -- or NULL.
 * Lookup order, first hit wins, whole-file replace, never merge:
 *   1. yew_xdg_config_dir()/completions/<name>.fl (the user's),
 *   2. the shipped runtime's completions/<name>.fl -- the runtime
 *      init.fl came from (yew_runtime_root), never a second search;
 *   3. a shipped spec whose `command` list names <name>.
 * A user file is re-checked by mtime at most once per prompt open.
 * `ed` may be NULL (the lexer asks).  A rejected file, or an installed
 * runtime with no completions/, is QUEUED for yew_compspec_notice.
 */
const YewCompSpec *yew_compspec_get(Ed *ed, const char *name);
/* Put one queued report on `ed`'s message line, once per session per
 * reason.  The `:` prompt calls it; returns whether it said anything. */
bool yew_compspec_notice(Ed *ed);
/* Drop every loaded spec and the alias index.  Test seam. */
void yew_compspec_invalidate_all(void);
/* The prompt closed: user files are re-stat'ed on their next lookup. */
void yew_compspec_prompt_closed(void);

const YewSpecNode *yew_compspec_root(const YewCompSpec *spec);
const char *yew_compspec_description(const YewCompSpec *spec);
/* Where it came from, for messages and tests: "completions/git.fl" for a
 * shipped file, the absolute path for a user file. */
const char *yew_compspec_origin(const YewCompSpec *spec);
/* The `precommand` map, or false when the spec has none. */
bool yew_compspec_precommand(const YewCompSpec *spec, YewShWrapper *out);

/*
 * §7: the description of the shipped spec answering `name`, from the
 * alias index -- no parse per keystroke.  NULL when there is none.
 */
const char *yew_compspec_describe(const char *name);

/* shctx's YewShWrapperLookup over specs (`ud` is the Ed, may be NULL). */
int yew_compspec_wrapper(void *ud, const char *name, YewShWrapper *out);

/*
 * §3: where the caret's word sits in the command tree.  Walks
 * argv[1 .. arg_index-1] of `ctx` against the tree.
 */
typedef struct YewSpecPoint {
    const YewSpecNode *node;         /* deepest node reached              */
    const YewSpecFlag *pending_flag; /* the flag whose value the caret is */
    u32 positional;                  /* the caret's slot at `node`        */
    bool after_equals;               /* caret is in `--flag=<here>`       */
    bool subcommands_allowed;        /* children, no positional consumed  */
    /*
     * Beyond §3's sketch.  `flags_ended`: a `--` was walked.
     * `command_at` != 0: argv[command_at] starts a NEW command (a
     * `command` arg slot, or a precommand spec's wrapped command) and the
     * caret belongs to it; the caller re-enters COMMAND position there.
     * `value_at`: for after_equals, the byte offset in the stem where the
     * value starts (just past the `=`).
     */
    bool flags_ended;
    u32 command_at;
    u32 value_at;
} YewSpecPoint;

bool yew_compspec_resolve(const YewCompSpec *spec, const YewShCtx *ctx,
                          YewSpecPoint *out);

/* The arg slot `positional` of `node`, clamped onto a trailing `repeat`
 * slot; NULL past the last slot of a node that does not repeat. */
const YewSpecArg *yew_compspec_slot(const YewSpecNode *node, u32 positional);

/* Built-in generator names (§5), for validation and routing. */
bool yew_compspec_builtin_generator(const char *name);

/*
 * Test seams.  `load_text` parses and validates a document as though it
 * were file `origin`; on rejection it returns NULL and writes the one-
 * line reason (file and key) into `err`.
 */
YewCompSpec *yew_compspec_load_text(const char *origin, const char *src,
                                    size_t len, char *err, size_t errsz);
void yew_compspec_free(YewCompSpec *spec);

/*
 * Sprint 57.25: a spec that did not come from a spec file -- the tree
 * parsed out of a command's `--help` -- in the SAME in-memory form, so
 * yew_compspec_resolve and the routing walk it unchanged.
 *
 * `new` is an empty spec (no `command`, no generators) owning an arena;
 * the builder allocates through yew_compspec_arena and fills the root.
 * `write_node` is the data form of a node (§1's node keys) as a Fletch
 * value in `vm`, for fl_data_write; `read_node` reads one back through
 * the spec reader's own node validation -- one schema, not two.
 */
YewCompSpec *yew_compspec_new(const char *origin);
Arena *yew_compspec_arena(YewCompSpec *spec);
YewSpecNode *yew_compspec_root_mut(YewCompSpec *spec);
FlValue yew_compspec_write_node(FlVm *vm, const YewSpecNode *node);
YewCompSpec *yew_compspec_read_node(const char *origin, const FlValue *node,
                                    char *err, size_t errsz);
/* The shipped spec files, sorted by name ("git.fl"); the index is built
 * from exactly this list. */
size_t yew_compspec_shipped_count(void);
const char *yew_compspec_shipped_name(size_t i);
/* Read and validate one shipped file by its listed name. */
bool yew_compspec_check_shipped(const char *file, char *err, size_t errsz);
/* Replace the compiled install prefix (NULL restores it) for the shared
 * runtime decision, and drop every loaded spec. */
void yew_compspec_test_set_default_root(const char *root);
/* "disk" or "embedded": where the shipped list was read from. */
const char *yew_compspec_shipped_source(void);
/* How many spec files have been parsed, for the "no parse per keystroke"
 * assertion. */
u32 yew_compspec_test_parse_count(void);

#endif
