// FreeType fonts rendered on demand into shared 8-bit alpha atlas pages.
#ifndef BORO_FONT_H
#define BORO_FONT_H

#include <psptypes.h>
#include <stddef.h>

typedef struct Font Font;

void font_init(void);
Font *font_load(const void *ttf, size_t size, int px);

float font_width(Font *f, const char *s, float tracking);
float font_ascent(const Font *f);
// Draws UTF-8 text with its baseline at y. Returns the pen x after the text.
float font_draw(Font *f, float x, float y, const char *s, u32 color, float tracking);
// Like font_draw, but cuts the text with an ellipsis to fit max_w.
void font_draw_fit(Font *f, float x, float y, const char *s, u32 color, float max_w);
// Right-aligned and centered variants (single line, no tracking).
void font_draw_right(Font *f, float right, float y, const char *s, u32 color);
void font_draw_center(Font *f, float cx, float y, const char *s, u32 color);

#endif
