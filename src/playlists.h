// Playlists, stored as M3U files in data/playlists/<name>.m3u so other
// players can open them too. A cover in MUSIC/PLAYLIST ART/<name>.jpg (or
// .png) replaces the album collage.
#ifndef BORO_PLAYLISTS_H
#define BORO_PLAYLISTS_H

#define PLAYLIST_NAME_MAX 24

typedef struct {
    char name[PLAYLIST_NAME_MAX + 1];
    int *songs, count, cap;
    char art[320]; // cover image path, or "" for the collage
} Playlist;

extern Playlist *playlists; // A-Z by name
extern int playlist_count;

void playlists_load(void); // once the library is ready
// These return the playlist's (new) index, or -1 when the name is taken or
// the file can't be written.
int playlist_create(const char *name);
int playlist_rename(int index, const char *name);
void playlist_delete(int index);
int playlist_add(int index, int song); // 0 when the song is already in it
void playlist_remove(int index, int pos);
// Up to 4 different albums' art songs for the collage. Returns how many.
int playlist_collage(const Playlist *p, int *art_songs);

#endif
