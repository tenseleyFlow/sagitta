#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint64_t u64;

enum { OUT_CAP = 64 * 1024 };

#define FNV64_OFFSET UINT64_C(14695981039346656037)
#define FNV64_PRIME UINT64_C(1099511628211)

typedef enum {
    PROFILE_CODE,
    PROFILE_UTF8,
    PROFILE_ALLNL,
    PROFILE_LONGLINES,
    PROFILE_NOLINE
} ProfileKind;

typedef struct {
    FILE *file;
    u8 bytes[OUT_CAP];
    size_t len;
    u64 written;
    u64 limit;
    u64 hash;
    bool failed;
} Output;

typedef struct {
    const char *name;
    ProfileKind kind;
} Profile;

static const Profile profiles[] = {
    {"100m-code", PROFILE_CODE},
    {"100m-utf8", PROFILE_UTF8},
    {"100m-allnl", PROFILE_ALLNL},
    {"1g-code", PROFILE_CODE},
    {"1g-longlines", PROFILE_LONGLINES},
    {"1g-noline", PROFILE_NOLINE}
};

static void usage(FILE *out)
{
    (void)fprintf(out,
        "usage:\n"
        "  gen-bigfile --profile NAME --size BYTES --seed HEX --output PATH\n"
        "  gen-bigfile --hash --profile NAME --size BYTES --seed HEX\n"
        "  gen-bigfile --verify PATH EXPECTED_FNV64\n");
}

static bool parse_u64(const char *text, u64 *out)
{
    char *end;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || text[0] == '-')
        return false;
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || *end != '\0')
        return false;
    *out = (u64)value;
    return true;
}

static const Profile *find_profile(const char *name)
{
    size_t i;

    for (i = 0U; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (strcmp(name, profiles[i].name) == 0)
            return &profiles[i];
    }
    return NULL;
}

/* Xorshift64* has a nonzero-state requirement. Keep seed zero reproducible. */
static u64 random_next(u64 *state)
{
    u64 x = *state;

    if (x == 0U)
        x = UINT64_C(0x9e3779b97f4a7c15);
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void output_flush(Output *out)
{
    size_t at = 0U;

    if (out->failed || out->len == 0U)
        return;
    if (out->file != NULL) {
        while (at < out->len) {
            size_t n = fwrite(out->bytes + at, 1U, out->len - at,
                              out->file);

            if (n == 0U) {
                out->failed = true;
                return;
            }
            at += n;
        }
    }
    out->len = 0U;
}

static void output_byte(Output *out, u8 byte)
{
    if (out->failed || out->written == out->limit)
        return;
    out->bytes[out->len++] = byte;
    out->hash ^= byte;
    out->hash *= FNV64_PRIME;
    out->written++;
    if (out->len == sizeof(out->bytes))
        output_flush(out);
}

static void output_bytes(Output *out, const u8 *bytes, size_t len)
{
    size_t i;

    for (i = 0U; i < len && out->written < out->limit; i++)
        output_byte(out, bytes[i]);
}

static void generate_code(Output *out, u64 *rng)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+-*/";

    while (out->written < out->limit) {
        u64 value = random_next(rng);
        u64 line_len = 48U + value % 33U;
        u64 indent = (value >> 8) % 5U;
        u64 col;

        for (col = 0U; col < line_len && out->written < out->limit; col++) {
            u8 byte;

            if (col < indent * 4U)
                byte = ' ';
            else if (col == indent * 4U)
                byte = (u8)"ifreturnstructstaticvoid"[(value >> 16) % 24U];
            else
                byte = (u8)alphabet[random_next(rng) %
                                     (sizeof(alphabet) - 1U)];
            output_byte(out, byte);
        }
        output_byte(out, '\n');
    }
}

static void generate_utf8(Output *out, u64 *rng)
{
    static const u8 ascii[] = " etaoinshrdlucmfwypvbgkqjxz0123456789";
    static const u8 two[][2] = {
        {0xC3U, 0xA9U}, {0xC3U, 0xB1U}, {0xCEU, 0xBBU}
    };
    static const u8 three[][3] = {
        {0xE4U, 0xB8U, 0xADU}, {0xE6U, 0x96U, 0x87U},
        {0xE2U, 0x98U, 0x83U}
    };
    static const u8 clusters[][16] = {
        {0xF0U, 0x9FU, 0x87U, 0xBAU, 0xF0U, 0x9FU, 0x87U, 0xB8U},
        {0xF0U, 0x9FU, 0x91U, 0xA9U, 0xE2U, 0x80U, 0x8DU,
         0xF0U, 0x9FU, 0x92U, 0xBBU},
        {0x31U, 0xEFU, 0xB8U, 0x8FU, 0xE2U, 0x83U, 0xA3U}
    };
    static const u8 cluster_lens[] = {8U, 11U, 7U};
    static const u8 invalid[][3] = {
        {0x80U, 0U, 0U}, {0xC0U, 0x80U, 0U},
        {0xE2U, 0x82U, 0U}, {0xFFU, 0U, 0U}
    };
    static const u8 invalid_lens[] = {1U, 2U, 2U, 1U};
    u64 col = 0U;

    while (out->written < out->limit) {
        u64 value = random_next(rng);
        u64 pick = value % 100U;

        if (col >= 48U + ((value >> 16) % 33U)) {
            if ((value & 3U) == 0U)
                output_byte(out, '\r');
            output_byte(out, '\n');
            col = 0U;
        } else if (pick == 0U) {
            size_t which = (size_t)((value >> 8) % 4U);

            output_bytes(out, invalid[which], invalid_lens[which]);
            col++;
        } else if (pick < 8U) {
            size_t which = (size_t)((value >> 8) % 3U);

            output_bytes(out, clusters[which], cluster_lens[which]);
            col++;
        } else if (pick < 20U) {
            size_t which = (size_t)((value >> 8) % 3U);

            output_bytes(out, three[which], sizeof(three[which]));
            col++;
        } else if (pick < 30U) {
            size_t which = (size_t)((value >> 8) % 3U);

            output_bytes(out, two[which], sizeof(two[which]));
            col++;
        } else {
            output_byte(out, ascii[(value >> 8) % (sizeof(ascii) - 1U)]);
            col++;
        }
    }
}

