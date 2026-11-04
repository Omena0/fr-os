#ifndef TETRIS_SOUND_H
#define TETRIS_SOUND_H

#include <stdbool.h>

void tetris_sound_init(void);
void tetris_sound_rotate(void);
void tetris_sound_lock(bool spin, int cleared);
void tetris_sound_shutdown(void);

#endif
