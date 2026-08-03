/* Generate Sagitta's Unicode 16.0.0 property trie from checked-in UCD data. */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_COUNT 0x110000u
#define TRIE_HI 0x30000u
#define SHIFT 7u
#define BLOCK_SIZE (1u << SHIFT)
#define STAGE1_LEN (TRIE_HI >> SHIFT)
#define MAX_BLOCKS STAGE1_LEN
#define MAX_PALETTE 256u
#define MAX_HI_RANGES 128u

#define WB_MASK 0x1Fu
#define WB_WHITE_SPACE 0x20u

#define GCB_MASK 0x000Fu
#define INCB_MASK 0x0030u
#define EXT_PICT 0x0040u
#define EAW_MASK 0x0380u
#define ZERO_WIDTH 0x0400u
#define EMOJI 0x0800u
#define EMOJI_PRESENTATION 0x1000u

typedef uint8_t U8;
typedef uint16_t U16;
typedef uint32_t U32;

typedef struct {
    U32 lo;
    U32 hi;
    U16 rec;
} Range;

static U16 *records;
static U16 stage1[STAGE1_LEN];
static U8 stage2[MAX_BLOCKS * BLOCK_SIZE];
static U16 palette[MAX_PALETTE];
static Range hi_ranges[MAX_HI_RANGES];
static size_t block_count;
static size_t palette_count;
static size_t hi_count;

static U8 *wb_records;
static U16 wb_stage1[STAGE1_LEN];
static U8 wb_stage2[MAX_BLOCKS * BLOCK_SIZE];
static Range wb_hi_ranges[MAX_HI_RANGES];
static size_t wb_block_count;
static size_t wb_hi_count;

static void die(const char *what, const char *path)
{
    if (path != NULL)
        fprintf(stderr, "gen-unicode-tables: %s: %s\n", what, path);
    else
        fprintf(stderr, "gen-unicode-tables: %s\n", what);
    exit(1);
}

static FILE *open_input(const char *dir, const char *name, char *path,
                        size_t path_size)
{
    FILE *fp;
    int n = snprintf(path, path_size, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= path_size)
        die("input path is too long", name);
    fp = fopen(path, "r");
    if (fp == NULL)
        die(strerror(errno), path);
    return fp;
}

static char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static U32 hex_cp(const char *s, const char *path, unsigned long line_no)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(s, &end, 16);
    while (isspace((unsigned char)*end))
        end++;
    if (errno != 0 || end == s || *end != '\0' || value >= CP_COUNT) {
        fprintf(stderr, "gen-unicode-tables: %s:%lu: bad codepoint '%s'\n",
                path, line_no, s);
        exit(1);
    }
    return (U32)value;
}

static void parse_range(char *field, U32 *lo, U32 *hi, const char *path,
                        unsigned long line_no)
{
    char *dots;
    field = trim(field);
    dots = strstr(field, "..");
    if (dots == NULL) {
        *lo = hex_cp(field, path, line_no);
        *hi = *lo;
        return;
    }
    *dots = '\0';
    *lo = hex_cp(trim(field), path, line_no);
    *hi = hex_cp(trim(dots + 2), path, line_no);
    if (*hi < *lo) {
        fprintf(stderr, "gen-unicode-tables: %s:%lu: reversed range\n",
                path, line_no);
        exit(1);
    }
}

static void set_field(U32 lo, U32 hi, U16 mask, U16 value)
{
    U32 cp;
    for (cp = lo; cp <= hi; cp++)
        records[cp] = (U16)((records[cp] & (U16)~mask) | value);
}

static int gcb_value(const char *name)
{
    static const char *const names[] = {
        "Other", "CR", "LF", "Control", "Extend", "ZWJ",
        "Regional_Indicator", "Prepend", "SpacingMark", "L", "V",
        "T", "LV", "LVT"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0)
            return (int)i;
    return -1;
}

static int eaw_value(const char *name)
{
    static const char *const names[] = {"N", "Na", "A", "W", "F", "H"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0)
            return (int)i;
    return -1;
}

static int incb_value(const char *name)
{
    static const char *const names[] = {"None", "Linker", "Consonant", "Extend"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0)
            return (int)i;
    return -1;
}

static int wb_value(const char *name)
{
    static const char *const names[] = {
        "Other", "CR", "LF", "Newline", "Extend", "Format", "ZWJ",
        "WSegSpace", "ALetter", "Hebrew_Letter", "Numeric", "Katakana",
        "ExtendNumLet", "Regional_Indicator", "MidLetter", "MidNum",
        "MidNumLet", "Single_Quote", "Double_Quote"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0)
            return (int)i;
    return -1;
}

