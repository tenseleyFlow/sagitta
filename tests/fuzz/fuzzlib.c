#define _POSIX_C_SOURCE 200809L

#include "fuzzlib.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "unicode/utf8.h"

enum {
    SAG_FUZZ_DEFAULT_ITERS = 200000,
    SAG_FUZZ_MAX_INPUT = 65536,
    SAG_FUZZ_WHY_CAP = 256
};

typedef struct {
    u8 *data;
    size_t len;
    size_t cap;
} FuzzBuf;

typedef struct {
    char *name;
    FuzzBuf bytes;
} CorpusEntry;

typedef struct {
    CorpusEntry *entries;
    size_t len;
    size_t cap;
} Corpus;

typedef struct {
    u64 state;
    u64 seed;
    u64 hash;
    size_t iteration;
    size_t iterations;
    u64 seconds;
    u64 deadline_ms;
    bool corpus_only;
    const char *target;
    SagFuzzCheck check;
    Corpus corpus;
} FuzzRun;

static volatile sig_atomic_t watchdog_iteration;

static void *xmalloc(size_t size);

static u64 monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        (void)fprintf(stderr, "fuzz: clock_gettime: %s\n", strerror(errno));
        _Exit(2);
    }
    return (u64)ts.tv_sec * UINT64_C(1000) + (u64)ts.tv_nsec / 1000000U;
}

static bool fail_at(char *why, size_t cap, const char *message, size_t off)
{
    (void)snprintf(why, cap, "%s at byte %zu", message, off);
    return false;
}

static bool check_incremental_utf8(const u8 *data, size_t len,
                                   const u32 *expected,
                                   size_t expected_len,
                                   char *why, size_t why_cap)
{
    SagU8Dec dec;
    size_t out_len = 0U;
    size_t i;

    sag_utf8_dec_init(&dec);
    for (i = 0U; i < len; i++) {
        u8 count = sag_utf8_push(&dec, data[i]);
        u8 j;

        for (j = 0U; j < count; j++) {
            if (out_len >= expected_len || dec.out[j] != expected[out_len])
                return fail_at(why, why_cap,
                               "incremental decoder disagrees with one-shot",
                               i);
            out_len++;
        }
    }
    {
        u8 count = sag_utf8_finish(&dec);
        u8 j;

        for (j = 0U; j < count; j++) {
            if (out_len >= expected_len || dec.out[j] != expected[out_len])
                return fail_at(why, why_cap,
                               "incremental finish disagrees with one-shot",
                               len);
            out_len++;
        }
    }
    if (out_len != expected_len)
        return fail_at(why, why_cap,
                       "incremental decoder omitted output", len);
    return true;
}

bool sag_fuzz_check_utf8(const u8 *data, size_t len,
                         char *why, size_t why_cap)
{
    u32 *decoded = xmalloc((len == 0U ? 1U : len) * sizeof(*decoded));
    u8 *roundtrip = xmalloc(len == 0U ? 1U : len);
    size_t decoded_len = 0U;
    size_t roundtrip_len = 0U;
    size_t pos = 0U;

    while (pos < len) {
        u32 cp;
        u8 encoded[SAG_UTF8_MAX];
        size_t consumed = sag_utf8_decode(data + pos, len - pos, &cp);
        size_t encoded_len;

        if (consumed == 0U || consumed > len - pos) {
            free(decoded);
            free(roundtrip);
            return fail_at(why, why_cap,
                           "decoder did not consume valid span", pos);
        }
        if ((cp >= 0xD800U && cp <= 0xDFFFU && !sag_utf8_is_escape(cp)) ||
            cp > 0x10FFFFU) {
            free(decoded);
            free(roundtrip);
            return fail_at(why, why_cap,
                           "decoder returned invalid scalar", pos);
        }
        encoded_len = sag_utf8_encode(cp, encoded);
        if (encoded_len == 0U || encoded_len > sizeof(encoded) ||
            roundtrip_len + encoded_len > len) {
            free(decoded);
            free(roundtrip);
            return fail_at(why, why_cap,
                           "decoded scalar did not re-encode", pos);
        }
        if (!sag_utf8_is_escape(cp) && encoded_len != consumed) {
            free(decoded);
            free(roundtrip);
            return fail_at(why, why_cap,
                           "non-canonical UTF-8 was accepted", pos);
        }
        (void)memcpy(roundtrip + roundtrip_len, encoded, encoded_len);
        roundtrip_len += encoded_len;
        decoded[decoded_len++] = cp;
        pos += consumed;
    }
    if (pos != len || roundtrip_len != len ||
        (len != 0U && memcmp(roundtrip, data, len) != 0)) {
        free(decoded);
        free(roundtrip);
        return fail_at(why, why_cap,
                       "decode/encode round-trip changed bytes", pos);
    }
    if (!check_incremental_utf8(data, len, decoded, decoded_len,
                                why, why_cap)) {
        free(decoded);
        free(roundtrip);
        return false;
    }
    free(decoded);
    free(roundtrip);
    return true;
}

