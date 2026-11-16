// Boro home screen: Music / Video / Games / Books tiles in the Stream or Web
// style, this week's stats, status bar.
#ifndef BORO_HOME_H
#define BORO_HOME_H

#include "sys.h"

enum { HOME_STAY, HOME_OPEN_MUSIC, HOME_OPEN_SETTINGS, HOME_OPEN_STATS, HOME_OPEN_VIDEO, HOME_OPEN_GAMES,
       HOME_NOW_PLAYING, HOME_RESUME_VIDEO, HOME_PLAY_GAME };

void home_init(void);
int home_update(const Input *in, float dt);
void home_draw(void);
void home_backdrop(void);      // the glow behind home, also used by the splash
void home_draw_no_music(void); // help card when MUSIC is empty

#endif