static void wb_set_field(U32 lo, U32 hi, U8 mask, U8 value)
{
    U32 cp;
    for (cp = lo; cp <= hi; cp++)
        wb_records[cp] = (U8)((wb_records[cp] & (U8)~mask) | value);
}

static void parse_wb_property_file(const char *dir, const char *name,
                                   int white_space)
{
    char path[4096];
    char line[4096];
    unsigned long line_no = 0;
    FILE *fp = open_input(dir, name, path, sizeof(path));

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *hash;
        char *missing;
        char *semi;
        char *range_field;
        char *property;
        U32 lo, hi;
        int value;
        line_no++;
        if (strchr(line, '\n') == NULL && !feof(fp))
            die("input line exceeds parser buffer", path);
        missing = strstr(line, "@missing:");
        if (missing != NULL) {
            memmove(line, missing + strlen("@missing:"),
                    strlen(missing + strlen("@missing:")) + 1u);
        }
        hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        range_field = trim(line);
        if (*range_field == '\0')
            continue;
        semi = strchr(range_field, ';');
        if (semi == NULL)
            die("property line has no semicolon", path);
        *semi = '\0';
        property = trim(semi + 1);
        semi = strchr(property, ';');
        if (semi != NULL)
            *semi = '\0';
        property = trim(property);
        parse_range(range_field, &lo, &hi, path, line_no);

        if (white_space) {
            if (strcmp(property, "White_Space") == 0)
                wb_set_field(lo, hi, WB_WHITE_SPACE, WB_WHITE_SPACE);
        } else {
            value = wb_value(property);
            if (value < 0)
                die("unknown Word_Break value", property);
            wb_set_field(lo, hi, WB_MASK, (U8)value);
        }
    }
    if (ferror(fp))
        die(strerror(errno), path);
    if (fclose(fp) != 0)
        die(strerror(errno), path);
}

static void parse_property_file(const char *dir, const char *name, int kind)
{
    char path[4096];
    char line[4096];
    unsigned long line_no = 0;
    FILE *fp = open_input(dir, name, path, sizeof(path));

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *hash;
        char *missing;
        char *semi;
        char *range_field;
        char *property;
        char *third = NULL;
        U32 lo, hi;
        int value;
        line_no++;
        if (strchr(line, '\n') == NULL && !feof(fp))
            die("input line exceeds parser buffer", path);
        missing = strstr(line, "@missing:");
        if (missing != NULL) {
            /* Parse defaults through the same whole-field path as data. */
            memmove(line, missing + strlen("@missing:"),
                    strlen(missing + strlen("@missing:")) + 1u);
        }
        hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        range_field = trim(line);
        if (*range_field == '\0')
            continue;
        semi = strchr(range_field, ';');
        if (semi == NULL)
            die("property line has no semicolon", path);
        *semi = '\0';
        property = trim(semi + 1);
        semi = strchr(property, ';');
        if (semi != NULL) {
            *semi = '\0';
            third = trim(semi + 1);
        }
        property = trim(property);
        parse_range(range_field, &lo, &hi, path, line_no);

        if (kind == 0) {
            value = gcb_value(property);
            if (value < 0)
                die("unknown Grapheme_Cluster_Break value", property);
            set_field(lo, hi, GCB_MASK, (U16)value);
        } else if (kind == 1) {
            value = eaw_value(property);
            if (value < 0)
                die("unknown East_Asian_Width value", property);
            set_field(lo, hi, EAW_MASK, (U16)((unsigned)value << 7));
        } else if (kind == 2) {
            U16 bit;
            if (strcmp(property, "Emoji") == 0)
                bit = EMOJI;
            else if (strcmp(property, "Emoji_Presentation") == 0)
                bit = EMOJI_PRESENTATION;
            else if (strcmp(property, "Extended_Pictographic") == 0)
                bit = EXT_PICT;
            else
                continue;
            set_field(lo, hi, bit, bit);
        } else {
            if (strcmp(property, "InCB") != 0)
                continue;
            if (third == NULL)
                die("InCB line has no value", path);
            third = trim(third);
            value = incb_value(third);
            if (value < 0)
                die("unknown Indic_Conjunct_Break value", third);
            set_field(lo, hi, INCB_MASK, (U16)((unsigned)value << 4));
        }
    }
    if (ferror(fp))
        die(strerror(errno), path);
    if (fclose(fp) != 0)
        die(strerror(errno), path);
}

