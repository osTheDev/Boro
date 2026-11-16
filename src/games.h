// Games: Recent and All Games grids with each game's icon and playtime.
#ifndef BORO_GAMES_H
#define BORO_GAMES_H

#include "sys.h"

void games_enter(void);
int games_update(const Input *in, float dt); // nonzero: back to home
void games_draw(float dt);

#endif
