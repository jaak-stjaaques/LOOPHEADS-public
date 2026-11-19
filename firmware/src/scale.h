/* SPDX-License-Identifier: GPL-3.0-only */
static const struct { const char *name; uint16_t steps; } scales[] = {
    {"CHROMATIC", 0xfffu},
    {"MAJOR", 0xab5u},
    {"NATURAL MINOR", 0x5adu},
    {"HARMONIC MINOR", 0x9adu},
    {"MELODIC MINOR", 0xaadu},
    {"DORIAN", 0x6adu},
    {"PHRYGIAN", 0x5abu},
    {"LYDIAN", 0xad5u},
    {"MIXOLYDIAN", 0x6b5u},
    {"LOCRIAN", 0x56bu},
    {"MAJOR PENT", 0x295u},
    {"MINOR PENT", 0x4a9u},
    {"BLUES", 0x4e9u},
    {"WHOLE TONE", 0x555u},
    {"DIM HALF-WHOLE", 0x6dbu},
    {"DOUBLE HARMONIC", 0x9b3u},
    {"HUNGARIAN MINOR", 0x9cdu},
    {"HIRAJOSHI", 0x18du},
    {"IN SEN", 0x4a3u},
};
#define SCALE_COUNT (sizeof scales / sizeof scales[0])

static int scale_contains(uint16_t steps, uint8_t root, uint8_t pitch)
{
    return (steps >> ((pitch + 12u - root) % 12u)) & 1u;
}

static uint8_t scale_note(uint8_t scale, uint8_t root, uint8_t pitch)
{
    uint16_t steps = scales[scale].steps;
    if (scale_contains(steps, root, pitch)) return pitch;
    /* At an equal distance, choose the lower note. Stay within the MIDI range. */
    for (uint8_t distance = 1; distance < 12; distance++) {
        if (pitch >= distance && scale_contains(steps, root, pitch - distance))
            return pitch - distance;
        if (pitch + distance <= 127 && scale_contains(steps, root, pitch + distance))
            return pitch + distance;
    }
    return pitch;
}
