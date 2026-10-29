/*
 * kbd.h — keyboard repeat (typematic) control.
 *
 * The PS/2 keyboard repeats a held key by itself. The delay before the first
 * repeat and the rate afterwards are one byte: bits 6-5 select the delay
 * (00 = 250 ms, 01 = 500 ms, 10 = 750 ms, 11 = 1000 ms) and bits 4-0 the rate
 * (0 = about 30 cps, 31 = 2 cps).
 *
 * A program that needs different behaviour from the prompt can change it and
 * put the old value back on exit:
 *
 *     int saved = kbd_typematic(KBD_TYPEMATIC_FAST);
 *     ...
 *     kbd_typematic(saved);
 *
 * This affects the PS/2 keyboard only. A serial terminal's auto-repeat comes
 * from the terminal emulator, not the guest.
 */
#ifndef KBD_H
#define KBD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 250 ms delay, ~13 cps: quick to start, calm enough for a text prompt. */
#define KBD_TYPEMATIC_DEFAULT  0x05
/* 250 ms delay, ~30 cps: for games that repeat a held direction. */
#define KBD_TYPEMATIC_FAST     0x00

/*
 * Set the typematic byte, returning the previous one. Pass a negative value
 * to query without changing anything. Returns -1 with errno on failure.
 */
int kbd_typematic(int value);

/*
 * Copy the held-scancode bitmap out: bit n is set while make code n is down.
 * A game uses this to build its own repeat timing instead of relying on the
 * keyboard's typematic delay, which cannot go below 250 ms.
 *
 * Returns 0, or -1 with errno. Scancodes for letters, digits, space, comma and
 * period are all below 64.
 */
int kbd_state(uint64_t *mask);

/* Scancodes of the keys games tend to hold. Set 1, US/Finnish physical keys. */
#define KBD_SC_A     0x1E
#define KBD_SC_S     0x1F
#define KBD_SC_D     0x20
#define KBD_SC_C     0x2E
#define KBD_SC_COMMA 0x33
#define KBD_SC_DOT   0x34
#define KBD_SC_SPACE 0x39

#ifdef __cplusplus
}
#endif

#endif /* KBD_H */
