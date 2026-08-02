#include "unicode/coords.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/coords_internal.h"
#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "unicode/width.h"
#include "util/log.h"

typedef struct {
    TextIter it;
    const TextBuf *tb;
    const u8 *chunk;
    u64 chunk_len;
    u64 chunk_pos;
    u64 off;
    u64 end;
    bool active;
} TextReader;

typedef struct {
    u32 cp;
    u8 bytes[SAG_UTF8_MAX];
    u8 len;
    u64 start;
    u64 end;
} StreamCp;

typedef struct {
    TextReader reader;
} ClusterReader;

typedef struct {
    u64 start;
    u64 end;
    u64 cells;
    u32 base_cp;
    bool tab;
} StreamCluster;

static void require_span(const TextBuf *tb, Span line)
{
    if (tb == NULL)
        SAG_BUG("unicode coordinates: NULL text buffer");
    if (line.lo > line.hi || line.hi > sag_textbuf_len(tb))
        SAG_BUG("unicode coordinates: invalid line span");
}

static void require_pos(Span line, ByteOff pos)
{
    if (pos.v < line.lo || pos.v > line.hi)
        SAG_BUG("unicode coordinates: offset outside line span");
}

static void reader_init(TextReader *reader, const TextBuf *tb,
                        u64 start, u64 end)
{
    reader->tb = tb;
    reader->chunk = NULL;
    reader->chunk_len = 0U;
    reader->chunk_pos = 0U;
    reader->off = start;
    reader->end = end;
    reader->active = start < end &&
                     sag_textiter_begin(&reader->it, tb, BYTEOFF(start));
    if (reader->active &&
        !sag_textiter_chunk(&reader->it, tb, &reader->chunk,
                            &reader->chunk_len))
        SAG_BUG("unicode coordinates: iterator has no first chunk");
}

static bool reader_get(TextReader *reader, u8 *out)
{
    if (reader->off >= reader->end)
        return false;
    if (!reader->active)
        SAG_BUG("unicode coordinates: iterator ended early");
    while (reader->chunk_pos == reader->chunk_len) {
        if (!sag_textiter_advance(&reader->it, reader->tb))
            SAG_BUG("unicode coordinates: iterator ended early");
        if (!sag_textiter_chunk(&reader->it, reader->tb, &reader->chunk,
                                &reader->chunk_len) ||
            reader->chunk_len == 0U)
            SAG_BUG("unicode coordinates: iterator yielded empty chunk");
        reader->chunk_pos = 0U;
    }
    *out = reader->chunk[(size_t)reader->chunk_pos++];
    reader->off++;
    return true;
}

static u8 utf8_candidate_len(u8 lead)
{
    if (lead < 0x80U)
        return 1U;
    if (lead >= 0xC2U && lead <= 0xDFU)
        return 2U;
    if (lead >= 0xE0U && lead <= 0xEFU)
        return 3U;
    if (lead >= 0xF0U && lead <= 0xF4U)
        return 4U;
    return 1U;
}

static bool reader_cp(TextReader *reader, StreamCp *out)
{
    TextReader probe;
    u8 wanted;
    u8 have = 0U;
    size_t used;

    if (reader->off >= reader->end)
        return false;
    out->start = reader->off;
    probe = *reader;
    if (!reader_get(&probe, &out->bytes[have++]))
        SAG_BUG("unicode coordinates: decoder made no progress");
    wanted = utf8_candidate_len(out->bytes[0]);
    while (have < wanted && reader_get(&probe, &out->bytes[have]))
        have++;
    used = sag_utf8_decode(out->bytes, have, &out->cp);
    if (used == 0U || used > have)
        SAG_BUG("unicode coordinates: decoder returned invalid length");
    out->len = (u8)used;
    while (used-- != 0U) {
        u8 ignored;
        if (!reader_get(reader, &ignored))
            SAG_BUG("unicode coordinates: decoder overran stream");
    }
    out->end = reader->off;
    return true;
}

static void cluster_reader_init(ClusterReader *reader, const TextBuf *tb,
                                u64 start, u64 end)
{
    reader_init(&reader->reader, tb, start, end);
}

static void cluster_reader_free(ClusterReader *reader)
{
    (void)reader;
}

static bool cluster_next(ClusterReader *reader, StreamCluster *out,
                         bool need_width)
{
    SagGbState gb;
    SagClusterWidthState width;
    StreamCp cp;
    u32 cp_count = 0U;
    u32 first_cp = 0U;

    if (reader->reader.off >= reader->reader.end)
        return false;
    out->start = reader->reader.off;
    out->cells = 0U;
    out->base_cp = 0U;
    out->tab = false;
    sag_gb_init(&gb);
    sag_cluster_width_init(&width);
    if (!reader_cp(&reader->reader, &cp))
        SAG_BUG("unicode coordinates: missing cluster head");
    (void)sag_gb_boundary(&gb, cp.cp);
    first_cp = cp.cp;
    out->base_cp = first_cp;
    cp_count = 1U;
    if (need_width)
        sag_cluster_width_push(&width, cp.cp);

    while (reader->reader.off < reader->reader.end) {
        TextReader probe = reader->reader;
        SagGbState next_gb = gb;

        if (!reader_cp(&probe, &cp))
            break;
        if (sag_gb_boundary(&next_gb, cp.cp))
            break;
        reader->reader = probe;
        gb = next_gb;
        if (cp_count != UINT32_MAX)
            cp_count++;
        if (need_width)
            sag_cluster_width_push(&width, cp.cp);
    }
    out->end = reader->reader.off;
    if (need_width) {
        out->tab = cp_count == 1U && first_cp == '\t';
        if (!out->tab) {
            int cells = sag_cluster_width_finish(&width);

            if (cells < 0)
                SAG_BUG("unicode coordinates: negative cluster width");
            out->cells = (u64)cells;
        }
    }
    return true;
}

