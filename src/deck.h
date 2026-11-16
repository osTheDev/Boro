// Head unit theme: a pixel car stereo display in one of several colors.
#ifndef BORO_DECK_H
#define BORO_DECK_H

#include "sys.h"

void deck_enter(void);
void deck_colors(u32 *fg, u32 *bg); // the head unit color picked in Settings
// Returns a MUSIC_* action.
int deck_update(const Input *in, float dt);
void deck_draw(float dt);

#endif
