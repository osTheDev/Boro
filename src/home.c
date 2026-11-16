#include "home.h"

#include <math.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "font.h"
#include "gfx.h"
#include "library.h"
#include "player.h"
#include "settings.h"
#include "stats.h"
#include "ui.h"
#include "glib.h"
#include "vlib.h"
#include "games.h"

enum { SEL_MUSIC, SEL_VIDEO, SEL_GAMES, SEL_BOOKS, SEL_STATS, SEL_COUNT };
#define TILES 4

#define ACCENT HEX(0x6C8CFF, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xB5B8C4, 255)
#define SUBTLE HEX(0xC9CBD4, 255)
#define LABEL HEX(0xA9ACB8, 255)
#define ICON_IDLE HEX(0x8A8D99, 255)

#define PAD 16

static Font *f_logo, *f_title, *f_big, *f_mid, *f_small, *f_small_b, *f_tiny;
static Texture *t_glow, *t_fill16, *t_ring16, *t_shadow16, *t_fill14, *t_ring14, *t_shadow14;
static Texture *t_music, *t_video, *t_games, *t_books, *t_thumb, *t_fill8, *t_ring8;
static Font *f_quit, *f_note, *f_button;

static int sel = SEL_MUSIC, last_tile = SEL_MUSIC;
static float sel_k[SEL_COUNT] = { 1, 0, 0, 0, 0 };
static float back_k[TILES] = { 1, 0, 0, 0 };
static char toast[96];
static float toast_t;
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

static Texture *icon_games(void)
{
    Texture *t = tex_create(32, 32);
    const float s = 32.0f / 24.0f, w = 1.8f * s;
    tex_stroke_round_rect(t, 2 * s, 7 * s, 20 * s, 11 * s, 5 * s, w);
    tex_stroke_line(t, 5 * s, 12.5f * s, 10 * s, 12.5f * s, w);
    tex_stroke_line(t, 7.5f * s, 10 * s, 7.5f * s, 15 * s, w);
    tex_fill_circle(t, 15.5f * s, 11 * s, 1.3f * s);
    tex_fill_circle(t, 18 * s, 14 * s, 1.3f * s);
    tex_upload(t);
    return t;
}

static Texture *icon_books(void)
{
    Texture *t = tex_create(32, 32);
    const float s = 32.0f / 24.0f, w = 1.8f * s;
    tex_stroke_round_rect(t, 4 * s, 3 * s, 16 * s, 18 * s, 2 * s, w);
    tex_stroke_line(t, 8 * s, 3 * s, 8 * s, 21 * s, w);
    tex_stroke_line(t, 11 * s, 8 * s, 17 * s, 8 * s, w);
    tex_stroke_line(t, 11 * s, 12 * s, 16 * s, 12 * s, w);
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
    t_games = icon_games();
    t_books = icon_books();
    t_thumb = tex_round_gradient(58, 33, 6, HEX(0x2B3A67, 255), HEX(0xC46A8A, 255));
    t_fill8 = tex_round_fill(8);
    t_ring8 = tex_round_ring(8, 1);
    f_quit = ui_font(FAMILY_SORA, 700, 17);
    f_note = ui_font(FAMILY_MANROPE, 500, 11);
    f_button = ui_font(FAMILY_SORA, 700, 12);
}

