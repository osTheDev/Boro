#include "home.h"

#include <math.h>
#include <pspctrl.h>
#include <stdio.h>
#include "art.h"
#include "font.h"
#include "gfx.h"
#include "library.h"
#include "player.h"
#include "settings.h"
#include "ui.h"

enum { SEL_MUSIC, SEL_VIDEO, SEL_STATS, SEL_COUNT };

#define ACCENT HEX(0x6C8CFF, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xB5B8C4, 255)
#define SUBTLE HEX(0xC9CBD4, 255)
#define LABEL HEX(0xA9ACB8, 255)
#define ICON_IDLE HEX(0x8A8D99, 255)

#define PAD 16
#define TILE_Y 40
#define TILE_H 128
#define TILE_W 218
#define STATS_Y 178
#define STATS_H 50

static Font *f_logo, *f_title, *f_big, *f_mid, *f_small, *f_small_b, *f_tiny;
static Texture *t_glow, *t_fill16, *t_ring16, *t_shadow16, *t_fill14, *t_ring14, *t_shadow14;
static Texture *t_music, *t_video, *t_thumb, *t_art, *t_cross, *t_square, *t_circle;

static int sel = SEL_MUSIC, last_tile = SEL_MUSIC;
static float sel_k[SEL_COUNT] = { 1, 0, 0 };
static float t_now;
static char toast[64];
static float toast_t;

static Texture *icon_music(void)
{
    Texture *t = tex_create(32, 32);
    const float s = 32.0f / 24.0f, w = 1.8f * s;
    tex_stroke_line(t, 9 * s, 18 * s, 9 * s, 5 * s, w);
    tex_stroke_line(t, 9 * s, 5 * s, 21 * s, 3 * s, w);
    tex_stroke_line(t, 21 * s, 3 * s, 21 * s, 16 * s, w);
    tex_stroke_circle(t, 6 * s, 18 * s, 3 * s, w);
    tex_stroke_circle(t, 18 * s, 16 * s, 3 * s, w);
    tex_upload(t);
    return t;
}

static Texture *icon_video(void)
{
    Texture *t = tex_create(32, 32);
    const float s = 32.0f / 24.0f, w = 1.8f * s;
    tex_stroke_round_rect(t, 2 * s, 5 * s, 15 * s, 14 * s, 2 * s, w);
    const float tri[] = { 17, 10, 22, 7, 22, 17, 17, 14 };
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        tex_stroke_line(t, tri[i * 2] * s, tri[i * 2 + 1] * s, tri[j * 2] * s, tri[j * 2 + 1] * s, w);
    }
    tex_upload(t);
    return t;
}

static Texture *button_glyph(int kind)
{
    Texture *t = tex_create(10, 10);
    if (kind == 0) {
        tex_stroke_line(t, 1.5f, 1.5f, 8.5f, 8.5f, 1.4f);
        tex_stroke_line(t, 8.5f, 1.5f, 1.5f, 8.5f, 1.4f);
    } else if (kind == 1) {
        tex_stroke_round_rect(t, 1.5f, 1.5f, 7, 7, 0.5f, 1.3f);
    } else {
        tex_stroke_circle(t, 5, 5, 3.8f, 1.3f);
    }
    tex_upload(t);
    return t;
}

