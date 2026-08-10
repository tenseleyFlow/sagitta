/*
 * Sprint 34 §8: the editor-API fuzzer.
 *
 * TODAY IT FUZZES HANDLES, which is the half DoD 3 names: "ASan-clean
 * under the crafted-handle fuzzer".  The `ed.run` half arrives with the
 * binding layer; the file is named for the whole deliverable so it grows
 * rather than being replaced.
 *
 * WHAT AN ADVERSARY ACTUALLY CONTROLS.  A script cannot forge an
 * FlValue's bits — but it can hold a handle across arbitrary editor
 * activity, and the {slot, gen} pair it holds then names whatever the
 * table has since done with that slot.  So the interesting input is not
 * random bytes into a parser, it is a random {kind, slot, gen} triple
 * against a table that has been churned into an arbitrary state.  That
 * is what this builds.
 *
 * THE INVARIANT IS "NEVER DEREFERENCES", and only a sanitizer can see
 * it: a resolver that skipped its checks would return a Buffer * into
 * freed or wrong memory and every assertion here would still pass.  So
 * the check below asserts the CHEAP half — a failed resolve raises kind
 * "handle" or "type", never anything else, and never succeeds for a
 * slot the table considers dead — and the ASan/valgrind lanes assert
 * the half that matters.  Running this outside a sanitizer proves much
 * less; that is a property of the bug class, not of the harness.
 *
 * PROVEN TO BITE, and worth recording precisely because the answer was
 * not the expected one.  Deleting resolve_slot's `h.slot >= t->n` bound
 * makes this report a heap-buffer-overflow inside resolve_slot within
 * one campaign — that is the crafted-handle attack, and it is what this
 * file is for.
 *
 * WHAT IT DOES NOT REACH: the span-marks-outlive-their-MarkSet shape.
 * Reaching it needs a buffer that genuinely CLOSES, and the scratch
 * buffer here lives for the whole run, so neutering fl_h_drop_buffer
 * changes nothing observable from in here.  That one is covered by
 * fl_handle_drop_buffer_releases_spans instead, which does fail when
 * the drop is neutered.  Do not read a green run of this fuzzer as
 * covering it.
 */
#define _POSIX_C_SOURCE 200809L

#include "fuzzlib.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "fl/handle.h"
#include "fl/diag.h"
#include "fl/std.h"
#include "fl/value.h"
#include "fl/vm.h"
#include "util/arena.h"
#include "util/intern.h"

enum {
    FLAPI_FUZZ_MAX_INPUT = 4U * 1024U,
    /* Each input byte drives one table operation; a few thousand is far
     * past the point where the free list and the generations have been
     * thoroughly scrambled. */
    FLAPI_FUZZ_MAX_OPS = 4096U,
    /* Live handles are capped so a run of pure allocations cannot turn
     * the fuzzer into a memory test. */
    FLAPI_FUZZ_MAX_LIVE = 64U
};

static const FlHandleKind FLAPI_KINDS[] = {FL_H_BUF, FL_H_CUR, FL_H_SPAN,
                                           FL_H_WIN, FL_H_RE};

/* The kind tags a resolver may legitimately report. */
static bool allowed_kind(const char *k)
{
    return strcmp(k, "handle") == 0 || strcmp(k, "type") == 0;
}

static void raised_kind(FlVm *vm, char *out, size_t cap)
{
    FlValue got = FL_NIL_V;
    FlStr *key;

    out[0] = '\0';
    if (vm->err.t != (u8)FL_MAP)
        return;
    key = fl_str_new(vm, "kind", 4U);
    if (!fl_map_get((FlMap *)vm->err.as.o, FL_OBJ_V(FL_STR, key), &got))
        return;
    if (got.t != (u8)FL_STR)
        return;
    (void)snprintf(out, cap, "%.*s", (int)((const FlStr *)got.as.o)->len,
                   ((const FlStr *)got.as.o)->b);
}

/* Resolves and reports whether it succeeded; `kind` gets the raise. */
static bool try_resolve(FlVm *vm, FlHandleKind want, FlValue v, char *kind,
                        size_t cap)
{
    bool ok;

    vm->err = FL_NIL_V;
    switch (want) {
    case FL_H_BUF:
        ok = fl_h_buf(vm, v) != NULL;
        break;
    case FL_H_WIN:
        ok = fl_h_win(vm, v) != NULL;
        break;
    case FL_H_CUR: {
        Win *w = NULL;

        ok = fl_h_cur(vm, v, &w) != NULL;
        break;
    }
    case FL_H_SPAN: {
        Buffer *b = NULL;
        Span s = {0U, 0U};

        ok = fl_h_span(vm, v, &b, &s);
        break;
    }
    default:
        ok = fl_h_re(vm, v) != NULL;
        break;
    }
    raised_kind(vm, kind, cap);
    return ok;
}

/*
 * A handle value assembled from fuzzer bytes.  Built by making a real
 * handle and then OVERWRITING its slot and generation, because the
 * encoding lives in handle.c and a test that reimplemented it would
 * drift from the thing it is testing.
 */
