// Games: Recent and All Games grids with each game's icon and playtime.
#ifndef BORO_GAMES_H
#define BORO_GAMES_H

#include "sys.h"

void games_enter(void);
void games_play(int index); // opens the game's page and starts it
int games_update(const Input *in, float dt); // nonzero: back to home
void games_draw(float dt);

#endif
