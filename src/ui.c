#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "sys.h"
#include "volume.h"

extern const char sora400_ttf[], sora400_ttf_end[], sora700_ttf[], sora700_ttf_end[];
extern const char sora800_ttf[], sora800_ttf_end[];
extern const char manrope500_ttf[], manrope500_ttf_end[], manrope700_ttf[], manrope700_ttf_end[];
extern const char manrope800_ttf[], manrope800_ttf_end[];
extern const char archivo400_ttf[], archivo400_ttf_end[], archivo600_ttf[], archivo600_ttf_end[];
extern const char archivo700_ttf[], archivo700_ttf_end[];
extern const char silkscreen400_ttf[], silkscreen400_ttf_end[], silkscreen700_ttf[], silkscreen700_ttf_end[];
extern const char vt323_ttf[], vt323_ttf_end[];

#define MAX_FONTS 64
#define MAX_RADIUS 80
#define MAX_ICONS 80

static struct { int family, weight, px; Font *font; } fonts[MAX_FONTS];
static int font_count;
static Texture *fills[MAX_RADIUS + 1], *rings[MAX_RADIUS + 1];
static struct { int icon, size; Texture *tex; } icons[MAX_ICONS];
static int icon_count;

void ui_init(void)
{
    font_init();
}

Font *ui_font(int family, int weight, int px)
{
    for (int i = 0; i < font_count; i++)
        if (fonts[i].family == family && fonts[i].weight == weight && fonts[i].px == px) return fonts[i].font;
    const char *data, *end;
    if (family == FAMILY_SORA) {
        data = weight >= 800 ? sora800_ttf : weight >= 600 ? sora700_ttf : sora400_ttf;
        end = weight >= 800 ? sora800_ttf_end : weight >= 600 ? sora700_ttf_end : sora400_ttf_end;
    } else if (family == FAMILY_MANROPE) {
        data = weight >= 800 ? manrope800_ttf : weight >= 600 ? manrope700_ttf : manrope500_ttf;
        end = weight >= 800 ? manrope800_ttf_end : weight >= 600 ? manrope700_ttf_end : manrope500_ttf_end;
    } else if (family == FAMILY_ARCHIVO) {
        data = weight >= 700 ? archivo700_ttf : weight >= 600 ? archivo600_ttf : archivo400_ttf;
        end = weight >= 700 ? archivo700_ttf_end : weight >= 600 ? archivo600_ttf_end : archivo400_ttf_end;
    } else if (family == FAMILY_SILKSCREEN) {
        data = weight >= 700 ? silkscreen700_ttf : silkscreen400_ttf;
        end = weight >= 700 ? silkscreen700_ttf_end : silkscreen400_ttf_end;
    } else {
        data = vt323_ttf;
        end = vt323_ttf_end;
    }
    Font *f = font_load(data, end - data, px);
    if (font_count < MAX_FONTS) fonts[font_count++] = (typeof(fonts[0])){ family, weight, px, f };
    return f;
}

static int radius_index(float r, float w, float h)
{
    float m = (w < h ? w : h) / 2;
    if (r > m) r = m;
    int ri = (int)(r + 0.5f);
    return ri < 0 ? 0 : ri > MAX_RADIUS ? MAX_RADIUS : ri;
}

void ui_rrect(float x, float y, float w, float h, float r, u32 color)
{
    if (w <= 0 || h <= 0) return;
    int ri = radius_index(r, w, h);
    if (ri == 0) {
        gfx_rect(x, y, w, h, color);
        return;
    }
    if (!fills[ri]) fills[ri] = tex_round_fill(ri);
    gfx_nine(fills[ri], ri, x, y, w, h, color);
}