bool sag_text_cluster_next(const TextBuf *tb, Span span, ByteOff at,
                           SagTextCluster *out)
{
    ClusterReader reader;
    StreamCluster cluster;

    require_span(tb, span);
    require_pos(span, at);
    if (out == NULL)
        SAG_BUG("sag_text_cluster_next: missing output");
    if (at.v == span.hi)
        return false;
    cluster_reader_init(&reader, tb, at.v, span.hi);
    if (!cluster_next(&reader, &cluster, true))
        SAG_BUG("sag_text_cluster_next: cluster scan made no progress");
    cluster_reader_free(&reader);
    if (cluster.end <= cluster.start || cluster.cells > UINT32_MAX)
        SAG_BUG("sag_text_cluster_next: invalid cluster");
    out->bytes = (Span){cluster.start, cluster.end};
    out->base_cp = cluster.base_cp;
    out->cells = (u32)cluster.cells;
    out->tab = cluster.tab;
    return true;
}

static u64 line_content_end(const TextBuf *tb, Span line)
{
    TextReader reader;
    u8 byte;

    if (line.lo == line.hi)
        return line.hi;
    reader_init(&reader, tb, line.hi - 1U, line.hi);
    if (!reader_get(&reader, &byte))
        SAG_BUG("unicode coordinates: cannot inspect line ending");
    if (byte != '\n')
        return line.hi;
    if (line.hi - line.lo >= 2U) {
        reader_init(&reader, tb, line.hi - 2U, line.hi - 1U);
        if (!reader_get(&reader, &byte))
            SAG_BUG("unicode coordinates: cannot inspect CRLF ending");
        if (byte == '\r')
            return line.hi - 2U;
    }
    return line.hi - 1U;
}

enum {
    SAG_GCOL_CHECKPOINT_STRIDE = 64,
    SAG_MOTION_CHECKPOINT_STRIDE = 512,
    SAG_SIMPLE_ASCII_BYPASS_BYTES = 64 * 1024,
    /* Keep the existing 8 MiB first-motion latency gate eager while the
     * 100 MiB open-budget fixtures defer their full Unicode index. */
    SAG_DEFER_INDEX_BYTES = 16 * 1024 * 1024
};

typedef struct {
    SagGbState gb;
    u64 cluster_start;
    u64 gcol;
    bool have_cluster;
    bool after_lf;
} IndexScanState;

typedef struct {
    SagGraphemeIndex *index;
    u64 next_motion;
} IndexBuilder;

static void index_push(SagGraphemeIndex *index, u64 off, u64 gcol)
{
    if (index->len != 0U) {
        u64 previous = index->data[index->len - 1U].off;

        if (off < previous)
            SAG_BUG("grapheme coordinate index is not ordered");
        if (off == previous)
            return;
    }
    if (index->len == index->cap) {
        size_t cap = index->cap == 0U ? 64U : index->cap * 2U;

        if (cap < index->cap)
            SAG_BUG("grapheme coordinate index capacity overflow");
        index->data = sag_xreallocarray(index->data, cap,
                                        sizeof(*index->data));
        index->cap = cap;
    }
    index->data[index->len++] = (SagGraphemeCheckpoint){off, gcol};
}

static void motion_index_push(SagGraphemeIndex *index,
                              SagGraphemeMotionCheckpoint checkpoint)
{
    if (index->motion_len != 0U) {
        u64 previous = index->motion[index->motion_len - 1U].off;

        if (checkpoint.off < previous)
            SAG_BUG("grapheme motion index is not ordered");
        if (checkpoint.off == previous)
            return;
    }
    if (index->motion_len == index->motion_cap) {
        size_t cap = index->motion_cap == 0U
                         ? 64U
                         : index->motion_cap * 2U;

        if (cap < index->motion_cap)
            SAG_BUG("grapheme motion index capacity overflow");
        index->motion = sag_xreallocarray(index->motion, cap,
                                          sizeof(*index->motion));
        index->motion_cap = cap;
    }
    index->motion[index->motion_len++] = checkpoint;
}

static void index_scan_init(IndexScanState *state)
{
    sag_gb_init(&state->gb);
    state->cluster_start = 0U;
    state->gcol = 0U;
    state->have_cluster = false;
    state->after_lf = false;
}

static void index_finalize_cluster(IndexBuilder *builder,
                                   const IndexScanState *state)
{
    if (builder == NULL || !state->have_cluster || state->after_lf ||
        state->gcol == 0U ||
        state->gcol % (u64)SAG_GCOL_CHECKPOINT_STRIDE != 0U)
        return;
    index_push(builder->index, state->cluster_start, state->gcol);
}

static void index_motion_maybe(IndexBuilder *builder,
                               const IndexScanState *state, u64 off)
{
    SagGraphemeMotionCheckpoint checkpoint;

    if (builder == NULL || off < builder->next_motion)
        return;
    checkpoint.off = off;
    checkpoint.cluster_start = state->cluster_start;
    checkpoint.gcol = state->gcol;
    checkpoint.prev_gcb = state->gb.prev_gcb;
    checkpoint.flags = state->gb.flags;
    checkpoint.have_cluster = state->have_cluster;
    checkpoint.after_lf = state->after_lf;
    motion_index_push(builder->index, checkpoint);
    if (builder->next_motion >
        UINT64_MAX - (u64)SAG_MOTION_CHECKPOINT_STRIDE)
        builder->next_motion = UINT64_MAX;
    else
        builder->next_motion += (u64)SAG_MOTION_CHECKPOINT_STRIDE;
}

static void index_consume_cp(IndexBuilder *builder, IndexScanState *state,
                             const StreamCp *cp)
{
    bool boundary = sag_gb_boundary(&state->gb, cp->cp);

    if (boundary) {
        index_finalize_cluster(builder, state);
        if (!state->have_cluster || state->after_lf) {
            state->gcol = 0U;
        } else {
            if (state->gcol == UINT64_MAX)
                SAG_BUG("grapheme coordinate index column overflow");
            state->gcol++;
        }
        state->cluster_start = cp->start;
        state->have_cluster = true;
        state->after_lf = false;
    } else if (!state->have_cluster) {
        SAG_BUG("grapheme index first codepoint was not a boundary");
    }
    if (cp->cp == '\n')
        state->after_lf = true;
    index_motion_maybe(builder, state, cp->end);
}

