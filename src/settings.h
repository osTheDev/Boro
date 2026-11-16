// User settings, kept in data/settings.txt, and the Settings screen.
#ifndef BORO_SETTINGS_H
#define BORO_SETTINGS_H

#include "sys.h"

enum { THEME_FLAC, THEME_POD, THEME_DECK, THEME_COUNT };
enum { DECK_ICE, DECK_AMBER, DECK_GREEN, DECK_RED, DECK_WHITE, DECK_PURPLE, DECK_GRADIENT, DECK_COLORS };

enum { DELAY_15S, DELAY_30S, DELAY_1M, DELAY_2M, DELAY_COUNT };
enum { FIT_FIT, FIT_ZOOM, FIT_STRETCH, FIT_COUNT };
enum { HOME_STREAM, HOME_WEB, HOME_STYLES };

typedef struct {
    int theme;      // THEME_*
    int deck_color; // DECK_*
    int deck_speed; // gradient cycle speed, 0..2
    int keep_on;    // screen stays lit while music plays
    int idle_vis;   // idle visualizer on
    int idle_delay; // DELAY_*
    int vis_style;  // 0 rotates every scene, else one scene (1-based)
    int resume_videos;
    int video_fit;  // FIT_*, for videos without their own choice
    int home_style; // HOME_*
    int book_speed; // index into book_speeds
} Settings;

extern Settings settings;
extern const char *const theme_names[THEME_COUNT];
extern const char *const deck_color_names[DECK_COLORS];
extern const char *const fit_names[FIT_COUNT];
extern const char *const home_style_names[HOME_STYLES];
#define BOOK_SPEEDS 6
extern const float book_speeds[BOOK_SPEEDS]; // 1, 1.25, 1.5, 1.75, 2, 0.75

void settings_load(const char *data_dir);
void settings_save(void);
float settings_idle_seconds(void);

// Sleep timer (not saved): counts down while music plays, then fades the
// music out and pauses it. Call once per frame.
void settings_frame(float dt);
void settings_sleep_cycle(void);  // Off, 15, 30, 45, 60, 90 minutes, then Off
int settings_sleep_left(void);    // minutes left, or 0 when off

void settings_enter(void);
// Returns 1 when the user backs out.
int settings_update(const Input *in, float dt);
void settings_draw(void);

#endif
