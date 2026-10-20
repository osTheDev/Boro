// FL/AC music theme (Spotify-style).
#ifndef BORO_FLAC_H
#define BORO_FLAC_H

#include "sys.h"

void flac_enter(void);
// Returns 1 when the user backs out to the Boro home screen.
int flac_update(const Input *in, float dt);
void flac_draw(float dt);

#endif