void home_init(void)
{
    f_logo = ui_font(FAMILY_SORA, 800, 16);
    f_title = ui_font(FAMILY_SORA, 700, 22);
    f_big = ui_font(FAMILY_SORA, 700, 18);
    f_mid = ui_font(FAMILY_SORA, 700, 13);
    f_small = ui_font(FAMILY_SORA, 400, 11);
    f_small_b = ui_font(FAMILY_SORA, 700, 10);
    f_tiny = ui_font(FAMILY_SORA, 400, 10);

    t_glow = tex_glow(128);
    t_fill16 = tex_round_fill(16);
    t_ring16 = tex_round_ring(16, 1.5f);
    t_shadow16 = tex_round_shadow(16, 18, 1);
    t_fill14 = tex_round_fill(14);
    t_ring14 = tex_round_ring(14, 1.5f);
    t_shadow14 = tex_round_shadow(14, 18, 1);
    t_music = icon_music();
    t_video = icon_video();
    t_thumb = tex_round_gradient(58, 33, 6, HEX(0x2B3A67, 255), HEX(0xC46A8A, 255));
    t_art = tex_round_gradient(16, 16, 3, HEX(0x234E70, 255), HEX(0x57A0D3, 255));
    t_cross = button_glyph(0);
    t_square = button_glyph(1);
    t_circle = button_glyph(2);
}

static void show_toast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_t = 2.2f;
}

int home_update(const Input *in, float dt)
{
    t_now += dt;
    if (toast_t > 0) toast_t -= dt;

    u32 p = in->pressed;
    if (p & PSP_CTRL_LEFT && sel == SEL_VIDEO) sel = SEL_MUSIC;
    if (p & PSP_CTRL_RIGHT && sel == SEL_MUSIC) sel = SEL_VIDEO;
    if (p & PSP_CTRL_DOWN && sel != SEL_STATS) { last_tile = sel; sel = SEL_STATS; }
    if (p & PSP_CTRL_UP && sel == SEL_STATS) sel = last_tile;

    if (p & PSP_CTRL_CROSS) {
        if (sel == SEL_MUSIC) return HOME_OPEN_MUSIC;
        show_toast(sel == SEL_VIDEO ? "Video arrives in milestone 5" : "Stats arrive in milestone 4");
    }
    if (p & PSP_CTRL_START) return HOME_OPEN_SETTINGS;
    if (p & PSP_CTRL_SQUARE && player_song() >= 0) player_toggle_pause();
    if (p & PSP_CTRL_CIRCLE) sys_quit();

    for (int i = 0; i < SEL_COUNT; i++) {
        float target = i == sel ? 1.0f : 0.0f;
        sel_k[i] += (target - sel_k[i]) * fminf(1.0f, dt * 12.0f);
    }
    return HOME_STAY;
}

// Glass panel: optional accent glow, translucent fill, hairline border.
static void panel(float x, float y, float w, float h, int big, float k)
{
    Texture *fill = big ? t_fill16 : t_fill14, *ring = big ? t_ring16 : t_ring14;
    Texture *shadow = big ? t_shadow16 : t_shadow14;
    int b = big ? 16 : 14, sb = (int)ceilf(b + 18.0f);
    if (k > 0.01f)
        gfx_nine(shadow, sb, x - 18, y - 18, w + 36, h + 36, color_alpha(ACCENT, 0.33f * k));
    gfx_nine(fill, b, x, y, w, h, RGBA(255, 255, 255, (int)(9 + 14 * k)));
    gfx_nine(ring, b, x, y, w, h, color_lerp(RGBA(255, 255, 255, 20), ACCENT, k));
}

static float pingpong_eq(float t, float dur, float off)
{
    float ph = fmodf((t + off) / dur, 2.0f);
    float u = ph < 1 ? ph : 2 - ph;
    u = u * u * (3 - 2 * u);
    return u < 0.5f ? 0.25f + (1.0f - 0.25f) * (u * 2) : 1.0f + (0.45f - 1.0f) * ((u - 0.5f) * 2);
}

