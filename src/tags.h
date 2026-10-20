// MP3 metadata: ID3v2.2/2.3/2.4 and ID3v1 tags, embedded art location,
// duration from the first frame (Xing/Info/VBRI or CBR) and LAME gapless info.
#ifndef BORO_TAGS_H
#define BORO_TAGS_H

#include <psptypes.h>

enum { ART_NONE, ART_JPEG, ART_PNG };

typedef struct {
    char title[128], artist[128], album[128], album_artist[128];
    int track, disc;
    u32 art_offset, art_size;
    int art_type;
    u32 audio_start;    // first byte the decoder should see (after ID3 and the Info frame)
    u32 audio_end;      // one past the last audio byte (before ID3v1)
    u32 duration_ms;
    int sample_rate;
    int enc_delay;      // LAME encoder delay in samples, -1 when unknown
    int enc_padding;    // LAME end padding in samples
    u32 total_samples;  // exact sample count when the Info frame gives it, else 0
} TrackInfo;

// Reads tags and stream info. Returns 0 on success, negative when the file
// can't be opened or holds no MPEG audio.
int tags_read(const char *path, TrackInfo *out);

#endif
