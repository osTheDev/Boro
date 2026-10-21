#include "video.h"

#include <math.h>
#include <pspctrl.h>
#include <psppower.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gfx.h"
#include "photos.h"
#include "player.h"
#include "settings.h"
#include "ui.h"
#include "vlib.h"
#include "vplayer.h"

#define ACCENT HEX(0x5B8CFF, 255)
#define BG HEX(0x0E0F13, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define DIM HEX(0x8A8D99, 255)
#define CHIP HEX(0x3A3D48, 255)
#define PILL HEX(0x23252D, 255)

#define HERO_X 14
#define HERO_Y 34
#define HERO_W 200
#define HERO_H 113
#define ROW_Y 159
#define TILE_W 96
#define TILE_H 54
#define TILE_GAP 10

// Tabs 1..3 match the VCAT_ values of their folders; Shows has no tab.
enum { TAB_ALL, TAB_PHOTOS = 4, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = { "All", "Movies", "Music", "Clips", "Photos" };
static const char *const cat_names[VCAT_COUNT] = { "Videos", "Movies", "Music video", "Clips", "Shows" };

static Font *f_tab, *f_tab_b, *f_title, *f_small, *f_small_b, *f_tiny, *f_chip, *f_time;
static Texture *t_fill12, *t_dot;
static int inited;

// Browser
static int tab, sort_az, sel;
static int list[1024], list_count;
static float scroll;

// Player
static int playing = -1;
static float overlay_t, volume_t, save_t, end_t;
static int gain = 100;
static int seeking; // -1 / 1 while the d-pad is held
static float seek_ms, seek_speed;

static void init(void)
{
    inited = 1;
    f_tab = ui_font(FAMILY_SORA, 400, 11);
    f_tab_b = ui_font(FAMILY_SORA, 700, 11);
    f_title = ui_font(FAMILY_SORA, 700, 16);
    f_small = ui_font(FAMILY_SORA, 400, 10);
    f_small_b = ui_font(FAMILY_SORA, 600, 10);
    f_tiny = ui_font(FAMILY_SORA, 400, 9);
    f_chip = ui_font(FAMILY_SORA, 400, 9);
    f_time = ui_font(FAMILY_SORA, 400, 10);
    // 16 texels: narrower 8-bit textures misalign on the GE. Drawn scaled down.
    t_fill12 = tex_round_fill(12);
    t_dot = tex_round_fill(18);
}

// Where a video picks up: nowhere when resuming is off in Settings.
static u32 resume_of(const Video *v) { return settings.resume_videos ? v->resume_ms : 0; }

// ---- Browser ----------------------------------------------------------------

static int cmp_recent(const void *a, const void *b)
{
    const Video *x = &videos[*(const int *)a], *y = &videos[*(const int *)b];
    u32 kx = x->watched > x->modified ? x->watched : x->modified;
    u32 ky = y->watched > y->modified ? y->watched : y->modified;
    if (kx != ky) return kx < ky ? 1 : -1;
    return strcasecmp(x->title, y->title);
}

static int cmp_az(const void *a, const void *b)
{
    return strcasecmp(videos[*(const int *)a].title, videos[*(const int *)b].title);
}

static void build_list(int keep)
{
    int was = list_count && sel < list_count ? list[sel] : -1;
    list_count = 0;
    for (int i = 0; i < video_count && list_count < (int)(sizeof(list) / sizeof(list[0])); i++)
        if (tab == TAB_ALL || (tab != TAB_PHOTOS && videos[i].category == tab)) list[list_count++] = i;
    qsort(list, list_count, sizeof(int), sort_az ? cmp_az : cmp_recent);
    sel = 0;
    if (keep)
        for (int i = 0; i < list_count; i++) if (list[i] == was) sel = i;
    scroll = sel;
}

void video_enter(void)
{
    if (!inited) init();
    vlib_scan();
    build_list(0);
    vlib_thumbs(1);
    if (tab == TAB_PHOTOS) photos_enter();
}

int video_playing(void) { return playing >= 0; }

static void start(int index, u32 from)
{
    vlib_thumbs(0);
    // The music would fight the soundtrack.
    if (player_song() >= 0 && !player_paused()) player_toggle_pause();
    if (vplayer_open(videos[index].path, from) != 0) {
        vlib_thumbs(1);
        return;
    }
    vplayer_set_gain(gain);
    playing = index;
    overlay_t = 3;
    save_t = 0;
    end_t = 0;
    seeking = 0;
}

static void stop(int finished)
{
    vlib_set_resume(playing, finished ? 0 : vplayer_position());
    vplayer_close();
    playing = -1;
    build_list(1);
    vlib_thumbs(1);
}

static int browser_update(const Input *in, float dt)
{
    u32 p = in->pressed, nav = in->nav;
    if (tab == TAB_PHOTOS && photos_viewing()) return photos_update(in, dt);
    int was = tab;
    if (p & PSP_CTRL_LTRIGGER) tab = (tab + TAB_COUNT - 1) % TAB_COUNT;
    if (p & PSP_CTRL_RTRIGGER) tab = (tab + 1) % TAB_COUNT;
    if (tab != was) {
        build_list(0);
        if (was == TAB_PHOTOS) photos_leave();
        if (tab == TAB_PHOTOS) photos_enter();
        return 0;
    }
    if (tab == TAB_PHOTOS) {
        if (!photos_update(in, dt)) return 0;
        photos_leave();
        vlib_thumbs(0);
        return 1;
    }
    if (p & PSP_CTRL_CIRCLE) {
        vlib_thumbs(0);
        return 1;
    }
    if (p & PSP_CTRL_SQUARE) { sort_az = !sort_az; build_list(1); }
    if (!list_count) return 0;
    if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
    if (nav & PSP_CTRL_RIGHT && sel < list_count - 1) sel++;
    Video *v = &videos[list[sel]];
    if (p & PSP_CTRL_CROSS) start(list[sel], resume_of(v));
    else if (p & PSP_CTRL_TRIANGLE) start(list[sel], 0);
    return 0;
}

// A thumbnail, or a gradient placeholder until one exists.
static void draw_thumb(int index, float x, float y, float w, float h)
{
    const Texture *t = vlib_thumb(index);
    if (t) {
        gfx_filter(1);
        gfx_tex(t, x, y, w, h, 0xFFFFFFFF);
    } else {
        u32 c0, c1;
        ui_placeholder_colors(index * 7 + 3, &c0, &c1);
        u32 mid = color_lerp(c0, c1, 0.5f);
        gfx_gradient(x, y, w, h, c0, mid, mid, c1);
    }
}

static void draw_badge(float right, float y, const char *s)
{
    float w = font_width(f_chip, s, 0) + 10;
    ui_rrect(right - w, y, w, 13, 3, RGBA(0, 0, 0, 153));
    font_draw(f_chip, right - w + 5, y + 10, s, TEXT, 0);
}

static void draw_top_bar(void)
{
    float x = 14, base = 22;
    float chip_w = font_width(f_chip, "L", 0) + 8;
    ui_ring(x, base - 9, chip_w, 11, 3, CHIP);
    font_draw(f_chip, x + 4, base - 1, "L", HEX(0x6E7180, 255), 0);
    x += chip_w + 14;
    for (int i = 0; i < TAB_COUNT; i++) {
        Font *f = i == tab ? f_tab_b : f_tab;
        float w = font_width(f, tab_names[i], 0);
        font_draw(f, x, base, tab_names[i], i == tab ? TEXT : DIM, 0);
        if (i == tab) gfx_rect(x, base + 4, w, 2, ACCENT);
        x += w + 14;
    }
    chip_w = font_width(f_chip, "R", 0) + 8;
    ui_ring(x, base - 9, chip_w, 11, 3, CHIP);
    font_draw(f_chip, x + 4, base - 1, "R", HEX(0x6E7180, 255), 0);

    float right = SCREEN_W - 14;
    ui_battery(f_small, right, base - 4, MUTED);
    const char *sort = sort_az ? "Sorted A\xE2\x80\x93Z" : "Sorted by recent";
    if (tab != TAB_PHOTOS) font_draw_right(f_small, right - 62, base, sort, DIM);
}

static void draw_hero(void)
{
    Video *v = &videos[list[sel]];
    draw_thumb(list[sel], HERO_X, HERO_Y, HERO_W, HERO_H);
    float cx = HERO_X + HERO_W / 2, cy = HERO_Y + HERO_H / 2;
    gfx_nine(t_dot, 18, cx - 18, cy - 18, 36, 36, RGBA(0, 0, 0, 115));
    ui_icon(ICON_PLAY, cx - 7, cy - 8, 16, 0xFFFFFFFF);
    if (resume_of(v) && v->duration_ms) {
        gfx_rect(HERO_X, HERO_Y + HERO_H - 4, HERO_W, 4, RGBA(255, 255, 255, 51));
        gfx_rect(HERO_X, HERO_Y + HERO_H - 4, HERO_W * (float)resume_of(v) / v->duration_ms, 4, ACCENT);
    }
    ui_round_corners(HERO_X, HERO_Y, HERO_W, HERO_H, 8, BG);
    char buf[64];
    if (v->duration_ms) {
        ui_format_time(buf, sizeof(buf), v->duration_ms);
        draw_badge(HERO_X + HERO_W - 6, HERO_Y + 6, buf);
    }

    float x = HERO_X + HERO_W + 14, w = SCREEN_W - 14 - x;
    // Two lines for long titles.
    const char *t = v->title;
    if (font_width(f_title, t, 0) <= w) {
        font_draw(f_title, x, HERO_Y + 15, t, TEXT, 0);
    } else {
        char line[96];
        int n = strlen(t), cut = 0;
        for (int i = 1; i <= n; i++) {
            if (t[i] != ' ' && t[i]) continue;
            snprintf(line, sizeof(line), "%.*s", i, t);
            if (font_width(f_title, line, 0) > w) break;
            cut = i;
        }
        if (!cut) cut = n / 2;
        snprintf(line, sizeof(line), "%.*s", cut, t);
        font_draw(f_title, x, HERO_Y + 15, line, TEXT, 0);
        font_draw_fit(f_title, x, HERO_Y + 34, t + cut + (t[cut] == ' '), TEXT, w);
    }
    float y = HERO_Y + (font_width(f_title, t, 0) <= w ? 33 : 52);
    if (v->width) snprintf(buf, sizeof(buf), "%s \xC2\xB7 MP4 \xC2\xB7 %d\xC3\x97%d", cat_names[v->category], v->width, v->height);
    else snprintf(buf, sizeof(buf), "%s \xC2\xB7 MP4", cat_names[v->category]);
    font_draw_fit(f_small, x, y, buf, MUTED, w);
    char when[24];
    if (resume_of(v)) {
        ui_format_time(when, sizeof(when), resume_of(v));
        snprintf(buf, sizeof(buf), "Stopped at %s", when);
    } else {
        snprintf(buf, sizeof(buf), v->watched ? "Watched" : "Not watched yet");
    }
    font_draw(f_small, x, y + 15, buf, HEX(0xC9CBD4, 255), 0);

    float by = HERO_Y + HERO_H - 22;
    const char *first = resume_of(v) ? "Resume" : "Play";
    float bw = font_width(f_small_b, first, 0) + 20;
    gfx_nine(t_fill12, 12, x, by, bw, 22, ACCENT);
    font_draw(f_small_b, x + 10, by + 15, first, BG, 0);
    if (resume_of(v)) {
        float x2 = x + bw + 6, bw2 = font_width(f_small_b, "Start over", 0) + 20;
        gfx_nine(t_fill12, 12, x2, by, bw2, 22, PILL);
        font_draw(f_small_b, x2 + 10, by + 15, "Start over", TEXT, 0);
    }
}

static void draw_row(float dt)
{
    // Keep the selection second from the left once the row scrolls.
    float target = sel > 0 ? sel - 1 : 0;
    scroll = ui_approach(scroll, target, dt, 14);
    gfx_clip(0, ROW_Y - 4, SCREEN_W, TILE_H + 24);
    for (int i = 0; i < list_count; i++) {
        float x = 14 + (i - scroll) * (TILE_W + TILE_GAP);
        if (x > SCREEN_W || x + TILE_W < 0) continue;
        if (i == sel) ui_rrect(x - 3, ROW_Y - 3, TILE_W + 6, TILE_H + 6, 8, ACCENT);
        if (i == sel) ui_rrect(x - 1, ROW_Y - 1, TILE_W + 2, TILE_H + 2, 7, BG);
        draw_thumb(list[i], x, ROW_Y, TILE_W, TILE_H);
        const Video *v = &videos[list[i]];
        if (resume_of(v) && v->duration_ms) {
            gfx_rect(x, ROW_Y + TILE_H - 3, TILE_W, 3, RGBA(255, 255, 255, 51));
            gfx_rect(x, ROW_Y + TILE_H - 3, TILE_W * (float)resume_of(v) / v->duration_ms, 3, ACCENT);
        }
        ui_round_corners(x, ROW_Y, TILE_W, TILE_H, 6, BG);
        font_draw_fit(f_tiny, x, ROW_Y + TILE_H + 13, v->title, i == sel ? TEXT : MUTED, TILE_W);
    }
    gfx_unclip();
}

static void draw_hints(int glyphs[], const char *labels[], int n, float right, float base, u32 color)
{
    float x = right;
    for (int i = n - 1; i >= 0; i--) {
        x -= ui_hint_width(f_tiny, glyphs[i], labels[i]);
        ui_hint(f_tiny, x, base, glyphs[i], labels[i], color);
        x -= 12;
    }
}

static void browser_draw(float dt)
{
    if (tab == TAB_PHOTOS && photos_viewing()) {
        photos_draw(dt);
        return;
    }
    gfx_begin(BG);
    draw_top_bar();
    if (tab == TAB_PHOTOS) {
        photos_draw(dt);
    } else if (!list_count) {
        const char *a = video_count ? "Nothing in this tab" : "No videos yet";
        const char *b = "Put MP4s in VIDEO on your memory stick,";
        const char *c = "in Movies, Music, Clips or Shows folders.";
        font_draw_center(f_title, SCREEN_W / 2, 120, a, TEXT);
        font_draw_center(f_small, SCREEN_W / 2, 142, b, MUTED);
        font_draw_center(f_small, SCREEN_W / 2, 156, c, MUTED);
        int g[] = { ICON_CIRCLE };
        const char *l[] = { "Back" };
        draw_hints(g, l, 1, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    } else {
        draw_hero();
        draw_row(dt);
        int resume = resume_of(&videos[list[sel]]) != 0;
        int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_SQUARE, ICON_CIRCLE };
        const char *l[] = { resume ? "Resume" : "Play", "Start over", "Sort", "Back" };
        draw_hints(g, l, 4, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    }
    gfx_end();
}

// ---- Player -----------------------------------------------------------------

static void player_input(const Input *in, float dt)
{
    u32 p = in->pressed;
    if (in->pressed || (in->held & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) overlay_t = 3;
    if (p & PSP_CTRL_CIRCLE) { stop(0); return; }
    if (p & PSP_CTRL_CROSS) vplayer_toggle_pause();
    u32 pos = vplayer_position(), dur = vplayer_mp4()->duration_ms;
    if (p & PSP_CTRL_LTRIGGER) vplayer_seek(pos > 10000 ? pos - 10000 : 0);
    if (p & PSP_CTRL_RTRIGGER) vplayer_seek(pos + 10000);
    if (in->nav & PSP_CTRL_UP && gain < 100) { gain += 10; volume_t = 1.5f; }
    if (in->nav & PSP_CTRL_DOWN && gain > 0) { gain -= 10; volume_t = 1.5f; }
    vplayer_set_gain(gain);

    // Holding left or right scrubs, faster the longer it's held; letting go
    // jumps there.
    int dir = in->held & PSP_CTRL_RIGHT ? 1 : in->held & PSP_CTRL_LEFT ? -1 : 0;
    if (dir) {
        if (seeking != dir) { seek_ms = seeking ? seek_ms : pos; seek_speed = 10000; seeking = dir; }
        seek_speed = fminf(seek_speed + dt * 40000, 180000);
        seek_ms += dir * seek_speed * dt;
        if (seek_ms < 0) seek_ms = 0;
        if (seek_ms > dur) seek_ms = dur;
    } else if (seeking) {
        vplayer_seek((u32)seek_ms);
        seeking = 0;
    }
}

static void player_update(const Input *in, float dt)
{
    scePowerTick(0); // no screen dimming while watching
    if (overlay_t > 0 && !vplayer_paused()) overlay_t -= dt;
    if (volume_t > 0) volume_t -= dt;
    player_input(in, dt);
    if (playing < 0) return;
    // Remember the spot now and then, in case the PSP is switched off.
    save_t += dt;
    if (save_t > 30) { save_t = 0; vlib_set_resume(playing, vplayer_position()); }
    if (vplayer_ended()) {
        end_t += dt;
        if (end_t > 0.5f) stop(1);
    }
}

static void player_draw(void)
{
    gfx_begin(HEX(0x000000, 255));
    const Texture *f = vplayer_frame();
    const Mp4 *m = vplayer_mp4();
    if (f && vplayer_video_ok()) {
        // Fit the picture to the screen, keeping its shape.
        float s = fminf((float)SCREEN_W / f->w, (float)SCREEN_H / f->h);
        float w = f->w * s, h = f->h * s;
        gfx_filter(1);
        gfx_tex(f, (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h, 0xFFFFFFFF);
    } else if (!vplayer_video_ok()) {
        font_draw_center(f_tab_b, SCREEN_W / 2, 128, "The picture can't be shown here", TEXT);
        font_draw_center(f_small, SCREEN_W / 2, 146, "The sound keeps playing", MUTED);
    }

    float a = fminf(1.0f, overlay_t / 0.3f);
    if (vplayer_paused() || seeking) a = 1;
    if (a > 0.01f) {
        const Video *v = &videos[playing];
        gfx_gradient(0, 0, SCREEN_W, 44, RGBA(0, 0, 0, (int)(178 * a)), RGBA(0, 0, 0, (int)(178 * a)), RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 0));
        font_draw_fit(f_tab_b, 14, 22, v->title, color_alpha(TEXT, a), SCREEN_W - 90);
        ui_battery(f_small, SCREEN_W - 14, 18, color_alpha(0xFFFFFFFF, a));

        float top = SCREEN_H - 84;
        gfx_gradient(0, top, SCREEN_W, 84, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 0), RGBA(0, 0, 0, (int)(204 * a)), RGBA(0, 0, 0, (int)(204 * a)));
        u32 dur = m->duration_ms, pos = seeking ? (u32)seek_ms : vplayer_position();
        char t0[16], t1[16];
        ui_format_time(t0, sizeof(t0), pos);
        ui_format_time(t1, sizeof(t1), dur);
        float by = top + 34;
        font_draw(f_time, 14, by + 4, t0, color_alpha(TEXT, a), 0);
        float bx = 14 + font_width(f_time, "0:00:00", 0) + 8 - (dur < 3600000 ? 14 : 0);
        float br = SCREEN_W - 14 - font_width(f_time, t1, 0) - 8;
        font_draw_right(f_time, SCREEN_W - 14, by + 4, t1, color_alpha(TEXT, a));
        float k = dur ? fminf(1.0f, (float)pos / dur) : 0;
        gfx_rect(bx, by, br - bx, 4, RGBA(255, 255, 255, (int)(64 * a)));
        gfx_rect(bx, by, (br - bx) * k, 4, color_alpha(ACCENT, a));
        ui_rrect(bx + (br - bx) * k - 6, by - 4, 12, 12, 6, color_alpha(0xFFFFFFFF, a));

        float cy = by + 26, cx = SCREEN_W / 2;
        gfx_nine(t_dot, 18, cx - 19, cy - 19, 38, 38, color_alpha(0xFFFFFFFF, a));
        ui_icon(vplayer_paused() ? ICON_PLAY : ICON_PAUSE, cx - 8, cy - 8, 16, color_alpha(BG, a));
        font_draw_center(f_tab_b, cx - 50, cy + 4, "\xE2\x88\x92" "10s", color_alpha(0xFFFFFFFF, a));
        font_draw_center(f_tab_b, cx + 50, cy + 4, "+10s", color_alpha(0xFFFFFFFF, a));

        int g[] = { ICON_CROSS, ICON_CIRCLE };
        const char *l[] = { vplayer_paused() ? "Play" : "Pause", "Exit" };
        u32 hc = color_alpha(HEX(0xD5D7DF, 255), a);
        float hy = SCREEN_H - 6;
        float x = 14;
        font_draw(f_chip, x, hy, "L/R Skip 10s", hc, 0);
        x += font_width(f_chip, "L/R Skip 10s", 0) + 12;
        x = ui_hint_arrows(f_chip, x, hy, 0, "Seek", hc) + 12;
        ui_hint_arrows(f_chip, x, hy, 1, "Volume", hc);
        draw_hints(g, l, 2, SCREEN_W - 14, hy, hc);
    }
    if (volume_t > 0) {
        float va = fminf(1.0f, volume_t / 0.3f);
        float x = SCREEN_W - 16 - 26, y = 62;
        gfx_nine(t_fill12, 12, x, y, 26, 100, RGBA(0, 0, 0, (int)(140 * va)));
        gfx_rect(x + 11, y + 10, 4, 60, RGBA(255, 255, 255, (int)(64 * va)));
        gfx_rect(x + 11, y + 10 + 60 * (1 - gain / 100.0f), 4, 60 * gain / 100.0f, color_alpha(0xFFFFFFFF, va));
        ui_icon(gain ? ICON_SPEAKER : ICON_MUTE, x + 7, y + 78, 12, color_alpha(0xFFFFFFFF, va));
    }
    gfx_end();
}

// ---- Entry points -------------------------------------------------------------

int video_update(const Input *in, float dt)
{
    if (playing >= 0) {
        player_update(in, dt);
        return 0;
    }
    return browser_update(in, dt);
}

void video_draw(float dt)
{
    if (playing >= 0) player_draw();
    else browser_draw(dt);
}