static void generate_repeat(Output *out, u64 *rng, ProfileKind kind)
{
    u64 line_at = 0U;

    while (out->written < out->limit) {
        u8 byte;

        if (kind == PROFILE_ALLNL) {
            byte = '\n';
        } else if (kind == PROFILE_LONGLINES &&
                   line_at == UINT64_C(256) * 1024U - 1U) {
            byte = '\n';
            line_at = 0U;
        } else {
            byte = (u8)('!' + random_next(rng) % 94U);
            line_at++;
        }
        output_byte(out, byte);
    }
}

static bool generate(const Profile *profile, u64 size, u64 seed, FILE *file,
                     u64 *hash)
{
    Output out;
    u64 rng = seed;

    (void)memset(&out, 0, sizeof(out));
    out.file = file;
    out.limit = size;
    out.hash = FNV64_OFFSET;
    if (profile->kind == PROFILE_CODE)
        generate_code(&out, &rng);
    else if (profile->kind == PROFILE_UTF8)
        generate_utf8(&out, &rng);
    else
        generate_repeat(&out, &rng, profile->kind);
    output_flush(&out);
    *hash = out.hash;
    return !out.failed && out.written == size;
}

static bool hash_file(const char *path, u64 *hash)
{
    u8 buf[OUT_CAP];
    FILE *file = fopen(path, "rb");
    u64 value = FNV64_OFFSET;
    bool ok = true;

    if (file == NULL)
        return false;
    for (;;) {
        size_t n = fread(buf, 1U, sizeof(buf), file);
        size_t i;

        for (i = 0U; i < n; i++) {
            value ^= buf[i];
            value *= FNV64_PRIME;
        }
        if (n != sizeof(buf)) {
            if (ferror(file))
                ok = false;
            break;
        }
    }
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
        return false;
    *hash = value;
    return true;
}

int main(int argc, char **argv)
{
    const char *profile_name = NULL;
    const char *output_path = NULL;
    const char *verify_path = NULL;
    const char *expected_text = NULL;
    const Profile *profile;
    u64 size = 0U;
    u64 seed = 0U;
    u64 hash;
    bool have_size = false;
    bool have_seed = false;
    bool hash_only = false;
    int i;

    if (argc == 4 && strcmp(argv[1], "--verify") == 0) {
        verify_path = argv[2];
        expected_text = argv[3];
    } else {
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--hash") == 0) {
                hash_only = true;
            } else if (i + 1 < argc && strcmp(argv[i], "--profile") == 0) {
                profile_name = argv[++i];
            } else if (i + 1 < argc && strcmp(argv[i], "--size") == 0) {
                have_size = parse_u64(argv[++i], &size);
            } else if (i + 1 < argc && strcmp(argv[i], "--seed") == 0) {
                have_seed = parse_u64(argv[++i], &seed);
            } else if (i + 1 < argc && strcmp(argv[i], "--output") == 0) {
                output_path = argv[++i];
            } else {
                usage(stderr);
                return 2;
            }
        }
    }
    if (verify_path != NULL) {
        u64 expected;

        if (!parse_u64(expected_text, &expected)) {
            (void)fprintf(stderr, "gen-bigfile: invalid FNV-64 '%s'\n",
                          expected_text);
            return 2;
        }
        if (!hash_file(verify_path, &hash)) {
            (void)fprintf(stderr, "gen-bigfile: cannot hash %s: %s\n",
                          verify_path, strerror(errno));
            return 2;
        }
        (void)printf("%016llx  %s%s\n", (unsigned long long)hash,
                     verify_path, hash == expected ? "" : " MISMATCH");
        return hash == expected ? 0 : 1;
    }
    profile = profile_name == NULL ? NULL : find_profile(profile_name);
    if (profile == NULL || !have_size || !have_seed ||
        (hash_only == (output_path != NULL))) {
        usage(stderr);
        return 2;
    }
    if (hash_only) {
        if (!generate(profile, size, seed, NULL, &hash))
            return 2;
        (void)printf("%016llx\n", (unsigned long long)hash);
        return 0;
    }
    {
        FILE *file = fopen(output_path, "wb");
        bool ok;

        if (file == NULL) {
            (void)fprintf(stderr, "gen-bigfile: cannot create %s: %s\n",
                          output_path, strerror(errno));
            return 2;
        }
        ok = generate(profile, size, seed, file, &hash);
        if (fclose(file) != 0)
            ok = false;
        if (!ok) {
            (void)fprintf(stderr, "gen-bigfile: write %s failed: %s\n",
                          output_path, strerror(errno));
            return 2;
        }
    }
    (void)printf("fixture profile=%s size=%llu seed=0x%016llx "
                 "fnv64=%016llx path=%s\n", profile->name,
                 (unsigned long long)size, (unsigned long long)seed,
                 (unsigned long long)hash, output_path);
    return 0;
}