static void *xmalloc(size_t size)
{
    void *p = malloc(size == 0U ? 1U : size);

    if (p == NULL) {
        (void)fprintf(stderr, "fuzz: out of memory\n");
        _Exit(2);
    }
    return p;
}

static void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size == 0U ? 1U : size);

    if (p == NULL) {
        (void)fprintf(stderr, "fuzz: out of memory\n");
        _Exit(2);
    }
    return p;
}

static char *xstrdup(const char *s)
{
    size_t len = strlen(s);
    char *copy = xmalloc(len + 1U);

    (void)memcpy(copy, s, len + 1U);
    return copy;
}

static void buf_reserve(FuzzBuf *buf, size_t need)
{
    size_t cap;

    if (need <= buf->cap)
        return;
    cap = buf->cap == 0U ? 32U : buf->cap;
    while (cap < need) {
        if (cap > SAG_FUZZ_MAX_INPUT / 2U) {
            cap = SAG_FUZZ_MAX_INPUT;
            break;
        }
        cap *= 2U;
    }
    buf->data = xrealloc(buf->data, cap);
    buf->cap = cap;
}

static void buf_assign(FuzzBuf *buf, const u8 *data, size_t len)
{
    if (len > SAG_FUZZ_MAX_INPUT)
        len = SAG_FUZZ_MAX_INPUT;
    buf_reserve(buf, len);
    if (len != 0U)
        (void)memcpy(buf->data, data, len);
    buf->len = len;
}

static void buf_insert(FuzzBuf *buf, size_t at, const u8 *data, size_t len)
{
    if (at > buf->len)
        at = buf->len;
    if (len > SAG_FUZZ_MAX_INPUT - buf->len)
        len = SAG_FUZZ_MAX_INPUT - buf->len;
    if (len == 0U)
        return;
    buf_reserve(buf, buf->len + len);
    (void)memmove(buf->data + at + len, buf->data + at, buf->len - at);
    (void)memcpy(buf->data + at, data, len);
    buf->len += len;
}

static void buf_delete(FuzzBuf *buf, size_t at, size_t len)
{
    if (at >= buf->len)
        return;
    if (len > buf->len - at)
        len = buf->len - at;
    (void)memmove(buf->data + at, buf->data + at + len,
                  buf->len - at - len);
    buf->len -= len;
}

static void corpus_add(Corpus *corpus, const char *name,
                       const u8 *data, size_t len)
{
    CorpusEntry *entry;

    if (len > SAG_FUZZ_MAX_INPUT)
        len = SAG_FUZZ_MAX_INPUT;
    if (corpus->len == corpus->cap) {
        size_t cap = corpus->cap == 0U ? 16U : corpus->cap * 2U;
        corpus->entries = xrealloc(corpus->entries,
                                   cap * sizeof(*corpus->entries));
        corpus->cap = cap;
    }
    entry = &corpus->entries[corpus->len++];
    (void)memset(entry, 0, sizeof(*entry));
    entry->name = xstrdup(name);
    buf_assign(&entry->bytes, data, len);
}

static void corpus_free(Corpus *corpus)
{
    size_t i;

    for (i = 0U; i < corpus->len; i++) {
        free(corpus->entries[i].name);
        free(corpus->entries[i].bytes.data);
    }
    free(corpus->entries);
}

static void corpus_sort(Corpus *corpus)
{
    size_t i;

    /* Stable insertion sort: corpus sizes are small, and raw qsort is banned. */
    for (i = 1U; i < corpus->len; i++) {
        CorpusEntry value = corpus->entries[i];
        size_t j = i;

        while (j > 0U && strcmp(corpus->entries[j - 1U].name,
                                value.name) > 0) {
            corpus->entries[j] = corpus->entries[j - 1U];
            j--;
        }
        corpus->entries[j] = value;
    }
}

