#include "harness.h"

#include "term/render.h"

#include <string.h>

typedef struct Rgb256Vector {
    u8 r, g, b, want;
} Rgb256Vector;

static const char *tier_sag_colors;
static const char *tier_term;

static const char *tier_getenv(const char *name)
{
    if (strcmp(name, "SAG_COLORS") == 0)
        return tier_sag_colors;
    if (strcmp(name, "TERM") == 0)
        return tier_term;
    return NULL;
}

void test_color_rgb_to_256_vectors(void)
{
    static const Rgb256Vector vectors[] = {
        {0u, 0u, 0u, 16u}, {255u, 255u, 255u, 231u},
        {47u, 47u, 47u, 236u}, {48u, 48u, 48u, 236u},
        {114u, 114u, 114u, 243u}, {115u, 115u, 115u, 243u},
        {255u, 0u, 0u, 196u}, {0u, 255u, 0u, 46u},
        {0u, 0u, 255u, 21u}, {128u, 0u, 0u, 88u},
        {0u, 128u, 0u, 28u}, {0u, 0u, 128u, 18u},
        {12u, 34u, 56u, 235u}, {50u, 100u, 150u, 60u},
        {94u, 134u, 174u, 67u}, {95u, 135u, 175u, 67u},
        {96u, 136u, 176u, 67u}, {200u, 100u, 20u, 166u},
        {20u, 100u, 200u, 26u}, {123u, 45u, 67u, 239u}
    };
    size_t i;

    for (i = 0u; i < SAG_ARRAY_LEN(vectors); i++) {
        SAG_ASSERT_EQ_U64(sag_rgb_to_256(vectors[i].r, vectors[i].g,
                                        vectors[i].b), vectors[i].want);
    }
}

void test_color_grayscale_ramp_roundtrips(void)
{
    u8 i;

    for (i = 0u; i < 24u; i++) {
        u8 value = (u8)(8u + 10u * i);

        SAG_ASSERT_EQ_U64(sag_rgb_to_256(value, value, value), 232u + i);
    }
}

void test_color_cube_wins_equal_distance(void)
{
    /* Black is both cube entry 16 and equidistant from the clamped ramp. */
    SAG_ASSERT_EQ_U64(sag_rgb_to_256(0u, 0u, 0u), 16u);
}

void test_color_rgb_to_16_canonical_and_tie(void)
{
    static const u8 ansi[16][3] = {
        {0x00u, 0x00u, 0x00u}, {0x80u, 0x00u, 0x00u},
        {0x00u, 0x80u, 0x00u}, {0x80u, 0x80u, 0x00u},
        {0x00u, 0x00u, 0x80u}, {0x80u, 0x00u, 0x80u},
        {0x00u, 0x80u, 0x80u}, {0xc0u, 0xc0u, 0xc0u},
        {0x80u, 0x80u, 0x80u}, {0xffu, 0x00u, 0x00u},
        {0x00u, 0xffu, 0x00u}, {0xffu, 0xffu, 0x00u},
        {0x00u, 0x00u, 0xffu}, {0xffu, 0x00u, 0xffu},
        {0x00u, 0xffu, 0xffu}, {0xffu, 0xffu, 0xffu}
    };
    u8 i;

    for (i = 0u; i < 16u; i++)
        SAG_ASSERT_EQ_U64(sag_rgb_to_16(ansi[i][0], ansi[i][1], ansi[i][2]),
                          i);
    SAG_ASSERT_EQ_U64(sag_rgb_to_16(0u, 0u, 64u), 0u);
}

void test_color_render_tier_environment(void)
{
    typedef struct TierVector {
        const char *forced;
        const char *term;
        bool cap_truecolor;
        u8 want;
    } TierVector;
    static const TierVector vectors[] = {
        {"truecolor", "dumb", false, 2u},
        {"256", "dumb", true, 1u},
        {"16", "xterm-direct", true, 0u},
        {NULL, "dumb", true, 2u},
        {NULL, "xterm-256color", false, 1u},
        {NULL, "screen-256color", false, 1u},
        {NULL, "xterm-direct", false, 1u},
        {NULL, "vt100", false, 0u},
        {NULL, NULL, false, 0u},
        {"bogus", "xterm-256color", false, 1u},
        {"", "vt100", true, 2u}
    };
    size_t i;

    for (i = 0u; i < SAG_ARRAY_LEN(vectors); i++) {
        TtyCaps caps = {0};

        caps.truecolor = vectors[i].cap_truecolor;
        tier_sag_colors = vectors[i].forced;
        tier_term = vectors[i].term;
        SAG_ASSERT_EQ_U64(sag_render_tier(&caps, tier_getenv), vectors[i].want);
    }
    tier_sag_colors = NULL;
    tier_term = NULL;
}