static void set_zero_width(U32 lo, U32 hi, const char *category)
{
    if (strcmp(category, "Mn") == 0 || strcmp(category, "Me") == 0 ||
        strcmp(category, "Cf") == 0)
        set_field(lo, hi, ZERO_WIDTH, ZERO_WIDTH);
}

static void parse_unicode_data(const char *dir)
{
    char path[4096];
    char line[4096];
    char pending_category[8] = "";
    U32 pending_lo = 0;
    unsigned long line_no = 0;
    int have_pending = 0;
    FILE *fp = open_input(dir, "UnicodeData.txt", path, sizeof(path));

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *fields[3];
        char *p = line;
        char *semi;
        U32 cp;
        int i;
        line_no++;
        for (i = 0; i < 3; i++) {
            fields[i] = p;
            semi = strchr(p, ';');
            if (semi == NULL)
                die("UnicodeData line has too few fields", path);
            *semi = '\0';
            p = semi + 1;
        }
        cp = hex_cp(fields[0], path, line_no);
        if (strstr(fields[1], ", First>") != NULL) {
            if (have_pending)
                die("nested UnicodeData First range", path);
            pending_lo = cp;
            if (strlen(fields[2]) >= sizeof(pending_category))
                die("General_Category is too long", path);
            strcpy(pending_category, fields[2]);
            have_pending = 1;
        } else if (strstr(fields[1], ", Last>") != NULL) {
            if (!have_pending || cp < pending_lo ||
                strcmp(fields[2], pending_category) != 0)
                die("malformed UnicodeData First/Last pair", path);
            set_zero_width(pending_lo, cp, pending_category);
            have_pending = 0;
        } else {
            if (have_pending)
                die("UnicodeData First not followed by Last", path);
            set_zero_width(cp, cp, fields[2]);
        }
    }
    if (have_pending)
        die("unterminated UnicodeData First range", path);
    if (ferror(fp))
        die(strerror(errno), path);
    if (fclose(fp) != 0)
        die(strerror(errno), path);
}

static U8 palette_index(U16 rec)
{
    size_t i;
    for (i = 0; i < palette_count; i++)
        if (palette[i] == rec)
            return (U8)i;
    if (palette_count == MAX_PALETTE)
        die("property palette exceeds 256 records", NULL);
    palette[palette_count] = rec;
    return (U8)palette_count++;
}

static void build_trie(void)
{
    U8 block[BLOCK_SIZE];
    size_t input_block;
    for (input_block = 0; input_block < STAGE1_LEN; input_block++) {
        size_t i;
        size_t found = block_count;
        for (i = 0; i < BLOCK_SIZE; i++)
            block[i] = palette_index(records[input_block * BLOCK_SIZE + i]);
        for (i = 0; i < block_count; i++) {
            if (memcmp(stage2 + i * BLOCK_SIZE, block, BLOCK_SIZE) == 0) {
                found = i;
                break;
            }
        }
        if (found == block_count) {
            if (block_count == MAX_BLOCKS)
                die("stage-2 block limit exceeded", NULL);
            memcpy(stage2 + block_count * BLOCK_SIZE, block, BLOCK_SIZE);
            block_count++;
        }
        stage1[input_block] = (U16)found;
    }
}

static void build_high_ranges(void)
{
    U32 cp = TRIE_HI;
    while (cp < CP_COUNT) {
        U32 lo;
        U16 rec;
        if (records[cp] == 0) {
            cp++;
            continue;
        }
        lo = cp;
        rec = records[cp++];
        while (cp < CP_COUNT && records[cp] == rec)
            cp++;
        if (hi_count == MAX_HI_RANGES)
            die("high-plane range limit exceeded", NULL);
        hi_ranges[hi_count].lo = lo;
        hi_ranges[hi_count].hi = cp - 1u;
        hi_ranges[hi_count].rec = rec;
        hi_count++;
    }
}

