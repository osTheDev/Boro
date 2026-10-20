// MP3 playback on the Media Engine. A decoder thread stays about half a
// second ahead of an output thread, so the next track is opened while the
// current one is still audible (gapless when both share a sample rate).
// Playback keeps going while the UI moves between screens.
#ifndef BORO_PLAYER_H
#define BORO_PLAYER_H

#include <psptypes.h>

void player_init(void);

// Plays songs[start], then the rest of the list. With shuffle, the list is
// shuffled with songs[start] first.
void player_play(const int *songs, int count, int start, int shuffle);

// Where playback was started from, shown by every theme's now playing.
enum { FROM_ALBUM, FROM_ARTIST, FROM_SONGS, FROM_PLAYLIST, FROM_RADIO };
void player_play_from(int kind, const char *name, const int *songs, int count, int start, int shuffle);
const char *player_source(int *kind); // the name, like "Night Drive" or "Nova Lane Radio"
void player_toggle_pause(void);
void player_set_gain(float g); // 0..1, on top of the system volume
void player_next(void);
void player_prev(void); // restarts the song if more than 3 seconds in
void player_set_shuffle(int on);
// Radio mode: next/prev do nothing and the list reshuffles endlessly. Cleared
// by player_play.
void player_set_locked(int on);
// Restarts the current song ms in. Ignored in radio mode.
void player_seek(u32 ms);
// Plays the order[] position index (from player_queue). Ignored in radio mode.
void player_jump(int index);
// Songs after the current one, in play order. *first_index is the order[]
// position of out[0].
int player_queue(int *out, int max, int *first_index);

// Copies up to n of the latest output samples (mono, -1..1). Returns how many.
int player_scope(float *out, int n);
int player_index(int *total); // position in play order, and the list length
int player_song(void); // the audible song, or -1 when nothing is loaded
int player_paused(void);
int player_shuffle(void);
int player_locked(void);
u32 player_position_ms(void);

#endif
