/*
 * YEW-F-096 — closeout checks consume ignored sprint contracts.
 *
 * Correct behavior: every canonical Sprint 0--60 contract, all fractional
 * and supporting contracts, the authoring contract, and the index are tracked
 * by Git.  A clean checkout must carry the same source evidence as a maintainer
 * worktree.  The obsolete pre-integration Sprint 57.11 mouse draft is not a
 * canonical contract and must remain untracked.
 *
 * Baseline failure: only 37 of the 97 canonical sprint Markdown files are
 * tracked.  Ignored local copies let check-closeouts.sh report success even
 * though a clean checkout cannot reproduce most of its 747 source rows.
 */
#include "audit.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TRACKED_CAP 65536u

static bool collect_tracked(char *out, size_t cap, size_t *out_len)
{
    int fds[2];
    pid_t pid;
    pid_t waited;
    size_t len = 0u;
    int status;

    if (pipe(fds) != 0)
        return false;
    pid = fork();
    if (pid < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return false;
    }
    if (pid == 0) {
        (void)close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        (void)close(fds[1]);
        (void)execlp("git", "git", "ls-files", "--", ".docs/sprints",
                     (char *)NULL);
        _exit(127);
    }
    (void)close(fds[1]);
    while (len < cap) {
        ssize_t got = read(fds[0], out + len, cap - len);

        if (got > 0) {
            len += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            (void)close(fds[0]);
            (void)waitpid(pid, &status, 0);
            return false;
        }
        break;
    }
    (void)close(fds[0]);
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (len == cap || waited != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return false;
    *out_len = len;
    return true;
}

static bool line_is(const char *text, size_t len, const char *wanted)
{
    size_t wanted_len = strlen(wanted);
    size_t at = 0u;

    while (at < len) {
        size_t end = at;

        while (end < len && text[end] != '\n')
            end++;
        if (end - at == wanted_len &&
            memcmp(text + at, wanted, wanted_len) == 0)
            return true;
        at = end < len ? end + 1u : end;
    }
    return false;
}

static size_t line_count(const char *text, size_t len)
{
    size_t count = 0u;
    size_t i;

    for (i = 0u; i < len; i++) {
        if (text[i] == '\n')
            count++;
    }
    return count;
}

static size_t sprint_prefix_count(const char *text, size_t len, unsigned id)
{
    char prefix[8];
    size_t prefix_len;
    size_t count = 0u;
    size_t at = 0u;

    (void)snprintf(prefix, sizeof(prefix), "s%02u-", id);
    prefix_len = strlen(prefix);
    while (at < len) {
        const char *line = text + at;
        size_t end = at;
        size_t slash = at;

        while (end < len && text[end] != '\n') {
            if (text[end] == '/')
                slash = end + 1u;
            end++;
        }
        if (end - slash >= prefix_len &&
            memcmp(text + slash, prefix, prefix_len) == 0)
            count++;
        at = end < len ? end + 1u : end;
        (void)line;
    }
    return count;
}

bool test_yew_f_096(char *why, size_t why_cap)
{
    static const char stale[] =
        ".docs/sprints/13-performance/"
        "s57_11-mouse-overhaul-context-menus.md";
    static const char sprint57[] =
        ".docs/sprints/13-performance/s57-size-and-embedded.md";
    char tracked[TRACKED_CAP];
    size_t tracked_len;
    size_t count;
    unsigned id;

    if (!collect_tracked(tracked, sizeof(tracked), &tracked_len)) {
        (void)snprintf(why, why_cap,
                       "cannot enumerate tracked sprint contracts");
        return false;
    }
    count = line_count(tracked, tracked_len);
    if (count != 97u) {
        (void)snprintf(why, why_cap,
                       "Git tracks %lu of 97 canonical sprint files",
                       (unsigned long)count);
        return false;
    }
    if (!line_is(tracked, tracked_len, ".docs/sprints/AUTHORING.md") ||
        !line_is(tracked, tracked_len, ".docs/sprints/index.md")) {
        (void)snprintf(why, why_cap,
                       "sprint authoring contract or index is untracked");
        return false;
    }
    if (line_is(tracked, tracked_len, stale)) {
        (void)snprintf(why, why_cap,
                       "obsolete Sprint 57.11 mouse draft is tracked");
        return false;
    }
    for (id = 0u; id <= 60u; id++) {
        size_t matches;

        if (id == 57u) {
            if (!line_is(tracked, tracked_len, sprint57)) {
                (void)snprintf(why, why_cap,
                               "canonical Sprint 57 contract is untracked");
                return false;
            }
            continue;
        }
        matches = sprint_prefix_count(tracked, tracked_len, id);
        if (matches != 1u) {
            (void)snprintf(why, why_cap,
                           "Sprint %u resolves to %lu tracked contracts", id,
                           (unsigned long)matches);
            return false;
        }
    }
    return true;
}
