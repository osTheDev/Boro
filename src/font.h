// FreeType-rasterized fonts packed into one shared GU texture atlas.
#ifndef BORO_FONT_H
#define BORO_FONT_H

#include <psptypes.h>
#include <stddef.h>

typedef struct Font Font;

void font_init(void);
Font *font_load(const void *ttf, size_t size, int px);
void font_finish(void); // upload the atlas once all fonts are loaded

float font_width(const Font *f, const char *s, float tracking);
float font_ascent(const Font *f);
// Draws UTF-8 text with its baseline at y. Returns the pen x after the text.
float font_draw(const Font *f, float x, float y, const char *s, u32 color, float tracking);
// Like font_draw, but cuts the text with an ellipsis to fit max_w.
void font_draw_fit(const Font *f, float x, float y, const char *s, u32 color, float max_w);

#endif
