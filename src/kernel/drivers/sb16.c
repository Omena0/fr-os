/*
 * sb16.c — Creative Sound Blaster 16 driver.
 *
 * 8-bit mono PCM playback via DMA channel 1, IRQ 5. Programmed I/O for
 * DSP commands, 8237 DMA for sample transfer. Sufficient for Tetris music
 * and sound effects.
 *
 * DSP programming model (8-bit single-cycle output):
 *   1. Reset DSP (write 1 then 0 to 0x226, poll 0x22E bit7, read 0x22A == 0xAA).
 *   2. Read DSP version (0xE1) and verify 4.5.
 *   3. Set time constant (0x40 + byte) for sample rate.
 *   4. Program DMA channel 1 (mask, flip-flop, address, count, page, mode, unmask).
 *   5. Enable speaker (0xD1).
 *   6. Start 8-bit single-cycle DMA (0xC0 + mode byte + count).
 *   DSP raises IRQ 5 when the buffer is done; handler acknowledges by
 *   reading 0x22E.
 *
 * All port accesses are byte-sized. Before each DSP command byte written to
 * 0x22C, bit 7 of 0x22C (write-buffer status) must be clear (DSP ready).
 */

#include <interrupt.h>
#include <io.h>
#include <klog.h>
#include <pmm.h>
#include <vmm.h>
#include <panic.h>
#include <percpu.h>
#include <types.h>

#include "sb16.h"

/* Audio mixer IRQ callback. */
void audio_irq(void);

KLOG_SUBSYSTEM("sb16");

#define DSP_WRITE_TIMEOUT_US 10000u

static volatile enum sb16_state sb16_state = SB16_STATE_IDLE;
static uint32_t sb16_sample_rate = 22050u;
static size_t sb16_dma_size = 0;

/* --------------------------------------------------------------- dsp ------ */

/* Write one command/data byte to the DSP. Before writing, poll the
 * write-buffer status port (0x22C) until bit 7 is clear, meaning the DSP
 * can accept a byte. Returns false on timeout. */
static bool dsp_write(uint8_t reg)
{
    unsigned guard;

    for (guard = 0; guard < DSP_WRITE_TIMEOUT_US; guard++) {
        if (!(inb(SB16_DSP_WSTAT) & 0x80u)) {
            outb(SB16_DSP_WRITE, reg);
            return true;
        }
        pause_cpu();
    }
    klog(KLOG_WARN, "sb16: dsp_write(0x%02x) timed out on write-buffer status\n",
          (unsigned)reg);
    return false;
}

/* Read the DSP status byte from the read-buffer status port (0x22E).
 * Bit 7 set means a byte can be read from the DSP data port (0x22A). */
static uint8_t dsp_status(void)
{
    return inb(SB16_DSP_RSTAT);
}

/* Write one byte to an SB16 mixer register. */
static void mixer_write(uint8_t reg, uint8_t val)
{
    outb(SB16_MIXER_INDEX, reg);
    outb(SB16_MIXER_DATA, val);
}

/* Reset the DSP per the spec:
 *   1. Write 0x01 to 0x226.
 *   2. Wait at least 3 microseconds.
 *   3. Write 0x00 to 0x226.
 *   4. Poll 0x22E until bit 7 is set.
 *   5. Read one byte from 0x22A; expect 0xAA.
 * Returns true if the DSP responds with 0xAA. */
static bool dsp_reset(void)
{
    outb(SB16_DSP_RESET, 0x01u);
    for (volatile int i = 0; i < 1000; i++)
        ; /* brief hold, well over 3 us */

    outb(SB16_DSP_RESET, 0x00u);

    /* Poll the read-buffer status port until bit 7 is set. */
    for (unsigned guard = 0; guard < 50000; guard++) {
        if (inb(SB16_DSP_RSTAT) & 0x80u)
            break;
        pause_cpu();
    }

    /* Read the acknowledge byte from the DSP data port. */
    if (inb(SB16_DSP_READ) == DSP_RESET_ACK)
        return true;

    klog(KLOG_WARN, "sb16: DSP reset: no 0xAA acknowledge on data port\n");
    return false;
}

