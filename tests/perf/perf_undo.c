#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#include "text/edit.h"

enum {
    WARM_CYCLES = 1000,
    GATE_CYCLES = 100000,
    RSS_BUDGET_BYTES = 1024 * 1024
};

static bool max_rss_bytes(u64 *out)
{
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0)
        return false;
#if defined(__APPLE__)
    *out = (u64)usage.ru_maxrss;
#else
    *out = (u64)usage.ru_maxrss * 1024U;
#endif
    return true;
}

static bool cycle(EditCtx *edit, u64 expected_add_len)
{
    if (!yew_undo(edit) || yew_textbuf_len(edit->tb) != 0U ||
        edit->tb->add.len != expected_add_len)
        return false;
    if (!yew_redo(edit) || yew_textbuf_len(edit->tb) != 8U ||
        edit->tb->add.len != expected_add_len)
        return false;
    return true;
}

int main(void)
{
    static const u8 payload[] = {'S', 'a', 'g', 'i', 't', 't', 'a', '\n'};
    TextBuf *tb = yew_textbuf_new();
    UndoTree *undo;
    EditCtx edit;
    u64 add_len;
    u64 rss_before;
    u64 rss_after;
    u64 rss_growth;
    size_t i;

    if (tb == NULL)
        return 2;
    undo = yew_undo_new(tb);
    if (undo == NULL) {
        yew_textbuf_free(tb);
        return 2;
    }
    (void)memset(&edit, 0, sizeof(edit));
    edit.tb = tb;
    edit.undo = undo;
    yew_undo_begin(&edit, YEW_TXN_PASTE);
    yew_edit_insert(&edit, BYTEOFF(0U), payload, sizeof(payload));
    yew_undo_end(&edit);
    add_len = tb->add.len;

    for (i = 0U; i < WARM_CYCLES; i++) {
        if (!cycle(&edit, add_len))
            goto fail;
    }
    if (!max_rss_bytes(&rss_before))
        goto fail;
    for (i = 0U; i < GATE_CYCLES; i++) {
        if (!cycle(&edit, add_len))
            goto fail;
    }
    if (!max_rss_bytes(&rss_after))
        goto fail;
    rss_growth = rss_after > rss_before ? rss_after - rss_before : 0U;
    yew_textbuf_check(tb);
    (void)printf("undo-perf: warm=%u cycles=%u add_bytes=%llu "
                 "rss_growth=%llu rss_budget=%u%s\n",
                 WARM_CYCLES, GATE_CYCLES,
                 (unsigned long long)add_len,
                 (unsigned long long)rss_growth, RSS_BUDGET_BYTES,
                 rss_growth <= RSS_BUDGET_BYTES ? "" : " OVER-BUDGET");
    yew_undo_free(undo);
    yew_textbuf_free(tb);
    return rss_growth <= RSS_BUDGET_BYTES ? 0 : 1;

fail:
    (void)fprintf(stderr, "undo-perf: undo/redo stability check failed\n");
    yew_undo_free(undo);
    yew_textbuf_free(tb);
    return 1;
}
