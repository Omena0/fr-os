/*
 * audio_test.c — verify SB16 audio playback.
 *
 * Plays a musical scale, a short Tetris-style melody, and a sound effect.
 * Run from Fr Init: `audio_test`
 */

#include <audio.h>
#include <stdio.h>
#include <unistd.h>

#define SAMPLE_RATE 22050u

/* Note frequencies (Hz). */
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_G5  784

/* Generate one note: square wave at `freq` for `duration_ms`. */
static void play_note(uint16_t freq, uint32_t duration_ms)
{
    int8_t buf[512];
    uint32_t period = SAMPLE_RATE / freq;
    uint32_t samples = SAMPLE_RATE * duration_ms / 1000u;
    uint32_t written = 0;
    uint32_t phase = 0;

    while (written < samples) {
        size_t chunk = sizeof(buf);
        if (chunk > samples - written)
            chunk = samples - written;

        for (size_t i = 0; i < chunk; i++) {
            buf[i] = (phase < period / 2) ? 0xFF : 0x00;
            phase++;
            if (phase >= period)
                phase = 0;
        }

        ssize_t n = audio_write(buf, chunk);
        if (n < 0) {
            printf("audio_write failed\n");
            return;
        }
        written += (uint32_t)n;
    }
}

/* Tetris theme (Korobeiniki) - first phrase, simplified. */
static void play_tetris_theme(void)
{
    static const struct { uint16_t freq; uint32_t ms; } notes[] = {
        { NOTE_E5, 300 }, { NOTE_E5, 300 },
        { NOTE_B4, 300 }, { NOTE_C5, 300 }, { NOTE_D5, 300 }, { NOTE_C5, 300 },
        { NOTE_B4, 600 },
        { NOTE_A4, 300 }, { NOTE_A4, 300 },
        { NOTE_B4, 300 }, { NOTE_C5, 300 }, { NOTE_D5, 300 }, { NOTE_E5, 300 },
        { NOTE_D5, 300 }, { NOTE_C5, 300 }, { NOTE_B4, 300 }, { NOTE_A4, 600 },
    };

    for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++)
        play_note(notes[i].freq, notes[i].ms);
}

/* Short "beep" sound effect. */
static void play_sfx(void)
{
    play_note(NOTE_C5, 100);
    usleep(50000);
    play_note(NOTE_E5, 100);
    usleep(50000);
    play_note(NOTE_G5, 200);
}

int main(void)
{
    printf("audio_test: starting SB16 audio test\n");

    if (audio_set_format(SAMPLE_RATE) < 0) {
        printf("audio_test: audio_set_format failed\n");
        return 1;
    }

    printf("audio_test: playing scale...\n");
    play_note(NOTE_C4, 400);
    play_note(NOTE_D4, 400);
    play_note(NOTE_E4, 400);
    play_note(NOTE_F4, 400);
    play_note(NOTE_G4, 400);
    play_note(NOTE_A4, 400);
    play_note(NOTE_B4, 400);
    play_note(NOTE_C5, 600);

    usleep(500000); /* 0.5 s gap */

    printf("audio_test: playing Tetris theme...\n");
    play_tetris_theme();

    usleep(500000);

    printf("audio_test: playing SFX...\n");
    play_sfx();

    usleep(500000);

    audio_stop();
    printf("audio_test: done\n");
    return 0;
}