void ui_ring(float x, float y, float w, float h, float r, u32 color)
{
    int ri = radius_index(r, w, h);
    if (ri == 0) {
        gfx_rect(x, y, w, 1, color);
        gfx_rect(x, y + h - 1, w, 1, color);
        gfx_rect(x, y + 1, 1, h - 2, color);
        gfx_rect(x + w - 1, y + 1, 1, h - 2, color);
        return;
    }
    if (!rings[ri]) rings[ri] = tex_round_ring(ri, 1.0f);
    gfx_nine(rings[ri], ri, x, y, w, h, color);
}

void ui_shadow(float x, float y, float w, float h, float r, float blur, u32 color)
{
    static Texture *cache[8];
    static int keys[8], count;
    int key = (int)r * 100 + (int)blur, i;
    for (i = 0; i < count && keys[i] != key; i++) {}
    if (i == count) {
        if (count == 8) return;
        keys[count] = key;
        cache[count++] = tex_round_shadow(r, blur, 0);
    }
    int b = (int)ceilf(r + blur);
    gfx_nine(cache[i], b, x - blur, y - blur, w + 2 * blur, h + 2 * blur, color);
}

// ---- Icons (paths on a 24-unit grid, like the mockups' SVGs) ---------------

static void poly(Texture *t, float s, const float *pts, int n)
{
    float xy[16];
    for (int i = 0; i < n * 2; i++) xy[i] = pts[i] * s;
    tex_fill_poly(t, xy, n);
}