/* Read the DSP version (command 0xE1). Returns true if version is 4.5. */
static bool dsp_version_ok(void)
{
    uint8_t major, minor;

    if (!dsp_write(DSP_READ_VERSION))
        return false;

    /* Wait for read-buffer status bit 7, then read two bytes. */
    for (unsigned guard = 0; guard < 50000; guard++) {
        if (dsp_status() & 0x80u) {
            major = inb(SB16_DSP_READ);
            for (guard = 0; guard < 50000; guard++) {
                if (dsp_status() & 0x80u) {
                    minor = inb(SB16_DSP_READ);
                    klog(KLOG_INFO, "sb16: DSP version %u.%u\n",
                          (unsigned)major, (unsigned)minor);
                    return major == 4 && minor == 5;
                }
                pause_cpu();
            }
            break;
        }
        pause_cpu();
    }
    klog(KLOG_WARN, "sb16: DSP version read timed out\n");
    return false;
}

/* ---------------------------------------------------------------- dma ----- */

/* Program the 8237 DMA channel 1 for an 8-bit transfer from memory to
 * the SB16 DSP data port. `size` must be <= 65536 bytes and the buffer
 * must not cross a 64 KiB boundary. */
static bool dma_program(phys_addr_t phys, size_t size)
{
    uint8_t lo, hi;
    uint8_t page = (uint8_t)(phys >> 16);

    if (size == 0 || size > 0x10000u)
        return false;
    /* The DMA address must not cross a 64 KiB boundary. */
    if ((phys & 0xFFFFu) + size > 0x10000u) {
        klog(KLOG_WARN, "sb16: DMA buffer 0x%lx+%zu crosses 64 KiB boundary\n",
              (unsigned long)phys, size);
        return false;
    }

    size--; /* 8237 count register holds count-1 */

    lo = (uint8_t)(size & 0xFFu);
    hi = (uint8_t)((size >> 8) & 0xFFu);

    /* Mask channel 1 while we program it. */
    outb(DMA_MASK_REG, DMA1_MASK_CH1_SET);

    /* Clear the byte pointer flip-flop so low/high byte pairs land right. */
    outb(DMA_CLEAR_FLIPFLOP, 0x00u);

    /* Base address (low then high). */
    outb(DMA1_BASE_ADDR, (uint8_t)(phys & 0xFFu));
    outb(DMA1_BASE_ADDR, (uint8_t)((phys >> 8) & 0xFFu));

    /* Base count (low then high). */
    outb(DMA1_BASE_COUNT, lo);
    outb(DMA1_BASE_COUNT, hi);

    /* Page register (upper address bits). */
    outb(DMA1_PAGE, page);

    /* Mode: channel 1, write transfer, increment, auto-init. */
    outb(DMA_MODE_REG, DMA1_MODE_WRITE);

    /* Unmask channel 1. */
    outb(DMA_MASK_REG, DMA1_MASK_CH1_CLR);

    return true;
}

/* Allocate a DMA-safe buffer: physically contiguous, below 16 MiB, and not
 * crossing a 64 KiB boundary. Returns the physical address or 0 on failure. */
static bool dma_alloc(size_t size, phys_addr_t *out_phys)
{
    /* Allocate twice as much so we can align within it to avoid a
     * 64 KiB boundary crossing. */
    size_t alloc_size = size + 0x1000u;
    void *vaddr = pmm_alloc_dma_range(alloc_size, PAGE_SIZE, out_phys);
    if (!vaddr)
        return false;

    phys_addr_t phys = *out_phys;
    /* Align up to the next 64 KiB boundary if needed. */
    phys_addr_t aligned = (phys + 0xFFFFu) & ~0xFFFFu;
    if (aligned + size > phys + alloc_size) {
        /* Not enough room after alignment; try again with more. */
        klog(KLOG_WARN, "sb16: DMA alignment failed, retrying\n");
        return false;
    }
    *out_phys = aligned;
    klog(KLOG_DEBUG, "sb16: DMA buffer phys 0x%lx size %zu (aligned to 0x%lx)\n",
          (unsigned long)aligned, size, (unsigned long)phys);
    return true;
}

/* -------------------------------------------------------------- playback -- */

