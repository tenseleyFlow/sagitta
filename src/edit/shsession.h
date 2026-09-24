#ifndef YEW_EDIT_SHSESSION_H
#define YEW_EDIT_SHSESSION_H

/*
 * Sprint 57.27: the persistent shell session.
 *
 * One long-lived `$SHELL -s` per editor that every plain `:!cmd` runs
 * inside, so `cd`, `export` and functions persist from one command to
 * the next (`shell.session = persistent`, the default; `fresh` is Sprint
 * 19's `$SHELL -c` per command, exactly).  Non-interactive, no rc files:
 * it reads what `$SHELL -c` reads (zsh's .zshenv, bash's $BASH_ENV) and
 * nothing more.
 *
 * THE FRAME.  The session is a YEW_SINK_FRAMED job whose stdin carries
 * one line per command:
 *
 *     __yew_run() { trap 'return 130' INT; W eval "$__yew_c"; };
 *     __yew_c='CMD'; __yew_run </dev/null 9>&-;
 *     W printf '\036%s %d\036' 'N-S' "$?" >&9; W trap : INT;
 *     W unset __yew_c; 'yew' --yew-env0 </dev/null >&9;
 *     W printf '\036%s.\036' 'N-S' >&9
 *
 *   - CMD is single-quoted and eval'd, so an unbalanced quote or a syntax
 *     error is eval's failure: the rest of the line still runs and the
 *     frame completes.  W is `command` (dash, bash, ksh: a special
 *     built-in's syntax error would otherwise EXIT a POSIX shell, and
 *     `command` also skips a user function named eval or printf) or
 *     `builtin` (zsh, whose `command` runs only external programs); the
 *     session's first frame probes which one the shell has.
 *   - The command runs inside `__yew_run`, whose INT trap RETURNS: with
 *     the idle `trap : INT` alone, a cancelled `sleep 9; make install`
 *     would go on to install (every shell resumes the list after a
 *     trapped SIGINT).  No arguments: `$#` is 0, as under `$SHELL -c`.
 *   - N is a 16-byte random per-session NONCE in hex and S the frame's
 *     sequence number, so neither a command's output nor a replay of an
 *     earlier frame's bytes can end a frame.
 *   - fd 9 is the session's own dup of the output pipe, opened by the
 *     prologue and closed for the command itself: a command's
 *     `exec >/dev/null` cannot swallow the markers.
 *   - After the status marker, `yew --yew-env0` (yew_job_self_exe) prints
 *     `YEW0 <len>:` and exactly <len> bytes: getcwd() (the logical $PWD
 *     when it names the same directory) and every exported NAME=value,
 *     each NUL-terminated.  Length-prefixed and NUL-separated, so a
 *     directory or value holding newlines, 0x1e or bytes >= 0x80 comes
 *     back exactly, and the end marker after it cannot be mistaken for
 *     part of it.
 *
 * The parser is a byte state machine over whatever one read delivered;
 * a partial marker or record waits in the link for the next read, so a
 * slow or split frame never blocks the event loop.
 *
 * Each command is a PROXY job (job.h) with its own `*job:N*` buffer and
 * *jobs* row.  A cancel sends SIGINT to the session's process group (the
 * prologue's `trap : INT` keeps the shell alive, the command dies, the
 * frame completes with 128+2); a second cancel within
 * YEW_SHSESSION_ESCALATE_MS -- or a kill_force -- SIGKILLs the group and
 * ends the session.  When the shell goes (`exit`, `exec`, death) the
 * next plain `:!` starts a new one in the LAST directory with the last
 * exports; `ed.shell.reset` starts over from the workspace root.
 *
 * NAMED LIMITS.  A background job started inside the session (`cmd &`)
 * may print into a LATER command's buffer, or into none.  A command
 * that closes fd 9 itself, or redefines `command`/`builtin`, can stall
 * its frame: cancel twice.  Job control (`bg`/`fg`/`jobs`) and sourcing
 * rc files are deferred; fish, nu, xonsh, elvish and pwsh run each `:!`
 * fresh (one message per editor).
 */

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

typedef struct Ed Ed;
typedef struct YewShSession YewShSession;

enum {
    /* A second cancel inside this window escalates to SIGKILL. */
    YEW_SHSESSION_ESCALATE_MS = 2000,
    /* The largest env record accepted; past it the frame completes
     * without updating the session's state. */
    YEW_SHSESSION_ENV_MAX = 8 * 1024 * 1024
};

/* `shell.session` is `persistent` and $SHELL is POSIX-family.  An
 * unsupported shell is announced once per editor. */
bool yew_shsession_wanted(Ed *ed);
/* A command is running inside the session now. */
bool yew_shsession_busy(Ed *ed);
/*
 * Run `cmdline` inside the session, starting one if there is none.
 * Returns the command's proxy job id (sink YEW_SINK_BUFFER, no buffer
 * yet -- the caller attaches it), or 0 with `err`.  Refused while busy.
 */
u32 yew_shsession_run(Ed *ed, const char *cmdline, char *err,
                      size_t errsz);

/*
 * The directory and exports the session last reported, absolute.  With
 * no session state (none yet, after ed.shell.reset, or `fresh`) the cwd
 * is the workspace root and the env is NULL -- "the standard job
 * environment", which yew_job_env builds from environ.
 */
const char *yew_shsession_cwd(Ed *ed);
const char *const *yew_shsession_env(Ed *ed);

/*
 * The event loop's end path, after yew_job_settle: a session whose shell
 * is gone finishes its running command, says so once, and releases its
 * job; abandoned shells are released once reaped.
 */
void yew_shsession_settle(Ed *ed);
/* End the session now without a message; its last state is kept
 * (`shell.session` switched to `fresh`). */
void yew_shsession_end(Ed *ed);
/* ed.shell.reset: end it and forget its directory and exports. */
void yew_shsession_reset(Ed *ed);
/* Editor teardown, BEFORE yew_jobs_free. */
void yew_shsession_free(Ed *ed);
/* The session shell's own (internal) job id, or 0. */
u32 yew_shsession_job(Ed *ed);

/* `yew --yew-env0`: the frame's env record on stdout.  Exit status. */
int yew_shsession_env0_main(void);

#endif
