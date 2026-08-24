#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <string.h>

#include "fl/diag.h"
#include "mod/plug/pkg.h"
#include "util/arena.h"
#include "util/buf.h"

static void spec_expect(const char *spec, const char *want)
{
    Arena a;
    DiagCtx dc;
    Bytebuf out;

    arena_init(&a);
    fl_diag_init(&dc, &a);
    bytebuf_init(&out);
    YEW_ASSERT(yew_pkg_resolve_spec(spec, &out, &dc));
    YEW_ASSERT_EQ_U64(out.len, strlen(want));
    YEW_ASSERT_EQ_MEM(out.data, want, out.len);
    YEW_ASSERT_EQ_U64(fl_diag_errors(&dc), 0U);
    bytebuf_free(&out);
    arena_free_all(&a);
}

static void spec_reject(const char *spec)
{
    Arena a;
    DiagCtx dc;
    Bytebuf out;

    arena_init(&a);
    fl_diag_init(&dc, &a);
    bytebuf_init(&out);
    YEW_ASSERT(!yew_pkg_resolve_spec(spec, &out, &dc));
    YEW_ASSERT_EQ_U64(out.len, 0U);
    YEW_ASSERT(fl_diag_errors(&dc) != 0U);
    bytebuf_free(&out);
    arena_free_all(&a);
}

void test_plug_pkg_spec_closed_resolution_table(void)
{
    spec_expect("gh:u/r", "https://github.com/u/r.git");
    spec_expect("gl:u/r", "https://gitlab.com/u/r.git");
    spec_expect("cb:u/r", "https://codeberg.org/u/r.git");
    spec_expect("sr:~u/r", "https://git.sr.ht/~u/r");
    spec_expect("https://host/r.git", "https://host/r.git");
    spec_expect("http://host/r.git", "http://host/r.git");
    spec_expect("ssh://host/r.git", "ssh://host/r.git");
    spec_expect("git://host/r.git", "git://host/r.git");
    spec_expect("git@host:path/r.git", "git@host:path/r.git");
    spec_expect("/tmp/r", "/tmp/r");
    spec_expect("./r", "./r");
    spec_expect("file:///tmp/r", "file:///tmp/r");
}

void test_plug_pkg_spec_rejects_ambiguous_and_injected_inputs(void)
{
    spec_reject("");
    spec_reject("u/r");
    spec_reject("../r");
    spec_reject("-u/r");
    spec_reject("gh:");
    spec_reject("gh:u/../r");
    spec_reject("gh:u/r\n--upload-pack=x");
    spec_reject("https://h/r\rbad");
    spec_reject("https://h/r\tcolumn");
    spec_reject("https://h/r\033]8;;https://invalid.example\a");
    spec_reject("https://h/r\x9bunsafe");
    spec_reject("git@hostonly");
    spec_reject("relative/path");
}

void test_plug_pkg_ref_and_pin_validation_is_closed(void)
{
    static const char rev[] = "0123456789abcdef0123456789abcdef01234567";
    char pin[45];
    char huge[301];

    (void)memset(huge, 'a', sizeof(huge) - 1U);
    huge[sizeof(huge) - 1U] = '\0';
    (void)memcpy(pin, "rev:", 4U);
    (void)memcpy(pin + 4U, rev, sizeof(rev));
    YEW_ASSERT(yew_pkg_ref_valid("v1.2.3"));
    YEW_ASSERT(yew_pkg_ref_valid("feature/name_1"));
    YEW_ASSERT(!yew_pkg_ref_valid("-bad"));
    YEW_ASSERT(!yew_pkg_ref_valid("a..b"));
    YEW_ASSERT(!yew_pkg_ref_valid("bad name"));
    YEW_ASSERT(!yew_pkg_ref_valid("bad\nname"));
    YEW_ASSERT(!yew_pkg_ref_valid(huge));
    YEW_ASSERT(yew_pkg_pin_valid("head"));
    YEW_ASSERT(yew_pkg_pin_valid("tag:v1.0"));
    YEW_ASSERT(yew_pkg_pin_valid("branch:main"));
    YEW_ASSERT(yew_pkg_pin_valid(pin));
    pin[43] = 'g';
    YEW_ASSERT(!yew_pkg_pin_valid(pin));
    YEW_ASSERT(!yew_pkg_pin_valid("rev:abc"));
    YEW_ASSERT(!yew_pkg_pin_valid("tag:-bad"));
    YEW_ASSERT(!yew_pkg_pin_valid("branch:a..b"));
    YEW_ASSERT(!yew_pkg_pin_valid("latest"));
}
