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
    bool tab;
    bool lf;
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
    out->tab = false;
    out->lf = false;
    sag_gb_init(&gb);
    sag_cluster_width_init(&width);
    if (!reader_cp(&reader->reader, &cp))
        SAG_BUG("unicode coordinates: missing cluster head");
    (void)sag_gb_boundary(&gb, cp.cp);
    first_cp = cp.cp;
    cp_count = 1U;
    out->lf = cp.cp == '\n';
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
        if (cp.cp == '\n')
            out->lf = true;
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

enum { SAG_GCOL_CHECKPOINT_STRIDE = 64 };

static void index_push(SagGraphemeIndex *index, u64 off, u64 gcol)
{
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

static void coords_index_rebuild(TextBuf *tb)
{
    SagGraphemeIndex *index = &tb->graphemes;
    ClusterReader reader;
    StreamCluster cluster;
    u64 gcol = 0U;

    index->len = 0U;
    cluster_reader_init(&reader, tb, 0U, sag_textbuf_len(tb));
    while (cluster_next(&reader, &cluster, false)) {
        if (cluster.lf) {
            gcol = 0U;
            continue;
        }
        if (gcol != 0U &&
            gcol % (u64)SAG_GCOL_CHECKPOINT_STRIDE == 0U)
            index_push(index, cluster.start, gcol);
        if (gcol == UINT64_MAX)
            SAG_BUG("grapheme coordinate index column overflow");
        gcol++;
    }
    cluster_reader_free(&reader);
    index->gen = tb->gen;
}

void sag_coords_index_seed(TextBuf *tb)
{
    if (tb == NULL)
        SAG_BUG("sag_coords_index_seed: NULL buffer");
    coords_index_rebuild(tb);
}

void sag_coords_index_dispose(TextBuf *tb)
{
    if (tb == NULL)
        return;
    free(tb->graphemes.data);
    memset(&tb->graphemes, 0, sizeof(tb->graphemes));
}

static const SagGraphemeIndex *coords_index(const TextBuf *tb)
{
    if (tb->graphemes.gen != tb->gen)
        coords_index_rebuild((TextBuf *)tb);
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

GCol sag_off_to_gcol(const TextBuf *tb, Span line, ByteOff pos)
{
    const SagGraphemeIndex *index;
    SagGraphemeCheckpoint checkpoint;
    ClusterReader reader;
    StreamCluster cluster;
    u64 end;
    u64 count;

    require_span(tb, line);
    require_pos(line, pos);
    end = line_content_end(tb, line);
    if (pos.v > end)
        pos.v = end;
    index = coords_index(tb);
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

static bool ri_restart_unproven(const u8 *window, size_t count,
                                bool truncated)
{
    enum { RESTART_CP_LIMIT = 64 };
    size_t cursor = count;
    size_t scanned;

    for (scanned = 0U; scanned < RESTART_CP_LIMIT; scanned++) {
        u32 cp;
        size_t used = sag_utf8_decode_prev(window, 0U, cursor, &cp);

        if (used == 0U || used > cursor ||
            cp < UINT32_C(0x1F1E6) || cp > UINT32_C(0x1F1FF))
            return false;
        cursor -= used;
    }
    return cursor != 0U || truncated;
}

ByteOff sag_grapheme_prev_boundary(const TextBuf *tb, ByteOff pos)
{
    enum { PREV_WINDOW = 512 };
    TextReader reader;
    Span span;
    u8 window[PREV_WINDOW];
    u8 previous;
    u8 before_previous;
    u64 start;
    size_t count = 0U;
    size_t found;
    u64 len;

    if (tb == NULL)
        SAG_BUG("sag_grapheme_prev_boundary: NULL text buffer");
    len = sag_textbuf_len(tb);
    if (pos.v > len)
        pos.v = len;
    if (pos.v == 0U)
        return BYTEOFF(0U);
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
    start = pos.v - span.lo > PREV_WINDOW
                ? pos.v - PREV_WINDOW
                : span.lo;
    reader_init(&reader, tb, start, pos.v);
    while (count < SAG_ARRAY_LEN(window) &&
           reader_get(&reader, &window[count]))
        count++;
    if (reader.off != pos.v)
        SAG_BUG("sag_grapheme_prev_boundary: window ended early");
    found = sag_gb_prev_bytes(window, count, count);
    if (!ri_restart_unproven(window, count, start != span.lo) &&
        (found != 0U || start == span.lo))
        return BYTEOFF(start + (u64)found);
    return sag_grapheme_prev(tb, pos);
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
