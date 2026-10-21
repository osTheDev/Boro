// Pocket use: HOLD locks Boro (and turns the screen off while music plays),
// pulling the headphones pauses the music, and the headphone remote's
// play/pause and skip keys work.
#ifndef BORO_POCKET_H
#define BORO_POCKET_H

#include "sys.h"

// Once per frame after reading input. Clears the input while locked.
// Returns 1 while the screen is off: skip drawing.
int pocket_frame(Input *in, float dt);
int pocket_screen_off(void);
int pocket_locked(void); // HOLD is on
void pocket_draw(void); // the lock card and the unplugged notice

#endif