static void index_scan_part(const TextBuf *tb, u64 start, u64 end,
                            IndexScanState *state, IndexBuilder *builder)
{
    TextReader reader;
    StreamCp cp;

    reader_init(&reader, tb, start, end);
    while (reader_cp(&reader, &cp))
        index_consume_cp(builder, state, &cp);
    if (reader.off != end)
        SAG_BUG("grapheme index scan ended early");
}

static void index_scan(const TextBuf *tb, u64 start, u64 end,
                       IndexScanState *state, IndexBuilder *builder)
{
    index_scan_part(tb, start, end, state, builder);
    index_finalize_cluster(builder, state);
}

static bool bytes_are_simple_ascii(const u8 *bytes, size_t len)
{
    size_t ones = SIZE_MAX / 0xffU;
    size_t highs = ones * 0x80U;
    size_t low_limit = ones * 0x20U;
    size_t del_bytes = ones * 0x7fU;
    size_t pos = 0U;

    while (len - pos >= sizeof(size_t)) {
        size_t word;
        size_t del;

        memcpy(&word, bytes + pos, sizeof(word));
        del = word ^ del_bytes;
        if ((word & highs) != 0U ||
            ((word - low_limit) & ~word & highs) != 0U ||
            ((del - ones) & ~del & highs) != 0U)
            return false;
        pos += sizeof(word);
    }
    while (pos < len) {
        if (bytes[pos] < 0x20U || bytes[pos] >= 0x7fU)
            return false;
        pos++;
    }
    return true;
}

static bool range_is_simple_ascii(const TextBuf *tb, u64 start, u64 end)
{
    TextIter iter;
    u64 remaining = end - start;

    if (remaining == 0U)
        return true;
    if (!sag_textiter_begin(&iter, tb, BYTEOFF(start)))
        SAG_BUG("simple ASCII scan cannot begin");
    do {
        const u8 *bytes;
        u64 chunk_len;
        size_t take;

        if (!sag_textiter_chunk(&iter, tb, &bytes, &chunk_len) ||
            chunk_len == 0U)
            SAG_BUG("simple ASCII scan found an empty chunk");
        if (chunk_len > remaining)
            chunk_len = remaining;
        if (chunk_len > SIZE_MAX)
            SAG_BUG("simple ASCII scan chunk is not addressable");
        take = (size_t)chunk_len;
        if (!bytes_are_simple_ascii(bytes, take))
            return false;
        remaining -= chunk_len;
        if (remaining == 0U)
            return true;
    } while (sag_textiter_advance(&iter, tb));
    SAG_BUG("simple ASCII scan ended early");
}

static void pending_clear(TextBuf *tb)
{
    SagGraphemePendingJournal *pending = &tb->graphemes.pending;
    u8 i;

    for (i = 0U; i < pending->len; i++) {
        if (pending->edits[i].after.active)
            sag_textsnap_release(tb, &pending->edits[i].after);
    }
    memset(pending, 0, sizeof(*pending));
}

static void coords_index_rebuild(TextBuf *tb)
{
    SagGraphemeIndex *index = &tb->graphemes;
    IndexScanState state;
    IndexBuilder builder;

    index->len = 0U;
    index->motion_len = 0U;
    index_scan_init(&state);
    builder.index = index;
    builder.next_motion = (u64)SAG_MOTION_CHECKPOINT_STRIDE;
    index_scan(tb, 0U, sag_textbuf_len(tb), &state, &builder);
    index->gen = tb->gen;
    index->simple_ascii = range_is_simple_ascii(
        tb, 0U, sag_textbuf_len(tb));
    index->simple_ascii_direct = false;
    index->initialized = true;
    pending_clear(tb);
}

void sag_coords_index_seed(TextBuf *tb)
{
    SagGraphemeIndex *index;

    if (tb == NULL)
        SAG_BUG("sag_coords_index_seed: NULL buffer");
    index = &tb->graphemes;
    if (sag_textbuf_len(tb) >= (u64)SAG_DEFER_INDEX_BYTES) {
        index->len = 0U;
        index->motion_len = 0U;
        index->gen = tb->gen;
        index->simple_ascii = range_is_simple_ascii(
            tb, 0U, sag_textbuf_len(tb));
        index->simple_ascii_direct = index->simple_ascii;
        index->initialized = index->simple_ascii;
        pending_clear(tb);
        return;
    }
    coords_index_rebuild(tb);
}

void sag_coords_index_dispose(TextBuf *tb)
{
    if (tb == NULL)
        return;
    pending_clear(tb);
    free(tb->graphemes.data);
    free(tb->graphemes.motion);
    memset(&tb->graphemes, 0, sizeof(tb->graphemes));
}

static size_t motion_lower_bound_off(const SagGraphemeIndex *index, u64 off)
{
    size_t lo = 0U;
    size_t hi = index->motion_len;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;

        if (index->motion[mid].off < off)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo;
}

static size_t motion_upper_bound_off(const SagGraphemeIndex *index, u64 off)
{
    size_t lo = 0U;
    size_t hi = index->motion_len;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;

        if (index->motion[mid].off <= off)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo;
}

static u64 shifted_offset(u64 off, u64 deleted_len, u64 inserted_len)
{
    if (deleted_len != 0U)
        return off - deleted_len;
    if (off > UINT64_MAX - inserted_len)
        SAG_BUG("grapheme index edit offset overflow");
    return off + inserted_len;
}

static u64 shifted_gcol(u64 gcol, u64 old_gcol, u64 new_gcol)
{
    if (new_gcol >= old_gcol) {
        u64 add = new_gcol - old_gcol;

        if (gcol > UINT64_MAX - add)
            SAG_BUG("grapheme index edit column overflow");
        return gcol + add;
    }
    if (gcol < old_gcol - new_gcol)
        SAG_BUG("grapheme index edit column underflow");
    return gcol - (old_gcol - new_gcol);
}

