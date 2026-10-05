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
#include <string.h>

#include "util/runtime_asset.h"

#define YEW_PERF_NO_PREFIX "/nonexistent/yew-perf-checkout-runtime-only"

static inline bool yew_perf_runtime_check(const char *prog)
{
    const char *env = getenv("YEW_RUNTIME_DIR");
    char *want;
    char *got;
    bool ok;

    if (env == NULL || env[0] == '\0') {
        (void)fprintf(stderr,
                      "%s: YEW_RUNTIME_DIR is unset; benchmarks measure "
                      "this checkout's runtime/ only (run them through "
                      "make)\n", prog);
        return false;
    }
    want = realpath("runtime", NULL);
    got = realpath(env, NULL);
    ok = want != NULL && got != NULL && strcmp(want, got) == 0;
    if (!ok)
        (void)fprintf(stderr,
                      "%s: YEW_RUNTIME_DIR=%s is not this checkout's "
                      "runtime/ (%s); refusing to measure another "
                      "runtime\n", prog, env,
                      want == NULL ? "no ./runtime here" : want);
    free(want);
    free(got);
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
