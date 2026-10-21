// JPEG and PNG decoding to tightly packed RGBA (4 bytes per pixel).
#ifndef BORO_IMAGE_H
#define BORO_IMAGE_H

#include <psptypes.h>

// JPEGs are scaled down while decoding, keeping the shorter side at least
// `want` pixels (0 keeps full size). PNGs always decode at full size. Pictures
// too big to fit in memory return NULL with *w set to -1; broken ones return
// NULL with *w set to 0.
u8 *image_decode(const u8 *data, u32 size, int want, int *w, int *h);

// Same, straight from a file, turned upright when a JPEG's EXIF says so.
u8 *image_load(const char *path, int want, int *w, int *h);

// Just the small preview a camera keeps inside a JPEG, upright, or NULL.
u8 *image_preview(const char *path, int *w, int *h);

// Decodes on the calling thread give up early (returning NULL) once
// cancelled() returns nonzero.
void image_cancel_on(int (*cancelled)(void));

// The biggest block of memory free in one piece, in bytes.
u32 image_room(void);

// The whole file, if it is no bigger than max bytes. Free with free().
u8 *image_read_file(const char *path, u32 max, u32 *size);

#endif
