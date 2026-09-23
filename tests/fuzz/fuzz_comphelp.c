/*
 * Sprint 57.25: the --help parser under arbitrary bytes.
 *
 * Help text is whatever a program on $PATH chose to print, so the parser
 * is an input boundary like any other.  Seeded from the captured layout
 * fixtures (tests/fuzz/corpus/fuzz_comphelp/ holds copies of them); the
 * first byte also picks how much of a subcommand path the usage rows are
 * read against.
 *
 * Asserted: termination (the harness watchdog), BOUNDED output (at most
 * 512 subcommands and 512 flags, descriptions within 60 bytes, names in
 * their grammar, aliases never duplicating a name), the tree's data form
 * reading back through the spec reader to the same tree, and -- in the
 * SAN=1 build -- no ASan/UBSan report.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl/data.h"
#include "fl/diag.h"
#include "fl/vm.h"
#include "ui/comphelp.h"
#include "ui/compspec.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

static bool name_ok(const char *s)
{
    size_t i;

    if (s == NULL || !(s[0] >= 'a' && s[0] <= 'z') || strlen(s) > 64U)
        return false;
    for (i = 1U; s[i] != '\0'; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-'))
            return false;
    }
    return true;
}

static bool check_tree(const YewSpecNode *root, char *why, size_t why_cap)
{
    u32 i;
    u32 k;

    if (root->n_subs > 512U || root->n_flags > 512U) {
        (void)snprintf(why, why_cap, "unbounded: %u subs, %u flags",
                       (unsigned)root->n_subs, (unsigned)root->n_flags);
        return false;
    }
    for (i = 0U; i < root->n_subs; i++) {
        const YewSpecNode *s = &root->subs[i];
        u32 j;

        if (!name_ok(s->name) || s->parent != root ||
            (s->desc != NULL && strlen(s->desc) > 60U)) {
            (void)snprintf(why, why_cap, "bad subcommand %u", (unsigned)i);
            return false;
        }
        for (k = 0U; k < s->n_aliases; k++) {
            if (!name_ok(s->aliases[k])) {
                (void)snprintf(why, why_cap, "bad alias of %s", s->name);
                return false;
            }
        }
        for (j = 0U; j < i; j++) {
            if (strcmp(root->subs[j].name, s->name) == 0) {
                (void)snprintf(why, why_cap, "duplicate %s", s->name);
                return false;
            }
        }
    }
    for (i = 0U; i < root->n_flags; i++) {
        const YewSpecFlag *f = &root->flags[i];

        if ((f->lng == NULL && f->shrt == NULL) ||
            (f->shrt != NULL && strlen(f->shrt) != 1U) ||
            (f->desc != NULL && strlen(f->desc) > 60U) ||
            (f->arg_optional && f->arg == NULL)) {
            (void)snprintf(why, why_cap, "bad flag %u", (unsigned)i);
            return false;
        }
    }
    return true;
}

/* The data form reads back, through the spec reader, to a tree with the
 * same shape. */
static bool check_round_trip(const YewCompSpec *spec, char *why,
                             size_t why_cap)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    Bytebuf out;
    FlValue v;
    YewCompSpec *back;
    char err[256];
    bool ok = true;

    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    (void)fl_vm_init(&vm, &arena, &in, &dc);
    bytebuf_init(&out);
    fl_data_write(&out, yew_compspec_write_node(&vm,
                                                yew_compspec_root(spec)),
                  0U);
    v = fl_data_read(&vm, (const char *)out.data, out.len, &dc);
    back = fl_diag_errors(&dc) == 0U
               ? yew_compspec_read_node("fuzz", &v, err, sizeof(err))
               : NULL;
    if (back == NULL) {
        (void)snprintf(why, why_cap, "data form does not read back");
        ok = false;
    } else if (yew_compspec_root(back)->n_subs !=
                   yew_compspec_root(spec)->n_subs ||
               yew_compspec_root(back)->n_flags !=
                   yew_compspec_root(spec)->n_flags) {
        (void)snprintf(why, why_cap, "data form changed the tree");
        ok = false;
    }
    yew_compspec_free(back);
    bytebuf_free(&out);
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
    return ok;
}

static bool check_comphelp(const u8 *data, size_t len, char *why,
                           size_t why_cap)
{
    static const char *const words[] = {"tool", "sub", "deeper"};
    u32 n_words = len == 0U ? 1U : (u32)(data[0] % 4U);
    YewCompSpec *spec;
    bool ok = true;

    spec = yew_comphelp_parse("help:fuzz", words, n_words,
                              (const char *)data, len);
    if (spec != NULL) {
        ok = check_tree(yew_compspec_root(spec), why, why_cap) &&
             check_round_trip(spec, why, why_cap);
        yew_compspec_free(spec);
    }
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_comphelp", NULL, check_comphelp);
}
