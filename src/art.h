// Album art decoded from embedded ID3 pictures on a worker thread, cached as
// textures with rounded corners baked in.
#ifndef BORO_ART_H
#define BORO_ART_H

#include "gfx.h"

enum { ART_SMALL, ART_LARGE, ART_ROUND, ART_KINDS }; // 72px, 150px, 72px circle

void art_init(void);
// Returns the texture for a song's embedded art, or NULL while it loads or
// when the song has none. Call every frame the art is visible.
const Texture *art_get(int song, int kind);
// Average color of the art (opaque), or the fallback while unknown.
u32 art_color(int song, u32 fallback);
void art_frame(void); // once per frame: collects finished decodes

#endif
