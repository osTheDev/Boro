// The Photos tab of the Video section: albums and loose photos from PICTURE
// in a grid, drawn under the section's tab bar.
#ifndef BORO_PHOTOS_H
#define BORO_PHOTOS_H

#include "sys.h"

void photos_enter(void); // the tab was opened: rescan and make thumbnails
void photos_leave(void);
int photos_update(const Input *in, float dt); // 1 when ○ should leave the section
int photos_viewing(void); // a photo fills the screen; photos_draw draws everything
void photos_draw(float dt);

#endif
