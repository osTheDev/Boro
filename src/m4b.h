// M4B / M4A audiobooks: AAC audio in an MP4 file. The sample tables stay in
// the file and are read as playback walks through them, so a long book needs
// almost no memory. Chapters come from a Nero chpl box or a QuickTime chapter
// track; title, author and cover from the iTunes tags.
#ifndef BORO_M4B_H
#define BORO_M4B_H

#include <psptypes.h>

#define M4B_CHAPTERS 512

typedef struct {
    u32 duration_ms;
    int rate, channels;
    char title[128], author[128];
    u32 cover_offset, cover_size; // the covr picture inside the file, or 0
    int cover_png;
    int chapter_count;            // 0 when the file has none
    u32 chapter_ms[M4B_CHAPTERS]; // start of each
    char chapter_name[M4B_CHAPTERS][64];
} M4bInfo;

// Reads the tags and chapters. Returns 0, or negative when it isn't an AAC
// audio file we can play.
int m4b_info(const char *path, M4bInfo *out);

// A file open for playing.
typedef struct M4b M4b;
M4b *m4b_open(const char *path);
void m4b_close(M4b *m);
int m4b_rate(const M4b *m);
// Moves to the AAC frame holding ms; returns that frame's start in ms.
u32 m4b_seek(M4b *m, u32 ms);
// Reads the next AAC frame into buf (cap bytes). Returns its size, 0 at the
// end, or negative on a read error.
int m4b_next(M4b *m, u8 *buf, u32 cap);
u32 m4b_max_frame(const M4b *m);

#endif
