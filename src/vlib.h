// Video library: MP4s under VIDEO sorted onto shelves by folder name
// (Movies, Shows, Music, Clips). Each folder under Shows is one show, with
// seasons from "Season N" folders or S01E03-style names. Thumbnails are made
// in the background and cached in data/thumbs; where each video was stopped
// is kept in data/video.txt.
#ifndef BORO_VLIB_H
#define BORO_VLIB_H

#include <psptypes.h>
#include "gfx.h"

enum { VCAT_OTHER, VCAT_MOVIES, VCAT_MUSIC, VCAT_CLIPS, VCAT_SHOWS, VCAT_COUNT };

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
    int fit;           // FIT_* picked while watching, -1 for the Settings default
    int show;          // index into shows, or -1
    int season, episode; // 0 when unknown
} Video;

typedef struct {
    char name[96];
    char dir[256];
    int count;         // episodes
    int watched;       // episodes watched to the end
} Show;

extern Video *videos;
extern int video_count;
extern Show *shows;
extern int show_count;

void vlib_init(const char *data_dir, const char *video_dir);
const char *vlib_dir(void); // like ms0:/VIDEO
void vlib_scan(void); // rereads the folder; keeps what's known

// Thumbnails are made one at a time while allowed; vlib_thumbs(0) waits for
// the one in progress so the decoder is free for playback.
void vlib_thumbs(int on);
const Texture *vlib_thumb(int index); // 160x90, or NULL

void vlib_set_resume(int index, u32 ms); // saves; ms near the end counts as finished
void vlib_set_fit(int index, int fit);
int vlib_continue(void); // the latest video stopped partway, or -1

// A show's episodes in watching order (season, then episode, then title).
int vlib_episodes(int show, int *out, int max);
// The episode to play next: one stopped partway, else the one after the
// last watched, else the first.
int vlib_next_up(int show);
u32 vlib_last_used(int show); // latest watched or added time of its episodes

// Portrait poster art (POSTER_W x POSTER_H), from cover.jpg/png or
// poster.jpg/png in a show's folder, or <video name>.jpg/png beside a movie.
// NULL when there is none; callers fall back to the thumbnail.
#define POSTER_W 64
#define POSTER_H 96
const Texture *vlib_show_poster(int show);
const Texture *vlib_video_poster(int index);

#endif
