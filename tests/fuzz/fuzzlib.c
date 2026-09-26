#define _POSIX_C_SOURCE 200809L

#include "fuzzlib.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "unicode/utf8.h"

#if YEW_COV
#include "cov.h"
#endif

enum {
    YEW_FUZZ_DEFAULT_ITERS = 200000,
    YEW_FUZZ_DEFAULT_WATCHDOG_SECONDS = 5,
    YEW_FUZZ_MAX_INPUT = 65536,
    YEW_FUZZ_WHY_CAP = 256
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
    unsigned int watchdog_seconds;
    bool corpus_only;
    bool coverage_report;
    const char *admit_dir;
    char guided_dir[256];
    const char *target;
    YewFuzzCheck check;
    Corpus corpus;
    Corpus dictionary;
#if YEW_COV
    size_t admitted;
    u64 admitted_edges;
    u64 unstable_edges;
#endif
} FuzzRun;

static volatile sig_atomic_t watchdog_iteration;
/* The input under check, published for the SIGALRM handler so a hang
 * leaves a replayable crash file instead of vanishing with _Exit.  Two
 * stores per check; the handler does only async-signal-safe work (mkdir,
 * open, write, close) on a path formatted once at startup. */
static const u8 *volatile watchdog_data;
static volatile size_t watchdog_len;
static char watchdog_path[320];
static size_t watchdog_path_len;

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
    YewU8Dec dec;
    size_t out_len = 0U;
    size_t i;

    yew_utf8_dec_init(&dec);
    for (i = 0U; i < len; i++) {
        u8 count = yew_utf8_push(&dec, data[i]);
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
        u8 count = yew_utf8_finish(&dec);
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

bool yew_fuzz_check_utf8(const u8 *data, size_t len,
                         char *why, size_t why_cap)
{
    u32 *decoded = xmalloc((len == 0U ? 1U : len) * sizeof(*decoded));
    u8 *roundtrip = xmalloc(len == 0U ? 1U : len);
    size_t decoded_len = 0U;
    size_t roundtrip_len = 0U;
    size_t pos = 0U;

    while (pos < len) {
        u32 cp;
        u8 encoded[YEW_UTF8_MAX];
        size_t consumed = yew_utf8_decode(data + pos, len - pos, &cp);
        size_t encoded_len;

        if (consumed == 0U || consumed > len - pos) {
            free(decoded);
            free(roundtrip);
            return fail_at(why, why_cap,
                           "decoder did not consume valid span", pos);
        }
        if ((cp >= 0xD800U && cp <= 0xDFFFU && !yew_utf8_is_escape(cp)) ||
            cp > 0x10FFFFU) {
            free(decoded);
            free(roundtrip);
            return fail_at(why, why_cap,
                           "decoder returned invalid scalar", pos);
        }
        encoded_len = yew_utf8_encode(cp, encoded);
        if (encoded_len == 0U || encoded_len > sizeof(encoded) ||
            roundtrip_len + encoded_len > len) {
            free(decoded);
            free(roundtrip);
            return fail_at(why, why_cap,
                           "decoded scalar did not re-encode", pos);
        }
        if (!yew_utf8_is_escape(cp) && encoded_len != consumed) {
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
        if (cap > YEW_FUZZ_MAX_INPUT / 2U) {
            cap = YEW_FUZZ_MAX_INPUT;
            break;
        }
        cap *= 2U;
    }
    buf->data = xrealloc(buf->data, cap);
    buf->cap = cap;
}

static void buf_assign(FuzzBuf *buf, const u8 *data, size_t len)
{
    if (len > YEW_FUZZ_MAX_INPUT)
        len = YEW_FUZZ_MAX_INPUT;
    buf_reserve(buf, len);
    if (len != 0U)
        (void)memcpy(buf->data, data, len);
    buf->len = len;
}

static void buf_insert(FuzzBuf *buf, size_t at, const u8 *data, size_t len)
{
    if (at > buf->len)
        at = buf->len;
    if (len > YEW_FUZZ_MAX_INPUT - buf->len)
        len = YEW_FUZZ_MAX_INPUT - buf->len;
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

    if (len > YEW_FUZZ_MAX_INPUT)
        len = YEW_FUZZ_MAX_INPUT;
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
        u8 encoded[YEW_UTF8_MAX];
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
        encoded_len = yew_utf8_encode(cp, encoded);
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

static bool path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);

    return path_len >= suffix_len &&
           strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool load_raw_file(Corpus *corpus, const char *path)
{
    FILE *fp = fopen(path, "rb");
    FuzzBuf bytes = {0};
    u8 chunk[4096];
    bool ok = true;

    if (fp == NULL) {
        (void)fprintf(stderr, "fuzz: cannot open corpus %s: %s\n", path,
                      strerror(errno));
        return false;
    }
    for (;;) {
        size_t room = YEW_FUZZ_MAX_INPUT - bytes.len;
        size_t want = room < sizeof(chunk) ? room : sizeof(chunk);
        size_t got;

        if (want == 0U)
            break;
        got = fread(chunk, 1U, want, fp);
        if (got != 0U)
            buf_insert(&bytes, bytes.len, chunk, got);
        if (got != want)
            break;
    }
    if (ferror(fp)) {
        (void)fprintf(stderr, "fuzz: cannot read corpus %s: %s\n", path,
                      strerror(errno == 0 ? EIO : errno));
        ok = false;
    } else if (bytes.len == YEW_FUZZ_MAX_INPUT && fgetc(fp) != EOF) {
        (void)fprintf(stderr, "fuzz: corpus entry exceeds %u bytes: %s\n",
                      (unsigned int)YEW_FUZZ_MAX_INPUT, path);
        ok = false;
    }
    if (fclose(fp) != 0) {
        (void)fprintf(stderr, "fuzz: cannot close corpus %s: %s\n", path,
                      strerror(errno));
        ok = false;
    }
    if (ok)
        corpus_add(corpus, path, bytes.data, bytes.len);
    free(bytes.data);
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
        } else if (S_ISREG(st.st_mode)) {
            bool loaded = path_has_suffix(child, ".bin") ?
                          load_raw_file(corpus, child) :
                          load_file_lines(corpus, child);

            if (!loaded) {
                ok = false;
                break;
            }
        }
    }
    if (closedir(dir) != 0) {
        (void)fprintf(stderr, "fuzz: cannot close corpus directory %s: %s\n",
                      path, strerror(errno));
        ok = false;
    }
    return ok;
}

