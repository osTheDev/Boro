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
#include "stats.h"
#include "ui.h"
#include "vlib.h"

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
static Texture *t_music, *t_video, *t_thumb, *t_art, *t_fill8, *t_ring8;
static Font *f_quit, *f_note, *f_button;

static int sel = SEL_MUSIC, last_tile = SEL_MUSIC;
static float sel_k[SEL_COUNT] = { 1, 0, 0 };
static float t_now;

// "Quit Boro?" prompt: Stay is picked first, so a double tap of ○ never quits.
static int quit_open, quit_sel;
static float quit_k;

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
    t_fill8 = tex_round_fill(8);
    t_ring8 = tex_round_ring(8, 1);
    f_quit = ui_font(FAMILY_SORA, 700, 17);
    f_note = ui_font(FAMILY_MANROPE, 500, 11);
    f_button = ui_font(FAMILY_SORA, 700, 12);
}

static void quit_update(u32 p)
{
    if (p & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) quit_sel ^= 1;
    if (p & PSP_CTRL_CIRCLE) quit_open = 0;
    if (p & PSP_CTRL_CROSS) {
        if (quit_sel) sys_quit();
        quit_open = 0;
    }
}

int home_update(const Input *in, float dt)
{
    t_now += dt;
    quit_k += ((quit_open ? 1.0f : 0.0f) - quit_k) * fminf(1.0f, dt * 14.0f);

    u32 p = in->pressed;
    if (quit_open) {
        quit_update(p);
        return HOME_STAY;
    }
    if (p & PSP_CTRL_LEFT && sel == SEL_VIDEO) sel = SEL_MUSIC;
    if (p & PSP_CTRL_RIGHT && sel == SEL_MUSIC) sel = SEL_VIDEO;
    if (p & PSP_CTRL_DOWN && sel != SEL_STATS) { last_tile = sel; sel = SEL_STATS; }
    if (p & PSP_CTRL_UP && sel == SEL_STATS) sel = last_tile;

    if (p & PSP_CTRL_CROSS) {
        if (sel == SEL_MUSIC) return HOME_OPEN_MUSIC;
        if (sel == SEL_STATS) return library_state() == LIB_READY ? HOME_OPEN_STATS : HOME_STAY;
        return HOME_OPEN_VIDEO;
    }
    if (p & PSP_CTRL_START) return HOME_OPEN_SETTINGS;
    if (p & PSP_CTRL_SQUARE && player_song() >= 0) player_toggle_pause();
    if (p & PSP_CTRL_CIRCLE) { quit_open = 1; quit_sel = 0; }

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

void home_backdrop(void)
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
        char sub[128];
        if (i == 0 && library_state() != LIB_READY)
            snprintf(sub, sizeof(sub), "Scanning\xE2\x80\xA6 %d songs", library_scan_count());
        else if (i == 0)
            snprintf(sub, sizeof(sub), "%s theme \xC2\xB7 Playlists \xC2\xB7 Radio", theme_names[settings.theme]);
        else if (vlib_continue() >= 0)
            snprintf(sub, sizeof(sub), "Continue: %s", videos[vlib_continue()].title);
        else
            snprintf(sub, sizeof(sub), video_count == 1 ? "%d video" : "%d videos", video_count);
        font_draw_fit(f_tiny, x + 14, y + TILE_H - 16, sub, SUBTLE, TILE_W - 28);
        if (i == 1 && vlib_continue() >= 0) {
            const Video *v = &videos[vlib_continue()];
            float k = v->duration_ms ? fminf(1.0f, (float)v->resume_ms / v->duration_ms) : 0;
            gfx_rect(x + 14, y + TILE_H - 9, TILE_W - 28, 2, RGBA(255, 255, 255, 30));
            gfx_rect(x + 14, y + TILE_H - 9, (TILE_W - 28) * k, 2, ACCENT);
        }
    }
}