static bool scan_state_matches(const IndexScanState *state,
                               const SagGraphemeMotionCheckpoint *old)
{
    return state->gb.prev_gcb == old->prev_gcb &&
           state->gb.flags == old->flags &&
           state->have_cluster == old->have_cluster &&
           state->after_lf == old->after_lf;
}

static u64 shifted_cluster_start(u64 off, Span old_range,
                                 u64 inserted_len)
{
    u64 deleted_len = old_range.hi - old_range.lo;

    if (off <= old_range.lo)
        return off;
    if (off < old_range.hi)
        return old_range.lo + inserted_len;
    return shifted_offset(off, deleted_len, inserted_len);
}

static u64 next_motion_threshold(u64 off)
{
    u64 stride = (u64)SAG_MOTION_CHECKPOINT_STRIDE;
    u64 remainder = off % stride;
    u64 add = remainder == 0U ? stride : stride - remainder;

    return off > UINT64_MAX - add ? UINT64_MAX : off + add;
}

static void coords_index_restore_simple_ascii(SagGraphemeIndex *index,
                                              u64 len, u64 gen)
{
    SagGbState gb;
    u64 off;

    index->len = 0U;
    index->motion_len = 0U;
    for (off = (u64)SAG_GCOL_CHECKPOINT_STRIDE; off < len;) {
        index_push(index, off, off);
        if (off > UINT64_MAX - (u64)SAG_GCOL_CHECKPOINT_STRIDE)
            break;
        off += (u64)SAG_GCOL_CHECKPOINT_STRIDE;
    }
    sag_gb_init(&gb);
    (void)sag_gb_boundary(&gb, (u32)'x');
    for (off = (u64)SAG_MOTION_CHECKPOINT_STRIDE; off <= len;
         off += (u64)SAG_MOTION_CHECKPOINT_STRIDE) {
        SagGraphemeMotionCheckpoint checkpoint;

        checkpoint.off = off;
        checkpoint.cluster_start = off - 1U;
        checkpoint.gcol = off - 1U;
        checkpoint.prev_gcb = gb.prev_gcb;
        checkpoint.flags = gb.flags;
        checkpoint.have_cluster = true;
        checkpoint.after_lf = false;
        motion_index_push(index, checkpoint);
        if (off > UINT64_MAX - (u64)SAG_MOTION_CHECKPOINT_STRIDE)
            break;
    }
    index->gen = gen;
    index->simple_ascii = true;
    index->simple_ascii_direct = false;
    index->initialized = true;
}

static void coords_index_apply_edit(SagGraphemeIndex *old_index,
                                    TextBuf *source,
                                    const SagGraphemePendingEdit *edit)
{
    SagGraphemeIndex next = {0};
    SagGraphemeMotionCheckpoint resume;
    IndexScanState state;
    IndexBuilder builder;
    Span old_range;
    Span old_affected;
    u64 deleted_len;
    u64 inserted_len;
    u64 old_gen;
    u64 scan_start;
    u64 scan_cursor;
    u64 rebuild_lo;
    u64 scan_end;
    u64 convergence_off = 0U;
    u64 convergence_old_gcol = 0U;
    u64 convergence_new_gcol = 0U;
    u64 convergence_old_cluster_start = 0U;
    u64 convergence_new_cluster_start = 0U;
    size_t motion_first;
    size_t motion_after;
    size_t candidate;
    size_t i;
    bool have_resume = false;
    bool converged = false;

    old_range = edit->range;
    inserted_len = edit->inserted_len;
    old_affected = edit->affected;
    old_gen = edit->old_gen;
    if (old_index->gen != old_gen || edit->new_gen != source->gen)
        SAG_BUG("grapheme index pending edit is inconsistent");
    if (old_range.lo > old_range.hi ||
        old_affected.lo > old_range.lo ||
        old_affected.hi < old_range.hi)
        SAG_BUG("grapheme index pending edit has invalid range");
    deleted_len = old_range.hi - old_range.lo;
    motion_first = motion_lower_bound_off(old_index, old_affected.lo);
    motion_after = motion_upper_bound_off(old_index, old_range.lo);
    if (motion_after > motion_first) {
        resume = old_index->motion[motion_after - 1U];
        have_resume = true;
    }
    if (have_resume) {
        state.gb.prev_gcb = resume.prev_gcb;
        state.gb.flags = resume.flags;
        state.cluster_start = resume.cluster_start;
        state.gcol = resume.gcol;
        state.have_cluster = resume.have_cluster;
        state.after_lf = resume.after_lf;
        scan_start = resume.off;
        rebuild_lo = resume.cluster_start > old_affected.lo
                         ? resume.cluster_start
                         : old_affected.lo;
    } else {
        index_scan_init(&state);
        scan_start = old_affected.lo;
        rebuild_lo = old_affected.lo;
    }
    if (old_affected.hi < deleted_len)
        SAG_BUG("grapheme index edit span underflow");
    scan_end = old_affected.hi - deleted_len;
    if (scan_end > UINT64_MAX - inserted_len)
        SAG_BUG("grapheme index edit span overflow");
    scan_end += inserted_len;

    for (i = 0U; i < old_index->len; i++) {
        if (old_index->data[i].off >= rebuild_lo)
            break;
        index_push(&next, old_index->data[i].off,
                   old_index->data[i].gcol);
    }
    for (i = 0U; i < old_index->motion_len; i++) {
        if (old_index->motion[i].off > scan_start)
            break;
        motion_index_push(&next, old_index->motion[i]);
    }
    builder.index = &next;
    builder.next_motion = next_motion_threshold(scan_start);
    scan_cursor = scan_start;
    candidate = motion_upper_bound_off(old_index, old_range.hi);
    for (; candidate < old_index->motion_len; candidate++) {
        const SagGraphemeMotionCheckpoint *checkpoint =
            &old_index->motion[candidate];
        u64 candidate_end;

        if (checkpoint->off > old_affected.hi)
            break;
        candidate_end = shifted_offset(checkpoint->off, deleted_len,
                                       inserted_len);
        if (candidate_end < scan_cursor || candidate_end > scan_end)
            continue;
        index_scan_part(source, scan_cursor, candidate_end, &state,
                        &builder);
        scan_cursor = candidate_end;
        if (!scan_state_matches(&state, checkpoint))
            continue;
        convergence_off = checkpoint->off;
        convergence_old_gcol = checkpoint->gcol;
        convergence_new_gcol = state.gcol;
        convergence_old_cluster_start = checkpoint->cluster_start;
        convergence_new_cluster_start = state.cluster_start;
        converged = true;
        break;
    }
    if (!converged) {
        index_scan_part(source, scan_cursor, scan_end, &state, &builder);
        index_finalize_cluster(&builder, &state);
    }

    for (i = 0U; i < old_index->len; i++) {
        SagGraphemeCheckpoint checkpoint = old_index->data[i];

        if (converged && checkpoint.off >= convergence_off &&
            checkpoint.off < old_affected.hi) {
            checkpoint.off = shifted_offset(checkpoint.off, deleted_len,
                                            inserted_len);
            checkpoint.gcol = shifted_gcol(checkpoint.gcol,
                                           convergence_old_gcol,
                                           convergence_new_gcol);
            index_push(&next, checkpoint.off, checkpoint.gcol);
            continue;
        }
        if (checkpoint.off < old_affected.hi)
            continue;
        checkpoint.off = shifted_offset(checkpoint.off, deleted_len,
                                        inserted_len);
        index_push(&next, checkpoint.off, checkpoint.gcol);
    }
    for (i = 0U; i < old_index->motion_len; i++) {
        SagGraphemeMotionCheckpoint checkpoint = old_index->motion[i];

        if (converged && checkpoint.off >= convergence_off &&
            checkpoint.off <= old_affected.hi) {
            checkpoint.off = shifted_offset(checkpoint.off, deleted_len,
                                            inserted_len);
            if (checkpoint.cluster_start ==
                convergence_old_cluster_start)
                checkpoint.cluster_start =
                    convergence_new_cluster_start;
            else
                checkpoint.cluster_start = shifted_cluster_start(
                    checkpoint.cluster_start, old_range, inserted_len);
            checkpoint.gcol = shifted_gcol(checkpoint.gcol,
                                           convergence_old_gcol,
                                           convergence_new_gcol);
            motion_index_push(&next, checkpoint);
            continue;
        }
        if (checkpoint.off <= old_affected.hi)
            continue;
        checkpoint.off = shifted_offset(checkpoint.off, deleted_len,
                                        inserted_len);
        checkpoint.cluster_start = shifted_cluster_start(
            checkpoint.cluster_start, old_range, inserted_len);
        motion_index_push(&next, checkpoint);
    }
    next.gen = edit->new_gen;
    free(old_index->data);
    free(old_index->motion);
    old_index->data = next.data;
    old_index->len = next.len;
    old_index->cap = next.cap;
    old_index->motion = next.motion;
    old_index->motion_len = next.motion_len;
    old_index->motion_cap = next.motion_cap;
    old_index->gen = next.gen;
    old_index->initialized = true;
}