static bool load_optional_dir(Corpus *corpus, const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            return true;
        (void)fprintf(stderr, "fuzz: cannot stat corpus %s: %s\n", path,
                      strerror(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        (void)fprintf(stderr, "fuzz: corpus path is not a directory: %s\n",
                      path);
        return false;
    }
    return load_dir(corpus, path);
}

static bool decode_dictionary_token(const char *line, size_t len,
                                    FuzzBuf *token)
{
    size_t i;

    token->len = 0U;
    for (i = 0U; i < len; i++) {
        u8 byte = (u8)line[i];

        if (byte == (u8)'\\' && i + 1U < len) {
            char escaped = line[i + 1U];

            if (escaped == 'x' && i + 3U < len) {
                u8 high;
                u8 low;

                if (hex_nibble(line[i + 2U], &high) &&
                    hex_nibble(line[i + 3U], &low)) {
                    byte = (u8)((high << 4) | low);
                    i += 3U;
                }
            } else if (escaped == 'n' || escaped == 'r' ||
                       escaped == 't' || escaped == '\\') {
                byte = escaped == 'n' ? (u8)'\n' :
                       escaped == 'r' ? (u8)'\r' :
                       escaped == 't' ? (u8)'\t' : (u8)'\\';
                i++;
            }
        }
        buf_insert(token, token->len, &byte, 1U);
    }
    return token->len != 0U;
}

static bool load_dictionary(Corpus *dictionary, const char *target)
{
    char path[256];
    FILE *fp;
    char *line = NULL;
    size_t line_cap = 0U;
    size_t line_no = 0U;
    ssize_t got;
    bool ok = true;

    if (snprintf(path, sizeof(path), "tests/fuzz/dict/%s.txt", target) >=
        (int)sizeof(path)) {
        (void)fprintf(stderr, "fuzz: dictionary path is too long: %s\n",
                      target);
        return false;
    }
    fp = fopen(path, "rb");
    if (fp == NULL)
        return errno == ENOENT;
    while ((got = getline(&line, &line_cap, fp)) >= 0) {
        size_t len = (size_t)got;
        size_t first = 0U;
        FuzzBuf token = {0};
        char name[320];

        line_no++;
        while (len != 0U && (line[len - 1U] == '\n' ||
                             line[len - 1U] == '\r'))
            len--;
        while (first < len && (line[first] == ' ' || line[first] == '\t'))
            first++;
        if (first == len || line[first] == '#')
            continue;
        if (!decode_dictionary_token(line + first, len - first, &token)) {
            (void)fprintf(stderr, "fuzz: empty dictionary token %s:%zu\n",
                          path, line_no);
            free(token.data);
            ok = false;
            break;
        }
        (void)snprintf(name, sizeof(name), "%s:%zu", path, line_no);
        corpus_add(dictionary, name, token.data, token.len);
        free(token.data);
    }
    if (ferror(fp)) {
        (void)fprintf(stderr, "fuzz: cannot read dictionary %s: %s\n", path,
                      strerror(errno == 0 ? EIO : errno));
        ok = false;
    }
    free(line);
    if (fclose(fp) != 0) {
        (void)fprintf(stderr, "fuzz: cannot close dictionary %s: %s\n", path,
                      strerror(errno));
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
    if (len > YEW_FUZZ_MAX_INPUT - buf->len)
        len = YEW_FUZZ_MAX_INPUT - buf->len;
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
    u8 b = leads[choose(run, YEW_ARRAY_LEN(leads))];

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

static void mutate_dictionary(FuzzRun *run, FuzzBuf *buf)
{
    const FuzzBuf *token =
        &run->dictionary.entries[choose(run, run->dictionary.len)].bytes;

    buf_insert(buf, choose(run, buf->len + 1U), token->data, token->len);
}

static const char *mutate(FuzzRun *run, FuzzBuf *buf)
{
    size_t op_count = run->dictionary.len == 0U ? 10U : 11U;
    size_t op = choose(run, op_count);

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
    case 9U: mutate_surrogate(run, buf); return "surrogate-inject";
    default: mutate_dictionary(run, buf); return "dictionary-splice";
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

#if YEW_COV
typedef struct {
    u32 state[8];
    u64 bit_len;
    u8 block[64];
    size_t block_len;
} FuzzSha256;

static u32 sha_rotr(u32 value, u32 count)
{
    return (value >> count) | (value << (32U - count));
}

static void sha256_block(FuzzSha256 *sha, const u8 block[64])
{
    static const u32 constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    u32 words[64];
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    u32 e;
    u32 f;
    u32 g;
    u32 h;
    size_t i;

    for (i = 0U; i < 16U; i++) {
        size_t off = i * 4U;

        words[i] = ((u32)block[off] << 24) |
                   ((u32)block[off + 1U] << 16) |
                   ((u32)block[off + 2U] << 8) |
                   (u32)block[off + 3U];
    }
    for (i = 16U; i < 64U; i++) {
        u32 x = words[i - 15U];
        u32 y = words[i - 2U];
        u32 s0 = sha_rotr(x, 7U) ^ sha_rotr(x, 18U) ^ (x >> 3);
        u32 s1 = sha_rotr(y, 17U) ^ sha_rotr(y, 19U) ^ (y >> 10);

        words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }
    a = sha->state[0];
    b = sha->state[1];
    c = sha->state[2];
    d = sha->state[3];
    e = sha->state[4];
    f = sha->state[5];
    g = sha->state[6];
    h = sha->state[7];
    for (i = 0U; i < 64U; i++) {
        u32 sum1 = sha_rotr(e, 6U) ^ sha_rotr(e, 11U) ^ sha_rotr(e, 25U);
        u32 choose_word = (e & f) ^ ((~e) & g);
        u32 temp1 = h + sum1 + choose_word + constants[i] + words[i];
        u32 sum0 = sha_rotr(a, 2U) ^ sha_rotr(a, 13U) ^ sha_rotr(a, 22U);
        u32 majority = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
    sha->state[5] += f;
    sha->state[6] += g;
    sha->state[7] += h;
}

static void sha256_init(FuzzSha256 *sha)
{
    static const u32 initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    (void)memset(sha, 0, sizeof(*sha));
    (void)memcpy(sha->state, initial, sizeof(initial));
}

static void sha256_update(FuzzSha256 *sha, const u8 *data, size_t len)
{
    size_t off = 0U;

    while (off < len) {
        size_t take = sizeof(sha->block) - sha->block_len;

        if (take > len - off)
            take = len - off;
        (void)memcpy(sha->block + sha->block_len, data + off, take);
        sha->block_len += take;
        off += take;
        if (sha->block_len == sizeof(sha->block)) {
            sha256_block(sha, sha->block);
            sha->bit_len += UINT64_C(512);
            sha->block_len = 0U;
        }
    }
}

static void sha256_final(FuzzSha256 *sha, u8 digest[32])
{
    u64 bits = sha->bit_len + (u64)sha->block_len * 8U;
    size_t i;

    sha->block[sha->block_len++] = 0x80U;
    if (sha->block_len > 56U) {
        (void)memset(sha->block + sha->block_len, 0,
                     sizeof(sha->block) - sha->block_len);
        sha256_block(sha, sha->block);
        sha->block_len = 0U;
    }
    (void)memset(sha->block + sha->block_len, 0, 56U - sha->block_len);
    for (i = 0U; i < 8U; i++)
        sha->block[63U - i] = (u8)(bits >> (i * 8U));
    sha256_block(sha, sha->block);
    for (i = 0U; i < 8U; i++) {
        digest[i * 4U] = (u8)(sha->state[i] >> 24);
        digest[i * 4U + 1U] = (u8)(sha->state[i] >> 16);
        digest[i * 4U + 2U] = (u8)(sha->state[i] >> 8);
        digest[i * 4U + 3U] = (u8)sha->state[i];
    }
}

static void sha256_prefix(const FuzzBuf *buf, char hex[33])
{
    static const char digits[] = "0123456789abcdef";
    FuzzSha256 sha;
    u8 digest[32];
    size_t i;

    sha256_init(&sha);
    sha256_update(&sha, buf->data, buf->len);
    sha256_final(&sha, digest);
    for (i = 0U; i < 16U; i++) {
        hex[i * 2U] = digits[digest[i] >> 4];
        hex[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    hex[32] = '\0';
}

static bool sha256_selftest(void)
{
    static const u8 abc[] = {'a', 'b', 'c'};
    FuzzBuf input = {0};
    char hex[33];

    sha256_prefix(&input, hex);
    if (strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb924") != 0)
        return false;
    input.data = (u8 *)abc;
    input.len = sizeof(abc);
    sha256_prefix(&input, hex);
    return strcmp(hex, "ba7816bf8f01cfea414140de5dae2223") == 0;
}
#endif

static void watchdog_say(const char *text, size_t len)
{
    while (len != 0U) {
        ssize_t wrote = write(STDERR_FILENO, text, len);

        if (wrote <= 0)
            return;
        text += wrote;
        len -= (size_t)wrote;
    }
}

static void watchdog(int signo)
{
    static const char expired[] = "fuzz: watchdog expired iter=";
    static const char saved[] = "; hung input saved to ";
    static const char unsaved[] = "; hung input could not be saved\n";
    const u8 *data = watchdog_data;
    size_t len = watchdog_len;
    unsigned long iteration = (unsigned long)watchdog_iteration;
    char digits[24];
    size_t at = sizeof(digits);
    bool ok = false;

    (void)signo;
    do {
        digits[--at] = (char)('0' + iteration % 10U);
        iteration /= 10U;
    } while (iteration != 0U && at != 0U);
    watchdog_say(expired, sizeof(expired) - 1U);
    watchdog_say(digits + at, sizeof(digits) - at);
    if (data != NULL && watchdog_path_len != 0U) {
        int fd;

        (void)mkdir("tests/fuzz/crashes", 0777);
        fd = open(watchdog_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            size_t off = 0U;

            ok = true;
            while (off < len) {
                ssize_t wrote = write(fd, data + off, len - off);

                if (wrote < 0 && errno == EINTR)
                    continue;
                if (wrote <= 0) {
                    ok = false;
                    break;
                }
                off += (size_t)wrote;
            }
            if (close(fd) != 0)
                ok = false;
        }
    }
    if (ok) {
        watchdog_say(saved, sizeof(saved) - 1U);
        watchdog_say(watchdog_path, watchdog_path_len);
        watchdog_say("\n", 1U);
    } else {
        watchdog_say(unsaved, sizeof(unsaved) - 1U);
    }
    _Exit(124);
}

static bool checked(FuzzRun *run, const FuzzBuf *buf,
                    char why[YEW_FUZZ_WHY_CAP])
{
    u8 *exact = xmalloc(buf->len == 0U ? 1U : buf->len);
    bool ok;

    if (buf->len != 0U)
        (void)memcpy(exact, buf->data, buf->len);
    watchdog_iteration = (sig_atomic_t)run->iteration;
    watchdog_len = buf->len;
    watchdog_data = exact;
    (void)alarm(run->watchdog_seconds);
    ok = run->check(exact, buf->len, why, YEW_FUZZ_WHY_CAP);
    (void)alarm(0U);
    watchdog_data = NULL;
    free(exact);
    return ok;
}

static void minimize(FuzzRun *run, FuzzBuf *buf)
{
    size_t granularity = 2U;
    char why[YEW_FUZZ_WHY_CAP];

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

#if YEW_COV
/* Shrink BUF while every edge in STABLE (a calibrated, reproducible subset
 * of what BUF first reached) is still reached.  The predicate is a fixed
 * edge set rather than "any unseen edge": the latter let a candidate trade
 * the calibrated edges for a one-shot edge (first-use initialisation, a
 * buffer growing past its high-water mark) that no later execution
 * reproduces.  A timed campaign stops shrinking at its deadline: every
 * accepted step already reaches STABLE, so the current input is admissible
 * and one expensive input cannot hold the campaign past its budget. */
static void minimize_coverage(FuzzRun *run, FuzzBuf *buf, const u32 *stable,
                              u32 stable_len)
{
    size_t granularity = 2U;
    char why[YEW_FUZZ_WHY_CAP];

    while (buf->len != 0U) {
        size_t chunk = (buf->len + granularity - 1U) / granularity;
        bool reduced = false;
        size_t at;

        for (at = 0U; at < buf->len; at += chunk) {
            FuzzBuf candidate = {0};
            size_t take = chunk;

            if (run->seconds != 0U && monotonic_ms() >= run->deadline_ms)
                return;
            if (take > buf->len - at)
                take = buf->len - at;
            buf_assign(&candidate, buf->data, buf->len);
            buf_delete(&candidate, at, take);
            yew_cov_reset();
            if (checked(run, &candidate, why) &&
                yew_cov_hit_all(stable, stable_len)) {
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

static bool file_matches(const char *path, const FuzzBuf *buf)
{
    int fd = open(path, O_RDONLY);
    size_t off = 0U;
    u8 bytes[4096];

    if (fd < 0)
        return false;
    for (;;) {
        ssize_t got = read(fd, bytes, sizeof(bytes));

        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0 || (size_t)got > buf->len - off ||
            (got != 0 && memcmp(bytes, buf->data + off, (size_t)got) != 0)) {
            (void)close(fd);
            return false;
        }
        if (got == 0)
            break;
        off += (size_t)got;
    }
    if (close(fd) != 0)
        return false;
    return off == buf->len;
}

static bool save_admission(FuzzRun *run, const FuzzBuf *buf, bool *created)
{
    char digest[33];
    char path[1024];
    int fd;
    size_t off = 0U;
    bool ok = true;

    *created = false;
    if (mkdir(run->admit_dir, 0777) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "fuzz: cannot create admission directory %s: "
                      "%s\n", run->admit_dir, strerror(errno));
        return false;
    }
    sha256_prefix(buf, digest);
    if (snprintf(path, sizeof(path), "%s/%s.bin", run->admit_dir, digest) >=
        (int)sizeof(path)) {
        (void)fprintf(stderr, "fuzz: admission path is too long: %s\n",
                      run->admit_dir);
        return false;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0 && errno == EEXIST) {
        if (!file_matches(path, buf)) {
            (void)fprintf(stderr,
                          "fuzz: SHA-256 prefix collision at %s\n", path);
            return false;
        }
        return true;
    }
    if (fd < 0) {
        (void)fprintf(stderr, "fuzz: cannot write admission %s: %s\n", path,
                      strerror(errno));
        return false;
    }
    while (off < buf->len) {
        ssize_t wrote = write(fd, buf->data + off, buf->len - off);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            ok = false;
            break;
        }
        off += (size_t)wrote;
    }
    if (ok && fsync(fd) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;
    if (!ok) {
        int saved_errno = errno == 0 ? EIO : errno;

        (void)unlink(path);
        (void)fprintf(stderr, "fuzz: cannot finish admission %s: %s\n", path,
                      strerror(saved_errno));
        return false;
    }
    corpus_add(&run->corpus, path, buf->data, buf->len);
    *created = true;
    return true;
}
#endif

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

#if YEW_COV
/* Admit one input whose execution reached NOVEL unseen edges; the coverage
 * map still holds that execution.  The input is re-executed once to
 * calibrate: only edges reached by both executions are treated as the
 * input's own (a campaign process keeps state across executions, so a
 * first-use or high-water-mark edge is reached once and never again).  The
 * input is minimized against that calibrated set and must reproduce it on a
 * final execution before it is admitted; if neither the minimized nor the
 * original input does, nothing is admitted.  The first execution's novel
 * edges (and the admitted execution's) are merged into the seen set so a
 * one-shot edge is not rediscovered forever; every merged edge not carried
 * by the admitted input is counted as unstable.  Returns
 * 0 to continue the campaign, otherwise the process exit status. */
static int admit_novel(FuzzRun *run, FuzzBuf *input, u32 novel)
{
    u32 *first = xmalloc((size_t)novel * sizeof(*first));
    u32 *stable = xmalloc((size_t)novel * sizeof(*stable));
    u32 first_len;
    u32 stable_len = 0U;
    u32 i;
    FuzzBuf original = {0};
    char why[YEW_FUZZ_WHY_CAP] = {0};
    bool created = false;
    bool reproduced = false;
    int status = 0;

    first_len = yew_cov_novel_ids(first, novel);
    buf_assign(&original, input->data, input->len);
    yew_cov_reset();
    if (!checked(run, input, why)) {
        (void)fprintf(stderr,
                      "%s: FAIL seed=%llu iter=%zu on calibration "
                      "re-execution: %s\n",
                      run->target, (unsigned long long)run->seed,
                      run->iteration, why);
        save_crash(run, input);
        status = 1;
        goto done;
    }
    for (i = 0U; i < first_len; i++) {
        if (yew_cov_hit_all(&first[i], 1U))
            stable[stable_len++] = first[i];
    }
    if (stable_len != 0U) {
        minimize_coverage(run, input, stable, stable_len);
        yew_cov_reset();
        if (!checked(run, input, why)) {
            (void)fprintf(stderr,
                          "%s: coverage minimizer made input fail: %s\n",
                          run->target, why);
            save_crash(run, input);
            status = 1;
            goto done;
        }
        reproduced = yew_cov_hit_all(stable, stable_len);
        if (!reproduced && input->len != original.len) {
            buf_assign(input, original.data, original.len);
            yew_cov_reset();
            if (!checked(run, input, why)) {
                (void)fprintf(stderr,
                              "%s: FAIL seed=%llu iter=%zu on admission "
                              "re-execution: %s\n",
                              run->target, (unsigned long long)run->seed,
                              run->iteration, why);
                save_crash(run, input);
                status = 1;
                goto done;
            }
            reproduced = yew_cov_hit_all(stable, stable_len);
        }
    }
    if (reproduced) {
        run->admitted_edges += yew_cov_merge_ids(stable, stable_len);
        run->unstable_edges += yew_cov_new_edges();
        yew_cov_merge();
    }
    run->unstable_edges += yew_cov_merge_ids(first, first_len);
    if (reproduced && run->admit_dir != NULL) {
        if (!save_admission(run, input, &created)) {
            status = 2;
            goto done;
        }
        if (created)
            run->admitted++;
    }

done:
    free(original.data);
    free(stable);
    free(first);
    return status;
}
#endif

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

static bool replay_corpus(FuzzRun *run)
{
    size_t i;

    for (i = 0U; i < run->corpus.len; i++) {
        const CorpusEntry *entry = &run->corpus.entries[i];
        char why[YEW_FUZZ_WHY_CAP] = {0};

        run->iteration = i;
#if YEW_COV
        yew_cov_reset();
#endif
        if (!checked(run, &entry->bytes, why)) {
            (void)fprintf(stderr, "%s: FAIL corpus=%s: %s\n",
                          run->target, entry->name, why);
            return false;
        }
#if YEW_COV
        yew_cov_merge();
#endif
    }
    return true;
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
    static const u8 ascii[] = "yew\n";
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

static void fuzz_run_free(FuzzRun *run)
{
    corpus_free(&run->corpus);
    corpus_free(&run->dictionary);
}

int yew_fuzz_main(int argc, char **argv, const char *target,
                  const char *corpus_dir, YewFuzzCheck check)
{
    FuzzRun run;
    struct sigaction action;
    size_t i;

    (void)memset(&run, 0, sizeof(run));
    run.seed = 1U;
    run.iterations = YEW_FUZZ_DEFAULT_ITERS;
    run.watchdog_seconds = YEW_FUZZ_DEFAULT_WATCHDOG_SECONDS;
    run.target = target;
    run.check = check;
    if (snprintf(run.guided_dir, sizeof(run.guided_dir),
                 "tests/fuzz/corpus/%s", target) >=
        (int)sizeof(run.guided_dir)) {
        (void)fprintf(stderr, "%s: guided corpus path is too long\n", target);
        return 2;
    }
#if YEW_COV
    run.admit_dir = run.guided_dir;
#endif
    for (i = 1U; i < (size_t)argc; i++) {
        if (parse_u64_option(argv[i], "--seed=", &run.seed))
            continue;
        if (parse_size_option(argv[i], "--iters=", &run.iterations))
            continue;
        if (parse_u64_option(argv[i], "--seconds=", &run.seconds) &&
            run.seconds != 0U)
            continue;
        {
            u64 watchdog_seconds;

            if (parse_u64_option(argv[i], "--watchdog-seconds=",
                                 &watchdog_seconds) &&
                watchdog_seconds != 0U && watchdog_seconds <= UINT_MAX) {
                run.watchdog_seconds = (unsigned int)watchdog_seconds;
                continue;
            }
        }
        if (strcmp(argv[i], "--corpus-only") == 0) {
            run.corpus_only = true;
            continue;
        }
        if (strcmp(argv[i], "--coverage-report") == 0) {
            run.coverage_report = true;
            continue;
        }
        if (strncmp(argv[i], "--admit-dir=", 12U) == 0 && argv[i][12] != '\0') {
            run.admit_dir = argv[i] + 12U;
            continue;
        }
        (void)fprintf(stderr,
                      "usage: %s [--seed=N] [--iters=N] [--seconds=N] "
                      "[--watchdog-seconds=N] [--corpus-only] "
                      "[--coverage-report] [--admit-dir=PATH]\n",
                      argv[0]);
        return 2;
    }
#if !YEW_COV
    if (run.coverage_report || run.admit_dir != NULL) {
        (void)fprintf(stderr,
                      "%s: coverage options require a COV=1 build\n",
                      target);
        return 2;
    }
#else
    if (!sha256_selftest()) {
        (void)fprintf(stderr, "%s: SHA-256 admission self-test failed\n",
                      target);
        return 2;
    }
#endif
    if (!crashes_empty()) {
        (void)fprintf(stderr,
                      "%s: tests/fuzz/crashes contains a crashing input\n",
                      target);
        return 1;
    }
    add_builtin_corpus(&run.corpus);
    if (corpus_dir != NULL && !load_dir(&run.corpus, corpus_dir)) {
        fuzz_run_free(&run);
        return 2;
    }
    if (!load_optional_dir(&run.corpus, run.guided_dir) ||
        (run.admit_dir != NULL && run.admit_dir != run.guided_dir &&
         strcmp(run.admit_dir, run.guided_dir) != 0 &&
         !load_optional_dir(&run.corpus, run.admit_dir))) {
        fuzz_run_free(&run);
        return 2;
    }
    if (!load_dictionary(&run.dictionary, target)) {
        fuzz_run_free(&run);
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
            fuzz_run_free(&run);
            return 2;
        }
        span = run.seconds * 1000U;
        run.deadline_ms = now > UINT64_MAX - span ? UINT64_MAX : now + span;
        run.iterations = SIZE_MAX;
    }
    if (snprintf(watchdog_path, sizeof(watchdog_path),
                 "tests/fuzz/crashes/%s-seed-%llu-watchdog.bin", target,
                 (unsigned long long)run.seed) < (int)sizeof(watchdog_path))
        watchdog_path_len = strlen(watchdog_path);
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = watchdog;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) {
        (void)fprintf(stderr, "%s: sigaction: %s\n", target,
                      strerror(errno));
        fuzz_run_free(&run);
        return 2;
    }
#if YEW_COV
    if (!replay_corpus(&run)) {
        fuzz_run_free(&run);
        return 1;
    }
#endif
    if (run.corpus_only) {
#if !YEW_COV
        if (!replay_corpus(&run)) {
            fuzz_run_free(&run);
            return 1;
        }
#endif
        (void)printf("%s: corpus=%zu exact replay ok\n", target,
                     run.corpus.len);
#if YEW_COV
        if (run.coverage_report) {
            (void)printf("%s: ", target);
            yew_cov_report(stdout);
            (void)printf(" corpus=%zu admitted=0 new_edges=0\n",
                         run.corpus.len);
        }
#endif
        fuzz_run_free(&run);
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
        char why[YEW_FUZZ_WHY_CAP] = {0};

        if (run.seconds != 0U && run.iteration != 0U &&
            monotonic_ms() >= run.deadline_ms)
            break;

        buf_assign(&input, seed->bytes.data, seed->bytes.len);
        for (m = 0U; m < mutations; m++)
            op = mutate(&run, &input);
        hash_bytes(&run, &input);
#if YEW_COV
        yew_cov_reset();
#endif
        if (!checked(&run, &input, why)) {
            (void)fprintf(stderr,
                          "%s: FAIL seed=%llu iter=%zu corpus=%s op=%s: %s\n",
                          target, (unsigned long long)run.seed,
                          run.iteration, seed->name, op, why);
            minimize(&run, &input);
            save_crash(&run, &input);
            free(input.data);
            fuzz_run_free(&run);
            return 1;
        }
#if YEW_COV
        {
            u32 new_edges = yew_cov_new_edges();

            if (new_edges != 0U) {
                int status = admit_novel(&run, &input, new_edges);

                if (status != 0) {
                    free(input.data);
                    fuzz_run_free(&run);
                    return status;
                }
            }
        }
#endif
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
#if YEW_COV
    if (run.coverage_report) {
        (void)printf("%s: ", target);
        yew_cov_report(stdout);
        (void)printf(" corpus=%zu admitted=%zu new_edges=%llu "
                     "unstable=%llu\n",
                     run.corpus.len, run.admitted,
                     (unsigned long long)run.admitted_edges,
                     (unsigned long long)run.unstable_edges);
    }
#endif
    fuzz_run_free(&run);
    return 0;
}
