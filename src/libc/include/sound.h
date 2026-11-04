#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sound_note {
    uint16_t frequency_hz;
    uint16_t bass_hz;
    uint16_t duration_ms;
} sound_note_t;

#define SOUND_MAX_NOTES 64u

/* Start the asynchronous sound worker at `sample_rate_hz`. */
int sound_init(unsigned int sample_rate_hz);

/* Replace the background sequence. The worker receives a private copy. */
int sound_play(const sound_note_t *notes, size_t count, bool loop);

/* Replace the current effect sequence without stopping background music. */
int sound_effect(const sound_note_t *notes, size_t count);

/* Stop the worker and silence playback. */
void sound_stop(void);

#endif
