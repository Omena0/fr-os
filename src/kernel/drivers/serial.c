/*
 * serial.c — 16550-compatible UART on COM1.
 *
 * 8N1 at 115200 baud, interrupts disabled. Transmission is polled so the
 * console works with interrupts off (early boot, panic) and from interrupt
 * handlers. Receive is polled from the input path because keyboard-style input
 * needs to be delivered to whichever thread is blocked in read().
 */

#include <io.h>
#include <kstring.h>
#include <drivers/serial.h>

/* Interrupt enable register bits. */
#define UART_IER_RX_AVAILABLE 0x01
#define UART_IER_TX_EMPTY    0x02

/* Interrupt identification bits (highest priority first). */
#define UART_IIR_NO_INT      0x01
#define UART_IIR_RX_AVAILABLE 0x04
#define UART_IIR_TX_EMPTY    0x02

/*
 * Bounded spin for the transmit-holding-register-empty bit. 2^20 spins is
 * several orders of magnitude of headroom; exceeding it means the UART is
 * absent or wedged, and continuing would hang the machine with no diagnostic
 * output.
 */
#define TX_SPIN_LIMIT (1u << 20)

void serial_init(void)
{
	outb(COM1 + 1, 0x00);      /* interrupts disabled */
	outb(COM1 + 3, 0x80);      /* DLAB on: divisor latch accessible */
	outb(COM1 + 0, 0x01);      /* divisor low  = 1 (115200 baud) */
	outb(COM1 + 1, 0x00);      /* divisor high = 0 */
	outb(COM1 + 3, 0x03);      /* 8 data bits, no parity, 1 stop bit */
	outb(COM1 + 2, 0xC7);      /* FIFO on, cleared, 14-byte receive trigger */
	outb(COM1 + 4, 0x03);      /* DTR | RTS: receiver and transmitter enabled */
}

void serial_putc_blocking(char c)
{
	unsigned int spin = TX_SPIN_LIMIT;

	while ((inb(COM1 + 5) & UART_LSR_TX_EMPTY) == 0) {
		if (--spin == 0)
			return;    /* UART absent or wedged: drop the byte */
		io_wait();
	}
	outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s)
{
	for (; *s; s++)
		serial_putc_blocking(*s);
}

bool serial_getc(char *out)
{
	if ((inb(COM1 + 5) & UART_LSR_DATA_READY) == 0)
		return false;
	*out = (char)inb(COM1);
	return true;
}
