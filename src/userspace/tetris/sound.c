#include <sound.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sound.h"

#define THEME_BPM 140u

struct theme_note {
    uint16_t frequency;
    uint8_t sixteenths;
};

/* Korobeiniki, arranged as a repeating eight-measure chiptune with bass. */
static const struct theme_note theme[] = {
    {659,4}, {494,2}, {523,2}, {587,2}, {659,1}, {587,1}, {523,2}, {494,2},
    {440,4}, {440,2}, {523,2}, {659,4}, {587,2}, {523,2},
    {494,6}, {523,2}, {587,4}, {659,4},
    {523,4}, {440,4}, {440,8},
    {0,2}, {587,4}, {698,2}, {880,4}, {784,2}, {698,2},
    {659,6}, {523,2}, {659,4}, {587,2}, {523,2},
    {494,4}, {494,2}, {523,2}, {587,4}, {659,4},
    {523,4}, {440,4}, {440,4}, {0,4},
};

static const uint16_t bass_roots[] = { 82, 110, 123, 82, 73, 82, 123, 110 };
static sound_note_t theme_sequence[sizeof(theme) / sizeof(theme[0])];
static bool sound_ready;

static const sound_note_t rotate_effect[] = {
    {880, 0, 18}, {1175, 0, 24},
};
static const sound_note_t clear_single[] = {
    {784, 0, 38}, {1047, 0, 42}, {1568, 0, 70},
};
static const sound_note_t clear_double[] = {
    {659, 0, 34}, {880, 0, 34}, {1175, 0, 42}, {1760, 0, 76},
};
static const sound_note_t clear_triple[] = {
    {523, 0, 30}, {659, 0, 32}, {880, 0, 36}, {1318, 0, 42}, {1976, 0, 90},
};
static const sound_note_t clear_tetris[] = {
    {523, 0, 28}, {659, 0, 28}, {784, 0, 32},
    {1047, 0, 38}, {1568, 0, 44}, {2093, 0, 110},
};
static const sound_note_t spin_effect[] = {
    {330, 0, 28}, {494, 0, 28}, {740, 0, 32}, {1109, 0, 36}, {1661, 0, 84},
};
static const sound_note_t spin_clear_effect[] = {
    {440, 0, 26}, {659, 0, 28}, {988, 0, 30}, {1480, 0, 36}, {1976, 0, 95},
};

void tetris_sound_init(void)
{
    uint32_t units = 0;
    uint32_t denominator = THEME_BPM * 4u;

    sound_ready = sound_init(22050u) == 0;
    if (!sound_ready)
        return;

    for (size_t i = 0; i < sizeof(theme) / sizeof(theme[0]); i++) {
        unsigned measure = units / 16;
        uint32_t duration_ms =
            ((uint32_t)theme[i].sixteenths * 60000u + denominator / 2u) /
            denominator;

        theme_sequence[i].frequency_hz = theme[i].frequency;
        theme_sequence[i].bass_hz = theme[i].frequency ? bass_roots[measure] : 0;
        theme_sequence[i].duration_ms = (uint16_t)duration_ms;
        units += theme[i].sixteenths;
        if (units >= 128)
            units = 0;
    }

    if (sound_play(theme_sequence,
                   sizeof(theme_sequence) / sizeof(theme_sequence[0]), true) < 0) {
        sound_stop();
        sound_ready = false;
    }
}

void tetris_sound_rotate(void)
{
    if (sound_ready)
        (void)sound_effect(rotate_effect,
                           sizeof(rotate_effect) / sizeof(rotate_effect[0]));
}

void tetris_sound_lock(bool spin, int cleared)
{
    const sound_note_t *notes = NULL;
    size_t count = 0;

    if (!sound_ready)
        return;
    if (spin && cleared > 0) {
        notes = spin_clear_effect;
        count = sizeof(spin_clear_effect) / sizeof(spin_clear_effect[0]);
    } else if (spin) {
        notes = spin_effect;
        count = sizeof(spin_effect) / sizeof(spin_effect[0]);
    } else if (cleared >= 4) {
        notes = clear_tetris;
        count = sizeof(clear_tetris) / sizeof(clear_tetris[0]);
    } else if (cleared == 3) {
        notes = clear_triple;
        count = sizeof(clear_triple) / sizeof(clear_triple[0]);
    } else if (cleared == 2) {
        notes = clear_double;
        count = sizeof(clear_double) / sizeof(clear_double[0]);
    } else if (cleared == 1) {
        notes = clear_single;
        count = sizeof(clear_single) / sizeof(clear_single[0]);
    }

    if (notes)
        (void)sound_effect(notes, count);
}

void tetris_sound_shutdown(void)
{
    if (sound_ready)
        sound_stop();
    sound_ready = false;
}