static bool hex_nibble(char c, u8 *out)
{
    if (c >= '0' && c <= '9') {
        *out = (u8)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (u8)(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (u8)(c - 'A' + 10);
        return true;
    }
    return false;
}

/* corpus.txt and width_golden.txt put their byte string in field two.
 * Accept either compact hex or whitespace-separated byte pairs. */
static bool parse_hex_field(const char *line, FuzzBuf *out)
{
    const char *begin = strchr(line, '|');
    const char *end;
    u8 high = 0U;
    bool have_high = false;

    if (begin == NULL)
        return false;
    begin++;
    end = strchr(begin, '|');
    if (end == NULL)
        return false;
    out->len = 0U;
    while (begin < end) {
        u8 nibble;

        if (!hex_nibble(*begin++, &nibble))
            continue;
        if (!have_high) {
            high = nibble;
            have_high = true;
        } else {
            u8 byte = (u8)((high << 4) | nibble);
            buf_insert(out, out->len, &byte, 1U);
            have_high = false;
        }
    }
    return !have_high && out->len != 0U;
}

static int parse_gbtest_line(const char *line, FuzzBuf *out)
{
    const u8 *p = (const u8 *)line;

    out->len = 0U;
    while (*p != 0U) {
        u32 cp = 0U;
        size_t digits = 0U;
        u8 encoded[SAG_UTF8_MAX];
        size_t encoded_len;
        u8 nibble;

        while (*p == (u8)' ' || *p == (u8)'\t')
            p++;
        if (*p == (u8)'#' || *p == (u8)'\r' || *p == (u8)'\n' ||
            *p == 0U)
            break;
        if (p[0] == 0xC3U && (p[1] == 0xB7U || p[1] == 0x97U)) {
            p += 2;
            continue;
        }
        while (hex_nibble((char)*p, &nibble)) {
            if (digits == 6U)
                return -1;
            cp = (cp << 4) | nibble;
            digits++;
            p++;
        }
        if (digits == 0U)
            return -1;
        encoded_len = sag_utf8_encode(cp, encoded);
        if (encoded_len == 0U)
            return -1;
        buf_insert(out, out->len, encoded, encoded_len);
    }
    return out->len == 0U ? 0 : 1;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash == NULL ? path : slash + 1;
}

static bool load_file_lines(Corpus *corpus, const char *path)
{
    FILE *fp = fopen(path, "rb");
    char *line = NULL;
    size_t line_cap = 0U;
    size_t line_no = 0U;
    ssize_t got;
    bool ok = true;
    bool gbtest = strcmp(path_basename(path),
                         "GraphemeBreakTest.txt") == 0;

    if (fp == NULL) {
        (void)fprintf(stderr, "fuzz: cannot open corpus %s: %s\n", path,
                      strerror(errno));
        return false;
    }
    while ((got = getline(&line, &line_cap, fp)) >= 0) {
        char *name;
        FuzzBuf parsed = {0};
        size_t len = (size_t)got;
        int gbtest_status = 0;

        line_no++;
        while (len != 0U && (line[len - 1U] == '\n' ||
                             line[len - 1U] == '\r'))
            len--;
        if (len == 0U || line[0] == '#')
            continue;
        name = xmalloc(strlen(path) + 32U);
        (void)snprintf(name, strlen(path) + 32U, "%s:%zu", path, line_no);
        if (gbtest)
            gbtest_status = parse_gbtest_line(line, &parsed);
        if (gbtest_status < 0) {
            (void)fprintf(stderr,
                          "fuzz: malformed grapheme corpus %s:%zu\n",
                          path, line_no);
            free(name);
            free(parsed.data);
            ok = false;
            break;
        }
        if (gbtest_status > 0 || parse_hex_field(line, &parsed))
            corpus_add(corpus, name, parsed.data, parsed.len);
        else if (!gbtest)
            corpus_add(corpus, name, (const u8 *)line, len);
        free(name);
        free(parsed.data);
    }
    if (ferror(fp)) {
        (void)fprintf(stderr, "fuzz: cannot read corpus %s: %s\n", path,
                      strerror(errno == 0 ? EIO : errno));
        ok = false;
    }
    free(line);
    if (fclose(fp) != 0) {
        (void)fprintf(stderr, "fuzz: cannot close corpus %s: %s\n", path,
                      strerror(errno));
        ok = false;
    }
    return ok;
}

static bool load_dir(Corpus *corpus, const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *ent;
    bool ok = true;

    if (dir == NULL) {
        (void)fprintf(stderr, "fuzz: cannot open corpus directory %s: %s\n",
                      path, strerror(errno));
        return false;
    }
    for (;;) {
        char child[1024];
        struct stat st;

        errno = 0;
        ent = readdir(dir);
        if (ent == NULL) {
            if (errno != 0) {
                (void)fprintf(stderr,
                              "fuzz: cannot read corpus directory %s: %s\n",
                              path, strerror(errno));
                ok = false;
            }
            break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >=
            (int)sizeof(child)) {
            (void)fprintf(stderr, "fuzz: corpus path is too long: %s/%s\n",
                          path, ent->d_name);
            ok = false;
            break;
        }
        if (stat(child, &st) != 0) {
            (void)fprintf(stderr, "fuzz: cannot stat corpus %s: %s\n",
                          child, strerror(errno));
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!load_dir(corpus, child)) {
                ok = false;
                break;
            }
        } else if (S_ISREG(st.st_mode) &&
                   !load_file_lines(corpus, child)) {
            ok = false;
            break;
        }
    }
    if (closedir(dir) != 0) {
        (void)fprintf(stderr, "fuzz: cannot close corpus directory %s: %s\n",
                      path, strerror(errno));
        ok = false;
    }
    return ok;
}