static void validate_records(void)
{
    U32 cp;
    const U16 used_bits = GCB_MASK | INCB_MASK | EXT_PICT | EAW_MASK |
                          ZERO_WIDTH | EMOJI | EMOJI_PRESENTATION;
    for (cp = 0; cp < CP_COUNT; cp++)
        if ((records[cp] & (U16)~used_bits) != 0)
            die("reserved packed-record bits are not zero", NULL);

    if ((records[0x0041u] & EAW_MASK) != (1u << 7) ||
        (records[0x0301u] & (GCB_MASK | INCB_MASK | ZERO_WIDTH)) !=
            (4u | (3u << 4) | ZERO_WIDTH) ||
        (records[0x094Du] & INCB_MASK) != (1u << 4) ||
        (records[0x1160u] & ZERO_WIDTH) == 0 ||
        (records[0x11FFu] & ZERO_WIDTH) == 0 ||
        (records[0x200Du] & GCB_MASK) != 5u ||
        (records[0x4E00u] & EAW_MASK) != (3u << 7) ||
        (records[0xAC00u] & GCB_MASK) != 12u ||
        (records[0xAC01u] & GCB_MASK) != 13u ||
        (records[0x1F600u] &
         (EXT_PICT | EAW_MASK | EMOJI | EMOJI_PRESENTATION)) !=
            (EXT_PICT | (3u << 7) | EMOJI | EMOJI_PRESENTATION))
        die("Unicode 16.0.0 semantic spot check failed", NULL);
}

static void build_wb_trie(void)
{
    size_t input_block;
    for (input_block = 0; input_block < STAGE1_LEN; input_block++) {
        const U8 *block = wb_records + input_block * BLOCK_SIZE;
        size_t i;
        size_t found = wb_block_count;
        for (i = 0; i < wb_block_count; i++) {
            if (memcmp(wb_stage2 + i * BLOCK_SIZE, block, BLOCK_SIZE) == 0) {
                found = i;
                break;
            }
        }
        if (found == wb_block_count) {
            if (wb_block_count == MAX_BLOCKS)
                die("word-break stage-2 block limit exceeded", NULL);
            memcpy(wb_stage2 + wb_block_count * BLOCK_SIZE, block,
                   BLOCK_SIZE);
            wb_block_count++;
        }
        wb_stage1[input_block] = (U16)found;
    }
}

static void build_wb_high_ranges(void)
{
    U32 cp = TRIE_HI;
    while (cp < CP_COUNT) {
        U32 lo;
        U8 rec;
        if (wb_records[cp] == 0) {
            cp++;
            continue;
        }
        lo = cp;
        rec = wb_records[cp++];
        while (cp < CP_COUNT && wb_records[cp] == rec)
            cp++;
        if (wb_hi_count == MAX_HI_RANGES)
            die("word-break high-plane range limit exceeded", NULL);
        wb_hi_ranges[wb_hi_count].lo = lo;
        wb_hi_ranges[wb_hi_count].hi = cp - 1u;
        wb_hi_ranges[wb_hi_count].rec = rec;
        wb_hi_count++;
    }
}

static void validate_wb_records(void)
{
    U32 cp;
    for (cp = 0; cp < CP_COUNT; cp++) {
        if ((wb_records[cp] & (U8)~(WB_MASK | WB_WHITE_SPACE)) != 0)
            die("word-break packed-record bits are invalid", NULL);
        if ((wb_records[cp] & WB_MASK) > 18u)
            die("word-break packed-record value is invalid", NULL);
    }
    if (wb_records[0x000Au] != (U8)(2u | WB_WHITE_SPACE) ||
        wb_records[0x0020u] != (U8)(7u | WB_WHITE_SPACE) ||
        wb_records[0x0041u] != 8u ||
        wb_records[0x0301u] != 4u ||
        wb_records[0x05D0u] != 9u ||
        wb_records[0x30A2u] != 11u ||
        wb_records[0x1F1E6u] != 13u)
        die("Unicode 16.0.0 word-break semantic spot check failed", NULL);
}

static void emit_u16(const char *decl, const U16 *values, size_t count)
{
    size_t i;
    printf("const u16 %s = {\n", decl);
    for (i = 0; i < count; i++) {
        if ((i % 8u) == 0)
            fputs("    ", stdout);
        printf("0x%04X", (unsigned)values[i]);
        if (i + 1u != count) {
            putchar(',');
            if ((i % 8u) != 7u)
                putchar(' ');
        }
        if ((i % 8u) == 7u || i + 1u == count)
            putchar('\n');
    }
    fputs("};\n\n", stdout);
}

