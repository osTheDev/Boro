// Game library: PSP games on the memory stick (ISO and CSO files under ISO,
// EBOOT.PBP games under PSP/GAME) with titles and icons read from the games
// themselves, plus the playtime the Boro Playtime plugin logs to
// data/playtime.txt. Games are found and their icons decoded on a background
// thread; nothing is ever moved or renamed.
#ifndef BORO_GLIB_H
#define BORO_GLIB_H

#include <psptypes.h>
#include "gfx.h"

enum { GTYPE_PSP, GTYPE_PS1, GTYPE_HOMEBREW };

#define GICON_W 108
#define GICON_H 60

typedef struct {
    char path[256];
    char title[96];
    char disc_id[16];
    int type;
    u32 seconds;        // played in total
    u32 sessions;
    char last[20];      // "YYYY-MM-DD HH:MM:SS" the last session started, or ""
    volatile int icon;  // 0 not read yet, 1 decoded, 2 uploaded, -1 none
    u8 *icon_px;        // GICON_W x GICON_H RGBA until uploaded
    Texture *tex;
} Game;

extern Game *games;
extern int game_count;

// Folds the plugin's last session into the log and reads the totals.
void glib_init(const char *data_dir, const char *app_dir);
void glib_scan(void);   // starts looking for games (once)
int glib_ready(void);   // the list is in (icons may still be coming)
void glib_frame(void);  // main thread: takes in the scan and uploads icons
const Texture *glib_icon(int index);

// The game played last, from the log even before the scan: title or NULL.
const char *glib_last_title(void);
const char *glib_app_dir(void); // Boro's own folder, like ms0:/PSP/GAME/Boro
int glib_last_game(void); // index into games, or -1

// The game page: seconds played on each of the last 7 days (today last), the
// first session, and the file's size.
typedef struct {
    u32 days[7];
    char first[20];
    u64 bytes;
} GameDetail;
void glib_detail(int index, GameDetail *d);
u8 *glib_pic1(int index, int *w, int *h); // RGBA, or NULL; free() it
int glib_launch(int index);               // starts the game; returns only on failure

// Game time for the Stats page and home bar: span_days back from today (0 for
// all time), the last 7 days one by one (today last), and the top 5 games.
typedef struct {
    u32 seconds;
    u32 days[7];
    int top_count;
    const char *top_title[5];
    int top_game[5]; // into games, or -1 before the scan or if it's gone
    u32 top_secs[5];
} GameStats;
void glib_stats(int span_days, GameStats *out);

void glib_format_time(char *out, int size, u32 seconds); // "42h 10m", "52m"
void glib_format_last(char *out, int size, const char *last); // "Today", "3 days ago", "Oct 2"

#endif
