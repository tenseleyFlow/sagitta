#include "util/base.h"

/* Keep this allocation in a second translation unit: the accounting key is
 * the spelling of file + line, even when string literals have distinct
 * addresses after compilation. */
void *yew_test_alloc_peer_site(void)
{
#if YEW_ALLOC_DEBUG
    return yew_xmalloc_at(7U, "alloc-shared.c", 700);
#else
    return NULL;
#endif
}