static void coords_index_apply_pending(TextBuf *tb, u64 through_gen)
{
    SagGraphemeIndex *index = &tb->graphemes;
    SagGraphemePendingJournal *pending = &index->pending;
    u8 i;

    if (pending->len == 0U ||
        pending->edits[0].old_gen != index->gen ||
        pending->edits[pending->len - 1U].new_gen != through_gen)
        SAG_BUG("grapheme index pending journal is inconsistent");
    for (i = 0U; i < pending->len; i++) {
        SagGraphemePendingEdit *edit = &pending->edits[i];
        TextBuf source = {0};

        source.backing = edit->after.backing;
        source.orig = edit->after.backing->orig;
        source.add = edit->after.backing->add;
        source.root = edit->after.root;
        source.gen = edit->after.gen;
        coords_index_apply_edit(index, &source, edit);
        sag_textsnap_release(tb, &edit->after);
    }
    memset(pending, 0, sizeof(*pending));
}

void sag_coords_index_note_edit(TextBuf *tb, Span old_range,
                                u64 inserted_len, Span old_affected,
                                u64 old_gen)
{
    SagGraphemeIndex *index;
    SagGraphemePendingJournal *pending;
    SagGraphemePendingEdit *edit;
    u64 expected_gen;

    if (tb == NULL)
        SAG_BUG("sag_coords_index_note_edit: NULL buffer");
    index = &tb->graphemes;
    pending = &index->pending;
    if (old_range.lo > old_range.hi ||
        old_affected.lo > old_range.lo ||
        old_affected.hi < old_range.hi)
        SAG_BUG("sag_coords_index_note_edit: invalid old range");
    if (!index->initialized) {
        if (pending->len != 0U)
            SAG_BUG("deferred grapheme index has pending edits");
        index->gen = tb->gen;
        return;
    }
    if (index->simple_ascii) {
        u64 old_len = sag_textbuf_len(tb) - inserted_len +
                      (old_range.hi - old_range.lo);
        u64 inserted_hi = old_range.lo + inserted_len;
        bool inserted_simple;

        if (inserted_hi < old_range.lo ||
            inserted_hi > sag_textbuf_len(tb))
            SAG_BUG("simple ASCII edit range overflow");
        inserted_simple = range_is_simple_ascii(tb, old_range.lo,
                                                inserted_hi);
        if (index->simple_ascii_direct && pending->len != 0U)
            SAG_BUG("direct simple ASCII index has pending edits");
        if (inserted_simple && pending->len == 0U &&
            (index->simple_ascii_direct ||
             old_len >= (u64)SAG_SIMPLE_ASCII_BYPASS_BYTES)) {
            index->gen = tb->gen;
            index->simple_ascii_direct = true;
            return;
        }
        if (!inserted_simple) {
            if (pending->len == 0U && index->gen == old_gen &&
                index->simple_ascii_direct)
                coords_index_restore_simple_ascii(index, old_len,
                                                  old_gen);
            index->simple_ascii = false;
            index->simple_ascii_direct = false;
        }
    }
    expected_gen = pending->len == 0U
                       ? index->gen
                       : pending->edits[pending->len - 1U].new_gen;
    if (expected_gen != old_gen || tb->gen != old_gen + 1U)
        SAG_BUG("sag_coords_index_note_edit: generation mismatch");
    if (pending->len == SAG_GRAPHEME_PENDING_MAX) {
        coords_index_apply_pending(tb, old_gen);
        if (index->gen != old_gen)
            SAG_BUG("sag_coords_index_note_edit: replay ended at wrong generation");
    }
    edit = &pending->edits[pending->len++];
    edit->range = old_range;
    edit->affected = old_affected;
    edit->inserted_len = inserted_len;
    edit->old_gen = old_gen;
    edit->new_gen = tb->gen;
    edit->after = sag_textbuf_snap(tb);
}

