// MP4 playback: H.264 on the PSP's video hardware and AAC on its audio
// codec, each in its own thread. The audio sets the clock and the screen
// shows the newest decoded frame that is due.
#ifndef BORO_VPLAYER_H
#define BORO_VPLAYER_H

#include <psptypes.h>
#include "gfx.h"
#include "mp4.h"

// Returns 0 on success, or a negative error (see mp4_open).
int vplayer_open(const char *path, u32 start_ms);
void vplayer_close(void);
const Mp4 *vplayer_mp4(void);

void vplayer_toggle_pause(void);
int vplayer_paused(void);
void vplayer_seek(u32 ms);
u32 vplayer_position(void);
int vplayer_ended(void);
int vplayer_video_ok(void); // 0 once the video hardware has failed to give a picture
void vplayer_set_gain(int percent); // 0..100

// The frame due now, or NULL before the first one. Call once per frame.
const Texture *vplayer_frame(void);

#endif
