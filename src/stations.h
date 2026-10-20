// Artist radio stations. Every radio-only artist (MUSIC/RADIO/<Artist>/) is a
// station automatically; stations for library artists are created by the
// user and kept in data/stations.txt.
#ifndef BORO_STATIONS_H
#define BORO_STATIONS_H

typedef struct {
    int radio; // 1: lib.radio[id], 0: lib.artists[id]
    int id;
} Station;

extern Station *stations;
extern int station_count;

void stations_load(void); // once the library is ready
const char *station_name(const Station *s);
int station_art_song(const Station *s); // -1 when none of its songs has art
void station_songs(const Station *s, int **songs, int *count);
// Returns the station's index, creating it when needed.
int stations_add(int radio, int id);
// Starts the station: its songs shuffled endlessly, no skips. Returns 0 when
// it has no songs.
int station_play(int index);
// Only user-created stations can be removed. Returns 1 when removed.
int stations_remove(int index);

#endif
