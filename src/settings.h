// User settings, kept in data/settings.txt, and the Settings screen.
#ifndef BORO_SETTINGS_H
#define BORO_SETTINGS_H

#include "sys.h"

enum { THEME_FLAC, THEME_POD, THEME_DECK, THEME_COUNT };
enum { DECK_ICE, DECK_AMBER, DECK_GREEN, DECK_RED, DECK_WHITE, DECK_PURPLE, DECK_GRADIENT, DECK_COLORS };

typedef struct {
    int theme;      // THEME_*
    int deck_color; // DECK_*
    int deck_speed; // gradient cycle speed, 0..2
} Settings;

extern Settings settings;
extern const char *const theme_names[THEME_COUNT];
extern const char *const deck_color_names[DECK_COLORS];

void settings_load(const char *data_dir);
void settings_save(void);

void settings_enter(void);
// Returns 1 when the user backs out.
int settings_update(const Input *in, float dt);
void settings_draw(void);

#endif
