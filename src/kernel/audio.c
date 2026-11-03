/*
 * audio.c — PCM mixer for the Sound Blaster 16.
 *
 * One mono 8-bit DMA ring buffer feeds the SB16. Userspace writes samples
 * with audio_write(); the mixer copies them ahead of the DMA playback
 * cursor. Underruns produce silence (zeros).
 *
 * Tetris music and SFX: userspace mixes music + SFX into one stream and
 * writes it here, or writes music in a loop and SFX on top whenever a
 * sound effect triggers.
 */

#include <audio.h>
#include <interrupt.h>
#include <io.h>
#include <klog.h>
#include <pmm.h>
#include <vmm.h>
#include <spinlock.h>
#include <types.h>

#include <drivers/sb16.h>

KLOG_SUBSYSTEM("audio");

#define AUDIO_BUFFER_SIZE 16384u   /* 16 KiB DMA ring (<= 64 KiB limit) */
#define AUDIO_SAMPLE_RATE 22050u   /* default 22 kHz */

static int8_t *audio_buf = NULL;
static phys_addr_t audio_buf_phys = 0;
static size_t audio_buf_size = 0;
static volatile size_t audio_write_pos = 0;
static volatile size_t audio_dma_pos = 0;
static uint32_t audio_rate = AUDIO_SAMPLE_RATE;
static bool audio_started = false;

/* Allocate the DMA buffer and start SB16 playback. */
bool audio_init(void)
{
    size_t alloc_size;
    void *vaddr;

    if (audio_buf)
        return true;

    /* Allocate extra so we can align to a 64 KiB boundary; ISA DMA buffers
     * must not cross a 64 KiB boundary. */
    alloc_size = AUDIO_BUFFER_SIZE + 0x1000u;
    vaddr = pmm_alloc_dma_range(alloc_size, PAGE_SIZE, &audio_buf_phys);
    if (!vaddr) {
        klog(KLOG_WARN, "audio: could not allocate %zu byte DMA buffer\n",
              alloc_size);
        return false;
    }

    /* Align the physical address up to 64 KiB. The direct map offset between
     * phys and virt is constant, so we can compute the corresponding virt. */
    audio_buf_phys = (audio_buf_phys + 0xFFFFu) & ~0xFFFFu;
    audio_buf = (int8_t *)phys_to_virt(audio_buf_phys);

    for (size_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
        audio_buf[i] = 0x80; /* 8-bit unsigned silence = 0x80 */

    audio_buf_size = AUDIO_BUFFER_SIZE;

    klog(KLOG_INFO, "audio: %zu byte DMA ring at phys 0x%lx\n",
          AUDIO_BUFFER_SIZE, (unsigned long)audio_buf_phys);
    return true;
}

/* Number of bytes the DMA has consumed so far, tracked via IRQ count. */
static size_t audio_dma_consumed(void)
{
    /* Each IRQ = one full buffer wrap. Between IRQs we don't know the
     * exact position, so we conservatively assume the DMA is at the
     * start of the current buffer lap. */
    return audio_dma_pos;
}

/* Write `count` bytes of mono 8-bit PCM into the ring. Returns bytes
 * actually written. Callers must ensure the rate matches the SB16's
 * programmed rate, or pitch will shift. */
size_t audio_write(const int8_t *samples, size_t count)
{
    size_t written = 0;
    size_t space;
    size_t wrap;
    u64 flags;

    klog(KLOG_DEBUG, "audio_write: count=%zu audio_buf=%p audio_started=%d\n",
          count, (void *)audio_buf, audio_started);

    if (!audio_init()) {
        klog(KLOG_WARN, "audio_write: audio_init() failed\n");
        return 0;
    }

    if (!samples || count == 0)
        return 0;

    flags = irq_save();

    /* If not yet playing, start SB16 now that the buffer has silence. */
    if (!audio_started) {
        if (!sb16_play_start(audio_buf_phys, AUDIO_BUFFER_SIZE, audio_rate)) {
            irq_restore(flags);
            return 0;
        }
        audio_started = true;
    }

    /* Space available = buffer size - (write_pos - dma_pos). Leave a
     * full buffer lap of headroom so the DMA never reads unwritten data. */
    space = AUDIO_BUFFER_SIZE - (audio_write_pos - audio_dma_consumed());
    if (space >= AUDIO_BUFFER_SIZE)
        space = 0; /* ring completely full or just wrapped */

    if (space == 0) {
        irq_restore(flags);
        return 0;
    }

    if (count > space)
        count = space;

    /* Copy samples, wrapping at the end of the buffer. */
    while (written < count) {
        wrap = AUDIO_BUFFER_SIZE - (audio_write_pos % AUDIO_BUFFER_SIZE);
        size_t chunk = count - written;
        if (chunk > wrap)
            chunk = wrap;

        for (size_t i = 0; i < chunk; i++)
            audio_buf[(audio_write_pos + i) % AUDIO_BUFFER_SIZE] =
                samples[written + i];

        audio_write_pos += chunk;
        written += chunk;
    }

    irq_restore(flags);
    return written;
}

void audio_stop(void)
{
    u64 flags;

    flags = irq_save();
    sb16_play_stop();
    audio_started = false;
    audio_write_pos = 0;
    audio_dma_pos = 0;
    irq_restore(flags);
}

void audio_set_rate(uint32_t rate_hz)
{
    u64 flags;

    flags = irq_save();
    audio_rate = rate_hz;
    irq_restore(flags);
}

bool audio_is_playing(void)
{
    return audio_started;
}

/* Called from sb16_irq() when the DMA buffer wraps. */
void audio_irq(void)
{
    audio_dma_pos += AUDIO_BUFFER_SIZE;

    /* Keep the positions from growing without bound. */
    if (audio_dma_pos > AUDIO_BUFFER_SIZE * 4u) {
        audio_write_pos -= AUDIO_BUFFER_SIZE * 3u;
        audio_dma_pos -= AUDIO_BUFFER_SIZE * 3u;
        if (audio_write_pos < audio_dma_pos)
            audio_write_pos = audio_dma_pos;
    }
}
