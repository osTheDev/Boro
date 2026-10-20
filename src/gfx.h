// GU drawing helpers: frame setup, rects, textured sprites, 9-slice panels
// and procedurally generated alpha textures (glows, rounded panels, icons).
#ifndef BORO_GFX_H
#define BORO_GFX_H

#include <psptypes.h>

#define SCREEN_W 480
#define SCREEN_H 272

// The GU expects colors as 0xAABBGGRR.
#define RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))
#define HEX(rgb, a) RGBA(((rgb) >> 16) & 0xFF, ((rgb) >> 8) & 0xFF, (rgb) & 0xFF, (a))

u32 color_alpha(u32 c, float a); // scale a color's alpha by a (0..1)
u32 color_lerp(u32 a, u32 b, float t);

typedef struct {
    int w, h;   // used size
    int tw, th; // allocated power-of-two size
    u32 *px;    // tw * th pixels, 16-byte aligned
} Texture;

Texture *tex_create(int w, int h);
void tex_upload(Texture *t); // call after writing pixels

// Alpha-only textures (white RGB), tinted when drawn.
Texture *tex_glow(int size);                              // soft radial falloff
Texture *tex_round_fill(float r);                         // rounded rect for 9-slice, border = ceil(r)
Texture *tex_round_ring(float r, float width);            // rounded outline for 9-slice
Texture *tex_round_shadow(float r, float blur);           // blurred rounded rect, border = ceil(r + blur)
Texture *tex_round_gradient(int w, int h, float r, u32 c0, u32 c1); // 135deg gradient with rounded corners

// Vector-ish shapes stroked into an alpha texture (accumulates coverage).
void tex_stroke_line(Texture *t, float x0, float y0, float x1, float y1, float width);
void tex_stroke_circle(Texture *t, float cx, float cy, float r, float width);
void tex_fill_circle(Texture *t, float cx, float cy, float r);
void tex_fill_poly(Texture *t, const float *xy, int n);
void tex_stroke_round_rect(Texture *t, float x, float y, float w, float h, float r, float width);

void gfx_init(void);
void gfx_begin(u32 clear);
void gfx_end(void);

void gfx_additive(int on);
void gfx_rect(float x, float y, float w, float h, u32 color);
void gfx_tex(const Texture *t, float x, float y, float w, float h, u32 color);
void gfx_tex_part(const Texture *t, float sx, float sy, float sw, float sh,
                  float x, float y, float w, float h, u32 color);
void gfx_nine(const Texture *t, int border, float x, float y, float w, float h, u32 color);

#endif