static FlValue crafted(FlHandleTable *t, FlHandleKind k, u32 slot, u32 gen)
{
    FlHandleSlot init;
    FlValue v;
    FlHandle h;

    (void)memset(&init, 0, sizeof(init));
    v = fl_h_make(t, k, &init);
    (void)fl_h_free(t, v);
    h.slot = slot;
    h.gen = gen;
    (void)memcpy(&v.as, &h, sizeof(h));
    return v;
}

static bool check_flapi(const u8 *data, size_t len, char *why, size_t cap)
{
    Arena arena;
    Interner in;
    DiagCtx dc;
    FlVm vm;
    Ed ed;
    FlValue live[FLAPI_FUZZ_MAX_LIVE];
    size_t nlive = 0U;
    size_t i;
    bool ok = true;

    if (len > FLAPI_FUZZ_MAX_INPUT)
        len = FLAPI_FUZZ_MAX_INPUT;
    arena_init(&arena);
    interner_init(&in, &arena);
    fl_diag_init(&dc, &arena);
    fl_vm_init(&vm, &arena, &in, &dc);
    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed)) {
        (void)snprintf(why, cap, "fixture: no scratch buffer");
        ok = false;
        goto done;
    }
    vm.ed = &ed;

    for (i = 0U; i < len && i < FLAPI_FUZZ_MAX_OPS && ok; i++) {
        u8 op = data[i];
        FlHandleKind k = FLAPI_KINDS[(op >> 3) % SAG_ARRAY_LEN(FLAPI_KINDS)];
        char kind[32];

        switch (op & 7U) {
        case 0: /* Take a real handle on a real object. */
            if (nlive < FLAPI_FUZZ_MAX_LIVE)
                live[nlive++] = fl_h_buf_make(&ed, sag_ed_doc(&ed));
            break;
        case 1:
            if (nlive < FLAPI_FUZZ_MAX_LIVE)
                live[nlive++] = fl_h_win_make(&ed, ed.win);
            break;
        case 2:
            if (nlive < FLAPI_FUZZ_MAX_LIVE)
                live[nlive++] = fl_h_span_make(&ed, sag_ed_doc(&ed), 0U, 0U);
            break;
        case 3: /* Release one, scrambling the free list. */
            if (nlive != 0U)
                (void)fl_h_free(&ed.handles, live[--nlive]);
            break;
        case 4: /* Resolve one we still hold: must SUCCEED or raise
                 * cleanly — never anything else. */
            if (nlive != 0U) {
                FlValue v = live[(size_t)(op >> 3) % nlive];

                (void)try_resolve(&vm, fl_h_kind_of(v), v, kind,
                                  sizeof(kind));
                if (kind[0] != '\0' && !allowed_kind(kind)) {
                    (void)snprintf(why, cap, "live handle raised '%s'",
                                   kind);
                    ok = false;
                }
            }
            break;
        case 5: { /* THE CRAFTED CASE: an arbitrary {slot, gen}. */
            u32 slot = (u32)data[(i + 1U) % len];
            u32 gen = (u32)data[(i + 2U) % len] |
                      ((u32)data[(i + 3U) % len] << 16);
            FlValue v = crafted(&ed.handles, k, slot, gen);
            bool got = try_resolve(&vm, k, v, kind, sizeof(kind));

            /*
             * A crafted handle may legitimately resolve: the fuzzer can
             * name a slot that happens to be live with a matching
             * generation, which is a HANDLE THE SCRIPT COULD HAVE HELD
             * anyway and not a forgery.  What may never happen is a
             * refusal reported as some other kind, or a refusal with no
             * error raised at all.
             */
            if (!got && !allowed_kind(kind)) {
                (void)snprintf(why, cap,
                               "crafted {%u,%u} refused with '%s'",
                               (unsigned)slot, (unsigned)gen,
                               kind[0] == '\0' ? "(none)" : kind);
                ok = false;
            }
            if (got && !fl_h_alive(&ed.handles, v)) {
                (void)snprintf(why, cap,
                               "crafted {%u,%u} resolved while dead",
                               (unsigned)slot, (unsigned)gen);
                ok = false;
            }
            break;
        }
        case 6: /* Close the objects under every handle taken so far. */
            fl_h_drop_buffer(&ed, sag_ed_doc(&ed)->id);
            break;
        default:
            /* A non-handle value must be a type error, never a crash. */
            (void)try_resolve(&vm, k, FL_NIL_V, kind, sizeof(kind));
            if (strcmp(kind, "type") != 0) {
                (void)snprintf(why, cap, "nil resolved as '%s'", kind);
                ok = false;
            }
            break;
        }
    }

done:
    vm.ed = NULL;
    sag_ed_free(&ed);
    fl_vm_free(&vm);
    interner_free(&in);
    arena_free_all(&arena);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_flapi",
                         "tests/fuzz/corpus/flapi", check_flapi);
}
