// H.264 decoding on the Media Engine through mpeg_vsh.prx: access units in
// MP4 form (length-prefixed NAL units) in, 32-bit pixels in a 512-wide
// buffer out. There is one decoder; open it again for a new file.
#ifndef BORO_AVC_H
#define BORO_AVC_H

#include "mp4.h"

#define AVC_STRIDE 512

int avc_available(void);
const char *avc_status(void); // "ok", or the first step that failed and its error // 0 when the firmware modules couldn't be loaded
// Returns 0, or a negative error.
int avc_open(const Mp4 *m);
void avc_close(void);
// Decodes one sample. For each picture that comes out, in display order,
// calls out(pixels, width, height, ctx); pixels are 0xAABBGGRR, AVC_STRIDE
// apart, or NULL when convert is 0 (a picture that will be dropped). first
// is 1 for the first sample after open. Returns the number of pictures, or
// a negative error.
typedef void (*AvcOut)(const u32 *px, int w, int h, void *ctx);
int avc_decode(const void *data, int size, int first, int convert, AvcOut out, void *ctx);

#endif
