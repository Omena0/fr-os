#ifndef AUDIO_H
#define AUDIO_H

#include <types.h>

/*
 * audio.h — PCM mixer and audio syscall interface.
 *
 * A single mono 8-bit DMA buffer feeds the SB16. Userspace writes PCM
 * samples through audio_write(); the mixer copies them into the DMA ring
 * ahead of the playback cursor. Multiple streams (Tetris music + SFX) are
 * mixed by userspace before a single write, or written sequentially into
 * the ring.
 */

/* Allocate the DMA buffer and start SB16 playback. Called lazily on first
 * audio_write(). */
bool audio_init(void);

/* Write `count` bytes of mono 8-bit PCM into the ring buffer. Returns the
 * number of bytes actually written (may be less if the ring is full). */
size_t audio_write(const int8_t *samples, size_t count);

/* Stop playback and drain the ring. */
void audio_stop(void);

/* Set the playback sample rate (Hz). Takes effect on next write. */
void audio_set_rate(uint32_t rate_hz);

/* IRQ handler called from sb16_irq() when the DMA buffer wraps. */
void audio_irq(void);

bool audio_is_playing(void);

#endif