static u64 prng_next(FuzzRun *run)
{
    u64 x = run->state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    run->state = x;
    return x * UINT64_C(2685821657736338717);
}

static size_t choose(FuzzRun *run, size_t limit)
{
    return limit == 0U ? 0U : (size_t)(prng_next(run) % (u64)limit);
}

static void mutate_flip(FuzzRun *run, FuzzBuf *buf)
{
    u8 byte;

    if (buf->len == 0U) {
        byte = (u8)prng_next(run);
        buf_insert(buf, 0U, &byte, 1U);
    } else {
        size_t at = choose(run, buf->len);
        buf->data[at] ^= (u8)(1U << choose(run, 8U));
    }
}

static void mutate_delete(FuzzRun *run, FuzzBuf *buf)
{
    size_t at;
    size_t len;

    if (buf->len == 0U)
        return;
    at = choose(run, buf->len);
    len = 1U + choose(run, buf->len - at);
    buf_delete(buf, at, len);
}

static void mutate_duplicate(FuzzRun *run, FuzzBuf *buf)
{
    u8 *copy;
    size_t from;
    size_t len;
    size_t at;

    if (buf->len == 0U)
        return;
    from = choose(run, buf->len);
    len = 1U + choose(run, buf->len - from);
    if (len > SAG_FUZZ_MAX_INPUT - buf->len)
        len = SAG_FUZZ_MAX_INPUT - buf->len;
    copy = xmalloc(len);
    (void)memcpy(copy, buf->data + from, len);
    at = choose(run, buf->len + 1U);
    buf_insert(buf, at, copy, len);
    free(copy);
}

static void mutate_swap(FuzzRun *run, FuzzBuf *buf)
{
    size_t a;
    size_t b;
    size_t max_len;
    size_t len;
    size_t i;

    if (buf->len < 2U)
        return;
    a = choose(run, buf->len - 1U);
    b = a + 1U + choose(run, buf->len - a - 1U);
    max_len = b - a;
    if (max_len > buf->len - b)
        max_len = buf->len - b;
    if (max_len == 0U)
        return;
    len = 1U + choose(run, max_len);
    for (i = 0U; i < len; i++) {
        u8 tmp = buf->data[a + i];
        buf->data[a + i] = buf->data[b + i];
        buf->data[b + i] = tmp;
    }
}

static void mutate_truncate(FuzzRun *run, FuzzBuf *buf)
{
    if (buf->len != 0U)
        buf->len = choose(run, buf->len + 1U);
}

static void mutate_splice(FuzzRun *run, FuzzBuf *buf)
{
    const FuzzBuf *other =
        &run->corpus.entries[choose(run, run->corpus.len)].bytes;
    size_t from;
    size_t len;
    size_t at = choose(run, buf->len + 1U);

    if (other->len == 0U)
        return;
    from = choose(run, other->len + 1U);
    len = other->len - from;
    if (len != 0U)
        len = 1U + choose(run, len);
    buf_insert(buf, at, other->data + from, len);
}

