// iPod theme: a classic menu on the left with art on the right.
#ifndef BORO_POD_H
#define BORO_POD_H

#include "sys.h"

void pod_enter(void);
// Returns a MUSIC_* action.
int pod_update(const Input *in, float dt);
void pod_draw(float dt);

#endif
