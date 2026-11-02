/*
 * audio.c — userspace PCM audio API.
 *
 * Thin wrappers over the audio syscalls.
 */

#include <audio.h>
#include <sys/syscall.h>

ssize_t sys_audio_write(const void *buf, size_t count);
int sys_audio_stop(void);
int sys_audio_set_format(unsigned int rate_hz);

ssize_t audio_write(const void *buf, size_t count)
{
    return sys_audio_write(buf, count);
}

int audio_stop(void)
{
    return sys_audio_stop();
}

int audio_set_format(unsigned int rate_hz)
{
    return sys_audio_set_format(rate_hz);
}
