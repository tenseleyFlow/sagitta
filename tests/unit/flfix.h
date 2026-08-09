#ifndef SAG_TEST_FLFIX_H
#define SAG_TEST_FLFIX_H

/*
 * Sprint 31's shared Fletch test fixture.
 *
 * Nine test files run programs and compare an answer; without one
 * fixture they would each grow their own compile-and-run plumbing and
 * their own idea of how to spell a raised error, and the ninth would
 * disagree with the first.
 *
 * A RESULT IS RENDERED AS TEXT, and a raise reads `!kind: msg`.  That
 * one convention is what lets a positive and a negative case sit on
 * adjacent lines in the same table, which is how the coverage comment
 * at the top of each file stays checkable by eye.
 *
 * The renderer here is deliberately DUMB -- scalars and nothing else.
 * A test that needs to see inside a list or a map calls fmt.repr in the
 * program under test, so `fmt` is exercised rather than trusted, and a
 * bug in it cannot quietly make another module's test pass.
 */

#include "harness.h"

#include "fl/std.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

typedef struct FlFix {
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    u32 ndiag;
    char first[256];
    /* The origin every program in this fixture compiles under. */
    FlOrigin origin;
    char tmp[256];
    bool has_tmp;
} FlFix;

/* CLI origin, no grants, the seven builtin modules registered. */
void flfix_open(FlFix *f);
void flfix_close(FlFix *f);

/*
 * A private directory, removed at close, with the fixture's origin
 * repointed at `<dir>/init.fl` -- so a quoted import and io.glob both
 * resolve inside it rather than against the process cwd.
 */
const char *flfix_tmpdir(FlFix *f);
void flfix_write(FlFix *f, const char *rel, const char *body);
void flfix_mkdir(FlFix *f, const char *rel);

/* Re-origins the fixture in place; `caps` is a mask of FL_CAP_*. */
void flfix_as(FlFix *f, u8 kind, u32 caps);

/*
 * Compiles and runs `src`, rendering the outcome into `out`:
 *
 *   a string result   the bytes themselves
 *   int/bool/nil      "42" / "true" / "nil"
 *   anything else     "<list>", "<map>", ...
 *   a raise           "!kind: msg"
 *   a parse failure   "!parse" (the first diagnostic lands in f->first)
 */
void flfix_run(FlFix *f, const char *src, char *out, size_t cap);

/* Asserts the rendered outcome.  A macro so the failure names the
 * CALLER's line, which is the line a reader needs. */
#define FL_EQ(f, src, want)                                                   \
    do {                                                                      \
        char flfix_got_[8192];                                                \
                                                                              \
        flfix_run((f), (src), flfix_got_, sizeof(flfix_got_));                \
        SAG_ASSERT_EQ_STR(flfix_got_, (want));                                \
    } while (0)

#endif /* SAG_TEST_FLFIX_H */
