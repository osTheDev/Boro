#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#define ATLAS 512
#define EXTRA_COUNT 4

typedef struct { short x, y, w, h, left, top; float adv; } Glyph;

struct Font {
    Glyph ascii[95];
    Glyph extra[EXTRA_COUNT];
    float ascent;
};

static const u32 extra_codes[EXTRA_COUNT] = { 0x00B7, 0x2026, 0x203A, 0x2014 };
static FT_Library ft;
static Texture *atlas;
static int pen_x = 1, pen_y = 1, row_h;

void font_init(void)
{
    FT_Init_FreeType(&ft);
    atlas = tex_create(ATLAS, ATLAS);
}

static void bake(FT_Face face, u32 code, Glyph *g)
{
    memset(g, 0, sizeof(*g));
    if (FT_Load_Char(face, code, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return;
    FT_GlyphSlot s = face->glyph;
    int w = s->bitmap.width, h = s->bitmap.rows;
    g->adv = s->advance.x / 64.0f;
    g->left = s->bitmap_left;
    g->top = s->bitmap_top;
    if (pen_x + w + 1 > ATLAS) {
        pen_x = 1;
        pen_y += row_h + 1;
        row_h = 0;
    }
    if (pen_y + h + 1 > ATLAS) return; // atlas full: glyph keeps its advance only
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 a = s->bitmap.buffer[y * s->bitmap.pitch + x];
            atlas->px[(pen_y + y) * atlas->tw + pen_x + x] = 0x00FFFFFF | (a << 24);
        }
    g->x = pen_x;
    g->y = pen_y;
    g->w = w;
    g->h = h;
    pen_x += w + 1;
    if (h > row_h) row_h = h;
}

Font *font_load(const void *ttf, size_t size, int px)
{
    FT_Face face;
    if (FT_New_Memory_Face(ft, ttf, size, 0, &face)) return NULL;
    FT_Set_Pixel_Sizes(face, 0, px);
    Font *f = calloc(1, sizeof(Font));
    f->ascent = face->size->metrics.ascender / 64.0f;
    for (int c = 0; c < 95; c++) bake(face, 32 + c, &f->ascii[c]);
    for (int i = 0; i < EXTRA_COUNT; i++) bake(face, extra_codes[i], &f->extra[i]);
    FT_Done_Face(face);
    return f;
}

void font_finish(void)
{
    tex_upload(atlas);
}

static u32 next_code(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    u32 c = *p++;
    if (c >= 0xF0) { c = (c & 0x07) << 18 | (p[0] & 0x3F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
    else if (c >= 0xE0) { c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
    else if (c >= 0xC0) { c = (c & 0x1F) << 6 | (p[0] & 0x3F); p += 1; }
    *s = (const char *)p;
    return c;
}

static const Glyph *glyph(const Font *f, u32 c)
{
    if (c >= 32 && c < 127) return &f->ascii[c - 32];
    for (int i = 0; i < EXTRA_COUNT; i++)
        if (extra_codes[i] == c) return &f->extra[i];
    return &f->ascii['?' - 32];
}

float font_width(const Font *f, const char *s, float tracking)
{
    float w = 0;
    while (*s) w += glyph(f, next_code(&s))->adv + tracking;
    return w > 0 ? w - tracking : 0;
}

float font_ascent(const Font *f)
{
    return f->ascent;
}

float font_draw(const Font *f, float x, float y, const char *s, u32 color, float tracking)
{
    x = (int)(x + 0.5f);
    y = (int)(y + 0.5f);
    while (*s) {
        const Glyph *g = glyph(f, next_code(&s));
        if (g->w) gfx_tex_part(atlas, g->x, g->y, g->w, g->h, (int)(x + g->left), y - g->top, g->w, g->h, color);
        x += g->adv + tracking;
    }
    return x;
}

void font_draw_fit(const Font *f, float x, float y, const char *s, u32 color, float max_w)
{
    if (font_width(f, s, 0) <= max_w) {
        font_draw(f, x, y, s, color, 0);
        return;
    }
    char buf[256];
    float dots = font_width(f, "\xE2\x80\xA6", 0);
    size_t n = 0;
    const char *p = s;
    float w = 0;
    while (*p && n < sizeof(buf) - 4) {
        const char *q = p;
        float a = glyph(f, next_code(&q))->adv;
        if (w + a + dots > max_w) break;
        memcpy(buf + n, p, q - p);
        n += q - p;
        w += a;
        p = q;
    }
    memcpy(buf + n, "\xE2\x80\xA6", 4);
    font_draw(f, x, y, buf, color, 0);
}
