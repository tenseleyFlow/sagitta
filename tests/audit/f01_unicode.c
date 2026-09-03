/* Sprint 58 F01 Q1: byte-exact consumer differential.
 *
 * Each of the 2^24 independent three-byte inputs first survives the core
 * decode/re-encode law. Records are newline-delimited before being passed
 * through the register, Fletch-string, JSON raw-byte, and UTF-16 coordinate
 * consumers, preventing a sequence from borrowing bytes from its neighbour.
 * The same consumers also receive every row of the Sprint 2 golden corpus.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl/diag.h"
#include "fl/gc.h"
#include "fl/vm.h"
#include "mod/lsp/json.h"
#include "text/register.h"
#include "unicode/u16.h"
#include "unicode/utf8.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/intern.h"

enum {
    TRIPLES_PER_SLAB = 65536U,
    RECORD_BYTES = 4U,
    SLAB_BYTES = TRIPLES_PER_SLAB * RECORD_BYTES
};

static bool fail(const char *consumer, const char *label, const char *why)
{
    (void)fprintf(stderr, "f01-unicode: %s: %s: %s\n",
                  consumer, label, why);
    return false;
}

static bool core_roundtrip(const u8 input[3])
{
    u8 encoded[3];
    size_t in_at = 0U;
    size_t out_at = 0U;

    while (in_at < 3U) {
        u8 bytes[YEW_UTF8_MAX];
        u32 cp;
        size_t consumed = yew_utf8_decode(input + in_at, 3U - in_at, &cp);
        size_t produced = yew_utf8_encode(cp, bytes);

        if (consumed == 0U || consumed > 3U - in_at ||
            produced != consumed || out_at + produced > sizeof(encoded))
            return false;
        (void)memcpy(encoded + out_at, bytes, produced);
        in_at += consumed;
        out_at += produced;
    }
    return out_at == 3U && memcmp(encoded, input, 3U) == 0;
}

static bool register_roundtrip(const TextBuf *tb, const u8 *bytes,
                               size_t len, const char *label)
{
    Registers regs;
    RegVal value;
    const RegVal *stored;
    bool ok;

    yew_reg_init(&regs);
    regs.clipboard_sync = YEW_CLIP_SYNC_OFF;
    yew_regval_init(&value);
    yew_regval_from_span(&value, tb, (Span){0U, (u64)len},
                         YEW_REG_CHARWISE, NULL);
    yew_reg_set(&regs, (u8)'a', &value);
    stored = yew_reg_get(&regs, (u8)'a');
    ok = stored != NULL && stored->bytes.len == len &&
         (len == 0U || memcmp(stored->bytes.data, bytes, len) == 0);
    yew_regval_free(&value);
    yew_reg_free(&regs);
    return ok || fail("register", label, "captured bytes changed");
}

static bool fletch_roundtrip(const u8 *bytes, size_t len, const char *label)
{
    Arena arena;
    Interner interner;
    DiagCtx diag;
    FlVm vm;
    FlStr *string;
    bool ok;

    if (len > UINT32_MAX)
        return fail("Fletch", label, "input exceeds FlStr length domain");
    arena_init(&arena);
    interner_init(&interner, &arena);
    fl_diag_init(&diag, &arena);
    (void)fl_vm_init(&vm, &arena, &interner, &diag);
    string = fl_str_new(&vm, (const char *)bytes, (u32)len);
    ok = string->len == (u32)len &&
         (len == 0U || memcmp(string->b, bytes, len) == 0);
    fl_vm_free(&vm);
    interner_free(&interner);
    arena_free_all(&arena);
    return ok || fail("Fletch", label, "FlStr bytes changed");
}

static bool json_roundtrip(const u8 *bytes, size_t len, const char *label)
{
    Bytebuf encoded;
    JsonW writer;
    Arena arena;
    JsonErr err = {0};
    JsonValue *value;
    bool has_raw;
    bool ok;

    if (len > UINT32_MAX)
        return fail("JSON", label, "input exceeds JSON string domain");
    bytebuf_init(&encoded);
    yew_jsonw_init(&writer, &encoded);
    yew_jsonw_str(&writer, bytes, (u32)len);
    arena_init(&arena);
    value = yew_json_parse(&arena, encoded.data, encoded.len, &err);
    has_raw = yew_utf8_validate(bytes, len) != len;
    ok = value != NULL && value->kind == YEW_JS_STR &&
         value->s.len == (u32)len &&
         (len == 0U || memcmp(value->s.p, bytes, len) == 0) &&
         (((value->flags & YEW_JSF_RAW_BYTE) != 0U) == has_raw);
    arena_free_all(&arena);
    bytebuf_free(&encoded);
    return ok || fail("JSON", label, "raw-byte string changed or lost flag");
}

static bool u16_boundary(const TextBuf *tb, Span line, u64 off, u64 units,
                         const char *label)
{
    U16Col got = yew_off_to_u16col(tb, line, BYTEOFF(off));
    ByteOff back = yew_u16col_to_off(tb, line, U16COL(units));

    if (got.v != units)
        return fail("u16", label, "byte offset mapped to wrong column");
    if (back.v != off)
        return fail("u16", label, "column did not map back to byte offset");
    return true;
}

static bool u16_span(const TextBuf *tb, const u8 *bytes, Span line,
                     const char *label)
{
    u64 end = line.hi;
    u64 at = line.lo;
    u64 units = 0U;

    if (end > line.lo && bytes[end - 1U] == (u8)'\n') {
        end--;
        if (end > line.lo && bytes[end - 1U] == (u8)'\r')
            end--;
    }
    if (!u16_boundary(tb, line, at, units, label))
        return false;
    while (at < end) {
        u32 cp;
        size_t consumed = yew_utf8_decode(bytes + (size_t)at,
                                          (size_t)(end - at), &cp);

        if (consumed == 0U || (u64)consumed > end - at)
            return fail("u16", label, "reference decoder made no progress");
        at += (u64)consumed;
        units += cp > 0xFFFFU ? 2U : 1U;
        if (!u16_boundary(tb, line, at, units, label))
            return false;
    }
    return true;
}

static bool u16_lines(const TextBuf *tb, const u8 *bytes, const char *label)
{
    u64 line;
    u64 count = yew_textbuf_line_count(tb);

    for (line = 0U; line < count; line++) {
        Span span = yew_textbuf_line_span(tb, LINENO(line));

        if (!u16_span(tb, bytes, span, label))
            return false;
    }
    return true;
}

static bool raw_consumers(const u8 *bytes, size_t len, const char *label,
                          bool fixed_records)
{
    TextBuf *tb = yew_textbuf_from_bytes(bytes, (u64)len);
    bool ok = register_roundtrip(tb, bytes, len, label) &&
              fletch_roundtrip(bytes, len, label) &&
              json_roundtrip(bytes, len, label);

    if (ok && fixed_records) {
        size_t at;

        for (at = 0U; at < len; at += RECORD_BYTES) {
            if (!u16_span(tb, bytes,
                          (Span){(u64)at, (u64)(at + RECORD_BYTES)}, label)) {
                ok = false;
                break;
            }
        }
    } else if (ok) {
        ok = u16_lines(tb, bytes, label);
    }
    yew_textbuf_free(tb);
    return ok;
}

static bool corpus_probe(size_t *cases)
{
    FILE *fp = fopen("tests/unit/fixtures/unicode/corpus.txt", "r");
    char line[2048];

    if (fp == NULL) {
        (void)fprintf(stderr, "f01-unicode: cannot open corpus: %s\n",
                      strerror(errno));
        return false;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        u8 bytes[512];
        char *name = line;
        char *hex;
        char *end_field;
        char *scan;
        size_t len = 0U;

        while (*name == ' ' || *name == '\t')
            name++;
        if (*name == '#' || *name == '\n' || *name == '\0')
            continue;
        hex = strchr(name, '|');
        if (hex == NULL)
            continue;
        *hex++ = '\0';
        end_field = strchr(hex, '|');
        if (end_field == NULL)
            continue;
        *end_field = '\0';
        scan = hex;
        while (*scan != '\0') {
            char *end;
            unsigned long value;

            while (*scan == ' ' || *scan == '\t')
                scan++;
            if (*scan == '\0')
                break;
            errno = 0;
            value = strtoul(scan, &end, 16);
            if (errno != 0 || end == scan || value > 0xFFUL ||
                len == sizeof(bytes)) {
                (void)fclose(fp);
                return fail("corpus", name, "malformed hex byte field");
            }
            bytes[len++] = (u8)value;
            scan = end;
        }
        if (!raw_consumers(bytes, len, name, false)) {
            (void)fclose(fp);
            return false;
        }
        (*cases)++;
    }
    if (ferror(fp)) {
        (void)fclose(fp);
        return fail("corpus", "read", "I/O error");
    }
    return fclose(fp) == 0;
}

static bool exhaustive_probe(size_t *cases)
{
    u8 *slab = malloc(SLAB_BYTES);
    unsigned int high;

    if (slab == NULL)
        return fail("exhaustive", "allocation", "out of memory");
    for (high = 0U; high <= 0xFFU; high++) {
        unsigned int low;
        char label[32];

        for (low = 0U; low <= 0xFFFFU; low++) {
            size_t at = (size_t)low * RECORD_BYTES;
            u8 input[3] = {
                (u8)high, (u8)(low >> 8U), (u8)low
            };

            if (!core_roundtrip(input)) {
                (void)snprintf(label, sizeof(label), "%02x%04x",
                               high, low);
                free(slab);
                return fail("UTF-8", label, "decode/re-encode changed bytes");
            }
            (void)memcpy(slab + at, input, sizeof(input));
            slab[at + 3U] = (u8)'\n';
        }
        (void)snprintf(label, sizeof(label), "three-byte slab %02x", high);
        if (!raw_consumers(slab, SLAB_BYTES, label, true)) {
            free(slab);
            return false;
        }
        *cases += TRIPLES_PER_SLAB;
    }
    free(slab);
    return true;
}

int main(void)
{
    size_t corpus_cases = 0U;
    size_t exhaustive_cases = 0U;

    if (!corpus_probe(&corpus_cases) || !exhaustive_probe(&exhaustive_cases))
        return 1;
    (void)printf("f01-unicode: corpus=%zu three-byte=%zu "
                 "consumers=register,fletch,json,u16 ok\n",
                 corpus_cases, exhaustive_cases);
    return 0;
}