static void rect(Texture *t, float s, float x, float y, float w, float h)
{
    const float p[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    poly(t, s, p, 4);
}

static void line(Texture *t, float s, float w, float x0, float y0, float x1, float y1)
{
    tex_stroke_line(t, x0 * s, y0 * s, x1 * s, y1 * s, w * s);
}

static void arc(Texture *t, float s, float w, float cx, float cy, float r, float a0, float a1)
{
    const int steps = 6;
    for (int i = 0; i < steps; i++) {
        float t0 = a0 + (a1 - a0) * i / steps, t1 = a0 + (a1 - a0) * (i + 1) / steps;
        line(t, s, w, cx + r * cosf(t0), cy + r * sinf(t0), cx + r * cosf(t1), cy + r * sinf(t1));
    }
}

static Texture *build_icon(int icon, int size)
{
    Texture *t = tex_create(size, size);
    float s = size / 24.0f;
    switch (icon) {
    case ICON_PLAY: { const float p[] = { 6, 4, 20, 12, 6, 20 }; poly(t, s, p, 3); break; }
    case ICON_PAUSE: rect(t, s, 6, 4, 4, 16); rect(t, s, 14, 4, 4, 16); break;
    case ICON_NEXT: { const float p[] = { 4, 5, 15, 12, 4, 19 }; poly(t, s, p, 3); rect(t, s, 16, 5, 2, 14); break; }
    case ICON_PREV: { const float p[] = { 20, 5, 20, 19, 9, 12 }; poly(t, s, p, 3); rect(t, s, 6, 5, 2, 14); break; }
    case ICON_SHUFFLE:
        line(t, s, 2, 16, 3, 21, 3); line(t, s, 2, 21, 3, 21, 8); line(t, s, 2, 4, 20, 21, 3);
        line(t, s, 2, 21, 16, 21, 21); line(t, s, 2, 21, 21, 16, 21); line(t, s, 2, 15, 15, 21, 21);
        line(t, s, 2, 4, 4, 9, 9);
        break;
    case ICON_REPEAT:
        line(t, s, 2, 17, 2, 21, 6); line(t, s, 2, 21, 6, 17, 10);
        line(t, s, 2, 3, 11, 3, 9); arc(t, s, 2, 6, 9, 3, M_PI, 1.5f * M_PI); line(t, s, 2, 6, 6, 21, 6);
        line(t, s, 2, 7, 22, 3, 18); line(t, s, 2, 3, 18, 7, 14);
        line(t, s, 2, 21, 13, 21, 15); arc(t, s, 2, 18, 15, 3, 0, 0.5f * M_PI); line(t, s, 2, 18, 18, 3, 18);
        break;
    case ICON_NOTE:
        line(t, s, 1.8f, 9, 18, 9, 5); line(t, s, 1.8f, 9, 5, 21, 3); line(t, s, 1.8f, 21, 3, 21, 16);
        tex_stroke_circle(t, 6 * s, 18 * s, 3 * s, 1.8f * s); tex_stroke_circle(t, 18 * s, 16 * s, 3 * s, 1.8f * s);
        break;
    case ICON_SPEAKER:
    case ICON_MUTE: {
        const float a[] = { 7, 9, 13, 4, 13, 20 }, b[] = { 7, 9, 13, 20, 7, 15 };
        rect(t, s, 3, 9, 5, 6); poly(t, s, a, 3); poly(t, s, b, 3);
        if (icon == ICON_MUTE) { line(t, s, 2, 16, 9, 22, 15); line(t, s, 2, 22, 9, 16, 15); }
        else { arc(t, s, 2, 13, 12, 4, -0.28f * M_PI, 0.28f * M_PI); arc(t, s, 2, 13, 12, 8, -0.28f * M_PI, 0.28f * M_PI); }
        break;
    }
    case ICON_PLUS: line(t, s, 2.5f, 12, 5, 12, 19); line(t, s, 2.5f, 5, 12, 19, 12); break;
    case ICON_BACKSPACE:
        line(t, s, 1.8f, 8, 6, 21, 6); line(t, s, 1.8f, 21, 6, 21, 18); line(t, s, 1.8f, 21, 18, 8, 18);
        line(t, s, 1.8f, 8, 18, 2, 12); line(t, s, 1.8f, 2, 12, 8, 6);
        line(t, s, 1.8f, 12, 9, 17, 15); line(t, s, 1.8f, 17, 9, 12, 15);
        break;
    case ICON_SHIFT:
        line(t, s, 1.8f, 12, 3, 21, 12); line(t, s, 1.8f, 21, 12, 16, 12); line(t, s, 1.8f, 16, 12, 16, 20);
        line(t, s, 1.8f, 16, 20, 8, 20); line(t, s, 1.8f, 8, 20, 8, 12); line(t, s, 1.8f, 8, 12, 3, 12);
        line(t, s, 1.8f, 3, 12, 12, 3);
        break;
    case ICON_CROSS: line(t, s, 3, 4, 4, 20, 20); line(t, s, 3, 20, 4, 4, 20); break;
    case ICON_SQUARE: tex_stroke_round_rect(t, 4 * s, 4 * s, 16 * s, 16 * s, 1 * s, 2.8f * s); break;
    case ICON_CIRCLE: tex_stroke_circle(t, 12 * s, 12 * s, 8.5f * s, 2.8f * s); break;
    case ICON_TRIANGLE: {
        line(t, s, 2.8f, 12, 4, 21, 19); line(t, s, 2.8f, 21, 19, 3, 19); line(t, s, 2.8f, 3, 19, 12, 4);
        break;
    }
    }
    tex_upload(t);
    return t;
}

void ui_icon(int icon, float x, float y, float size, u32 color)
{
    int px = (int)(size + 0.5f), i;
    for (i = 0; i < icon_count && (icons[i].icon != icon || icons[i].size != px); i++) {}
    if (i == icon_count) {
        if (icon_count == MAX_ICONS) return;
        icons[icon_count++] = (typeof(icons[0])){ icon, px, build_icon(icon, px) };
    }
    gfx_tex(icons[i].tex, (int)(x + 0.5f), (int)(y + 0.5f), px, px, color);
}

void ui_battery(Font *f, float right, float cy, u32 color)
{
    int charging, pct = sys_battery(&charging);
    char text[16];
    snprintf(text, sizeof(text), pct >= 0 ? "%d%%" : "AC", pct);
    float bw = 17, bh = 9, bx = right - bw - 3, by = (int)(cy - bh / 2);
    ui_ring(bx, by, bw, bh, 2, color);
    gfx_rect(bx + bw + 1, by + 2.5f, 2, 4, color);
    float level = pct >= 0 ? pct / 100.0f : 1.0f;
    ui_rrect(bx + 2, by + 2, 13 * level, bh - 4, 1, color);
    font_draw(f, bx - 4 - font_width(f, text, 0), (int)(cy + font_ascent(f) * 0.36f), text, color, 0);
}

float ui_hint_width(Font *f, int glyph, const char *label)
{
    (void)glyph;
    return font_ascent(f) + 3 + font_width(f, label, 0);
}

float ui_hint(Font *f, float x, float baseline, int glyph, const char *label, u32 color)
{
    float size = (int)(font_ascent(f) * 0.95f + 0.5f);
    ui_icon(glyph, x, baseline - size + 1, size, color);
    return font_draw(f, x + size + 3, baseline, label, color, 0);
}

static const u32 placeholder[][2] = {
    { 0xD4513B, 0xF2B84B }, { 0x234E70, 0x57A0D3 }, { 0x3E2A5C, 0xA06CD5 }, { 0x1E4D2B, 0x7BC47F },
    { 0x5C3A21, 0xE0A060 }, { 0x2D2D2D, 0x8C8C8C }, { 0x6B1F3A, 0xE05C8A }, { 0x1B3B4F, 0x4FB3BF },
};

void ui_placeholder_colors(int seed, u32 *c0, u32 *c1)
{
    int i = (unsigned)seed % (sizeof(placeholder) / sizeof(placeholder[0]));
    *c0 = HEX(placeholder[i][0], 255);
    *c1 = HEX(placeholder[i][1], 255);
}

void ui_placeholder_art(float x, float y, float size, float r, int seed)
{
    u32 c0, c1;
    ui_placeholder_colors(seed, &c0, &c1);
    u32 mid = color_lerp(c0, c1, 0.5f);
    if (r <= 0.5f) {
        gfx_gradient(x, y, size, size, c0, mid, mid, c1);
    } else {
        // Rounded: approximate the gradient with the mid color under the corners.
        ui_rrect(x, y, size, size, r, mid);
        gfx_gradient(x + r, y + r, size - 2 * r, size - 2 * r, c0, mid, mid, c1);
    }
    ui_icon(ICON_NOTE, x + size * 0.3f, y + size * 0.3f, size * 0.4f, RGBA(255, 255, 255, 70));
}

void ui_format_time(char *out, int size, u32 ms)
{
    u32 s = ms / 1000;
    if (s >= 3600) snprintf(out, size, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(out, size, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

float ui_approach(float v, float target, float dt, float speed)
{
    float k = 1.0f - expf(-dt * speed);
    return v + (target - v) * k;
}

void ui_volume(Font *f, u32 panel, u32 track, u32 fill, u32 text)
{
    float a = volume_shown();
    int v = volume_level();
    if (a <= 0 || v < 0) return;
    float w = 210, h = 30, x = (SCREEN_W - w) / 2, y = 10 - 8 * (1 - a);
    ui_shadow(x, y + 3, w, h, h / 2, 8, RGBA(0, 0, 0, (int)(110 * a)));
    ui_rrect(x, y, w, h, h / 2, color_alpha(panel, a));
    ui_icon(v ? ICON_SPEAKER : ICON_MUTE, x + 12, y + 7, 16, color_alpha(text, a));
    float bx = x + 38, bw = w - 38 - 40, by = y + h / 2 - 2;
    ui_rrect(bx, by, bw, 4, 2, color_alpha(track, a));
    if (v) ui_rrect(bx, by, fmaxf(4, bw * v / VOLUME_MAX), 4, 2, color_alpha(fill, a));
    char n[12];
    snprintf(n, sizeof(n), "%d", v);
    font_draw_right(f, x + w - 14, y + h / 2 + font_ascent(f) * 0.4f, n, color_alpha(text, a));
}
