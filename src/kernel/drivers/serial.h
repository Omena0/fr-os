/*
 * serial.h — COM1 UART driver.
 *
 * The serial port is the kernel's first and most reliable output channel: it
 * works before paging, before interrupts, and after a panic has trashed
 * everything else. Every path that must be able to report a failure uses it.
 */
#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <types.h>

#define COM1 0x3F8

/* Line status register bits. */
#define UART_LSR_DATA_READY  0x01
#define UART_LSR_TX_EMPTY    0x20

void serial_init(void);
void serial_puts(const char *s);

/* Non-blocking: returns true if a byte was consumed. */
bool serial_getc(char *out);

/* Blocking write with a bounded spin, for panic paths where interrupts are off
 * and an interrupt-driven transmit-complete interrupt will never fire. */
void serial_putc_blocking(char c);

#endif /* DRIVERS_SERIAL_H */