static void mutate_split_utf8(FuzzRun *run, FuzzBuf *buf)
{
    size_t start = choose(run, buf->len + 1U);
    size_t i;

    for (i = 0U; i < buf->len; i++) {
        size_t at = (start + i) % buf->len;
        u8 b = buf->data[at];
        size_t tails = b >= 0xC2U && b <= 0xDFU ? 1U :
                       b >= 0xE0U && b <= 0xEFU ? 2U :
                       b >= 0xF0U && b <= 0xF4U ? 3U : 0U;

        if (tails != 0U && at + 1U < buf->len) {
            size_t cut = 1U + choose(run, tails);
            if (cut > buf->len - at - 1U)
                cut = buf->len - at - 1U;
            buf_delete(buf, at + 1U, cut);
            return;
        }
    }
    mutate_flip(run, buf);
}

static void mutate_lead_class(FuzzRun *run, FuzzBuf *buf)
{
    static const u8 leads[] = {
        0x80U, 0xC0U, 0xC2U, 0xE0U, 0xE1U, 0xEDU,
        0xF0U, 0xF1U, 0xF4U, 0xF5U, 0xFFU
    };
    u8 b = leads[choose(run, SAG_ARRAY_LEN(leads))];

    if (buf->len == 0U)
        buf_insert(buf, 0U, &b, 1U);
    else
        buf->data[choose(run, buf->len)] = b;
}

static void mutate_continuation(FuzzRun *run, FuzzBuf *buf)
{
    u8 b = (u8)(0x80U | choose(run, 0x40U));

    buf_insert(buf, choose(run, buf->len + 1U), &b, 1U);
}

static void mutate_surrogate(FuzzRun *run, FuzzBuf *buf)
{
    static const u8 surrogate[] = {0xEDU, 0xA0U, 0x80U};

    buf_insert(buf, choose(run, buf->len + 1U), surrogate,
               sizeof(surrogate));
}

static const char *mutate(FuzzRun *run, FuzzBuf *buf)
{
    size_t op = choose(run, 10U);

    switch (op) {
    case 0U: mutate_flip(run, buf); return "byte-flip";
    case 1U: mutate_delete(run, buf); return "chunk-delete";
    case 2U: mutate_duplicate(run, buf); return "chunk-duplicate";
    case 3U: mutate_swap(run, buf); return "chunk-swap";
    case 4U: mutate_truncate(run, buf); return "truncate";
    case 5U: mutate_splice(run, buf); return "corpus-splice";
    case 6U: mutate_split_utf8(run, buf); return "utf8-split";
    case 7U: mutate_lead_class(run, buf); return "lead-class";
    case 8U: mutate_continuation(run, buf); return "lone-continuation";
    default: mutate_surrogate(run, buf); return "surrogate-inject";
    }
}

static void hash_bytes(FuzzRun *run, const FuzzBuf *buf)
{
    size_t i;
    u64 len = (u64)buf->len;

    for (i = 0U; i < sizeof(len); i++) {
        run->hash ^= (u8)(len >> (i * 8U));
        run->hash *= UINT64_C(1099511628211);
    }
    for (i = 0U; i < buf->len; i++) {
        run->hash ^= buf->data[i];
        run->hash *= UINT64_C(1099511628211);
    }
}

static void watchdog(int signo)
{
    static const char message[] = "fuzz: watchdog expired\n";
    ssize_t written;

    (void)signo;
    (void)watchdog_iteration;
    written = write(STDERR_FILENO, message, sizeof(message) - 1U);
    (void)written;
    _Exit(124);
}

static bool checked(FuzzRun *run, const FuzzBuf *buf,
                    char why[SAG_FUZZ_WHY_CAP])
{
    u8 *exact = xmalloc(buf->len == 0U ? 1U : buf->len);
    bool ok;

    if (buf->len != 0U)
        (void)memcpy(exact, buf->data, buf->len);
    watchdog_iteration = (sig_atomic_t)run->iteration;
    (void)alarm(5U);
    ok = run->check(exact, buf->len, why, SAG_FUZZ_WHY_CAP);
    (void)alarm(0U);
    free(exact);
    return ok;
}