static void emit_u8(const char *decl, const U8 *values, size_t count)
{
    size_t i;
    printf("const u8 %s = {\n", decl);
    for (i = 0; i < count; i++) {
        if ((i % 16u) == 0)
            fputs("    ", stdout);
        printf("%uu", (unsigned)values[i]);
        if (i + 1u != count) {
            putchar(',');
            if ((i % 16u) != 15u)
                putchar(' ');
        }
        if ((i % 16u) == 15u || i + 1u == count)
            putchar('\n');
    }
    fputs("};\n\n", stdout);
}

static void emit_output(void)
{
    static const char *const hashes[] = {
        "DerivedCoreProperties.txt 39d35161f2954497f69e08bdb9e701493f476a3d30222de20028feda36c1dabd",
        "EastAsianWidth.txt 43adc76c0686a42cb370764eb8cfe2b2a45b10b855e5572a2db4a0eecce15d5b",
        "GraphemeBreakProperty.txt c29360bd6f7132811d701d29069541e827eb44bfc4c8fbde8c370d6982689dc1",
        "GraphemeBreakTest.txt ee2b9354d270ac061b29f09662cafea06341d77e704b8cc6bd72aaeeda363cb5",
        "UnicodeData.txt ff58e5823bd095166564a006e47d111130813dcf8bf234ef79fa51a870edb48f",
        "emoji-data.txt f1365a5173eee18e1f98b240cdc492e84a25f1ce7e0c9d1094eb29c41a22696a"
    };
    size_t i;
    /* The emitted SagURange ABI is three naturally aligned 32/32/16-bit
     * fields and the sprint contract budgets each entry as 12 bytes. Keep
     * the generated report independent of this generator host's padding. */
    size_t table_bytes = STAGE1_LEN * 2u + block_count * BLOCK_SIZE +
                         palette_count * 2u + hi_count * 12u;
    unsigned dedup = (unsigned)(((STAGE1_LEN - block_count) * 100u) /
                                STAGE1_LEN);

    fputs("/* GENERATED by scripts/gen-unicode-tables from UCD 16.0.0 — do not edit;\n"
          " * regenerate with 'make unicode-tables'.\n"
          " */\n#include \"tables.h\"\n\n", stdout);
    emit_u16("sag_u_stage1[SAG_TRIE_HI >> SAG_TRIE_SHIFT]", stage1,
             STAGE1_LEN);
    emit_u8("sag_u_stage2[]", stage2, block_count * BLOCK_SIZE);
    emit_u16("sag_u_pal[]", palette, palette_count);
    fputs("const struct SagURange sag_u_hi[] = {\n", stdout);
    for (i = 0; i < hi_count; i++)
        printf("    {0x%06Xu, 0x%06Xu, 0x%04Xu}%s\n",
               (unsigned)hi_ranges[i].lo, (unsigned)hi_ranges[i].hi,
               (unsigned)hi_ranges[i].rec, i + 1u == hi_count ? "" : ",");
    fputs("};\n\n", stdout);
    printf("const u32 sag_u_hi_len = %uu;\n\n", (unsigned)hi_count);
    fputs("u16 sag_u_hi_lookup(u32 cp)\n{\n"
          "    size_t lo = 0;\n"
          "    size_t hi = sag_u_hi_len;\n"
          "    while (lo < hi) {\n"
          "        size_t mid = lo + (hi - lo) / 2u;\n"
          "        if (cp < sag_u_hi[mid].lo)\n"
          "            hi = mid;\n"
          "        else if (cp > sag_u_hi[mid].hi)\n"
          "            lo = mid + 1u;\n"
          "        else\n"
          "            return sag_u_hi[mid].rec;\n"
          "    }\n"
          "    return 0;\n"
          "}\n\n", stdout);
    fputs("_Static_assert(sizeof(sag_u_stage1) + sizeof(sag_u_stage2) +\n"
          "               sizeof(sag_u_pal) + sizeof(sag_u_hi) <= 64u * 1024u,\n"
          "               \"unicode tables exceed the 64 KiB budget (s02 DoD 2)\");\n\n",
          stdout);
    printf("/* UCD 16.0.0; %zu bytes; %zu/%u unique stage-2 blocks; %u%% dedup.\n",
           table_bytes, block_count, (unsigned)STAGE1_LEN, dedup);
    for (i = 0; i < sizeof(hashes) / sizeof(hashes[0]); i++)
        printf(" * sha256 %s\n", hashes[i]);
    fputs(" */\n", stdout);
}

