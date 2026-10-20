#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#define PAGE 512
#define MAX_PAGES 6

typedef struct {
    u32 code;
    short x, y, w, h, left, top;
    unsigned char page, used;
    float adv;
} Glyph;

struct Font {
    FT_Face face;
    float ascent;
    Glyph *table; // open addressing, cap entries
    int cap, count;
};

static FT_Library ft;
static Texture *pages[MAX_PAGES];
static int page_count, pen_x = 1, pen_y = 1, row_h;

void font_init(void)
{
    FT_Init_FreeType(&ft);
}

Font *font_load(const void *ttf, size_t size, int px)
{
    Font *f = calloc(1, sizeof(Font));
    if (FT_New_Memory_Face(ft, ttf, size, 0, &f->face)) {
        free(f);
        return NULL;
    }
    FT_Set_Pixel_Sizes(f->face, 0, px);
    f->ascent = f->face->size->metrics.ascender / 64.0f;
    f->cap = 256;
    f->table = calloc(f->cap, sizeof(Glyph));
    return f;
}

static int place(int w, int h)
{
    if (page_count == 0 || pen_x + w + 1 > PAGE) {
        pen_x = 1;
        pen_y += row_h + 1;
        row_h = 0;
    }
    if (page_count == 0 || pen_y + h + 1 > PAGE) {
        if (page_count == MAX_PAGES) return 0;
        pages[page_count++] = tex_create_alpha8(PAGE, PAGE);
        pen_x = pen_y = 1;
        row_h = 0;
    }
    return 1;
}

static void bake(Font *f, Glyph *g)
{
    if (FT_Load_Char(f->face, g->code, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return;
    FT_GlyphSlot s = f->face->glyph;
    if (!s->glyph_index && g->code != ' ') {
        g->w = -1; // missing from this font; drawn as '?'
        return;
    }
    int w = s->bitmap.width, h = s->bitmap.rows;
    g->adv = s->advance.x / 64.0f;
    g->left = s->bitmap_left;
    g->top = s->bitmap_top;
    if (!w || !h || !place(w, h)) return;
    Texture *t = pages[page_count - 1];
    unsigned char *dst = (unsigned char *)t->px;
    for (int y = 0; y < h; y++)
        memcpy(dst + (pen_y + y) * t->tw + pen_x, s->bitmap.buffer + y * s->bitmap.pitch, w);
    tex_upload_rows(t, pen_y, h);
    g->x = pen_x;
    g->y = pen_y;
    g->w = w;
    g->h = h;
    g->page = page_count - 1;
    pen_x += w + 1;
    if (h > row_h) row_h = h;
}

static void grow(Font *f)
{
    Glyph *old = f->table;
    int old_cap = f->cap;
    f->cap *= 2;
    f->table = calloc(f->cap, sizeof(Glyph));
    for (int i = 0; i < old_cap; i++) {
        if (!old[i].used) continue;
        int j = old[i].code & (f->cap - 1);
        while (f->table[j].used) j = (j + 1) & (f->cap - 1);
        f->table[j] = old[i];
    }
    free(old);
}

static const Glyph *glyph(Font *f, u32 code)
{
    int i = code & (f->cap - 1);
    while (f->table[i].used) {
        if (f->table[i].code == code) break;
        i = (i + 1) & (f->cap - 1);
    }
    Glyph *g = &f->table[i];
    if (!g->used) {
        g->used = 1;
        g->code = code;
        bake(f, g);
        if (++f->count * 2 > f->cap) {
            grow(f);
            return glyph(f, code);
        }
    }
    if (g->w < 0) return code == '?' ? g : glyph(f, '?');
    return g;
}

static u32 next_code(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    u32 c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 0x07) << 18 | (p[0] & 0x3F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = (c & 0x1F) << 6 | (p[0] & 0x3F); p += 1; }
    *s = (const char *)p;
    return c;
}

float font_width(Font *f, const char *s, float tracking)
{
    float w = 0;
    while (*s) w += glyph(f, next_code(&s))->adv + tracking;
    return w > 0 ? w - tracking : 0;
}

float font_ascent(const Font *f)
{
    return f->ascent;
}

float font_draw(Font *f, float x, float y, const char *s, u32 color, float tracking)
{
    x = (int)(x + 0.5f);
    y = (int)(y + 0.5f);
    while (*s) {
        const Glyph *g = glyph(f, next_code(&s));
        if (g->w > 0)
            gfx_tex_part(pages[g->page], g->x, g->y, g->w, g->h, (int)(x + g->left), y - g->top, g->w, g->h, color);
        x += g->adv + tracking;
    }
    return x;
}

void font_draw_fit(Font *f, float x, float y, const char *s, u32 color, float max_w)
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
    while (*p && n < sizeof(buf) - 8) {
        const char *q = p;
        float a = glyph(f, next_code(&q))->adv;
        if (w + a + dots > max_w) break;
        memcpy(buf + n, p, q - p);
        n += q - p;
        w += a;
        p = q;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    memcpy(buf + n, "\xE2\x80\xA6", 4);
    font_draw(f, x, y, buf, color, 0);
}

void font_draw_right(Font *f, float right, float y, const char *s, u32 color)
{
    font_draw(f, right - font_width(f, s, 0), y, s, color, 0);
}

void font_draw_center(Font *f, float cx, float y, const char *s, u32 color)
{
    font_draw(f, cx - font_width(f, s, 0) / 2, y, s, color, 0);
}