static const SagGraphemeIndex *coords_index(const TextBuf *tb)
{
    if (!tb->graphemes.initialized) {
        TextBuf *mutable = (TextBuf *)tb;

        if (tb->graphemes.pending.len != 0U)
            SAG_BUG("deferred grapheme index has pending edits");
        coords_index_rebuild(mutable);
    } else if (tb->graphemes.gen != tb->gen) {
        TextBuf *mutable = (TextBuf *)tb;

        if (tb->graphemes.pending.len != 0U)
            coords_index_apply_pending(mutable, tb->gen);
        else
            SAG_BUG("grapheme index generation changed without an edit");
    }
    return &tb->graphemes;
}

static size_t index_lower_bound_off(const SagGraphemeIndex *index, u64 off)
{
    size_t lo = 0U;
    size_t hi = index->len;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;

        if (index->data[mid].off < off)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return lo;
}

static SagGraphemeCheckpoint index_before_off(const SagGraphemeIndex *index,
                                              Span line, u64 end, u64 off)
{
    size_t lo = index_lower_bound_off(index, line.lo);
    size_t hi = index_lower_bound_off(index, end);
    size_t first = lo;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;

        if (index->data[mid].off <= off)
            lo = mid + 1U;
        else
            hi = mid;
    }
    if (lo == first)
        return (SagGraphemeCheckpoint){line.lo, 0U};
    return index->data[lo - 1U];
}

static SagGraphemeCheckpoint index_before_gcol(
    const SagGraphemeIndex *index, Span line, u64 end, u64 gcol)
{
    size_t first = index_lower_bound_off(index, line.lo);
    size_t lo = first;
    size_t hi = index_lower_bound_off(index, end);

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;

        if (index->data[mid].gcol <= gcol)
            lo = mid + 1U;
        else
            hi = mid;
    }
    if (lo == first)
        return (SagGraphemeCheckpoint){line.lo, 0U};
    return index->data[lo - 1U];
}

static u64 add_cells(u64 cells, u64 width)
{
    return width > UINT64_MAX - cells ? UINT64_MAX : cells + width;
}

static u64 cluster_cells(const StreamCluster *cluster, u64 cells, u32 tabw)
{
    if (!cluster->tab)
        return cluster->cells;
    if (tabw == 0U)
        tabw = 1U;
    return (u64)tabw - cells % (u64)tabw;
}

u32 sag_tab_cells(CCol at, u32 tabw)
{
    if (tabw == 0U)
        tabw = 1U;
    return tabw - (u32)(at.v % (u64)tabw);
}

static bool coords_simple_ascii(const TextBuf *tb)
{
    return tb->graphemes.initialized && tb->graphemes.simple_ascii &&
           tb->graphemes.gen == tb->gen;
}

static ByteOff simple_col_to_off(Span line, u64 col)
{
    u64 len = line.hi - line.lo;

    if (col <= len)
        return BYTEOFF(line.lo + col);
    if (len == 0U)
        return BYTEOFF(line.hi);
    return BYTEOFF(line.hi - 1U);
}

GCol sag_off_to_gcol(const TextBuf *tb, Span line, ByteOff pos)
{
    const SagGraphemeIndex *index;
    SagGraphemeCheckpoint checkpoint;
    IndexScanState state;
    TextReader stream;
    StreamCp cp;
    ClusterReader reader;
    StreamCluster cluster;
    u64 end;
    u64 count;
    size_t first;
    size_t after;

    require_span(tb, line);
    require_pos(line, pos);
    if (coords_simple_ascii(tb))
        return (GCol){pos.v - line.lo};
    end = line_content_end(tb, line);
    if (pos.v > end)
        pos.v = end;
    if (pos.v == line.lo)
        return (GCol){0U};
    index = coords_index(tb);
    first = motion_lower_bound_off(index, line.lo);
    after = motion_upper_bound_off(index, pos.v);
    if (after > first) {
        const SagGraphemeMotionCheckpoint *motion =
            &index->motion[after - 1U];
        bool boundary = pos.v == end;

        state.gb.prev_gcb = motion->prev_gcb;
        state.gb.flags = motion->flags;
        state.cluster_start = motion->cluster_start;
        state.gcol = motion->gcol;
        state.have_cluster = motion->have_cluster;
        state.after_lf = motion->after_lf;
        index_scan_part(tb, motion->off, pos.v, &state, NULL);
        if (!boundary) {
            u8 next;

            reader_init(&stream, tb, pos.v, end);
            if (!reader_get(&stream, &next))
                SAG_BUG("unicode coordinates: missing position byte");
            if ((next & 0xC0U) != 0x80U) {
                SagGbState probe = state.gb;

                reader_init(&stream, tb, pos.v, end);
                if (!reader_cp(&stream, &cp))
                    SAG_BUG("unicode coordinates: missing next codepoint");
                boundary = sag_gb_boundary(&probe, cp.cp);
            }
        }
        if (boundary && state.have_cluster && !state.after_lf &&
            state.gcol != UINT64_MAX)
            state.gcol++;
        if (boundary || pos.v == state.cluster_start)
            return (GCol){state.gcol};
    }
    checkpoint = index_before_off(index, line, end, pos.v);
    count = checkpoint.gcol;
    cluster_reader_init(&reader, tb, checkpoint.off, end);
    while (cluster_next(&reader, &cluster, false)) {
        if (pos.v < cluster.end)
            break;
        if (count != UINT64_MAX)
            count++;
    }
    cluster_reader_free(&reader);
    return (GCol){count};
}

