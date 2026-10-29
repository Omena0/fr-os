/*
 * ps2mouse.h — PS/2 auxiliary-port (mouse) driver.
 *
 * The 8042 can proxy a second PS/2 device on its auxiliary port; the keystrokes
 * and mouse packets share the output buffer and are distinguished by the
 * auxiliary bit in the status byte. This driver owns the auxiliary device: it
 * enables it, decodes its three-byte movement packets and publishes an absolute
 * cursor position.
 */
#ifndef DRIVERS_PS2MOUSE_H
#define DRIVERS_PS2MOUSE_H

#include <interrupt.h>
#include <types.h>
#include <uapi/mouse.h>

/* Enable the auxiliary port and data reporting and install IRQ12. Call after
 * ps2kbd_init(), which writes the controller configuration byte. */
void ps2mouse_init(void);

/* IRQ12 handler (VECTOR_IRQ_BASE + 12). */
void ps2mouse_irq(struct interrupt_frame *frame);

/*
 * Feed one byte read from the 8042 output buffer that the status byte marked
 * as auxiliary. Called by the keyboard IRQ handler when a mouse packet byte
 * arrives on IRQ1: the two devices share the buffer, so the interrupt that
 * happens to fire first must not drop the other device's data.
 */
void ps2mouse_handle_byte(u8 byte);

/* Copy the current state out. Safe to call from process context while the IRQ
 * updates it. Returns false if no device was detected. */
bool ps2mouse_get_state(struct mouse_state *out);

#endif /* DRIVERS_PS2MOUSE_H */