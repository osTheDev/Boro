// Full-screen photo viewer: left/right flips through a run of photos, L/R
// zooms 1x/2x/4x, the analog stick pans, and the name bar fades after 2s.
#ifndef BORO_PVIEWER_H
#define BORO_PVIEWER_H

#include "sys.h"

// Shows photos[first + index] out of photos[first .. first + count).
// context names the run ("Photos" or an album) in the top bar.
void pviewer_open(int first, int count, int index, const char *context);
int pviewer_update(const Input *in, float dt); // 1 once closed with ○
void pviewer_draw(void);                      // the whole screen
int pviewer_current(void);                    // index within the run

#endif