static void show_toast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_t = 2.5f;
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
    if (p & PSP_CTRL_LEFT && sel > SEL_MUSIC && sel != SEL_STATS) sel--;
    if (p & PSP_CTRL_RIGHT && sel < SEL_BOOKS) sel++;
    if (p & PSP_CTRL_DOWN && sel != SEL_STATS) { last_tile = sel; sel = SEL_STATS; }
    if (p & PSP_CTRL_UP && sel == SEL_STATS) sel = last_tile;

    if (p & PSP_CTRL_CROSS) {
        if (sel == SEL_MUSIC) return HOME_OPEN_MUSIC;
        if (sel == SEL_STATS) return library_state() == LIB_READY ? HOME_OPEN_STATS : HOME_STAY;
        if (sel == SEL_BOOKS) { show_toast("Audiobooks are coming in the next update"); return HOME_STAY; }
        return sel == SEL_GAMES ? HOME_OPEN_GAMES : HOME_OPEN_VIDEO;
    }
    if (p & PSP_CTRL_TRIANGLE) {
        int t = sel == SEL_STATS ? last_tile : sel;
        if (t == SEL_MUSIC && player_song() >= 0) return HOME_NOW_PLAYING;
        if (t == SEL_VIDEO && vlib_continue() >= 0) return HOME_RESUME_VIDEO;
        if (t == SEL_GAMES && glib_last_game() >= 0) return HOME_PLAY_GAME;
    }
    if (p & PSP_CTRL_START) return HOME_OPEN_SETTINGS;
    if (p & PSP_CTRL_SQUARE && player_song() >= 0) player_toggle_pause();
    if (p & PSP_CTRL_CIRCLE) { quit_open = 1; quit_sel = 0; }

    for (int i = 0; i < SEL_COUNT; i++) {
        float target = i == sel ? 1.0f : 0.0f;
        sel_k[i] += (target - sel_k[i]) * fminf(1.0f, dt * 12.0f);
    }
    int shown = sel == SEL_STATS ? last_tile : sel;
    for (int i = 0; i < TILES; i++) back_k[i] = ui_approach(back_k[i], i == shown ? 1.0f : 0.0f, dt, 5);
    if (toast_t > 0) toast_t -= dt;
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

// ---- Shared: this week's numbers and what each tile would pick up -------------

static Stats week;
static GameStats gweek;

static void refresh_week(void)
{
    static float refresh_t = -1;
    if (library_state() != LIB_READY || (refresh_t >= 0 && t_now - refresh_t < 5)) return;
    refresh_t = t_now;
    stats_compute(PERIOD_WEEK, &week);
    glib_stats(7, &gweek);
}

typedef struct {
    const char *label;  // "MUSIC · NOW PLAYING"
    char title[128];
    char sub[160];
    float progress;     // 0..1, or -1 for none
    const char *action; // the △ button, or NULL
} Hero;

static void hero_of(int tile, Hero *h)
{
    h->progress = -1;
    h->action = NULL;
    h->title[0] = h->sub[0] = 0;
    if (tile == SEL_MUSIC) {
        int song = player_song();
        if (song >= 0) {
            const Song *s = &lib.songs[song];
            char pos[16], len[16];
            ui_format_time(pos, sizeof(pos), player_position_ms());
            ui_format_time(len, sizeof(len), s->duration_ms);
            h->label = player_paused() ? "MUSIC \xC2\xB7 PAUSED" : "MUSIC \xC2\xB7 NOW PLAYING";
            snprintf(h->title, sizeof(h->title), "%s", song_title(s));
            snprintf(h->sub, sizeof(h->sub), "%s \xC2\xB7 %s / %s", song_artist(s), pos, len);
            if (s->duration_ms) h->progress = fminf(1.0f, (float)player_position_ms() / s->duration_ms);
            h->action = "Now Playing";
        } else {
            h->label = "MUSIC";
            snprintf(h->title, sizeof(h->title), "FL/AC");
            if (library_state() != LIB_READY)
                snprintf(h->sub, sizeof(h->sub), "Scanning\xE2\x80\xA6 %d songs", library_scan_count());
            else
                snprintf(h->sub, sizeof(h->sub), "%d songs \xC2\xB7 %s theme \xC2\xB7 Playlists \xC2\xB7 Radio",
                         lib.song_count, theme_names[settings.theme]);
        }
    } else if (tile == SEL_VIDEO) {
        int c = vlib_continue();
        if (c >= 0) {
            const Video *v = &videos[c];
            h->label = "VIDEO \xC2\xB7 CONTINUE WATCHING";
            snprintf(h->title, sizeof(h->title), "%s", v->show >= 0 ? shows[v->show].name : v->title);
            u32 left = v->duration_ms > v->resume_ms ? (v->duration_ms - v->resume_ms) / 60000 : 0;
            if (v->show >= 0 && v->episode)
                snprintf(h->sub, sizeof(h->sub), "Season %d \xC2\xB7 Ep %d \xC2\xB7 %u min left", v->season ? v->season : 1, v->episode, (unsigned)left);
            else if (v->show >= 0)
                snprintf(h->sub, sizeof(h->sub), "%s \xC2\xB7 %u min left", v->title, (unsigned)left);
            else
                snprintf(h->sub, sizeof(h->sub), "%u min left", (unsigned)left);
            if (v->duration_ms) h->progress = fminf(1.0f, (float)v->resume_ms / v->duration_ms);
            h->action = "Resume";
        } else {
            h->label = "VIDEO";
            snprintf(h->title, sizeof(h->title), "TV & Movies");
            snprintf(h->sub, sizeof(h->sub), video_count == 1 ? "%d video" : "%d videos", video_count);
        }
    } else if (tile == SEL_GAMES) {
        int g = glib_last_game();
        if (g >= 0) {
            char played[24], last[24];
            glib_format_time(played, sizeof(played), games[g].seconds);
            glib_format_last(last, sizeof(last), games[g].last);
            h->label = "GAMES \xC2\xB7 LAST PLAYED";
            snprintf(h->title, sizeof(h->title), "%s", games[g].title);
            snprintf(h->sub, sizeof(h->sub), "%s played \xC2\xB7 %s", played, last);
            h->action = "Play";
        } else {
            h->label = "GAMES";
            snprintf(h->title, sizeof(h->title), "%s", glib_last_title() ? glib_last_title() : "Your games");
            if (glib_ready())
                snprintf(h->sub, sizeof(h->sub), game_count == 1 ? "%d game" : "%d games", game_count);
            else
                snprintf(h->sub, sizeof(h->sub), "ISO, CSO and EBOOT games");
        }
    } else {
        h->label = "BOOKS \xC2\xB7 COMING NEXT";
        snprintf(h->title, sizeof(h->title), "Audiobooks");
        snprintf(h->sub, sizeof(h->sub), "Chapters, resume and speed, in the next update");
    }
}

