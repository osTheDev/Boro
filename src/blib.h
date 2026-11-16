// Audiobook library: each folder under AUDIOBOOKS holding MP3s is one book
// (folders of folders, like AUDIOBOOKS/<Author>/<Book>, work too), and each
// MP3 loose in AUDIOBOOKS is a book of its own. Files play in name order
// (numbers counted as numbers). Covers come from cover/folder .jpg/.png in the
// book's folder, else the art embedded in its first MP3. Where each book was
// left is kept in data/books.txt.
#ifndef BORO_BLIB_H
#define BORO_BLIB_H

#include <psptypes.h>
#include "player.h"

typedef struct {
    char dir[256];        // the book's folder, or the file for a single-MP3 book
    char title[96];
    char author[96];      // from the tags, or ""
    char cover[300];      // picture file, or "" for none
    int track_count;
    BookTrack *tracks;    // NULL until the background pass has read them
    char (*names)[96];    // each track's title
    u32 total_ms;         // 0 until read
    int pos_track;        // where it was left
    u32 pos_ms;
    char last[20];        // "YYYY-MM-DD HH:MM:SS" last listened, or ""
    int finished;
} Book;

extern Book *books;
extern int book_count;

void blib_init(const char *data_dir, const char *books_dir);
const char *blib_dir(void);  // like ms0:/AUDIOBOOKS
void blib_scan(void);        // finds books and reads their tracks in the background (once)
int blib_ready(void);        // every book's tracks are read
u32 blib_elapsed(int book);  // listened so far, from the saved position
// Starts the book where it was left (from the start once finished), or at a
// track's start.
void blib_play(int book);
void blib_play_track(int book, int track);
void blib_restart(int book); // back to the start, not finished
int blib_playing(void);      // the book loaded in the player, or -1
// Main loop, every frame: notes the position now and then, and when a book ends.
void blib_frame(float dt);
void blib_save(void);        // writes the current position now
int blib_current(void);      // the book listened to last that isn't finished, or -1

#endif
