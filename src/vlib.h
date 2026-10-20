// Video library: MP4s under VIDEO (Movies, Shows and Clips folders become
// tabs), thumbnails made in the background and cached in data/thumbs, and
// where each video was stopped, kept in data/video.txt.
#ifndef BORO_VLIB_H
#define BORO_VLIB_H

#include <psptypes.h>
#include "gfx.h"

enum { VCAT_OTHER, VCAT_MOVIES, VCAT_SHOWS, VCAT_CLIPS, VCAT_COUNT };

typedef struct {
    char path[256];
    char title[96];
    int category;
    u32 modified;      // file time, seconds
    u32 duration_ms;   // 0 until the thumbnail pass has read it
    int width, height;
    int thumb;         // 0 not made yet, 1 made, -1 no picture
    u32 resume_ms;     // where it was stopped, 0 for the start
    u32 watched;       // last time it was played, seconds; 0 never
} Video;

extern Video *videos;
extern int video_count;

void vlib_init(const char *data_dir, const char *video_dir);
void vlib_scan(void); // rereads the folder; keeps what's known

// Thumbnails are made one at a time while allowed; vlib_thumbs(0) waits for
// the one in progress so the decoder is free for playback.
void vlib_thumbs(int on);
const Texture *vlib_thumb(int index); // 160x90, or NULL

void vlib_set_resume(int index, u32 ms); // saves; ms near the end counts as finished
int vlib_continue(void); // the latest video stopped partway, or -1

#endif