static void draw_stats(void)
{
    // The week so far, refreshed every few seconds.
    static Stats week;
    static float refresh_t = -1;
    if (library_state() == LIB_READY && (refresh_t < 0 || t_now - refresh_t > 5)) {
        refresh_t = t_now;
        stats_compute(PERIOD_WEEK, &week);
    }
    float x = PAD, y = STATS_Y, w = SCREEN_W - 2 * PAD;
    panel(x, y, w, STATS_H, 0, sel_k[SEL_STATS]);
    char time[24], plays[16];
    stats_format_time(time, sizeof(time), week.ms);
    snprintf(plays, sizeof(plays), "%d", week.plays);
    const char *artist = week.top_count[TOP_ARTISTS] ? lib.artists[week.top[TOP_ARTISTS][0]].name : "\xE2\x80\x94";
    float cx = x + 12, top = y + 8;
    font_draw(f_tiny, cx, top + 9, "THIS WEEK", LABEL, 1);
    font_draw(f_big, cx, top + 30, time, TEXT, 0);
    cx += fmaxf(font_width(f_tiny, "THIS WEEK", 1), font_width(f_big, time, 0)) + 14;
    font_draw(f_tiny, cx, top + 9, "Plays", LABEL, 0);
    font_draw(f_mid, cx, top + 26, plays, TEXT, 0);
    cx += fmaxf(font_width(f_tiny, "Plays", 0), font_width(f_mid, plays, 0)) + 14;
    font_draw(f_tiny, cx, top + 9, "Top artist", LABEL, 0);

    float link_w = font_width(f_small_b, "Stats \xE2\x80\xBA", 0);
    float lx = x + w - 12 - link_w;
    font_draw(f_small_b, lx, y + STATS_H / 2 + 4, "Stats \xE2\x80\xBA", ACCENT, 0);
    float chart_x = lx - 14 - 7 * 9 + 3, base = y + STATS_H / 2 + 15;
    font_draw_fit(f_mid, cx, top + 26, artist, TEXT, chart_x - 10 - cx);
    u32 max = 1;
    for (int d = 0; d < 7; d++) if (week.bars[d] > max) max = week.bars[d];
    for (int d = 0; d < 7; d++) {
        float h = week.bars[d] ? fmaxf(3, 30.0f * week.bars[d] / max) : 3;
        gfx_rect(chart_x + d * 9, base - h, 6, h, d == 6 ? ACCENT : RGBA(255, 255, 255, 46));
    }
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

    if (quit_open) return; // the prompt draws its own hints
    float x = SCREEN_W - PAD - ui_hint_width(f_tiny, ICON_CIRCLE, "Exit");
    ui_hint(f_tiny, x, y + 12, ICON_CIRCLE, "Exit", MUTED);
    x -= 8 + font_width(f_tiny, "START Settings", 0);
    font_draw(f_tiny, x, y + 12, "START Settings", MUTED, 0);
    x -= 8 + ui_hint_width(f_tiny, ICON_CROSS, "Open");
    ui_hint(f_tiny, x, y + 12, ICON_CROSS, "Open", MUTED);
}

static void draw_quit(void)
{
    float k = quit_k;
    if (k < 0.01f) return;
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(5, 6, 9, (int)(205 * k)));

    const float x = 130, y = 70, w = 220, h = 126;
    gfx_nine(t_shadow16, 34, x - 18, y - 18, w + 36, h + 36, color_alpha(ACCENT, 0.3f * k));
    gfx_nine(t_fill14, 14, x, y, w, h, RGBA(28, 30, 40, (int)(240 * k)));
    gfx_nine(t_ring14, 14, x, y, w, h, color_alpha(HEX(0x6C8CFF, 150), k));
    font_draw_center(f_quit, x + w / 2, y + 31, "Quit Boro?", color_alpha(TEXT, k));
    const char *note = player_song() >= 0 && !player_paused() ? "Music will stop playing." : "You'll go back to the PSP menu.";
    font_draw_center(f_note, x + w / 2, y + 54, note, color_alpha(MUTED, k));

    static const char *labels[] = { "Stay", "Quit" };
    for (int i = 0; i < 2; i++) {
        float bx = x + 16 + i * 100, by = y + h - 44;
        if (i == quit_sel) {
            gfx_nine(t_fill8, 8, bx, by, 88, 28, color_alpha(ACCENT, k));
        } else {
            gfx_nine(t_fill8, 8, bx, by, 88, 28, RGBA(255, 255, 255, (int)(18 * k)));
            gfx_nine(t_ring8, 8, bx, by, 88, 28, RGBA(255, 255, 255, (int)(40 * k)));
        }
        font_draw_center(f_button, bx + 44, by + 18, labels[i], color_alpha(i == quit_sel ? TEXT : MUTED, k));
    }

    float hy = SCREEN_H - 8 - 4;
    float hx = SCREEN_W - PAD - ui_hint_width(f_tiny, ICON_CIRCLE, "Cancel");
    ui_hint(f_tiny, hx, hy, ICON_CIRCLE, "Cancel", color_alpha(MUTED, k));
    hx -= 8 + ui_hint_width(f_tiny, ICON_CROSS, "Select");
    ui_hint(f_tiny, hx, hy, ICON_CROSS, "Select", color_alpha(MUTED, k));
}

void home_draw_no_music(void)
{
    gfx_begin(HEX(0x0B0C10, 255));
    home_backdrop();
    Font *logo = ui_font(FAMILY_SORA, 800, 13);
    float x = font_draw(logo, PAD, 22, "FL", TEXT, 0);
    x = font_draw(logo, x, 22, "/", ACCENT, 0);
    font_draw(logo, x, 22, "AC", TEXT, 0);
    ui_empty_card(SCREEN_H / 2, "No music yet", "MP3 files into", library_music_dir(),
                  "Subfolders are fine. Radio-only artists go in MUSIC/RADIO/.");
    float hx = SCREEN_W - PAD - ui_hint_width(f_tiny, ICON_CIRCLE, "Back");
    ui_hint(f_tiny, hx, SCREEN_H - 8, ICON_CIRCLE, "Back", MUTED);
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}

void home_draw(void)
{
    gfx_begin(HEX(0x0B0C10, 255));
    home_backdrop();
    draw_status();
    draw_tiles();
    draw_stats();
    draw_footer();
    draw_quit();
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
