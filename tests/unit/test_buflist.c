/* Sprint 19 §Prerequisites(b): the workspace buffer list and the scratch
 * flags job output needs.  The list holds POINTERS, so these tests pin the
 * property that actually matters — a Win's Buffer* stays valid across
 * growth, which a value array would silently break. */
#include "harness.h"

#include <string.h>

#include "edit/ed.h"

static void buflist_fixture(Ed *ed)
{
    sag_ed_init(ed);
    SAG_ASSERT(sag_ed_open_scratch(ed));
}

void test_buflist_document_occupies_slot_zero(void)
{
    Ed ed;

    buflist_fixture(&ed);
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, 1U);
    SAG_ASSERT(ed.ws.bufs[0] == &ed.buffer);
    SAG_ASSERT(ed.win->buf == &ed.buffer);
    /* The document buffer is not scratch and has no synthetic name. */
    SAG_ASSERT_EQ_U64(ed.buffer.flags, 0U);
    SAG_ASSERT_NULL(ed.buffer.name);
    sag_ed_free(&ed);
}

void test_buflist_scratch_new_sets_flags_and_name(void)
{
    Ed ed;
    Buffer *job;

    buflist_fixture(&ed);
    job = sag_ws_scratch_new(&ed, "*job:1 echo*", SAG_BUF_NOUNDO);
    SAG_ASSERT_NOT_NULL(job);
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, 2U);
    SAG_ASSERT(ed.ws.bufs[1] == job);
    /* SCRATCH is forced on regardless of what the caller passed. */
    SAG_ASSERT((job->flags & SAG_BUF_SCRATCH) != 0U);
    SAG_ASSERT((job->flags & SAG_BUF_NOUNDO) != 0U);
    SAG_ASSERT_EQ_STR(job->name, "*job:1 echo*");
    SAG_ASSERT_NULL(job->path);
    SAG_ASSERT_NOT_NULL(job->tb);
    SAG_ASSERT_NOT_NULL(job->undo);
    SAG_ASSERT_NOT_NULL(job->marks);
    /* A scratch buffer never opens a journal: it has no file to protect. */
    SAG_ASSERT_NULL(job->jrn);
    sag_ed_free(&ed);
}

void test_buflist_label_prefers_name_then_path(void)
{
    Ed ed;
    Buffer *job;

    buflist_fixture(&ed);
    SAG_ASSERT_EQ_STR(sag_buf_label(&ed.buffer), "[No Name]");
    ed.buffer.path = (char *)"notes.txt";
    SAG_ASSERT_EQ_STR(sag_buf_label(&ed.buffer), "notes.txt");
    job = sag_ws_scratch_new(&ed, "*jobs*", 0U);
    SAG_ASSERT_EQ_STR(sag_buf_label(job), "*jobs*");
    ed.buffer.path = NULL;
    sag_ed_free(&ed);
}

void test_buflist_pointers_survive_growth(void)
{
    Ed ed;
    Buffer *first;
    Buffer *held[12];
    u32 i;

    buflist_fixture(&ed);
    first = sag_ws_scratch_new(&ed, "*job:1*", 0U);
    SAG_ASSERT_NOT_NULL(first);
    ed.win->buf = first;
    /* Force several reallocations of the list.  A value array would move
     * `first` out from under ed.win->buf here; a pointer array cannot. */
    for (i = 0U; i < SAG_ARRAY_LEN(held); i++) {
        char name[32];

        (void)snprintf(name, sizeof(name), "*job:%u*", (unsigned)(i + 2U));
        held[i] = sag_ws_scratch_new(&ed, name, 0U);
        SAG_ASSERT_NOT_NULL(held[i]);
    }
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, (u64)SAG_ARRAY_LEN(held) + 2U);
    SAG_ASSERT(ed.win->buf == first);
    SAG_ASSERT(ed.ws.bufs[1] == first);
    for (i = 0U; i < SAG_ARRAY_LEN(held); i++)
        SAG_ASSERT(ed.ws.bufs[i + 2U] == held[i]);
    ed.win->buf = &ed.buffer;
    sag_ed_free(&ed);
}

