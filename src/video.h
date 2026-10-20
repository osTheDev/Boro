// Video: the thumbnail browser and the full-screen player.
#ifndef BORO_VIDEO_H
#define BORO_VIDEO_H

#include "sys.h"

void video_enter(void);
// Returns 1 when the user backs out to home.
int video_update(const Input *in, float dt);
void video_draw(float dt);
int video_playing(void); // the player is up (no idle visualizer)

#endif
