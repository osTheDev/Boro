// Listening stats: every stretch of listening is logged to data/stats.log,
// and the Stats page and home glance bar sum it up by week, month or all
// time. Radio-only songs count toward listening time and radio stations but
// never toward plays or top songs, artists and albums.
#ifndef BORO_STATS_H
#define BORO_STATS_H

#include <psptypes.h>
#include "sys.h"

enum { PERIOD_WEEK, PERIOD_MONTH, PERIOD_ALL, PERIOD_COUNT };
enum { TOP_SONGS, TOP_ARTISTS, TOP_ALBUMS, TOP_KINDS };

#define TOP_MAX 5

typedef struct {
    u32 ms;                 // listening time, radio included
    int plays, prev_plays;  // prev_plays: the period before, -1 for all time
    int streak;             // days in a row with listening, up to today
    int bar_count;
    u32 bars[7];
    char bar_labels[7][4];
    int today_bar;          // index of the bar that holds today
    int top[TOP_KINDS][TOP_MAX], top_plays[TOP_KINDS][TOP_MAX], top_count[TOP_KINDS];
    char station[64];       // most listened radio station, "" when none
    u32 station_ms;
} Stats;

void stats_load(const char *data_dir); // once the library is ready
void stats_frame(float dt);            // every frame: counts listening time
void stats_flush(void);                // logs the song in progress

void stats_compute(int period, Stats *out);
void stats_format_time(char *out, int size, u32 ms); // "6h 42m"

void stats_enter(void);
// Returns 1 when the user backs out.
int stats_update(const Input *in, float dt);
void stats_draw(float dt);

#endif