static void emit_wb_output(void)
{
    static const char *const hashes[] = {
        "PropList.txt 53d614508e2a0b2305a8aa21cd60d993de9326cdf65993660dfcce4503548583",
        "WordBreakProperty.txt 476464e71a4b7b779b8ba7c5671f4338fea77da8e6b6b05fb82b3fdd14603779",
        "WordBreakTest.txt ad985d5721f3fa6b45495663dfe44180f2f68976100dee0ea7451ef1a8f838e8"
    };
    size_t i;
    size_t table_bytes = STAGE1_LEN * 2u + wb_block_count * BLOCK_SIZE +
                         wb_hi_count * 12u;
    unsigned dedup = (unsigned)(((STAGE1_LEN - wb_block_count) * 100u) /
                                STAGE1_LEN);

    fputs("/* GENERATED by scripts/gen-unicode-tables from UCD 16.0.0 — do not edit;\n"
          " * regenerate with 'scripts/gen-unicode-tables --word-break ucd/16.0.0'.\n"
          " */\n#include \"wordbreak.h\"\n\n", stdout);
    emit_u16("sag_wb_stage1[SAG_WB_TRIE_HI >> SAG_WB_TRIE_SHIFT]",
             wb_stage1, STAGE1_LEN);
    emit_u8("sag_wb_stage2[]", wb_stage2,
            wb_block_count * BLOCK_SIZE);
    fputs("const struct SagWbRange sag_wb_hi[] = {\n", stdout);
    for (i = 0; i < wb_hi_count; i++)
        printf("    {0x%06Xu, 0x%06Xu, 0x%02Xu}%s\n",
               (unsigned)wb_hi_ranges[i].lo,
               (unsigned)wb_hi_ranges[i].hi,
               (unsigned)wb_hi_ranges[i].rec,
               i + 1u == wb_hi_count ? "" : ",");
    fputs("};\n\n", stdout);
    printf("const u32 sag_wb_hi_len = %uu;\n\n", (unsigned)wb_hi_count);
    fputs("_Static_assert(sizeof(sag_wb_stage1) + sizeof(sag_wb_stage2) +\n"
          "               sizeof(sag_wb_hi) <= 64u * 1024u,\n"
          "               \"word-break tables exceed the 64 KiB budget\");\n\n",
          stdout);
    printf("/* UCD 16.0.0; %zu bytes; %zu/%u unique stage-2 blocks; %u%% dedup.\n",
           table_bytes, wb_block_count, (unsigned)STAGE1_LEN, dedup);
    for (i = 0; i < sizeof(hashes) / sizeof(hashes[0]); i++)
        printf(" * sha256 %s\n", hashes[i]);
    fputs(" */\n", stdout);
}

static void generate_word_break(const char *dir)
{
    wb_records = calloc(CP_COUNT, sizeof(*wb_records));
    if (wb_records == NULL)
        die("out of memory", NULL);
    parse_wb_property_file(dir, "WordBreakProperty.txt", 0);
    parse_wb_property_file(dir, "PropList.txt", 1);
    validate_wb_records();
    build_wb_trie();
    build_wb_high_ranges();
    emit_wb_output();
    free(wb_records);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--word-break") == 0) {
        generate_word_break(argv[2]);
        if (ferror(stdout))
            die("failed writing generated output", NULL);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s [--word-break] UCD-DIRECTORY\n", argv[0]);
        return 1;
    }
    records = calloc(CP_COUNT, sizeof(*records));
    if (records == NULL)
        die("out of memory", NULL);

    parse_unicode_data(argv[1]);
    /* UAX #11 terminal-width policy: conjoining Hangul V/T jamo do not
     * advance independently, regardless of their General_Category. */
    set_field(0x1160u, 0x11FFu, ZERO_WIDTH, ZERO_WIDTH);
    parse_property_file(argv[1], "GraphemeBreakProperty.txt", 0);
    parse_property_file(argv[1], "emoji-data.txt", 2);
    parse_property_file(argv[1], "EastAsianWidth.txt", 1);
    parse_property_file(argv[1], "DerivedCoreProperties.txt", 3);
    validate_records();
    build_trie();
    build_high_ranges();
    if (palette_count > 64u)
        die("property palette exceeds the 64-record budget", NULL);
    if (block_count > 320u)
        die("stage-2 exceeds the 320-block budget", NULL);
    if (hi_count > 24u)
        die("high-plane table exceeds the 24-range budget", NULL);
    emit_output();
    free(records);
    if (ferror(stdout))
        die("failed writing generated output", NULL);
    return 0;
}
