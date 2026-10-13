/*
 * YEW-F-092 — interactive search refinement repaints the full viewport.
 *
 * Correct behavior: changing /na to /nai damages only rows whose match
 * highlights changed (plus footer state), never the entire document.
 */
#include "audit.h"

#include <stdio.h>

#include "edit/ed.h"
#include "search/searchui.h"
#include "term/input.h"
#include "text/piece.h"
#include "ui/cmdline.h"

static Key typed(char c)
{
    Key key = {0};

    key.code = (u32)(u8)c;
    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.ntext = 1U;
    key.text[0] = (u8)c;
    return key;
}

bool test_yew_f_092(char *why, size_t why_cap)
{
    static const u8 doc[] =
        "aaa nail one\n"
        "bbb two\n"
        "ccc three\n"
        "ddd four\n"
        "eee five\n"
        "fff nail six\n";
    Ed ed;
    EditCtx ec;
    Key key;
    bool passed;

    yew_ed_init(&ed);
    if (!yew_ed_open_scratch(&ed)) {
        (void)snprintf(why, why_cap, "cannot open scratch buffer");
        yew_ed_free(&ed);
        return false;
    }
    ec = yew_ed_edit_ctx(&ed);
    if (!yew_edit_insert(&ec, BYTEOFF(0U), doc, sizeof(doc) - 1U)) {
        (void)snprintf(why, why_cap, "cannot seed search fixture");
        yew_ed_free(&ed);
        return false;
    }
    ed.win->rect.h = 10U;
    ed.win->rect.w = 80U;
    yew_search_open(&ed, ed.win, false);
    key = typed('n');
    (void)yew_cmdline_key(&ed, &key);
    key = typed('a');
    (void)yew_cmdline_key(&ed, &key);

    ed.full_damage = false;
    ed.layout_dirty = false;
    ed.doc_damage_lo = 0U;
    ed.doc_damage_hi = 0U;
    key = typed('i');
    (void)yew_cmdline_key(&ed, &key);
    passed = !ed.full_damage && ed.doc_damage_hi < ed.win->rect.h;
    if (!passed) {
        (void)snprintf(why, why_cap,
                       "refinement damage=%u..%u full=%u viewport=%u",
                       (unsigned)ed.doc_damage_lo,
                       (unsigned)ed.doc_damage_hi,
                       ed.full_damage ? 1U : 0U,
                       (unsigned)ed.win->rect.h);
    }
    yew_ed_free(&ed);
    return passed;
}