void sb16_init(void)
{
    //klog_set_subsystem_level("sb16", KLOG_DEBUG);

    if (!dsp_reset()) {
        klog(KLOG_WARN, "sb16: DSP reset failed — no card at 0x%04x\n",
              SB16_BASE);
        return;
    }

    if (!dsp_version_ok()) {
        klog(KLOG_WARN, "sb16: unexpected DSP version\n");
        return;
    }

    /* Speaker on. */
    dsp_write(DSP_SPEAKER_ON);

    /* Set mixer volume to full on master and PCM channels. */
    mixer_write(MIXER_MASTER_LEFT, MIXER_VOL_FULL);
    mixer_write(MIXER_MASTER_RIGHT, MIXER_VOL_FULL);
    mixer_write(MIXER_PCM_LEFT, MIXER_VOL_FULL);
    mixer_write(MIXER_PCM_RIGHT, MIXER_VOL_FULL);

    /* Install IRQ handler before unmasking. */
    idt_set_handler(SB16_IRQ_VECTOR, sb16_irq, IST_NONE, 0);

    /* Unmask IRQ 5 in the master PIC. */
    {
        u64 flags = irq_save();
        uint8_t mask = inb(0x21u); /* PIC1 data port */
        mask &= (uint8_t)~(1u << SB16_IRQ);
        outb(0x21u, mask);
        klog(KLOG_DEBUG, "sb16: PIC1 mask before unmask IRQ5: 0x%02x, after: 0x%02x\n",
              mask | (1u << SB16_IRQ), mask);
        irq_restore(flags);
    }

    klog(KLOG_INFO, "sb16: Sound Blaster 16 at 0x%04x, IRQ %u, DMA ch1\n",
          SB16_BASE, SB16_IRQ);
    sb16_state = SB16_STATE_IDLE;
}

bool sb16_play_start(phys_addr_t dma_phys, size_t dma_size, uint32_t rate_hz)
{
    uint8_t time_const;

    klog(KLOG_DEBUG, "sb16_play_start: phys=0x%lx size=%zu rate=%u state=%d\n",
          (unsigned long)dma_phys, dma_size, rate_hz, (int)sb16_state);

    if (sb16_state != SB16_STATE_IDLE) {
        klog(KLOG_WARN, "sb16_play_start: not idle (state=%d)\n",
              (int)sb16_state);
        return false;
    }
    if (dma_size == 0 || dma_size > 0x10000u)
        return false;
    if (rate_hz < 4000u || rate_hz > SB16_MAX_RATE)
        return false;

    /* Time constant: 256 - (1000000 / sample_rate). */
    time_const = (uint8_t)(256u - (1000000u / rate_hz));

    /* Stop any previous playback first. */
    dsp_write(DSP_EXIT_DMA);

    /* Set time constant. */
    if (!dsp_write(DSP_SET_TIME_CONST))
        return false;
    if (!dsp_write(time_const))
        return false;

    /* Program DMA channel 1. */
    if (!dma_program(dma_phys, dma_size)) {
        klog(KLOG_WARN, "sb16_play_start: dma_program() failed\n");
        return false;
    }

    /* Enable speaker. */
    dsp_write(DSP_SPEAKER_ON);

    /* Log DMA address and page for verification. */
    klog(KLOG_DEBUG, "sb16: DMA programmed: phys=0x%lx size=%zu page=0x%02x mode=0x%02x\n",
          (unsigned long)dma_phys, dma_size, (uint8_t)(dma_phys >> 16), inb(0x0B));

    /* Start 8-bit auto-init DAC output (loops continuously).
     * 0x14 is the direct auto-init output command. */
    if (!dsp_write(0x14u))
        return false;
    if (!dsp_write((uint8_t)((dma_size - 1) & 0xFFu))) /* count low */
        return false;
    if (!dsp_write((uint8_t)(((dma_size - 1) >> 8) & 0xFFu))) /* count high */
        return false;

    sb16_sample_rate = rate_hz;
    sb16_dma_size = dma_size;
    sb16_state = SB16_STATE_PLAYING;
    klog(KLOG_DEBUG, "sb16_play_start: SUCCESS — playback started %u Hz, %zu byte buffer\n",
          rate_hz, dma_size);
    klog(KLOG_INFO, "sb16: playback started %u Hz, %zu byte buffer\n",
          rate_hz, dma_size);
    return true;
}

void sb16_play_stop(void)
{
    if (sb16_state != SB16_STATE_PLAYING)
        return;

    dsp_write(DSP_EXIT_DMA);
    dsp_write(DSP_SPEAKER_OFF);

    sb16_state = SB16_STATE_IDLE;
    klog(KLOG_DEBUG, "sb16: playback stopped\n");
}

bool sb16_is_playing(void)
{
    return sb16_state == SB16_STATE_PLAYING;
}

/* ---------------------------------------------------------------- irq ----- */

void sb16_irq(struct interrupt_frame *frame)
{
    UNUSED(frame);

    /* Acknowledge the SB16 interrupt by reading 0x22E (8-bit DMA IRQ ack). */
    (void)dsp_status();

    audio_irq();

    /* Issue EOI for IRQ 5 (master PIC). */
    pic_eoi(SB16_IRQ_VECTOR);

    klog(KLOG_DEBUG, "sb16: irq — dma buffer done\n");
}
