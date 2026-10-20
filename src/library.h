// Music library: memory stick scan, on-disk cache, and the album / artist /
// song views. Artists under MUSIC/RADIO/<Artist>/ are radio-only: they are
// kept in the library for stations but left out of every other view.
#ifndef BORO_LIBRARY_H
#define BORO_LIBRARY_H

#include <psptypes.h>

typedef struct {
    u32 path, title, artist, album, album_artist; // string pool offsets
    u32 size, mtime;                              // detect changed files
    u32 art_offset, art_size;
    u32 duration_ms;
    u32 audio_start, audio_end, total_samples;
    s32 enc_delay, enc_padding, sample_rate;
    u16 track, disc;
    u8 art_type, radio;
    u8 pad[2];
    // Filled in after loading, not stored in the cache.
    s32 album_id, artist_id, radio_id;
} Song;

typedef struct {
    const char *name, *artist;
    int *songs, count; // disc / track order
    int art_song;      // a song with embedded art, or -1
    int artist_id;
} Album;

typedef struct {
    const char *name;
    int *albums, album_count; // A-Z
    int *songs, song_count;   // album order
    int art_song;
} Artist;

typedef struct {
    const char *name; // folder name under MUSIC/RADIO
    int *songs, count;
} RadioArtist;

typedef struct {
    Song *songs;
    int song_count;
    Album *albums;
    int album_count;
    Artist *artists;
    int artist_count;
    RadioArtist *radio;
    int radio_count;
    int *songs_az; // browsable songs, A-Z by title
    int songs_az_count;
    int *albums_by_artist; // album ids sorted by artist, then title
} Library;

enum { LIB_IDLE, LIB_SCANNING, LIB_READY };

extern Library lib;

// Starts loading the cache and rescanning ms0:/MUSIC on a background thread.
void library_start(const char *data_dir, const char *music_dir);
int library_state(void);
int library_scan_count(void); // files looked at so far during a scan

const char *lib_str(u32 offset);
static inline const char *song_title(const Song *s) { return lib_str(s->title); }
static inline const char *song_artist(const Song *s) { return lib_str(s->artist); }
static inline const char *song_album(const Song *s) { return lib_str(s->album); }
static inline const char *song_path(const Song *s) { return lib_str(s->path); }

// Album ids A-Z by name, or by artist then name.
int *library_albums_sorted(int by_artist);

// Recently played albums, newest first (album ids).
int library_recent(int *out, int max);
void library_note_played(int song);

#endif
