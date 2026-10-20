// MP4 reader: finds the H.264 and AAC tracks and where each sample sits in
// the file, so the player can read them one at a time.
#ifndef BORO_MP4_H
#define BORO_MP4_H

#include <psptypes.h>

typedef struct {
    int count;
    u32 timescale;
    u32 *offset, *size;
    u32 *dts;      // decode time of each sample, in timescale units
    s32 *cts;      // composition offset, or NULL when there are no B-frames
    int *sync;     // keyframe sample indexes, or NULL when every sample is one
    int sync_count;
} Mp4Track;

typedef struct {
    u32 duration_ms;
    int has_video, has_audio;
    // H.264
    Mp4Track video;
    int width, height, profile, nal_length;
    u8 *sps, *pps;
    int sps_size, pps_size;
    // AAC
    Mp4Track audio;
    int sample_rate, channels;
} Mp4;

// Returns 0 on success, or a negative error. mp4_close frees what it holds.
int mp4_open(Mp4 *m, const char *path);
void mp4_close(Mp4 *m);

u32 mp4_ms(const Mp4Track *t, u32 ticks);
int mp4_sample_at(const Mp4Track *t, u32 ms); // last sample starting at or before ms
int mp4_keyframe_before(const Mp4 *m, int sample);

#endif