static const char *const tile_names[TILES] = { "Music", "Video", "Games", "Books" };

// ---- Last game's PIC1, read on its own thread ---------------------------------

static volatile int pic_busy, pic_w, pic_h;
static u8 *volatile pic_px;
static int pic_for = -1, pic_index;
static Texture *pic_tex;

static int pic_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int w = 0, h = 0;
    u8 *px = glib_pic1(pic_index, &w, &h);
    if (px && (w <= 0 || h <= 0 || w > 512 || h > 512)) { free(px); px = NULL; }
    pic_w = w;
    pic_h = h;
    pic_px = px;
    pic_busy = 0;
    return sceKernelExitDeleteThread(0);
}

static void pic_frame(void)
{
    u8 *px = pic_px;
    if (px) {
        pic_px = NULL;
        if (pic_tex) tex_destroy(pic_tex);
        pic_tex = tex_create(pic_w, pic_h);
        if (pic_tex) {
            for (int y = 0; y < pic_h; y++) memcpy(pic_tex->px + y * pic_tex->tw, px + y * pic_w * 4, pic_w * 4);
            tex_upload(pic_tex);
        }
        free(px);
    }
    int g = glib_last_game();
    if (g < 0 || g == pic_for || pic_busy || !glib_ready()) return;
    pic_for = pic_index = g;
    pic_busy = 1;
    SceUID th = sceKernelCreateThread("boro_home_pic", pic_thread, 0x30, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) pic_busy = 0;
}

// ---- Stream: big art for the tile you're on, tiles in a row --------------------

static const u32 back_c0[TILES] = { HEX(0x3A2E6E, 255), HEX(0x1E3E6E, 255), HEX(0x1F5A44, 255), HEX(0x7A4A3A, 255) };
static const u32 back_c1[TILES] = { HEX(0x14121F, 255), HEX(0x0E1626, 255), HEX(0x0D1F18, 255), HEX(0x2A1A26, 255) };

