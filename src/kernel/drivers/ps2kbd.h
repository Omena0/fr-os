/*
 * ps2kbd.h — PS/2 keyboard driver.
 *
 * Handles 8042 controller initialization and scancode translation to ASCII.
 * The driver delivers keystrokes through a registered callback.
 */
#ifndef DRIVERS_PS2KBD_H
#define DRIVERS_PS2KBD_H

#include <interrupt.h>

/* Callback for delivering translated keystrokes. Called from IRQ context
 * with interrupts disabled. */
typedef void (*ps2kbd_callback_t)(char c);

/* Initialize the PS/2 controller and keyboard. Registers the callback for
 * keystroke delivery. Must be called after IDT is initialized but before
 * the keyboard interrupt line is unmasked. */
void ps2kbd_init(ps2kbd_callback_t callback);

/* Change the keyboard's typematic delay/rate byte and return the previous
 * value. Bits 6-5 are the delay (00 = 250 ms) and bits 4-0 the repeat rate
 * (0 = fastest). Process context only; it talks to the 8042 directly. */
u8 ps2kbd_set_typematic(u8 value);

/* The last value this driver applied or was asked to apply. */
u8 ps2kbd_get_typematic(void);

/* IRQ handler for keyboard interrupt (IRQ1 / VECTOR_IRQ_KEYBOARD). */
void ps2kbd_irq(struct interrupt_frame *frame);

#endif /* DRIVERS_PS2KBD_H */