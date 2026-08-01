#include "unicode/width.h"

#include "unicode/grapheme.h"
#include "unicode/tables.h"
#include "unicode/utf8.h"
#include "util/log.h"

#include <limits.h>

static SagWidthOpts width_opts;

enum {
    SAG_CWF_SEEN = 1U << 0,
    SAG_CWF_STARTS_ESCAPE = 1U << 1,
    SAG_CWF_HAVE_BASE = 1U << 2,
    SAG_CWF_VS15 = 1U << 3,
    SAG_CWF_VS16 = 1U << 4,
    SAG_CWF_ZWJ = 1U << 5,
    SAG_CWF_EXT_PICT = 1U << 6
};

_Static_assert(sizeof(SagClusterWidthState) <= 12U,
               "cluster width state bloat");

void sag_width_set_opts(const SagWidthOpts *opts)
{
    width_opts.ambiguous_wide = opts != NULL && opts->ambiguous_wide;
}

static int cp_width_record(u32 cp, bool ignore_emoji_presentation)
{
    u16 rec;
    SagEaw eaw;

    if (sag_utf8_is_escape(cp))
        return 4;
    if (cp == 0x0009u)
        return -1;
    if (cp <= 0x001fu || cp == 0x007fu)
        return 2;
    if (cp >= 0x0080u && cp <= 0x009fu)
        return 4;
    if (cp == 0x00adu)
        return 1;

    rec = sag_u_rec(cp);
    if ((rec & SAG_U_ZERO_WIDTH) != 0u)
        return 0;

    eaw = (SagEaw)((rec & SAG_U_EAW_MASK) >> SAG_U_EAW_SHIFT);
    if (eaw == SAG_EAW_W || eaw == SAG_EAW_F)
        return 2;
    if (!ignore_emoji_presentation &&
        (rec & SAG_U_EMOJI_PRESENTATION) != 0u)
        return 2;
    if (eaw == SAG_EAW_A)
        return width_opts.ambiguous_wide ? 2 : 1;
    return 1;
}

int sag_cp_width(u32 cp)
{
    return cp_width_record(cp, false);
}

void sag_cluster_width_init(SagClusterWidthState *state)
{
    if (state == NULL)
        SAG_BUG("sag_cluster_width_init: NULL state");
    state->base_cp = 0U;
    state->base_rec = 0U;
    state->flags = 0U;
    state->ri_count = 0U;
}

void sag_cluster_width_push(SagClusterWidthState *state, u32 cp)
{
    u16 rec;
    u8 gcb;

    if (state == NULL)
        SAG_BUG("sag_cluster_width_push: NULL state");
    rec = sag_utf8_is_escape(cp) ? 0U : sag_u_rec(cp);
    gcb = (u8)(rec & SAG_U_GCB_MASK);
    if ((state->flags & SAG_CWF_SEEN) == 0U) {
        state->flags |= SAG_CWF_SEEN;
        if (sag_utf8_is_escape(cp))
            state->flags |= SAG_CWF_STARTS_ESCAPE;
    }
    if (gcb == (u8)SAG_GCB_RI) {
        if (state->ri_count < 3U)
            state->ri_count++;
    }
    if (cp == 0xfe0eU)
        state->flags |= SAG_CWF_VS15;
    else if (cp == 0xfe0fU)
        state->flags |= SAG_CWF_VS16;
    else if (cp == 0x200dU)
        state->flags |= SAG_CWF_ZWJ;
    if ((rec & SAG_U_EXT_PICT) != 0U)
        state->flags |= SAG_CWF_EXT_PICT;
    if ((state->flags & SAG_CWF_HAVE_BASE) == 0U &&
        gcb != (u8)SAG_GCB_PREPEND) {
        state->base_cp = cp;
        state->base_rec = rec;
        state->flags |= SAG_CWF_HAVE_BASE;
    }
}

int sag_cluster_width_finish(const SagClusterWidthState *state)
{
    if (state == NULL)
        SAG_BUG("sag_cluster_width_finish: NULL state");
    if ((state->flags & SAG_CWF_SEEN) == 0U)
        return 0;
    if ((state->flags & SAG_CWF_STARTS_ESCAPE) != 0U)
        return 4;
    if (state->ri_count == 2U)
        return 2;
    if ((state->flags & SAG_CWF_HAVE_BASE) == 0U)
        return 0;
    if (state->base_cp == 0x0009U)
        return 0;
    if ((state->flags & SAG_CWF_VS16) != 0U &&
        (state->base_rec & SAG_U_EMOJI) != 0U)
        return 2;
    if ((state->flags & SAG_CWF_VS15) != 0U)
        return cp_width_record(state->base_cp, true);
    if ((state->flags & (SAG_CWF_ZWJ | SAG_CWF_EXT_PICT)) ==
        (SAG_CWF_ZWJ | SAG_CWF_EXT_PICT))
        return 2;
    return sag_cp_width(state->base_cp);
}

int sag_cluster_width(const u8 *s, size_t len)
{
    SagClusterWidthState state;
    size_t pos = 0u;
    u32 cp;

    if (s == NULL || len == 0u)
        return 0;

    sag_cluster_width_init(&state);
    while (pos < len) {
        size_t used = sag_utf8_decode(s + pos, len - pos, &cp);

        sag_cluster_width_push(&state, cp);
        pos += used;
    }
    return sag_cluster_width_finish(&state);
}

static size_t cluster_end(const u8 *s, size_t len, size_t pos)
{
    size_t next = sag_gb_next_bytes(s, len, pos);

    if (next <= pos || next > len)
        SAG_BUG("grapheme iterator returned invalid boundary %zu for "
                "offset %zu of %zu", next, pos, len);
    return next;
}

int sag_str_width(const u8 *s, size_t len, u32 tabw)
{
    size_t pos = 0u;
    int cells = 0;

    if (s == NULL)
        return 0;
    if (tabw == 0u)
        tabw = 1u;

    while (pos < len) {
        size_t ascii = pos;

        /* Every printable ASCII byte is one scalar, cluster and cell. */
        while (ascii < len && s[ascii] >= 0x20u && s[ascii] <= 0x7eu)
            ascii++;
        if (ascii != pos) {
            if (ascii - pos > (size_t)(INT_MAX - cells))
                return INT_MAX;
            cells += (int)(ascii - pos);
            pos = ascii;
            continue;
        }

        size_t next = cluster_end(s, len, pos);
        int width = next - pos == 1u && s[pos] == 0x09u
                        ? -1
                        : sag_cluster_width(s + pos, next - pos);

        if (width < 0) {
            u32 tab_cells = tabw - ((u32)cells % tabw);

            if (tab_cells > (u32)INT_MAX)
                return INT_MAX;
            width = (int)tab_cells;
        }
        if (width > INT_MAX - cells)
            return INT_MAX;
        cells += width;
        pos = next;
    }
    return cells;
}

size_t sag_str_clip(const u8 *s, size_t len, int max_cells, int *out_cells)
{
    size_t pos = 0u;
    int cells = 0;

    if (out_cells != NULL)
        *out_cells = 0;
    if (s == NULL || max_cells <= 0)
        return 0u;

    while (pos < len) {
        size_t next = cluster_end(s, len, pos);
        int width = next - pos == 1u && s[pos] == 0x09u
                        ? 1
                        : sag_cluster_width(s + pos, next - pos);

        /* Labels have no tab-stop context; retain a tab as one cell. */
        if (width > max_cells - cells)
            break;
        cells += width;
        pos = next;
    }

    if (out_cells != NULL)
        *out_cells = cells;
    return pos;
}