static void draw_back(int tile, float a)
{
    u32 tint = color_alpha(0xFFFFFFFF, a);
    const Texture *t = NULL;
    gfx_filter(1);
    if (tile == SEL_MUSIC && player_song() >= 0 && (t = art_get(player_song(), ART_LARGE))) {
        // The middle of the cover, stretched soft across the screen.
        float sw = t->w * 0.8f, sh = sw * SCREEN_H / SCREEN_W;
        gfx_tex_part(t, (t->w - sw) / 2, (t->h - sh) / 2, sw, sh, 0, 0, SCREEN_W, SCREEN_H, tint);
        return;
    }
    if (tile == SEL_VIDEO && vlib_continue() >= 0 && (t = vlib_thumb(vlib_continue()))) {
        gfx_tex(t, 0, 0, SCREEN_W, SCREEN_H, tint);
        return;
    }
    if (tile == SEL_GAMES && glib_last_game() >= 0 && glib_last_game() == pic_for && pic_tex) {
        gfx_tex(pic_tex, 0, 0, SCREEN_W, SCREEN_H, tint);
        return;
    }
    if (tile == SEL_GAMES && glib_last_game() >= 0 && (t = glib_icon(glib_last_game()))) {
        gfx_tex(t, 0, 0, SCREEN_W, SCREEN_H, tint);
        return;
    }
    u32 c0 = color_alpha(back_c0[tile], a), c1 = color_alpha(back_c1[tile], a);
    gfx_gradient(0, 0, SCREEN_W, SCREEN_H, c1, c0, c1, c1);
}

static void stream_hero(void)
{
    int tile = sel == SEL_STATS ? last_tile : sel;
    Hero h;
    hero_of(tile, &h);
    float y = 60;
    font_draw(f_tiny, PAD, y, h.label, ACCENT, 1);
    font_draw_fit(f_title, PAD, y + 26, h.title, TEXT, 300);
    font_draw_fit(f_small, PAD, y + 44, h.sub, MUTED, 300);
    if (h.progress >= 0) {
        gfx_rect(PAD, y + 52, 200, 2, RGBA(255, 255, 255, 40));
        gfx_rect(PAD, y + 52, 200 * h.progress, 2, ACCENT);
    }
    if (h.action) {
        float bw = ui_hint_width(f_button, ICON_TRIANGLE, h.action) + 24, by = y + 62;
        gfx_nine(t_fill8, 8, PAD, by, bw, 22, RGBA(242, 242, 245, 240));
        ui_hint(f_button, PAD + 12, by + 15, ICON_TRIANGLE, h.action, HEX(0x0B0C10, 255));
    }
}

static void stream_tiles(void)
{
    static Texture **icons[TILES] = { &t_music, &t_video, &t_games, &t_books };
    float x = PAD, bottom = 230;
    for (int i = 0; i < TILES; i++) {
        float k = sel_k[i], w = 92 + 16 * k, h = 54 + 10 * k, y = bottom - h;
        panel(x, y, w, h, 0, k);
        gfx_nine(t_fill14, 14, x, y, w, h, RGBA(16, 18, 26, (int)(150 - 40 * k)));
        gfx_tex(*icons[i], x + 10, y + 9, 20, 20, color_lerp(ICON_IDLE, ACCENT, k));
        if (i == SEL_MUSIC && player_song() >= 0 && !player_paused())
            for (int b = 0; b < 4; b++) {
                static const float dur[4] = { 0.5f, 0.7f, 0.45f, 0.8f };
                float bh = 12 * pingpong_eq(t_now, dur[b], b * 0.13f);
                gfx_rect(x + w - 30 + b * 5, y + 9 + 12 - bh, 3, bh, ACCENT);
            }
        font_draw(f_mid, x + 10, y + h - 10, tile_names[i], color_lerp(MUTED, TEXT, k), 0);
        x += w + 12;
    }
}

