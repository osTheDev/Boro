// Photo library: JPEGs and PNGs under PICTURE. Each folder there is an album
// holding everything inside it, subfolders too; pictures at the top level are
// loose. Thumbnails are made in the background and cached in data/pthumbs.
#ifndef BORO_PLIB_H
#define BORO_PLIB_H

#include <psptypes.h>
#include "gfx.h"

#define PTHUMB_W 106
#define PTHUMB_H 70

typedef struct {
    char path[256];
    char name[64];
    u32 modified; // file time, seconds
    int thumb;    // 0 not made yet, 1 made, -1 can't be read
} Photo;

typedef struct {
    char name[64];
    int first, count; // its photos sit together in photos[]
} PhotoAlbum;

// Albums by name, each with its photos by name, then the loose photos.
extern Photo *photos;
extern int photo_count, loose_first;
extern PhotoAlbum *palbums;
extern int palbum_count;

void plib_init(const char *data_dir, const char *picture_dir);
const char *plib_dir(void); // like ms0:/PICTURE
void plib_scan(void);

void plib_thumbs(int on);
void plib_thumbs_wait(void); // until the thumbnail in progress (if any) is done
void plib_want(int index); // made next; call each frame for what's on screen
const Texture *plib_thumb(int index); // PTHUMB_W x PTHUMB_H, or NULL

#endif