static void minimize(FuzzRun *run, FuzzBuf *buf)
{
    size_t granularity = 2U;
    char why[SAG_FUZZ_WHY_CAP];

    while (buf->len != 0U) {
        size_t chunk = (buf->len + granularity - 1U) / granularity;
        bool reduced = false;
        size_t at;

        for (at = 0U; at < buf->len; at += chunk) {
            FuzzBuf candidate = {0};
            size_t take = chunk;

            if (take > buf->len - at)
                take = buf->len - at;
            buf_assign(&candidate, buf->data, buf->len);
            buf_delete(&candidate, at, take);
            if (!checked(run, &candidate, why)) {
                buf_assign(buf, candidate.data, candidate.len);
                reduced = true;
                free(candidate.data);
                granularity = granularity > 2U ? granularity - 1U : 2U;
                break;
            }
            free(candidate.data);
        }
        if (reduced)
            continue;
        if (granularity >= buf->len)
            break;
        granularity *= 2U;
        if (granularity > buf->len)
            granularity = buf->len;
    }
}

static void save_crash(FuzzRun *run, const FuzzBuf *buf)
{
    char path[256];
    int fd;
    size_t off = 0U;

    if (mkdir("tests/fuzz/crashes", 0777) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "fuzz: cannot create crash directory: %s\n",
                      strerror(errno));
        return;
    }
    (void)snprintf(path, sizeof(path),
                   "tests/fuzz/crashes/%s-seed-%llu-iter-%zu.bin",
                   run->target, (unsigned long long)run->seed,
                   run->iteration);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        (void)fprintf(stderr, "fuzz: cannot write %s: %s\n", path,
                      strerror(errno));
        return;
    }
    while (off < buf->len) {
        ssize_t wrote = write(fd, buf->data + off, buf->len - off);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            break;
        off += (size_t)wrote;
    }
    (void)close(fd);
    (void)fprintf(stderr, "fuzz: minimized input saved to %s (%zu bytes)\n",
                  path, buf->len);
}

static bool crashes_empty(void)
{
    DIR *dir = opendir("tests/fuzz/crashes");
    struct dirent *ent;
    bool empty = true;

    if (dir == NULL)
        return errno == ENOENT;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0 &&
            strcmp(ent->d_name, ".gitkeep") != 0) {
            empty = false;
            break;
        }
    }
    (void)closedir(dir);
    return empty;
}

static bool parse_u64_option(const char *arg, const char *prefix, u64 *out)
{
    char *end;
    unsigned long long value;
    size_t prefix_len = strlen(prefix);

    if (strncmp(arg, prefix, prefix_len) != 0)
        return false;
    errno = 0;
    value = strtoull(arg + prefix_len, &end, 0);
    if (errno != 0 || *end != '\0' || end == arg + prefix_len)
        return false;
    *out = (u64)value;
    return true;
}

static bool parse_size_option(const char *arg, const char *prefix, size_t *out)
{
    u64 value;

    if (!parse_u64_option(arg, prefix, &value) || value > (u64)SIZE_MAX)
        return false;
    *out = (size_t)value;
    return true;
}

static void add_builtin_corpus(Corpus *corpus)
{
    static const u8 ascii[] = "Sagitta\n";
    static const u8 valid[] = {0xE2U, 0x82U, 0xACU, 0xF0U, 0x9FU,
                               0x91U, 0x8DU};
    static const u8 invalid[] = {0x80U, 0xC0U, 0x80U, 0xEDU, 0xA0U,
                                 0x80U, 0xFFU};
    static const u8 grapheme[] = {0x65U, 0xCCU, 0x81U, 0xF0U, 0x9FU,
                                  0x87U, 0xA6U, 0xF0U, 0x9FU, 0x87U,
                                  0xBAU};

    corpus_add(corpus, "builtin/empty", NULL, 0U);
    corpus_add(corpus, "builtin/ascii", ascii, sizeof(ascii) - 1U);
    corpus_add(corpus, "builtin/valid", valid, sizeof(valid));
    corpus_add(corpus, "builtin/invalid", invalid, sizeof(invalid));
    corpus_add(corpus, "builtin/grapheme", grapheme, sizeof(grapheme));
}