static void draw_backdrop(void)
{
    float a = (1 - cosf(t_now * 2 * M_PI / 14.0f)) * 0.5f;
    float b = (1 - cosf(t_now * 2 * M_PI / 18.0f)) * 0.5f;
    gfx_additive(1);
    float sa = 1.0f + 0.15f * a, sb = 1.1f - 0.15f * b;
    float ax = -60 + 150 + 40 * a, ay = -80 + 130 + 18 * a;
    float bx = SCREEN_W + 80 - 170 - 50 * b, by = SCREEN_H + 110 - 140 - 14 * b;
    gfx_tex(t_glow, ax - 150 * sa, ay - 130 * sa, 300 * sa, 260 * sa, color_alpha(ACCENT, 0.42f));
    gfx_tex(t_glow, bx - 170 * sb, by - 140 * sb, 340 * sb, 280 * sb, HEX(0xC46A8A, 70));
    gfx_additive(0);
    for (int y = SCREEN_H - 1; y >= 0; y -= 3) gfx_rect(0, y, SCREEN_W, 1, RGBA(255, 255, 255, 5));
}

static void draw_status(void)
{
    font_draw(f_logo, PAD, 26, "BORO", TEXT, 4);
    float dot_x = PAD + font_width(f_logo, "BORO", 4) + 10;
    float pulse = 0.6f + 0.4f * (0.5f - 0.5f * cosf(t_now * 2 * M_PI / 2.4f));
    gfx_additive(1);
    gfx_tex(t_glow, dot_x - 9, 20 - 9, 18, 18, color_alpha(ACCENT, 0.8f * pulse));
    gfx_additive(0);
    gfx_tex(t_fill14, dot_x - 3, 17, 6, 6, color_alpha(ACCENT, pulse));

    char clock[16], pct[12];
    int charging, batt = sys_battery(&charging);
    sys_clock(clock, sizeof(clock));
    snprintf(pct, sizeof(pct), batt >= 0 ? "%d%%" : "AC", batt);

    float bx = SCREEN_W - PAD - 22, by = 15;
    gfx_rect(bx, by, 19, 1, MUTED);
    gfx_rect(bx, by + 10, 19, 1, MUTED);
    gfx_rect(bx, by, 1, 11, MUTED);
    gfx_rect(bx + 18, by, 1, 11, MUTED);
    gfx_rect(bx + 19, by + 3, 2, 5, MUTED);
    float level = batt >= 0 ? batt / 100.0f : 1.0f;
    gfx_rect(bx + 2, by + 2, 15 * level, 7, charging ? ACCENT : MUTED);

    float px = bx - 10 - font_width(f_small, pct, 0);
    font_draw(f_small, px, 24, pct, MUTED, 0);
    font_draw(f_small, px - 10 - font_width(f_small, clock, 0), 24, clock, MUTED, 0);
}

static void draw_tiles(void)
{
    for (int i = 0; i < 2; i++) {
        float x = PAD + i * (TILE_W + 12), y = TILE_Y, k = sel_k[i];
        panel(x, y, TILE_W, TILE_H, 1, k);
        gfx_tex(i == 0 ? t_music : t_video, x + 14, y + 12, 32, 32, color_lerp(ICON_IDLE, ACCENT, k));
        if (i == 0) {
            for (int b = 0; b < 5; b++) {
                static const float dur[5] = { 0.5f, 0.7f, 0.45f, 0.8f, 0.6f };
                float h = 16 * pingpong_eq(t_now, dur[b], b * 0.13f);
                gfx_rect(x + TILE_W - 14 - 23 + b * 5, y + 12 + 16 - h, 3, h, ACCENT);
            }
        } else {
            gfx_tex(t_thumb, x + TILE_W - 14 - 58, y + 12, 58, 33, 0xFFFFFFFF);
        }
        font_draw(f_title, x + 14, y + TILE_H - 34, i == 0 ? "Music" : "Video", TEXT, 0);
        char sub[64];
        if (i == 0 && library_state() != LIB_READY)
            snprintf(sub, sizeof(sub), "Scanning\xE2\x80\xA6 %d songs", library_scan_count());
        else if (i == 0)
            snprintf(sub, sizeof(sub), "%s theme \xC2\xB7 Playlists \xC2\xB7 Radio", theme_names[settings.theme]);
        else
            snprintf(sub, sizeof(sub), "Browse your videos");
        font_draw(f_tiny, x + 14, y + TILE_H - 16, sub, SUBTLE, 0);
    }
}