void test_buflist_find_matches_by_name(void)
{
    Ed ed;
    Buffer *a;
    Buffer *b;

    buflist_fixture(&ed);
    a = sag_ws_scratch_new(&ed, "*job:1 make*", 0U);
    b = sag_ws_scratch_new(&ed, "*jobs*", 0U);
    SAG_ASSERT(sag_ws_scratch_find(&ed, "*job:1 make*") == a);
    SAG_ASSERT(sag_ws_scratch_find(&ed, "*jobs*") == b);
    SAG_ASSERT_NULL(sag_ws_scratch_find(&ed, "*absent*"));
    /* Slot 0 is skipped: the document is not findable as scratch. */
    SAG_ASSERT_NULL(sag_ws_scratch_find(&ed, "[No Name]"));
    sag_ed_free(&ed);
}

void test_buflist_show_buffer_switches_focus(void)
{
    Ed ed;
    Buffer *job;

    buflist_fixture(&ed);
    job = sag_ws_scratch_new(&ed, "*job:1*", 0U);
    SAG_ASSERT(sag_ed_show_buffer(&ed, job));
    SAG_ASSERT(ed.win->buf == job);
    SAG_ASSERT(ed.full_damage);
    /* Idempotent, and the document is always reachable again. */
    SAG_ASSERT(sag_ed_show_buffer(&ed, job));
    SAG_ASSERT(sag_ed_show_buffer(&ed, &ed.buffer));
    SAG_ASSERT(ed.win->buf == &ed.buffer);
    sag_ed_free(&ed);
}

void test_buflist_show_buffer_rejects_foreign(void)
{
    Ed ed;
    Buffer stranger;

    buflist_fixture(&ed);
    (void)memset(&stranger, 0, sizeof(stranger));
    /* A buffer that is not in the list must not become focusable — that is
     * how a Win ends up pointing at storage nobody owns. */
    SAG_ASSERT(!sag_ed_show_buffer(&ed, &stranger));
    SAG_ASSERT(ed.win->buf == &ed.buffer);
    sag_ed_free(&ed);
}

void test_buflist_drop_refocuses_document(void)
{
    Ed ed;
    Buffer *job;

    buflist_fixture(&ed);
    job = sag_ws_scratch_new(&ed, "*job:1*", 0U);
    SAG_ASSERT(sag_ed_show_buffer(&ed, job));
    SAG_ASSERT(ed.win->buf == job);
    /* Dropping the focused buffer must not leave the window dangling. */
    sag_ws_scratch_drop(&ed, job);
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, 1U);
    SAG_ASSERT(ed.win->buf == &ed.buffer);
    sag_ed_free(&ed);
}

void test_buflist_drop_compacts_and_ignores_document(void)
{
    Ed ed;
    Buffer *a;
    Buffer *b;
    Buffer *c;

    buflist_fixture(&ed);
    a = sag_ws_scratch_new(&ed, "*job:1*", 0U);
    b = sag_ws_scratch_new(&ed, "*job:2*", 0U);
    c = sag_ws_scratch_new(&ed, "*job:3*", 0U);
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, 4U);
    sag_ws_scratch_drop(&ed, b);
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, 3U);
    SAG_ASSERT(ed.ws.bufs[1] == a);
    SAG_ASSERT(ed.ws.bufs[2] == c);
    SAG_ASSERT_NULL(sag_ws_scratch_find(&ed, "*job:2*"));
    /* Dropping the document is a no-op, not a use-after-free. */
    sag_ws_scratch_drop(&ed, &ed.buffer);
    SAG_ASSERT_EQ_U64(ed.ws.nbufs, 3U);
    SAG_ASSERT(ed.ws.bufs[0] == &ed.buffer);
    sag_ed_free(&ed);
}
