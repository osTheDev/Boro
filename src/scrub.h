// Scrubbing with the analog stick on now playing, shared by every theme: the
// target position moves while the stick is pushed and is applied when it
// lets go.
#ifndef BORO_SCRUB_H
#define BORO_SCRUB_H

#include "sys.h"

void scrub_update(const Input *in, float dt);
u32 scrub_position(void); // where the progress bar should show
int scrub_active(void);

#endif
