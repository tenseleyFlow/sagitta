#define _POSIX_C_SOURCE 200809L

#include "fl/flapi.h"

#include <string.h>

#include "edit/ed.h"
#include "fl/handle.h"
#include "fl/std.h"
#include "fl/value.h"
#include "text/piece.h"
#include "util/buf.h"

/*
 * THE ONE THING TO KNOW ABOUT THIS FILE: it does not mutate.
 *
 * scripts/check-fl-choke.sh fails the build if anything under src/fl/
 * names sag_edit_*, sag_textbuf_* (writers), sag_undo_record, the
 * register API or sag_cset_*.  Reading through TextIter is deliberate
 * for that reason as well as the obvious one -- a Fletch call becomes an
 * editor EFFECT in exactly one place, and that place does not exist yet.
 */

/* The editor, or a raised "handle" saying why there is not one. */
static Ed *api_ed(FlVm *vm)
{
    if (vm->ed == NULL) {
        (void)fl_raise(vm, "handle",
                       "no editor: this build of the prompt has none");
        return NULL;
    }
    return vm->ed;
}

static bool b_current(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Ed *ed = api_ed(vm);
    Buffer *b;

    (void)a;
    (void)n;
    if (ed == NULL)
        return false;
    b = sag_ed_doc(ed);
    if (b == NULL) {
        (void)fl_raise(vm, "handle", "no current buffer");
        return false;
    }
    *out = fl_h_buf_make(ed, b);
    return true;
}

static bool b_len(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b;

    (void)n;
    /* fl_h_buf raises "handle" for a dead, closed or wrong-kind value,
     * and "type" for a non-handle -- both spec §9, neither our business
     * to re-word here. */
    b = fl_h_buf(vm, a[0]);
    if (b == NULL)
        return false;
    *out = FL_INT_V((i64)sag_textbuf_len(b->tb));
    return true;
}

static bool b_path(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b;

    (void)n;
    b = fl_h_buf(vm, a[0]);
    if (b == NULL)
        return false;
    /*
     * NIL for a scratch buffer, not "".  A script asking "where does
     * this live" has to be able to tell "nowhere yet" from "a file whose
     * name is the empty string", and only one of those is representable
     * as a path.
     */
    if (b->path == NULL) {
        *out = FL_NIL_V;
        return true;
    }
    *out = FL_OBJ_V(FL_STR, fl_str_new(vm, b->path, (u32)strlen(b->path)));
    return true;
}

static bool b_text(FlVm *vm, FlValue *a, u32 n, FlValue *out)
{
    Buffer *b;
    Bytebuf text;
    TextIter it;

    (void)n;
    b = fl_h_buf(vm, a[0]);
    if (b == NULL)
        return false;
    bytebuf_init(&text);
    if (sag_textiter_begin(&it, b->tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            if (!sag_textiter_chunk(&it, b->tb, &bytes, &len))
                break;
            bytebuf_append(&text, bytes, (size_t)len);
        } while (sag_textiter_advance(&it, b->tb));
    }
    /*
     * The buffer's bytes are whatever the user has, INCLUDING invalid
     * UTF-8 -- invariant 2 says we neither lose nor repair them, and a
     * Fletch string is a byte string (spec §4), so this hands them over
     * unchanged rather than sanitising on the way out.
     */
    *out = FL_OBJ_V(FL_STR, fl_str_new(vm, (const char *)text.data,
                                       (u32)text.len));
    bytebuf_free(&text);
    return true;
}

static const FlNativeDef BUF_DEFS[] = {
    {"current", b_current, 0U, 0U, 0U, "() -> buffer"},
    {"len",     b_len,     1U, 1U, 0U, "(buffer) -> int"},
    {"path",    b_path,    1U, 1U, 0U, "(buffer) -> str|nil"},
    {"text",    b_text,    1U, 1U, 0U, "(buffer) -> str"}
};

const FlModuleDef fl_mod_buf = {
    "buf", BUF_DEFS, (u32)SAG_ARRAY_LEN(BUF_DEFS), NULL, 0U
};