ByteOff sag_gcol_to_off(const TextBuf *tb, Span line, GCol g)
{
    const SagGraphemeIndex *index;
    SagGraphemeCheckpoint checkpoint;
    ClusterReader reader;
    StreamCluster cluster;
    u64 end;
    u64 count;
    u64 last = line.lo;
    bool have_cluster = false;

    require_span(tb, line);
    if (coords_simple_ascii(tb))
        return simple_col_to_off(line, g.v);
    end = line_content_end(tb, line);
    index = coords_index(tb);
    checkpoint = index_before_gcol(index, line, end, g.v);
    count = checkpoint.gcol;
    cluster_reader_init(&reader, tb, checkpoint.off, end);
    while (cluster_next(&reader, &cluster, false)) {
        if (count == g.v) {
            cluster_reader_free(&reader);
            return BYTEOFF(cluster.start);
        }
        last = cluster.start;
        have_cluster = true;
        count++;
    }
    cluster_reader_free(&reader);
    if (g.v == count || end < line.hi || !have_cluster)
        return BYTEOFF(end);
    return BYTEOFF(last);
}

CharCol sag_off_to_charcol(const TextBuf *tb, Span line, ByteOff pos)
{
    TextReader reader;
    StreamCp cp;
    u64 end;
    u64 count = 0U;

    require_span(tb, line);
    require_pos(line, pos);
    if (coords_simple_ascii(tb))
        return (CharCol){pos.v - line.lo};
    end = line_content_end(tb, line);
    if (pos.v > end)
        pos.v = end;
    reader_init(&reader, tb, line.lo, end);
    while (reader_cp(&reader, &cp)) {
        if (pos.v < cp.end)
            break;
        if (count != UINT64_MAX)
            count++;
    }
    return (CharCol){count};
}

CCol sag_off_to_ccol(const TextBuf *tb, Span line, ByteOff pos, u32 tabw)
{
    ClusterReader reader;
    StreamCluster cluster;
    u64 end;
    u64 cells = 0U;

    require_span(tb, line);
    require_pos(line, pos);
    if (coords_simple_ascii(tb))
        return (CCol){pos.v - line.lo};
    end = line_content_end(tb, line);
    if (pos.v > end)
        pos.v = end;
    cluster_reader_init(&reader, tb, line.lo, end);
    while (cluster_next(&reader, &cluster, true)) {
        if (pos.v < cluster.end)
            break;
        cells = add_cells(cells, cluster_cells(&cluster, cells, tabw));
    }
    cluster_reader_free(&reader);
    return (CCol){cells};
}

ByteOff sag_ccol_to_off(const TextBuf *tb, Span line, CCol c, u32 tabw)
{
    ClusterReader reader;
    StreamCluster cluster;
    u64 end;
    u64 cells = 0U;
    u64 last = line.lo;
    bool have_cluster = false;

    require_span(tb, line);
    if (coords_simple_ascii(tb))
        return simple_col_to_off(line, c.v);
    end = line_content_end(tb, line);
    cluster_reader_init(&reader, tb, line.lo, end);
    while (cluster_next(&reader, &cluster, true)) {
        u64 next;

        if (c.v <= cells) {
            cluster_reader_free(&reader);
            return BYTEOFF(cluster.start);
        }
        next = add_cells(cells, cluster_cells(&cluster, cells, tabw));
        if (c.v < next) {
            cluster_reader_free(&reader);
            return BYTEOFF(cluster.start);
        }
        last = cluster.start;
        have_cluster = true;
        cells = next;
    }
    cluster_reader_free(&reader);
    if (c.v == cells || end < line.hi || !have_cluster)
        return BYTEOFF(end);
    return BYTEOFF(last);
}

ByteOff sag_ccol_to_off_padded(const TextBuf *tb, Span line, CCol c,
                               u32 tabw)
{
    u64 end;
    CCol end_col;

    require_span(tb, line);
    end = line_content_end(tb, line);
    end_col = sag_off_to_ccol(tb, line, BYTEOFF(end), tabw);
    if (c.v >= end_col.v)
        return BYTEOFF(end);
    return sag_ccol_to_off(tb, line, c, tabw);
}

u64 sag_ccol_shortfall(CCol target, CCol landed)
{
    if (landed.v > target.v)
        SAG_BUG("cell-column landing exceeds target");
    return target.v - landed.v;
}

CCol sag_ccol_max(CCol left, CCol right)
{
    return left.v >= right.v ? left : right;
}

static Span motion_span(const TextBuf *tb, ByteOff pos, bool previous)
{
    LineNo line = sag_textbuf_line_of(tb, pos);
    Span span = sag_textbuf_line_span(tb, line);

    if (previous && pos.v == span.lo && line.v != 0U)
        span = sag_textbuf_line_span(tb, LINENO(line.v - 1U));
    return span;
}

ByteOff sag_grapheme_next(const TextBuf *tb, ByteOff pos)
{
    ClusterReader reader;
    StreamCluster cluster;
    Span span;
    u64 len;

    if (tb == NULL)
        SAG_BUG("sag_grapheme_next: NULL text buffer");
    len = sag_textbuf_len(tb);
    if (pos.v >= len)
        return BYTEOFF(len);
    if (coords_simple_ascii(tb))
        return BYTEOFF(pos.v + 1U);
    span = motion_span(tb, pos, false);
    cluster_reader_init(&reader, tb, span.lo, span.hi);
    while (cluster_next(&reader, &cluster, false)) {
        if (pos.v < cluster.end) {
            cluster_reader_free(&reader);
            return BYTEOFF(cluster.end);
        }
    }
    cluster_reader_free(&reader);
    return BYTEOFF(span.hi);
}