static void stream_stats(void)
{
    refresh_week();
    char all[24], music[24], game[24], line[160];
    stats_format_time(all, sizeof(all), week.ms + gweek.seconds * 1000);
    stats_format_time(music, sizeof(music), week.ms);
    stats_format_time(game, sizeof(game), gweek.seconds * 1000);
    snprintf(line, sizeof(line), "This week %s \xC2\xB7 Music %s \xC2\xB7 Games %s", all, music, game);
    float k = sel_k[SEL_STATS], y = 248;
    if (k > 0.01f) gfx_nine(t_fill8, 8, PAD - 6, y - 12, SCREEN_W - 2 * PAD + 12, 17, color_alpha(ACCENT, 0.18f * k));
    font_draw_fit(f_tiny, PAD, y, line, color_lerp(MUTED, TEXT, k), 360);
    font_draw_right(f_small_b, SCREEN_W - PAD, y, "Stats \xE2\x80\xBA", color_lerp(MUTED, ACCENT, k));
}

static void hints(Font *f, float y, u32 color)
{
    int tile = sel == SEL_STATS ? last_tile : sel;
    Hero h;
    hero_of(tile, &h);
    float x = SCREEN_W - PAD - ui_hint_width(f, ICON_CIRCLE, "Exit");
    ui_hint(f, x, y, ICON_CIRCLE, "Exit", color);
    x -= 10 + font_width(f, "START Settings", 0);
    font_draw(f, x, y, "START Settings", color, 0);
    if (h.action) {
        x -= 10 + ui_hint_width(f, ICON_TRIANGLE, h.action);
        ui_hint(f, x, y, ICON_TRIANGLE, h.action, color);
    }
    x -= 10 + ui_hint_width(f, ICON_CROSS, "Open");
    ui_hint(f, x, y, ICON_CROSS, "Open", color);
}

static void draw_toast(void)
{
    if (toast_t <= 0) return;
    float a = fminf(1.0f, toast_t * 3), w = font_width(f_small, toast, 0) + 28;
    gfx_nine(t_fill8, 8, (SCREEN_W - w) / 2, 128, w, 26, RGBA(28, 30, 40, (int)(235 * a)));
    gfx_nine(t_ring8, 8, (SCREEN_W - w) / 2, 128, w, 26, color_alpha(ACCENT, a));
    font_draw_center(f_small, SCREEN_W / 2, 145, toast, color_alpha(TEXT, a));
}

static void stream_draw(void)
{
    pic_frame();
    for (int i = 0; i < TILES; i++) if (back_k[i] > 0.01f) draw_back(i, back_k[i]);
    // Darken toward the text on the left and into the tiles below.
    gfx_gradient(0, 0, SCREEN_W, SCREEN_H, HEX(0x0B0C10, 235), HEX(0x0B0C10, 70), HEX(0x0B0C10, 235), HEX(0x0B0C10, 70));
    gfx_gradient(0, 130, SCREEN_W, SCREEN_H - 130, HEX(0x0B0C10, 0), HEX(0x0B0C10, 0), HEX(0x0B0C10, 255), HEX(0x0B0C10, 255));
    gfx_rect(0, 0, SCREEN_W, 34, HEX(0x0B0C10, 60));
    draw_status();
    stream_hero();
    stream_tiles();
    stream_stats();
    if (!quit_open) hints(f_tiny, 266, MUTED);
}

// ---- Web: Boro as an early-internet home page ----------------------------------

#define W_TEAL HEX(0x008080, 255)
#define W_TEAL2 HEX(0x007676, 255)
#define W_FACE HEX(0xD4D0C8, 255)
#define W_SILVER HEX(0xC0C0C0, 255)
#define W_NAVY HEX(0x000080, 255)
#define W_WHITE HEX(0xFFFFFF, 255)
#define W_BLACK HEX(0x000000, 255)
#define W_DARK HEX(0x404040, 255)
#define W_GRAY HEX(0x808080, 255)
#define W_LINK HEX(0x0000EE, 255)
#define W_MAROON HEX(0x800000, 255)

static void bevel(float x, float y, float w, float h, int down, u32 face)
{
    u32 hi = down ? W_DARK : W_WHITE, lo = down ? W_WHITE : W_DARK;
    gfx_rect(x, y, w, h, face);
    gfx_rect(x, y, w, 1, hi);
    gfx_rect(x, y, 1, h, hi);
    gfx_rect(x, y + h - 1, w, 1, lo);
    gfx_rect(x + w - 1, y, 1, h, lo);
}

