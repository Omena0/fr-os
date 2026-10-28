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

/* IRQ handler for keyboard interrupt (IRQ1 / VECTOR_IRQ_KEYBOARD). */
void ps2kbd_irq(struct interrupt_frame *frame);

#endif /* DRIVERS_PS2KBD_H */