ByteOff sag_grapheme_next_boundary(const TextBuf *tb, ByteOff pos)
{
    ClusterReader reader;
    StreamCluster cluster;
    Span span;
    u64 len;

    if (tb == NULL)
        SAG_BUG("sag_grapheme_next_boundary: NULL text buffer");
    len = sag_textbuf_len(tb);
    if (pos.v >= len)
        return BYTEOFF(len);
    if (coords_simple_ascii(tb))
        return BYTEOFF(pos.v + 1U);
    span = motion_span(tb, pos, false);
    cluster_reader_init(&reader, tb, pos.v, span.hi);
    if (!cluster_next(&reader, &cluster, false))
        SAG_BUG("sag_grapheme_next_boundary: no cluster at offset");
    cluster_reader_free(&reader);
    return BYTEOFF(cluster.end);
}

ByteOff sag_grapheme_prev(const TextBuf *tb, ByteOff pos)
{
    ClusterReader reader;
    StreamCluster cluster;
    Span span;
    u64 previous;
    u64 len;

    if (tb == NULL)
        SAG_BUG("sag_grapheme_prev: NULL text buffer");
    len = sag_textbuf_len(tb);
    if (pos.v > len)
        pos.v = len;
    if (pos.v == 0U)
        return BYTEOFF(0U);
    if (coords_simple_ascii(tb))
        return BYTEOFF(pos.v - 1U);
    span = motion_span(tb, pos, true);
    previous = span.lo;
    cluster_reader_init(&reader, tb, span.lo, span.hi);
    while (cluster_next(&reader, &cluster, false)) {
        if (pos.v <= cluster.start) {
            cluster_reader_free(&reader);
            return BYTEOFF(previous);
        }
        if (pos.v < cluster.end) {
            cluster_reader_free(&reader);
            return BYTEOFF(cluster.start);
        }
        previous = cluster.start;
        if (pos.v == cluster.end)
            continue;
    }
    cluster_reader_free(&reader);
    return BYTEOFF(previous);
}

ByteOff sag_grapheme_prev_boundary(const TextBuf *tb, ByteOff pos)
{
    const SagGraphemeIndex *index;
    IndexScanState state;
    TextReader reader;
    StreamCp cp;
    Span span;
    u8 previous;
    u8 before_previous;
    u64 scan_start;
    u64 len;
    size_t first;
    size_t after;

    if (tb == NULL)
        SAG_BUG("sag_grapheme_prev_boundary: NULL text buffer");
    len = sag_textbuf_len(tb);
    if (pos.v > len)
        pos.v = len;
    if (pos.v == 0U)
        return BYTEOFF(0U);
    if (coords_simple_ascii(tb))
        return BYTEOFF(pos.v - 1U);
    span = motion_span(tb, pos, true);
    reader_init(&reader, tb, pos.v - 1U, pos.v);
    if (!reader_get(&reader, &previous))
        SAG_BUG("sag_grapheme_prev_boundary: cannot inspect byte");
    if (previous < 0x80U) {
        if (pos.v - span.lo == 1U)
            return BYTEOFF(pos.v - 1U);
        reader_init(&reader, tb, pos.v - 2U, pos.v - 1U);
        if (!reader_get(&reader, &before_previous))
            SAG_BUG("sag_grapheme_prev_boundary: cannot inspect prefix");
        if (before_previous < 0x80U) {
            if (previous == '\n' && before_previous == '\r')
                return BYTEOFF(pos.v - 2U);
            return BYTEOFF(pos.v - 1U);
        }
    }
    index = coords_index(tb);
    first = motion_lower_bound_off(index, span.lo);
    after = motion_upper_bound_off(index, pos.v);
    if (after > first) {
        const SagGraphemeMotionCheckpoint *checkpoint =
            &index->motion[after - 1U];

        state.gb.prev_gcb = checkpoint->prev_gcb;
        state.gb.flags = checkpoint->flags;
        state.cluster_start = checkpoint->cluster_start;
        state.gcol = checkpoint->gcol;
        state.have_cluster = checkpoint->have_cluster;
        state.after_lf = checkpoint->after_lf;
        scan_start = checkpoint->off;
    } else {
        index_scan_init(&state);
        scan_start = span.lo;
    }
    reader_init(&reader, tb, scan_start, pos.v);
    while (reader_cp(&reader, &cp))
        index_consume_cp(NULL, &state, &cp);
    if (reader.off != pos.v)
        SAG_BUG("sag_grapheme_prev_boundary: checkpoint ended early");
    if (!state.have_cluster)
        SAG_BUG("sag_grapheme_prev_boundary: checkpoint has no cluster");
    return BYTEOFF(state.cluster_start);
}

bool sag_is_grapheme_boundary(const TextBuf *tb, ByteOff pos)
{
    ClusterReader reader;
    StreamCluster cluster;
    Span span;
    u64 len;

    if (tb == NULL)
        SAG_BUG("sag_is_grapheme_boundary: NULL text buffer");
    len = sag_textbuf_len(tb);
    if (pos.v > len)
        return false;
    if (coords_simple_ascii(tb))
        return true;
    if (pos.v == 0U || pos.v == len)
        return true;
    span = motion_span(tb, pos, false);
    if (pos.v == span.lo)
        return true;
    cluster_reader_init(&reader, tb, span.lo, span.hi);
    while (cluster_next(&reader, &cluster, false)) {
        if (pos.v == cluster.end) {
            cluster_reader_free(&reader);
            return true;
        }
        if (pos.v < cluster.end)
            break;
    }
    cluster_reader_free(&reader);
    return false;
}
