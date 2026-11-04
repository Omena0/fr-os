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
#define TETRIS_BPM 140u

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
#define NOTE_F5  698
#define NOTE_G5  784
#define NOTE_A5  880

/* Generate one note: square wave at `freq` for `duration_ms`. */
static void play_note(uint16_t freq, uint32_t duration_ms)
{
    int8_t buf[512];
    uint32_t period = freq ? SAMPLE_RATE / freq : 0;
    uint32_t samples = SAMPLE_RATE * duration_ms / 1000u;
    uint32_t written = 0;
    uint32_t phase = 0;

    while (written < samples) {
        size_t chunk = sizeof(buf);
        if (chunk > samples - written)
            chunk = samples - written;

        for (size_t i = 0; i < chunk; i++) {
            if (freq == 0) {
                buf[i] = (int8_t)0x80;
            } else {
                buf[i] = (phase < period / 2) ? (int8_t)0xFF : 0x00;
                phase++;
                if (phase >= period)
                    phase = 0;
            }
        }

        size_t offset = 0;
        while (offset < chunk) {
            ssize_t n = audio_write(buf + offset, chunk - offset);
            if (n < 0) {
                printf("audio_write failed\n");
                return;
            }
            if (n == 0) {
                usleep(1000);
                continue;
            }
            offset += (size_t)n;
        }
        written += (uint32_t)chunk;
    }
}

/* Tetris Theme A, first eight measures, at the reference sheet's 140 BPM. */
static void play_tetris_theme(void)
{
    static const struct { uint16_t freq; uint8_t sixteenths; } notes[] = {
        { NOTE_E5, 4 }, { NOTE_B4, 2 }, { NOTE_C5, 2 }, { NOTE_D5, 2 },
        { NOTE_E5, 1 }, { NOTE_D5, 1 }, { NOTE_C5, 2 }, { NOTE_B4, 2 },
        { NOTE_A4, 4 }, { NOTE_A4, 2 }, { NOTE_C5, 2 }, { NOTE_E5, 4 },
        { NOTE_D5, 2 }, { NOTE_C5, 2 },
        { NOTE_B4, 6 }, { NOTE_C5, 2 }, { NOTE_D5, 4 }, { NOTE_E5, 4 },
        { NOTE_C5, 4 }, { NOTE_A4, 4 }, { NOTE_A4, 8 },
        { 0, 2 }, { NOTE_D5, 4 }, { NOTE_F5, 2 }, { NOTE_A5, 4 },
        { NOTE_G5, 2 }, { NOTE_F5, 2 },
        { NOTE_E5, 6 }, { NOTE_C5, 2 }, { NOTE_E5, 4 },
        { NOTE_D5, 2 }, { NOTE_C5, 2 },
        { NOTE_B4, 4 }, { NOTE_B4, 2 }, { NOTE_C5, 2 },
        { NOTE_D5, 4 }, { NOTE_E5, 4 },
        { NOTE_C5, 4 }, { NOTE_A4, 4 }, { NOTE_A4, 4 }, { 0, 4 },
    };

    for (unsigned repeat = 0; repeat < 2; repeat++) {
        for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
            uint32_t denominator = TETRIS_BPM * 4u;
            uint32_t duration_ms =
                ((uint32_t)notes[i].sixteenths * 60000u + denominator / 2u) /
                denominator;

            play_note(notes[i].freq, duration_ms);
        }
    }
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
