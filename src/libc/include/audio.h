#ifndef AUDIO_H
#define AUDIO_H

#include <sys/types.h>

/*
 * audio.h — userspace PCM audio API.
 *
 * audio_write() feeds mono 8-bit unsigned PCM samples into the kernel's
 * SB16 DMA ring buffer. The caller must supply samples at the rate the
 * SB16 is programmed for (default 22050 Hz), or pitch will shift.
 *
 * Returns the number of bytes actually consumed (may be less than count if
 * the DMA ring is full); -1 on error.
 */
ssize_t audio_write(const void *buf, size_t count);

/* Stop playback and silence the output. */
int audio_stop(void);

/* Set the playback sample rate in Hz. Takes effect on the next write. */
int audio_set_format(unsigned int rate_hz);

#endif