static void draw_stats(void)
{
    float x = PAD, y = STATS_Y, w = SCREEN_W - 2 * PAD;
    panel(x, y, w, STATS_H, 0, sel_k[SEL_STATS]);
    float cx = x + 12, top = y + 8;
    font_draw(f_tiny, cx, top + 9, "THIS WEEK", LABEL, 1);
    font_draw(f_big, cx, top + 30, "0m", TEXT, 0);
    cx += fmaxf(font_width(f_tiny, "THIS WEEK", 1), font_width(f_big, "0m", 0)) + 14;
    font_draw(f_tiny, cx, top + 9, "Plays", LABEL, 0);
    font_draw(f_mid, cx, top + 26, "0", TEXT, 0);
    cx += font_width(f_tiny, "Plays", 0) + 14;
    font_draw(f_tiny, cx, top + 9, "Top artist", LABEL, 0);
    font_draw_fit(f_mid, cx, top + 26, "\xE2\x80\x94", TEXT, 150);

    float link_w = font_width(f_small_b, "Stats \xE2\x80\xBA", 0);
    float lx = x + w - 12 - link_w;
    font_draw(f_small_b, lx, y + STATS_H / 2 + 4, "Stats \xE2\x80\xBA", ACCENT, 0);
    float chart_x = lx - 14 - 7 * 9 + 3, base = y + STATS_H / 2 + 15;
    for (int d = 0; d < 7; d++)
        gfx_rect(chart_x + d * 9, base - 3, 6, 3, d == 6 ? ACCENT : RGBA(255, 255, 255, 46));
}

static void draw_footer(void)
{
    float y = SCREEN_H - 8 - 16;
    int song = player_song();
    const Texture *art = song >= 0 ? art_get(song, ART_SMALL) : NULL;
    gfx_tex(art ? art : t_art, PAD, y, 16, 16, 0xFFFFFFFF);
    if (song >= 0) {
        char line[300];
        snprintf(line, sizeof(line), "%s \xC2\xB7 %s", song_title(&lib.songs[song]), song_artist(&lib.songs[song]));
        font_draw_fit(f_tiny, PAD + 24, y + 12, line, MUTED, 230);
    } else {
        font_draw(f_tiny, PAD + 24, y + 12, "Nothing playing", MUTED, 0);
    }

    static const char *labels[3] = { "Open", "START Settings", "Exit" };
    Texture *glyphs[3] = { t_cross, NULL, t_circle };
    float x = SCREEN_W - PAD;
    for (int i = 2; i >= 0; i--) {
        x -= font_width(f_tiny, labels[i], 0);
        font_draw(f_tiny, x, y + 12, labels[i], MUTED, 0);
        if (glyphs[i]) {
            x -= 13;
            gfx_tex(glyphs[i], x, y + 3, 10, 10, MUTED);
        }
        x -= 8;
    }
}

static void draw_toast(void)
{
    if (toast_t <= 0) return;
    float a = fminf(1.0f, toast_t / 0.3f) * fminf(1.0f, (2.2f - toast_t) / 0.15f);
    float w = font_width(f_small, toast, 0) + 28, h = 26;
    float x = (SCREEN_W - w) / 2, y = STATS_Y + STATS_H + 6 - 8 * (1 - a);
    gfx_nine(t_fill14, 14, x, y, w, h, HEX(0x1A1C24, (int)(235 * a)));
    gfx_nine(t_ring14, 14, x, y, w, h, color_alpha(ACCENT, 0.7f * a));
    font_draw(f_small, x + 14, y + 17, toast, color_alpha(TEXT, a), 0);
}

void home_draw(void)
{
    gfx_begin(HEX(0x0B0C10, 255));
    draw_backdrop();
    draw_status();
    draw_tiles();
    draw_stats();
    draw_footer();
    draw_toast();
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
