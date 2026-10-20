// Boro home screen: Music / Video tiles, at-a-glance stats bar, status bar.
#ifndef BORO_HOME_H
#define BORO_HOME_H

#include "sys.h"

enum { HOME_STAY, HOME_OPEN_MUSIC, HOME_OPEN_SETTINGS };

void home_init(void);
int home_update(const Input *in, float dt);
void home_draw(void);

#endif
