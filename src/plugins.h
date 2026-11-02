// Settings > Plugins: turns the Boro plugins on or off in ARK's plugin list
// and changes their options in SEPLUGINS/boro.ini. The plugins read both
// when the PSP starts, so changes apply after a restart.
#ifndef BORO_PLUGINS_H
#define BORO_PLUGINS_H

#include "sys.h"

void plugins_enter(void);
int plugins_on_count(void);              // plugins installed and turned on
int plugins_update(const Input *in, float dt); // 1 to go back
void plugins_draw(void);

#endif
