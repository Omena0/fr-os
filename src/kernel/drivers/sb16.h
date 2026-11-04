#ifndef SB16_H
#define SB16_H

#include <types.h>

/*
 * sb16.h — Creative Sound Blaster 16 (CT1740 / CT2960) driver interface.
 *
 * Programs the SB16 DSP for 8-bit mono PCM playback on DMA channel 1, IRQ 5.
 * Enough to drive Tetris-style music and sound effects.
 *
 * All port offsets are relative to the SB16 base I/O address (0x220).
 */

/* SB16 base I/O port (ISA default). */
#define SB16_BASE     0x220u

/* DSP data ports. */
#define SB16_DSP_RESET    (SB16_BASE + 0x06u)   /* DSP reset (write) */
#define SB16_DSP_READ     (SB16_BASE + 0x0Au)   /* DSP read data (read) */
#define SB16_DSP_WRITE    (SB16_BASE + 0x0Cu)   /* DSP write data (write) */
#define SB16_DSP_WSTAT    (SB16_BASE + 0x0Cu)   /* DSP write-buffer status (read) */
#define SB16_DSP_RSTAT    (SB16_BASE + 0x0Eu)   /* DSP read-buffer status + 8-bit IRQ ack (read) */

/* Mixer ports. */
#define SB16_MIXER_INDEX  (SB16_BASE + 0x04u)   /* Mixer register index (write) */
#define SB16_MIXER_DATA   (SB16_BASE + 0x05u)   /* Mixer register data (read/write) */

/* Mixer registers. */
#define MIXER_MASTER_LEFT     0x04u   /* Master volume, left channel */
#define MIXER_MASTER_RIGHT    0x05u   /* Master volume, right channel */
#define MIXER_PCM_LEFT        0x22u   /* PCM output volume, left channel */
#define MIXER_PCM_RIGHT       0x23u   /* PCM output volume, right channel */

/* Mixer volume: 0x00 = full volume (no attenuation). Top nibble is
 * attenuation in 1.5 dB steps. */
#define MIXER_VOL_FULL 0x00u

/* DSP commands. */
#define DSP_RESET_ACK         0xAAu
#define DSP_READ_VERSION      0xE1u   /* read DSP version: 2 bytes from 0x22A */
#define DSP_SPEAKER_ON        0xD1u
#define DSP_SPEAKER_OFF       0xD3u
#define DSP_SET_TIME_CONST    0x40u
#define DSP_DMA_8BIT_SINGLE   0xC0u   /* 8-bit single-cycle DAC output */
#define DSP_DMA_8BIT_AUTOOUT  0x1Cu   /* Auto-init DMA transfer mode (mode cmd) */
#define DSP_EXIT_DMA          0xDCu   /* exit DMA mode */

/* 8237 DMA controller ports. */
#define DMA_MODE_REG      0x0Bu
#define DMA_CLEAR_FLIPFLOP 0x0Cu
#define DMA_MASK_REG      0x0Au

/* DMA channel 1 registers (8-bit controller). */
#define DMA1_BASE_ADDR    0x02u
#define DMA1_BASE_COUNT   0x03u
#define DMA1_PAGE         0x83u
#define DMA1_MODE_WRITE   0x51u   /* ch1, write transfer, increment, auto-init */
#define DMA1_MODE_SINGLE  0x48u   /* ch1, write transfer, increment, single */
#define DMA1_MASK_CH1_SET   0x05u   /* mask channel 1 (single-channel write) */
#define DMA1_MASK_CH1_CLR   0x01u   /* unmask channel 1 (single-channel write) */

/* IRQ 5 (master 8259). */
#define SB16_IRQ          5u
#define SB16_IRQ_VECTOR   VECTOR_IRQ_PARALLEL

/* Playback state. */
enum sb16_state {
    SB16_STATE_IDLE,
    SB16_STATE_PLAYING,
    SB16_STATE_STOPPED,
};

/* Max sample rate we program directly via DSP. */
#define SB16_MAX_RATE     44100u

void sb16_init(void);
bool sb16_play_start(phys_addr_t dma_phys, size_t dma_size, uint32_t rate_hz);
void sb16_play_stop(void);
bool sb16_is_playing(void);

/* IRQ handler — installed at SB16_IRQ_VECTOR. */
void sb16_irq(struct interrupt_frame *frame);

#endif