int sag_fuzz_main(int argc, char **argv, const char *target,
                  const char *corpus_dir, SagFuzzCheck check)
{
    FuzzRun run;
    struct sigaction action;
    size_t i;

    (void)memset(&run, 0, sizeof(run));
    run.seed = 1U;
    run.iterations = SAG_FUZZ_DEFAULT_ITERS;
    run.target = target;
    run.check = check;
    for (i = 1U; i < (size_t)argc; i++) {
        if (parse_u64_option(argv[i], "--seed=", &run.seed))
            continue;
        if (parse_size_option(argv[i], "--iters=", &run.iterations))
            continue;
        if (parse_u64_option(argv[i], "--seconds=", &run.seconds) &&
            run.seconds != 0U)
            continue;
        if (strcmp(argv[i], "--corpus-only") == 0) {
            run.corpus_only = true;
            continue;
        }
        (void)fprintf(stderr,
                      "usage: %s [--seed=N] [--iters=N] [--seconds=N] "
                      "[--corpus-only]\n",
                      argv[0]);
        return 2;
    }
    if (!crashes_empty()) {
        (void)fprintf(stderr,
                      "%s: tests/fuzz/crashes contains a crashing input\n",
                      target);
        return 1;
    }
    add_builtin_corpus(&run.corpus);
    if (corpus_dir != NULL && !load_dir(&run.corpus, corpus_dir)) {
        corpus_free(&run.corpus);
        return 2;
    }
    corpus_sort(&run.corpus);
    run.state = run.seed == 0U ? UINT64_C(0x9E3779B97F4A7C15) : run.seed;
    run.hash = UINT64_C(1469598103934665603);
    if (run.seconds != 0U) {
        u64 now = monotonic_ms();
        u64 span;

        if (run.seconds > UINT64_MAX / 1000U) {
            (void)fprintf(stderr, "%s: duration is too large\n", target);
            corpus_free(&run.corpus);
            return 2;
        }
        span = run.seconds * 1000U;
        run.deadline_ms = now > UINT64_MAX - span ? UINT64_MAX : now + span;
        run.iterations = SIZE_MAX;
    }
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = watchdog;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) {
        (void)fprintf(stderr, "%s: sigaction: %s\n", target,
                      strerror(errno));
        corpus_free(&run.corpus);
        return 2;
    }
    if (run.corpus_only) {
        for (run.iteration = 0U; run.iteration < run.corpus.len;
             run.iteration++) {
            const CorpusEntry *entry = &run.corpus.entries[run.iteration];
            char why[SAG_FUZZ_WHY_CAP] = {0};

            if (!checked(&run, &entry->bytes, why)) {
                (void)fprintf(stderr, "%s: FAIL corpus=%s: %s\n",
                              target, entry->name, why);
                corpus_free(&run.corpus);
                return 1;
            }
        }
        (void)printf("%s: corpus=%zu exact replay ok\n", target,
                     run.corpus.len);
        corpus_free(&run.corpus);
        return 0;
    }
    for (run.iteration = 0U; run.iteration < run.iterations;
         run.iteration++) {
        const CorpusEntry *seed =
            &run.corpus.entries[choose(&run, run.corpus.len)];
        FuzzBuf input = {0};
        const char *op = "none";
        size_t mutations = 1U + choose(&run, 4U);
        size_t m;
        char why[SAG_FUZZ_WHY_CAP] = {0};

        if (run.seconds != 0U && run.iteration != 0U &&
            monotonic_ms() >= run.deadline_ms)
            break;

        buf_assign(&input, seed->bytes.data, seed->bytes.len);
        for (m = 0U; m < mutations; m++)
            op = mutate(&run, &input);
        hash_bytes(&run, &input);
        if (!checked(&run, &input, why)) {
            (void)fprintf(stderr,
                          "%s: FAIL seed=%llu iter=%zu corpus=%s op=%s: %s\n",
                          target, (unsigned long long)run.seed,
                          run.iteration, seed->name, op, why);
            minimize(&run, &input);
            save_crash(&run, &input);
            free(input.data);
            corpus_free(&run.corpus);
            return 1;
        }
        free(input.data);
    }
    if (run.seconds != 0U) {
        (void)printf("%s: seed=%llu seconds=%llu iters=%zu corpus=%zu "
                     "hash=%016llx ok\n",
                     target, (unsigned long long)run.seed,
                     (unsigned long long)run.seconds, run.iteration,
                     run.corpus.len, (unsigned long long)run.hash);
    } else {
        (void)printf("%s: seed=%llu iters=%zu corpus=%zu hash=%016llx ok\n",
                     target, (unsigned long long)run.seed, run.iterations,
                     run.corpus.len, (unsigned long long)run.hash);
    }
    corpus_free(&run.corpus);
    return 0;
}