static void dotted_rect(float x, float y, float w, float h, u32 c)
{
    for (float i = 0; i < w; i += 2) { gfx_rect(x + i, y, 1, 1, c); gfx_rect(x + i, y + h - 1, 1, 1, c); }
    for (float i = 0; i < h; i += 2) { gfx_rect(x, y + i, 1, 1, c); gfx_rect(x + w - 1, y + i, 1, 1, c); }
}

// Little pixel icons, 3 screen pixels per dot.
static void pixel_icon(int tile, float x, float y, u32 c)
{
    static const char *const art[TILES][6] = {
        { "...###", "...#.#", "...#..", ".###..", "####..", ".##..." },
        { "######", "#....#", "#.##.#", "#.##.#", "#....#", "######" },
        { "......", ".####.", "#.##.#", "######", "##..##", "......" },
        { "#####.", "#.#..#", "#.#..#", "#.#..#", "#####.", "......" },
    };
    for (int j = 0; j < 6; j++)
        for (int i = 0; art[tile][j][i]; i++)
            if (art[tile][j][i] == '#') gfx_rect(x + i * 3, y + j * 3, 3, 3, c);
}

static void web_draw(void)
{
    Font *f_px = ui_font(FAMILY_SILKSCREEN, 400, 8), *f_pxb = ui_font(FAMILY_SILKSCREEN, 700, 8);
    Font *f_vt = ui_font(FAMILY_VT323, 400, 16), *f_h = ui_font(FAMILY_SERIF, 700, 22);
    Font *f_s = ui_font(FAMILY_SERIF, 400, 12), *f_sb = ui_font(FAMILY_SERIF, 700, 13);
    refresh_week();

    gfx_rect(0, 0, SCREEN_W, SCREEN_H, W_TEAL);
    for (int y = 0; y < SCREEN_H; y += 16)
        for (int x = ((y / 16) & 1) * 16; x < SCREEN_W; x += 32) gfx_rect(x, y, 16, 16, W_TEAL2);

    // The browser window.
    bevel(8, 6, 464, 245, 0, W_SILVER);
    gfx_gradient(11, 9, 458, 15, W_NAVY, HEX(0x1084D0, 255), W_NAVY, HEX(0x1084D0, 255));
    font_draw(f_pxb, 16, 20, "BORO HOME PAGE - NAVIGATOR", W_WHITE, 0);
    static const char *const box[3] = { "_", "o", "x" };
    for (int i = 0; i < 3; i++) {
        bevel(422 + i * 16, 11, 14, 11, 0, W_SILVER);
        font_draw_center(f_px, 429 + i * 16, 20, box[i], W_BLACK);
    }
    gfx_rect(14, 28, 452, 218, W_WHITE);

    // Heading.
    font_draw(f_h, 24, 52, "Welcome to Boro!", W_MAROON, 0);
    char clock[16], status[64];
    int charging, batt = sys_battery(&charging);
    sys_clock(clock, sizeof(clock));
    if (batt >= 0) snprintf(status, sizeof(status), "%d%% battery%s \xC2\xB7 %s", batt, charging ? " (charging)" : "", clock);
    else snprintf(status, sizeof(status), "Plugged in \xC2\xB7 %s", clock);
    font_draw_right(f_s, 456, 50, status, W_BLACK);
    gfx_rect(24, 60, 432, 1, W_GRAY);
    gfx_rect(24, 61, 432, 1, W_WHITE);

    // The four buttons.
    for (int i = 0; i < TILES; i++) {
        float x = 24 + i * 110, y = 68;
        int on = sel == i;
        bevel(x, y, 100, 80, on, W_FACE);
        pixel_icon(i, x + 41, y + 12, on ? W_NAVY : W_BLACK);
        float tw = font_width(f_sb, tile_names[i], 0), tx = x + 50 - tw / 2;
        font_draw(f_sb, tx, y + 50, tile_names[i], on ? W_MAROON : W_LINK, 0);
        gfx_rect(tx, y + 52, tw, 1, on ? W_MAROON : W_LINK);
        char sub[96];
        if (i == SEL_MUSIC) snprintf(sub, sizeof(sub), "%s", player_song() >= 0 ? song_title(&lib.songs[player_song()]) : "FL/AC");
        else if (i == SEL_VIDEO) snprintf(sub, sizeof(sub), "%s", vlib_continue() >= 0 ? videos[vlib_continue()].title : "TV & Movies");
        else if (i == SEL_GAMES) snprintf(sub, sizeof(sub), "%s", glib_last_title() ? glib_last_title() : "Games");
        else snprintf(sub, sizeof(sub), "Coming soon");
        font_draw_fit(f_px, x + 6, y + 70, sub, W_BLACK, 88);
        if (on) dotted_rect(x + 3, y + 3, 94, 74, W_BLACK);
        if (i == SEL_BOOKS && fmodf(t_now, 1.0f) < 0.6f) {
            gfx_rect(x + 64, y + 6, 30, 11, HEX(0xFF0000, 255));
            font_draw(f_pxb, x + 67, y + 15, "NEW!", HEX(0xFFFF00, 255), 0);
        }
    }

    // Hit counter: this week's time.
    char all[24], music[24], game[24], line[160];
    stats_format_time(all, sizeof(all), week.ms + gweek.seconds * 1000);
    stats_format_time(music, sizeof(music), week.ms);
    stats_format_time(game, sizeof(game), gweek.seconds * 1000);
    int on = sel == SEL_STATS;
    float lw = font_width(f_sb, "This week on Boro:", 0);
    font_draw(f_sb, 24, 170, "This week on Boro:", on ? W_LINK : W_BLACK, 0);
    if (on) { gfx_rect(24, 172, lw, 1, W_LINK); dotted_rect(21, 157, lw + 6, 18, W_BLACK); }
    float cx = 24 + lw + 10;
    for (const char *c = all; *c; c++) {
        if (*c == ' ') continue;
        char ch[2] = { *c, 0 };
        gfx_rect(cx, 157, 13, 18, W_BLACK);
        font_draw_center(f_vt, cx + 6.5f, 171, ch, HEX(0x00FF00, 255));
        cx += 15;
    }
    if (on) font_draw(f_s, cx + 8, 170, "\xE2\x86\x90 click for Stats!", W_MAROON, 0);
    snprintf(line, sizeof(line), "Music %s  \xC2\xB7  Games %s  \xC2\xB7  Top game: %s", music, game,
             gweek.top_count ? gweek.top_title[0] : "\xE2\x80\x94");
    font_draw_fit(f_s, 24, 190, line, W_BLACK, 432);

    // Marquee.
    gfx_rect(24, 200, 432, 17, HEX(0xFFFFCC, 255));
    gfx_rect(24, 200, 432, 1, W_GRAY);
    gfx_rect(24, 216, 432, 1, W_GRAY);
    char news[300];
    int song = player_song();
    if (song >= 0)
        snprintf(news, sizeof(news), ">>> NOW PLAYING: %s - %s <<<   *   Thanks for visiting!", song_title(&lib.songs[song]), song_artist(&lib.songs[song]));
    else
        snprintf(news, sizeof(news), ">>> Welcome to Boro! Pick a link above to get started <<<   *   Thanks for visiting!");
    float nw = font_width(f_s, news, 0), off = fmodf(t_now * 40, nw + 432);
    gfx_clip(25, 200, 430, 17);
    font_draw(f_s, 456 - off, 212, news, HEX(0xCC0000, 255), 0);
    gfx_unclip();

    font_draw(f_px, 24, 232, "BEST VIEWED ON PSP AT 480X272", W_GRAY, 0);
    font_draw_right(f_sb, 456, 234, "[ START: Settings ]", W_LINK);

    // Status bar.
    bevel(8, 253, 464, 15, 0, W_SILVER);
    if (!quit_open) hints(f_px, 264, W_BLACK);
    else font_draw(f_px, 14, 264, "Done", W_BLACK, 0);
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
    if (settings.home_style == HOME_WEB) web_draw();
    else stream_draw();
    draw_toast();
    draw_quit();
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
