#include "skin.h"

#include <stdio.h>
#include "gfx.h"
#include "settings.h"
#include "sys.h"
#include "ui.h"

int skin_web(void) { return settings.home_style == HOME_WEB; }

void skin_stream_back(u32 glow)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, SK_BG);
    gfx_gradient(0, 0, SCREEN_W, 150, color_alpha(glow, 0.55f), glow, color_alpha(glow, 0.15f), color_alpha(glow, 0.6f));
    gfx_gradient(0, 0, SCREEN_W, SCREEN_H, HEX(0x0B0C10, 225), HEX(0x0B0C10, 70), HEX(0x0B0C10, 225), HEX(0x0B0C10, 70));
    gfx_gradient(0, 100, SCREEN_W, SCREEN_H - 100, HEX(0x0B0C10, 60), HEX(0x0B0C10, 60), HEX(0x0B0C10, 255), HEX(0x0B0C10, 255));
}

void skin_stream_header(const char *page)
{
    Font *logo = ui_font(FAMILY_SORA, 800, 16), *f = ui_font(FAMILY_SORA, 500, 11);
    font_draw(logo, 16, 26, "BORO", SK_TEXT, 4);
    float x = 16 + font_width(logo, "BORO", 4) + 10;
    char t[64];
    snprintf(t, sizeof(t), "\xE2\x80\xBA  %s", page);
    font_draw(f, x, 25, t, SK_MUTED, 0);
    ui_battery(ui_font(FAMILY_SORA, 400, 10), SCREEN_W - 16, 20, SK_MUTED);
}

void skin_glass(float x, float y, float w, float h, int on)
{
    ui_rrect(x, y, w, h, 7, on ? HEX(0x22252F, 235) : HEX(0x181A22, 215));
    ui_ring(x, y, w, h, 7, on ? SK_ACCENT : HEX(0x30323A, 255));
    if (on) ui_ring(x + 1, y + 1, w - 2, h - 2, 6, color_alpha(SK_ACCENT, 0.5f));
}

float skin_pill_width(Font *f, const char *text) { return font_width(f, text, 0) + 16; }

float skin_pill(Font *f, float x, float y, const char *text, int on)
{
    float w = skin_pill_width(f, text);
    ui_rrect(x, y, w, 15, 7.5f, on ? SK_ACCENT : HEX(0x282B36, 230));
    font_draw(f, x + 8, y + 11, text, on ? SK_BG : SK_TEXT, 0);
    return w;
}

void skin_bevel(float x, float y, float w, float h, int down, u32 face)
{
    u32 hi = down ? W_DARK : W_WHITE, lo = down ? W_WHITE : W_DARK;
    gfx_rect(x, y, w, h, face);
    gfx_rect(x, y, w, 1, hi);
    gfx_rect(x, y, 1, h, hi);
    gfx_rect(x, y + h - 1, w, 1, lo);
    gfx_rect(x + w - 1, y, 1, h, lo);
}

void skin_dotted_rect(float x, float y, float w, float h, u32 c)
{
    for (float i = 0; i < w; i += 2) { gfx_rect(x + i, y, 1, 1, c); gfx_rect(x + i, y + h - 1, 1, 1, c); }
    for (float i = 0; i < h; i += 2) { gfx_rect(x, y + i, 1, 1, c); gfx_rect(x + w - 1, y + i, 1, 1, c); }
}

void skin_web_window(const char *title)
{
    Font *f_px = ui_font(FAMILY_SILKSCREEN, 400, 8), *f_pxb = ui_font(FAMILY_SILKSCREEN, 700, 8);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, W_TEAL);
    for (int y = 0; y < SCREEN_H; y += 16)
        for (int x = ((y / 16) & 1) * 16; x < SCREEN_W; x += 32) gfx_rect(x, y, 16, 16, W_TEAL2);
    skin_bevel(8, 6, 464, 245, 0, W_SILVER);
    gfx_gradient(11, 9, 458, 15, W_NAVY, HEX(0x1084D0, 255), W_NAVY, HEX(0x1084D0, 255));
    font_draw(f_pxb, 16, 20, title, W_WHITE, 0);
    static const char *const box[3] = { "_", "o", "x" };
    for (int i = 0; i < 3; i++) {
        skin_bevel(422 + i * 16, 11, 14, 11, 0, W_SILVER);
        font_draw_center(f_px, 429 + i * 16, 20, box[i], W_BLACK);
    }
    gfx_rect(14, 28, 452, 218, W_WHITE);
}

void skin_web_status(const char *text)
{
    skin_bevel(8, 253, 464, 16, 0, W_SILVER);
    font_draw(ui_font(FAMILY_SILKSCREEN, 400, 8), 14, 264, text, W_BLACK, 0);
}

float skin_counter(float x, float y, const char *digits)
{
    Font *f_vt = ui_font(FAMILY_VT323, 400, 16);
    for (const char *c = digits; *c; c++) {
        if (*c == ' ') continue;
        char ch[2] = { *c, 0 };
        gfx_rect(x, y, 13, 18, W_BLACK);
        font_draw_center(f_vt, x + 6.5f, y + 14, ch, HEX(0x00FF00, 255));
        x += 15;
    }
    return x;
}
