#ifndef YEW_TESTS_PERF_RUNTIME_H
#define YEW_TESTS_PERF_RUNTIME_H

/*
 * Benchmarks measure the checkout's runtime/, never an installed one.
 *
 * The perf targets export YEW_RUNTIME_DIR=$(abspath runtime).  Set, it is
 * the one runtime directory: every shipped file resolves under it alone
 * (yew_runtime_root, yew_runtime_file), and a child yew inherits it.
 *
 * yew_perf_runtime_check refuses to measure otherwise: YEW_RUNTIME_DIR
 * unset, empty, or naming any directory but ./runtime (benchmarks run
 * from the repository root, as their fixture paths already require) is
 * a hard error.  Benchmarks that only spawn yew call it alone.
 *
 * yew_perf_runtime_pin, for benchmarks linked against the core, also
 * points the compiled install prefix at a path that cannot exist, so a
 * resolver that ever consulted the prefix would fail to load rather
 * than quietly read an installed copy.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "util/runtime_asset.h"

#define YEW_PERF_NO_PREFIX "/nonexistent/yew-perf-checkout-runtime-only"

/*
 * Same directory by identity (device + inode), not by resolved path: the
 * check needs only stat(2), which every POSIX feature level declares --
 * realpath(3) is hidden by musl unless _XOPEN_SOURCE/_GNU_SOURCE is set,
 * and the benchmarks that include this header do not all set it.
 */
static inline bool yew_perf_runtime_check(const char *prog)
{
    const char *env = getenv("YEW_RUNTIME_DIR");
    struct stat want;
    struct stat got;
    bool have_want;
    bool ok;

    if (env == NULL || env[0] == '\0') {
        (void)fprintf(stderr,
                      "%s: YEW_RUNTIME_DIR is unset; benchmarks measure "
                      "this checkout's runtime/ only (run them through "
                      "make)\n", prog);
        return false;
    }
    have_want = stat("runtime", &want) == 0 && S_ISDIR(want.st_mode);
    ok = have_want && stat(env, &got) == 0 && S_ISDIR(got.st_mode) &&
         got.st_dev == want.st_dev && got.st_ino == want.st_ino;
    if (!ok)
        (void)fprintf(stderr,
                      "%s: YEW_RUNTIME_DIR=%s is not this checkout's "
                      "runtime/ (%s); refusing to measure another "
                      "runtime\n", prog, env,
                      have_want ? "./runtime" : "no ./runtime here");
    return ok;
}

static inline bool yew_perf_runtime_pin(const char *prog)
{
    if (!yew_perf_runtime_check(prog))
        return false;
    yew_runtime_test_set_prefix(YEW_PERF_NO_PREFIX);
    return true;
}

#endif
