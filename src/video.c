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
#include "plib.h"
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
#define GRID_COLS 4

enum { TAB_HOME, TAB_TVM, TAB_MUSIC, TAB_PHOTOS, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = { "Home", "TV & Movies", "Music Videos", "Photos" };

static Font *f_tab, *f_tab_b, *f_title, *f_hero, *f_small, *f_small_b, *f_tiny, *f_chip, *f_time;
static Texture *t_fill12, *t_dot;
static int inited;

// Browser
static int tab, sort_az, sel;
static int list[1024], list_count;
static float scroll;

// Player
static int playing = -1;
static float overlay_t, volume_t, save_t, end_t, fit_t;
static int gain = 100;
static int seeking; // -1 / 1 while the d-pad is held
static float seek_ms, seek_speed;

static void init(void)
{
    inited = 1;
    f_tab = ui_font(FAMILY_SORA, 400, 11);
    f_tab_b = ui_font(FAMILY_SORA, 700, 11);
    f_title = ui_font(FAMILY_SORA, 700, 16);
    f_hero = ui_font(FAMILY_SORA, 700, 20);
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

// Clips live in Photos, among the pictures.
static int is_music(const Video *v) { return v->category == VCAT_MUSIC; }
static int is_clip(const Video *v) { return v->category == VCAT_CLIPS; }

static int tab_has(int t, const Video *v)
{
    if (t == TAB_TVM) return !is_music(v) && !is_clip(v);
    if (t == TAB_MUSIC) return is_music(v);
    return 0;
}

static void build_home(void);

static void build_list(int keep)
{
    int was = list_count && sel < list_count ? list[sel] : -1;
    list_count = 0;
    for (int i = 0; i < video_count && list_count < (int)(sizeof(list) / sizeof(list[0])); i++)
        if (tab_has(tab, &videos[i])) list[list_count++] = i;
    qsort(list, list_count, sizeof(int), sort_az ? cmp_az : cmp_recent);
    sel = 0;
    if (keep)
        for (int i = 0; i < list_count; i++) if (list[i] == was) sel = i;
    scroll = sel;
    build_home();
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

void video_save(void)
{
    if (playing >= 0) vlib_set_resume(playing, vplayer_position());
}

static void start(int index, u32 from)
{
    vlib_thumbs(0);
    plib_thumbs(0);
    plib_thumbs_wait();
    // The music would fight the soundtrack.
    if (player_song() >= 0 && !player_paused()) player_toggle_pause();
    if (vplayer_open(videos[index].path, from) != 0) {
        vlib_thumbs(1);
        if (tab == TAB_PHOTOS) plib_thumbs(1);
        return;
    }
    vplayer_set_gain(gain);
    playing = index;
    overlay_t = 3;
    fit_t = 0;
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
    if (tab == TAB_PHOTOS) plib_thumbs(1);
}

// ---- Home ---------------------------------------------------------------------
//
// A banner for what to watch next, then a row of TV & Movies posters (each show
// once) and a row of music videos. Items: a video index, or -(show + 1).

#define POSTER_TW 54
#define POSTER_TH 78
#define HOME_ROWS 2

static int rows[HOME_ROWS][512], row_count[HOME_ROWS], row_sel[HOME_ROWS];
static float row_scroll[HOME_ROWS];
static int home_row;
static float page_y;
static int hero = -1, hero_new; // hero_new: nothing stopped partway, so the newest

static int item_video(int item) { return item >= 0 ? item : vlib_next_up(-item - 1); }

static u32 item_time(int item)
{
    if (item < 0) return vlib_last_used(-item - 1);
    const Video *v = &videos[item];
    return v->watched > v->modified ? v->watched : v->modified;
}

static int cmp_item(const void *a, const void *b)
{
    u32 x = item_time(*(const int *)a), y = item_time(*(const int *)b);
    return x == y ? 0 : x < y ? 1 : -1;
}

// TV & Movies tab and show page
enum { FILTER_ALL, FILTER_SHOWS, FILTER_MOVIES, FILTER_COUNT };
static const char *const filter_names[FILTER_COUNT] = { "All", "Shows", "Movies" };
static int tvm[512], tvm_count, tvm_sel, tvm_filter;
static float tvm_scroll;

static int show_open = -1, show_sel, season_sel;
static int seasons[64], season_count;
static int eps[512], ep_count;
static float show_scroll;

static void build_tvm(void);
static void build_episodes(int keep_video);

static void build_home(void)
{
    int keep[HOME_ROWS];
    for (int r = 0; r < HOME_ROWS; r++) keep[r] = row_count[r] ? rows[r][row_sel[r]] : 0;
    row_count[0] = row_count[1] = 0;
    for (int s = 0; s < show_count && row_count[0] < 512; s++)
        if (shows[s].count) rows[0][row_count[0]++] = -(s + 1);
    for (int i = 0; i < video_count; i++) {
        const Video *v = &videos[i];
        int r = is_music(v) ? 1 : 0;
        if (is_clip(v) || (r == 0 && v->show >= 0) || row_count[r] >= 512) continue;
        rows[r][row_count[r]++] = i;
    }
    for (int r = 0; r < HOME_ROWS; r++) {
        qsort(rows[r], row_count[r], sizeof(int), cmp_item);
        row_sel[r] = 0;
        for (int i = 0; i < row_count[r]; i++) if (rows[r][i] == keep[r]) row_sel[r] = i;
        row_scroll[r] = row_sel[r] > 0 ? row_sel[r] - 1 : 0;
    }
    if (!row_count[home_row]) home_row = row_count[0] ? 0 : 1;
    hero = vlib_continue();
    hero_new = hero < 0;
    if (hero_new)
        for (int i = 0; i < video_count; i++)
            if (hero < 0 || videos[i].modified > videos[hero].modified) hero = i;
    build_tvm();
    if (show_open >= 0) build_episodes(ep_count ? eps[show_sel] : -1);
}


// ---- TV & Movies tab and show page --------------------------------------------


static void build_tvm(void)
{
    int was = tvm_count ? tvm[tvm_sel] : 0;
    tvm_count = 0;
    for (int i = 0; i < row_count[0]; i++) {
        int it = rows[0][i];
        if ((tvm_filter == FILTER_SHOWS && it >= 0) || (tvm_filter == FILTER_MOVIES && it < 0)) continue;
        tvm[tvm_count++] = it;
    }
    tvm_sel = 0;
    for (int i = 0; i < tvm_count; i++) if (tvm[i] == was) tvm_sel = i;
    tvm_scroll = tvm_sel > 0 ? tvm_sel - 1 : 0;
}

// The season's episodes into eps; keeps the selection on the same video.
static void build_episodes(int keep_video)
{
    static int all[512];
    int n = vlib_episodes(show_open, all, 512);
    ep_count = 0;
    show_sel = 0;
    for (int i = 0; i < n; i++) {
        if (season_count > 1 && videos[all[i]].season != seasons[season_sel]) continue;
        if (all[i] == keep_video) show_sel = ep_count;
        eps[ep_count++] = all[i];
    }
    show_scroll = show_sel > 0 ? show_sel - 1 : 0;
}

static void open_show(int show)
{
    static int all[512];
    show_open = show;
    // Opens on the first episode; the Next Up line still says where you left off.
    int n = vlib_episodes(show, all, 512), next = n ? all[0] : -1;
    season_count = 0;
    for (int i = 0; i < n; i++) {
        int sn = videos[all[i]].season, dup = 0;
        for (int k = 0; k < season_count; k++) if (seasons[k] == sn) dup = 1;
        if (!dup && season_count < 64) seasons[season_count++] = sn;
    }
    season_sel = 0;
    for (int k = 0; k < season_count; k++) if (next >= 0 && seasons[k] == videos[next].season) season_sel = k;
    build_episodes(next);
}

// An episode's name without the show and S01E03 part of the file name.
static void episode_name(const Video *v, char *out, int size)
{
    const char *t = v->title, *rest = NULL;
    for (const char *p = t; *p && !rest; p++) {
        if ((*p == 'S' || *p == 's') && p[1] >= '0' && p[1] <= '9') {
            const char *q = p + 1;
            while (*q >= '0' && *q <= '9') q++;
            if ((*q == 'E' || *q == 'e') && q[1] >= '0' && q[1] <= '9') {
                q++;
                while (*q >= '0' && *q <= '9') q++;
                rest = q;
            }
        }
    }
    if (rest) {
        while (*rest == ' ' || *rest == '.' || *rest == '-' || *rest == '_') rest++;
        snprintf(out, size, "%s", rest);
        for (char *c = out; *c; c++) if (*c == '.') *c = ' ';
        if (*out) return;
    }
    snprintf(out, size, "%s", t);
}

static void season_label(int sn, char *out, int size)
{
    if (sn) snprintf(out, size, "Season %d", sn);
    else snprintf(out, size, "%s", season_count > 1 ? "Other" : "Season 1");
}

static void start(int index, u32 from);

static int show_update(const Input *in)
{
    u32 p = in->pressed, nav = in->nav;
    if (p & PSP_CTRL_CIRCLE) { show_open = -1; return 0; }
    int was = season_sel;
    if (p & PSP_CTRL_LTRIGGER && season_sel > 0) season_sel--;
    if (p & PSP_CTRL_RTRIGGER && season_sel < season_count - 1) season_sel++;
    if (season_sel != was) { build_episodes(-1); return 0; }
    if (!ep_count) return 0;
    if (nav & PSP_CTRL_UP && show_sel > 0) show_sel--;
    if (nav & PSP_CTRL_DOWN && show_sel < ep_count - 1) show_sel++;
    const Video *v = &videos[eps[show_sel]];
    if (p & PSP_CTRL_CROSS) start(eps[show_sel], resume_of(v));
    else if (p & PSP_CTRL_TRIANGLE) start(eps[show_sel], 0);
    return 0;
}

static int tvm_update(const Input *in)
{
    u32 p = in->pressed, nav = in->nav;
    if (p & PSP_CTRL_CIRCLE) {
        vlib_thumbs(0);
        return 1;
    }
    if (p & PSP_CTRL_SQUARE) { tvm_filter = (tvm_filter + 1) % FILTER_COUNT; build_tvm(); }
    if (!tvm_count) return 0;
    if (nav & PSP_CTRL_LEFT && tvm_sel > 0) tvm_sel--;
    if (nav & PSP_CTRL_RIGHT && tvm_sel < tvm_count - 1) tvm_sel++;
    int it = tvm[tvm_sel];
    if (p & PSP_CTRL_CROSS) {
        if (it < 0) open_show(-it - 1);
        else start(it, resume_of(&videos[it]));
    } else if (p & PSP_CTRL_TRIANGLE && it >= 0) {
        start(it, 0);
    }
    return 0;
}

static int home_update(const Input *in)
{
    u32 p = in->pressed, nav = in->nav;
    if (p & PSP_CTRL_CIRCLE) {
        vlib_thumbs(0);
        return 1;
    }
    if (p & PSP_CTRL_TRIANGLE && hero >= 0) { start(hero, resume_of(&videos[hero])); return 0; }
    if (nav & PSP_CTRL_UP && home_row > 0 && row_count[home_row - 1]) home_row--;
    if (nav & PSP_CTRL_DOWN && home_row < HOME_ROWS - 1 && row_count[home_row + 1]) home_row++;
    int r = home_row, n = row_count[r];
    if (!n) return 0;
    if (nav & PSP_CTRL_LEFT && row_sel[r] > 0) row_sel[r]--;
    if (nav & PSP_CTRL_RIGHT && row_sel[r] < n - 1) row_sel[r]++;
    if (p & PSP_CTRL_CROSS) {
        int it = rows[r][row_sel[r]];
        if (it < 0) open_show(-it - 1);
        else start(it, resume_of(&videos[it]));
    }
    return 0;
}

static int browser_update(const Input *in, float dt)
{
    u32 p = in->pressed, nav = in->nav;
    if (tab == TAB_PHOTOS && photos_viewing()) return photos_update(in, dt);
    if (show_open >= 0) return show_update(in);
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
        int r = photos_update(in, dt), clip = photos_take_play();
        if (clip >= 0) start(clip, resume_of(&videos[clip]));
        if (!r) return 0;
        photos_leave();
        vlib_thumbs(0);
        return 1;
    }
    if (tab == TAB_HOME) return home_update(in);
    if (tab == TAB_TVM) return tvm_update(in);
    if (p & PSP_CTRL_CIRCLE) {
        vlib_thumbs(0);
        return 1;
    }
    if (p & PSP_CTRL_SQUARE) { sort_az = !sort_az; build_list(1); }
    if (!list_count) return 0;
    if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
    if (nav & PSP_CTRL_RIGHT && sel < list_count - 1) sel++;
    if (nav & PSP_CTRL_UP && sel >= GRID_COLS) sel -= GRID_COLS;
    if (nav & PSP_CTRL_DOWN && sel + GRID_COLS < list_count) sel += GRID_COLS;
    else if (nav & PSP_CTRL_DOWN && sel / GRID_COLS < (list_count - 1) / GRID_COLS) sel = list_count - 1;
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
    const char *sort = sort_az ? "A\xE2\x80\x93Z" : "Recent";
    if (tab == TAB_TVM || tab == TAB_MUSIC) font_draw_right(f_small, right - 62, base, sort, DIM);
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

// A video's thumbnail cut to the middle, or the placeholder.
static void draw_thumb_crop(int index, float x, float y, float w, float h)
{
    const Texture *t = vlib_thumb(index);
    if (!t) { draw_thumb(index, x, y, w, h); return; }
    float sw = t->w, sh = t->h;
    if (sw / sh > w / h) sw = sh * w / h; else sh = sw * h / w;
    gfx_filter(1);
    gfx_tex_part(t, (t->w - sw) / 2, (t->h - sh) / 2, sw, sh, x, y, w, h, 0xFFFFFFFF);
}

static void draw_poster(int item, float x, float y, float w, float h)
{
    const Texture *t = item < 0 ? vlib_show_poster(-item - 1) : vlib_video_poster(item);
    int v = item_video(item);
    if (t) { gfx_filter(1); gfx_tex(t, x, y, w, h, 0xFFFFFFFF); }
    else if (v >= 0) draw_thumb_crop(v, x, y, w, h);
}

static void draw_progress(float x, float y, float w, const Video *v)
{
    if (!resume_of(v) || !v->duration_ms) return;
    gfx_rect(x, y, w, 3, RGBA(255, 255, 255, 51));
    gfx_rect(x, y, w * (float)resume_of(v) / v->duration_ms, 3, ACCENT);
}

static void draw_home_hero(float oy)
{
    if (hero < 0) return;
    const Video *v = &videos[hero];
    float ix = 180, iy = 28 + oy, iw = SCREEN_W - ix, ih = 128;
    draw_thumb_crop(hero, ix, iy, iw, ih);
    u32 clear = BG & 0x00FFFFFF;
    gfx_gradient(ix, iy, 110, ih, BG, clear, BG, clear);
    gfx_gradient(ix, iy + ih - 34, iw, 34, clear, clear, BG, BG);

    float x = 14, y = 42 + oy, w = 200;
    font_draw(f_chip, x, y, hero_new ? "NEW" : "CONTINUE WATCHING", ACCENT, 0);
    const char *name = v->show >= 0 ? shows[v->show].name : v->title;
    font_draw_fit(f_hero, x, y + 24, name, TEXT, w);
    char buf[160], left[24] = "";
    if (resume_of(v) && v->duration_ms > resume_of(v)) {
        u32 m = (v->duration_ms - resume_of(v) + 59999) / 60000;
        snprintf(left, sizeof(left), "%u min left", (unsigned)m);
    }
    char ep[96];
    episode_name(v, ep, sizeof(ep));
    if (v->show >= 0 && v->episode)
        snprintf(buf, sizeof(buf), "S%d E%d \xC2\xB7 %s%s%s", v->season ? v->season : 1, v->episode, ep, *left ? " \xC2\xB7 " : "", left);
    else if (v->show >= 0)
        snprintf(buf, sizeof(buf), "%s%s%s", ep, *left ? " \xC2\xB7 " : "", left);
    else if (*left)
        snprintf(buf, sizeof(buf), "%s", left);
    else if (v->duration_ms) {
        char d[16];
        ui_format_time(d, sizeof(d), v->duration_ms);
        if (v->year) snprintf(buf, sizeof(buf), "%d \xC2\xB7 %s \xC2\xB7 %s", v->year, d, v->watched ? "Watched" : "Not watched yet");
        else snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s", d, v->watched ? "Watched" : "Not watched yet");
    } else snprintf(buf, sizeof(buf), "%s", v->watched ? "Watched" : "Not watched yet");
    font_draw_fit(f_small, x, y + 42, buf, MUTED, w + 40);
    if (resume_of(v) && v->duration_ms) {
        gfx_rect(x, y + 50, 150, 3, RGBA(255, 255, 255, 40));
        gfx_rect(x, y + 50, 150 * (float)resume_of(v) / v->duration_ms, 3, ACCENT);
    }
    float by = y + 60;
    const char *first = resume_of(v) ? "Resume" : "Play";
    float bw = font_width(f_small_b, first, 0) + 34;
    gfx_nine(t_fill12, 12, x, by, bw, 22, ACCENT);
    ui_icon(ICON_TRIANGLE, x + 8, by + 6, 10, BG);
    font_draw(f_small_b, x + 22, by + 15, first, BG, 0);
}

static void draw_home_row(int r, float y, float dt)
{
    int n = row_count[r];
    if (!n) return;
    int music = r == 1;
    float tw = music ? TILE_W : POSTER_TW, th = music ? TILE_H : POSTER_TH, gap = music ? TILE_GAP : 8;
    char label[32];
    font_draw(f_tab_b, 14, y, music ? "Music Videos" : "TV & Movies", TEXT, 0);
    snprintf(label, sizeof(label), "%d", n);
    font_draw(f_small, 14 + font_width(f_tab_b, music ? "Music Videos" : "TV & Movies", 0) + 6, y, label, DIM, 0);
    float target = row_sel[r] > 0 ? row_sel[r] - 1 : 0;
    row_scroll[r] = ui_approach(row_scroll[r], target, dt, 14);
    float ty = y + 12;
    if (ty > SCREEN_H || ty + th < 30) return;
    gfx_clip(0, 26, SCREEN_W, SCREEN_H - 26 - 18);
    for (int i = 0; i < n; i++) {
        float x = 14 + (i - row_scroll[r]) * (tw + gap);
        if (x > SCREEN_W || x + tw < 0) continue;
        int item = rows[r][i], on = r == home_row && i == row_sel[r];
        if (on) ui_rrect(x - 3, ty - 3, tw + 6, th + 6, 8, ACCENT);
        if (on) ui_rrect(x - 1, ty - 1, tw + 2, th + 2, 7, BG);
        if (music) draw_thumb(item, x, ty, tw, th); else draw_poster(item, x, ty, tw, th);
        int v = item_video(item);
        if (v >= 0) draw_progress(x, ty + th - 3, tw, &videos[v]);
        ui_round_corners(x, ty, tw, th, 5, BG);
        if (!music && item >= 0) {
            float bw = font_width(f_chip, "MOVIE", 0) + 6;
            ui_rrect(x + 3, ty + 3, bw, 11, 3, RGBA(0, 0, 0, 190));
            font_draw(f_chip, x + 6, ty + 11, "MOVIE", TEXT, 0);
        }
        const char *name = item < 0 ? shows[-item - 1].name : videos[item].title;
        font_draw_fit(f_tiny, x, ty + th + 11, name, on ? TEXT : MUTED, tw);
    }
    gfx_unclip();
}

static void home_draw(float dt)
{
    // Moving to the second row slides the page up under the tabs.
    float want = home_row == 1 && row_count[0] ? -(146 + POSTER_TH + 30 - 44) : 0;
    page_y = ui_approach(page_y, want, dt, 12);
    gfx_clip(0, 26, SCREEN_W, SCREEN_H - 26);
    draw_home_hero(page_y);
    gfx_unclip();
    float y = 146 + page_y;
    if (row_count[0]) { draw_home_row(0, y, dt); y += POSTER_TH + 30; }
    draw_home_row(1, y, dt);
    int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE };
    const char *l[] = { "Open", hero >= 0 && resume_of(&videos[hero]) ? "Resume" : "Play newest", "Back" };
    gfx_rect(0, SCREEN_H - 20, SCREEN_W, 20, BG);
    draw_hints(g, l, 3, SCREEN_W - 14, SCREEN_H - 8, MUTED);
}

static void draw_pill(float x, float y, const char *label, int on)
{
    float w = font_width(f_small, label, 0) + 18;
    if (on) gfx_nine(t_fill12, 12, x, y, w, 17, TEXT);
    else ui_ring(x, y, w, 17, 8, CHIP);
    font_draw(f_small, x + 9, y + 12, label, on ? BG : MUTED, 0);
}

static float pill_width(const char *label) { return font_width(f_small, label, 0) + 18 + 6; }

static void minutes(char *out, int size, u32 ms)
{
    u32 m = (ms + 59999) / 60000;
    if (m >= 60) snprintf(out, size, "%uh %um", (unsigned)(m / 60), (unsigned)(m % 60));
    else snprintf(out, size, "%u min", (unsigned)m);
}

// "14 min left", or the length, or "".
static void time_note(const Video *v, char *out, int size)
{
    *out = 0;
    if (resume_of(v) && v->duration_ms > resume_of(v)) {
        minutes(out, size, v->duration_ms - resume_of(v));
        strncat(out, " left", size - strlen(out) - 1);
    } else if (v->duration_ms) {
        minutes(out, size, v->duration_ms);
    }
}

// "1999 · 1h 26m" (either part may be missing).
static void year_note(const Video *v, char *out, int size)
{
    char t[32];
    time_note(v, t, sizeof(t));
    if (v->year && *t) snprintf(out, size, "%d \xC2\xB7 %s", v->year, t);
    else if (v->year) snprintf(out, size, "%d", v->year);
    else snprintf(out, size, "%s", t);
}

static int season_total(int show)
{
    int seen[64], n = 0;
    for (int i = 0; i < video_count; i++) {
        if (videos[i].show != show) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) if (seen[k] == videos[i].season) dup = 1;
        if (!dup && n < 64) seen[n++] = videos[i].season;
    }
    return n ? n : 1;
}

static void tvm_draw(float dt)
{
    float x = 14;
    for (int i = 0; i < FILTER_COUNT; i++) { draw_pill(x, 32, filter_names[i], i == tvm_filter); x += pill_width(filter_names[i]); }
    int ns = 0, nm = 0;
    for (int i = 0; i < row_count[0]; i++) { if (rows[0][i] < 0) ns++; else nm++; }
    char buf[160], note[32];
    snprintf(buf, sizeof(buf), "%d show%s \xC2\xB7 %d movie%s", ns, ns == 1 ? "" : "s", nm, nm == 1 ? "" : "s");
    font_draw_right(f_small, SCREEN_W - 14, 44, buf, DIM);
    if (!tvm_count) {
        font_draw_center(f_title, SCREEN_W / 2, 130, tvm_filter == FILTER_MOVIES ? "No movies yet" : "No shows yet", TEXT);
        font_draw_center(f_small, SCREEN_W / 2, 150, tvm_filter == FILTER_MOVIES ? "Movies go in VIDEO/Movies." : "Shows go in VIDEO/Movies/<Show>/.", MUTED);
    } else {
        float tw = 58, th = 84, gap = 7, ty = 58;
        tvm_scroll = ui_approach(tvm_scroll, tvm_sel > 0 ? tvm_sel - 1 : 0, dt, 14);
        gfx_clip(0, 50, SCREEN_W, 120);
        for (int i = 0; i < tvm_count; i++) {
            float px = 14 + (i - tvm_scroll) * (tw + gap);
            if (px > SCREEN_W || px + tw < 0) continue;
            int it = tvm[i], on = i == tvm_sel, v = item_video(it);
            if (on) ui_rrect(px - 3, ty - 3, tw + 6, th + 6, 8, ACCENT);
            if (on) ui_rrect(px - 1, ty - 1, tw + 2, th + 2, 7, BG);
            draw_poster(it, px, ty, tw, th);
            if (v >= 0) draw_progress(px, ty + th - 3, tw, &videos[v]);
            ui_round_corners(px, ty, tw, th, 5, BG);
            if (it >= 0) {
                float bw = font_width(f_chip, "MOVIE", 0) + 6;
                ui_rrect(px + 3, ty + 3, bw, 11, 3, RGBA(0, 0, 0, 190));
                font_draw(f_chip, px + 6, ty + 11, "MOVIE", TEXT, 0);
            }
            font_draw_fit(f_tiny, px, ty + th + 12, it < 0 ? shows[-it - 1].name : videos[it].title, on ? TEXT : MUTED, tw);
            if (it < 0) {
                int n = season_total(-it - 1), yr = shows[-it - 1].year;
                if (yr) snprintf(note, sizeof(note), "%d \xC2\xB7 %d season%s", yr, n, n == 1 ? "" : "s");
                else snprintf(note, sizeof(note), "%d season%s", n, n == 1 ? "" : "s");
            }
            else year_note(&videos[it], note, sizeof(note));
            font_draw_fit(f_chip, px, ty + th + 23, note, DIM, tw);
        }
        gfx_unclip();

        int it = tvm[tvm_sel];
        float y = 196;
        if (it < 0) {
            const Show *sh = &shows[-it - 1];
            int n = season_total(-it - 1);
            font_draw_fit(f_title, 14, y, sh->name, TEXT, SCREEN_W - 28);
            char yr[16] = "";
            if (sh->year) snprintf(yr, sizeof(yr), " \xC2\xB7 %d", sh->year);
            snprintf(buf, sizeof(buf), "Show%s \xC2\xB7 %d season%s \xC2\xB7 %d episode%s \xC2\xB7 %d watched", yr, n, n == 1 ? "" : "s", sh->count, sh->count == 1 ? "" : "s", sh->watched);
            font_draw_fit(f_small, 14, y + 17, buf, MUTED, SCREEN_W - 28);
            int nx = vlib_next_up(-it - 1);
            if (nx >= 0) {
                const Video *v = &videos[nx];
                char ep[96];
                episode_name(v, ep, sizeof(ep));
                time_note(v, note, sizeof(note));
                if (v->episode) snprintf(buf, sizeof(buf), "%s: E%d \xC2\xB7 %s", resume_of(v) ? "Continue" : "Next up", v->episode, ep);
                else snprintf(buf, sizeof(buf), "%s: %s", resume_of(v) ? "Continue" : "Next up", ep);
                font_draw_fit(f_small, 14, y + 31, buf, MUTED, SCREEN_W - 28);
            }
        } else {
            const Video *v = &videos[it];
            font_draw_fit(f_title, 14, y, v->title, TEXT, SCREEN_W - 28);
            year_note(v, note, sizeof(note));
            snprintf(buf, sizeof(buf), "Movie%s%s \xC2\xB7 %s", *note ? " \xC2\xB7 " : "", note, resume_of(v) ? "Stopped partway" : v->watched ? "Watched" : "Not watched yet");
            font_draw_fit(f_small, 14, y + 17, buf, MUTED, SCREEN_W - 28);
        }
    }
    int it = tvm_count ? tvm[tvm_sel] : 0;
    int movie = tvm_count && it >= 0, resume = movie && resume_of(&videos[it]);
    int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_SQUARE, ICON_CIRCLE };
    const char *l[] = { !tvm_count ? "Open" : movie ? (resume ? "Resume" : "Play") : "Open", "Start over", "Filter", "Back" };
    if (movie) draw_hints(g, l, 4, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    else {
        int g2[] = { ICON_CROSS, ICON_SQUARE, ICON_CIRCLE };
        const char *l2[] = { "Open", "Filter", "Back" };
        draw_hints(g2, l2, 3, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    }
}

static void show_draw(float dt)
{
    gfx_begin(BG);
    const Show *sh = &shows[show_open];
    int next = vlib_next_up(show_open);
    // Backdrop: the next episode's picture fading into the page.
    if (next >= 0 && vlib_thumb(next)) {
        draw_thumb_crop(next, 0, 0, SCREEN_W, 124);
        gfx_rect(0, 0, SCREEN_W, 124, RGBA(14, 15, 19, 150));
        gfx_gradient(0, 64, SCREEN_W, 60, BG & 0x00FFFFFF, BG & 0x00FFFFFF, BG, BG);
    }
    font_draw(f_small, 14, 20, "TV & Movies", MUTED, 0);
    ui_battery(f_small, SCREEN_W - 14, 16, MUTED);
    draw_poster(-(show_open + 1), 14, 30, 66, 96);
    ui_round_corners(14, 30, 66, 96, 5, BG);
    font_draw_fit(f_hero, 92, 60, sh->name, TEXT, SCREEN_W - 106);
    char buf[160], note[32];
    int n = season_total(show_open);
    char yr[16] = "";
    if (sh->year) snprintf(yr, sizeof(yr), "%d \xC2\xB7 ", sh->year);
    snprintf(buf, sizeof(buf), "%s%d season%s \xC2\xB7 %d episode%s \xC2\xB7 %d watched", yr, n, n == 1 ? "" : "s", sh->count, sh->count == 1 ? "" : "s", sh->watched);
    font_draw_fit(f_small, 92, 76, buf, MUTED, SCREEN_W - 106);
    if (season_count > 1) {
        float x = 92;
        for (int k = 0; k < season_count && x < SCREEN_W - 40; k++) {
            char lab[24];
            season_label(seasons[k], lab, sizeof(lab));
            draw_pill(x, 88, lab, k == season_sel);
            x += pill_width(lab);
        }
    }

    float top = 136, rh = 29;
    int vis = 4;
    float target = show_sel - 1;
    if (target > ep_count - vis) target = ep_count - vis;
    if (target < 0) target = 0;
    show_scroll = ui_approach(show_scroll, target, dt, 14);
    gfx_clip(0, top - 4, SCREEN_W, rh * vis + 2);
    for (int i = 0; i < ep_count; i++) {
        float y = top + (i - show_scroll) * rh;
        if (y < top - rh || y > top + rh * vis) continue;
        const Video *v = &videos[eps[i]];
        if (i == show_sel) { ui_rrect(10, y - 3, SCREEN_W - 20, 27, 5, HEX(0x1E2027, 255)); ui_ring(10, y - 3, SCREEN_W - 20, 27, 5, ACCENT); }
        draw_thumb_crop(eps[i], 20, y, 40, 22);
        draw_progress(20, y + 19, 40, v);
        ui_round_corners(20, y, 40, 22, 3, i == show_sel ? HEX(0x1E2027, 255) : BG);
        char ep[96];
        episode_name(v, ep, sizeof(ep));
        if (v->episode) snprintf(buf, sizeof(buf), "%d. %s", v->episode, ep);
        else snprintf(buf, sizeof(buf), "%s", ep);
        font_draw_fit(f_small_b, 70, y + 9, buf, TEXT, SCREEN_W - 150);
        time_note(v, note, sizeof(note));
        font_draw(f_tiny, 70, y + 21, note, MUTED, 0);
        if (v->watched && !v->resume_ms) font_draw_right(f_tiny, SCREEN_W - 22, y + 15, "Watched", ACCENT);
    }
    gfx_unclip();
    if (!ep_count) font_draw_center(f_small, SCREEN_W / 2, 180, "No episodes in this season", MUTED);

    int resume = ep_count && resume_of(&videos[eps[show_sel]]);
    if (season_count > 1) {
        int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE };
        const char *l[] = { resume ? "Resume" : "Play", "Start over", "Back" };
        font_draw(f_tiny, 14, SCREEN_H - 8, "L/R Season", MUTED, 0);
        draw_hints(g, l, 3, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    } else {
        int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE };
        const char *l[] = { resume ? "Resume" : "Play", "Start over", "Back" };
        draw_hints(g, l, 3, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    }
    ui_volume(f_small_b, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}

// "Artist - Song" names split in two; the artist is "" when there's no dash.
static void split_name(const char *t, char *artist, int asize, char *song, int ssize)
{
    const char *d = strstr(t, " - ");
    if (!d) { *artist = 0; snprintf(song, ssize, "%s", t); return; }
    snprintf(artist, asize, "%.*s", (int)(d - t), t);
    snprintf(song, ssize, "%s", d + 3);
}

// Music Videos: a grid of thumbnails, scrolling a row at a time.
static void draw_grid(float dt)
{
    float tw = 104, th = 58, gx = 10, rh = th + 30, top = 36;
    int row = sel / GRID_COLS, rows_n = (list_count + GRID_COLS - 1) / GRID_COLS;
    float target = row > 0 ? row - 1 : 0;
    if (target > rows_n - 2) target = rows_n - 2 > 0 ? rows_n - 2 : 0;
    scroll = ui_approach(scroll, target, dt, 14);
    gfx_clip(0, 28, SCREEN_W, SCREEN_H - 28 - 20);
    char artist[96], song[96];
    for (int i = 0; i < list_count; i++) {
        float x = 17 + (i % GRID_COLS) * (tw + gx), y = top + (i / GRID_COLS - scroll) * rh;
        if (y > SCREEN_H || y + rh < 28) continue;
        const Video *v = &videos[list[i]];
        int on = i == sel;
        if (on) ui_rrect(x - 3, y - 3, tw + 6, th + 6, 8, ACCENT);
        if (on) ui_rrect(x - 1, y - 1, tw + 2, th + 2, 7, BG);
        draw_thumb(list[i], x, y, tw, th);
        draw_progress(x, y + th - 3, tw, v);
        ui_round_corners(x, y, tw, th, 6, BG);
        split_name(v->title, artist, sizeof(artist), song, sizeof(song));
        font_draw_fit(f_tiny, x, y + th + 12, song, on ? TEXT : MUTED, tw);
        if (*artist) font_draw_fit(f_chip, x, y + th + 23, artist, DIM, tw);
    }
    gfx_unclip();
    gfx_rect(0, SCREEN_H - 20, SCREEN_W, 20, BG);
}

static void browser_draw(float dt)
{
    if (tab == TAB_PHOTOS && photos_viewing()) {
        photos_draw(dt);
        return;
    }
    if (show_open >= 0) {
        show_draw(dt);
        return;
    }
    gfx_begin(BG);
    draw_top_bar();
    if (tab == TAB_PHOTOS) {
        photos_draw(dt);
    } else if (!video_count) {
        ui_empty_card(150, "No videos yet", "MP4 videos into", vlib_dir(), "Put them in Movies, Shows, Music and Clips folders.");
        int g[] = { ICON_CIRCLE };
        const char *l[] = { "Back" };
        draw_hints(g, l, 1, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    } else if (tab == TAB_HOME) {
        home_draw(dt);
    } else if (tab == TAB_TVM) {
        tvm_draw(dt);
    } else if (!list_count) {
        font_draw_center(f_title, SCREEN_W / 2, 120, "Nothing in this tab", TEXT);
        const char *c = tab == TAB_MUSIC ? "Music videos go in VIDEO/Music." : "Movies go in VIDEO/Movies, shows in VIDEO/Shows/<Show>.";
        font_draw_center(f_small, SCREEN_W / 2, 142, c, MUTED);
        int g[] = { ICON_CIRCLE };
        const char *l[] = { "Back" };
        draw_hints(g, l, 1, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    } else {
        draw_grid(dt);
        int resume = resume_of(&videos[list[sel]]) != 0;
        int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_SQUARE, ICON_CIRCLE };
        const char *l[] = { resume ? "Resume" : "Play", "Start over", "Sort", "Back" };
        draw_hints(g, l, 4, SCREEN_W - 14, SCREEN_H - 8, MUTED);
    }
    ui_volume(f_small_b, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}

// ---- Player -----------------------------------------------------------------

static void player_input(const Input *in, float dt)
{
    u32 p = in->pressed;
    // SELECT shows its own pill, so it leaves the controls hidden.
    if ((in->pressed & ~PSP_CTRL_SELECT) || (in->held & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) overlay_t = 3;
    if (p & PSP_CTRL_CIRCLE) { stop(0); return; }
    if (p & PSP_CTRL_SELECT) {
        Video *v = &videos[playing];
        vlib_set_fit(playing, ((v->fit < 0 ? settings.video_fit : v->fit) + 1) % FIT_COUNT);
        fit_t = 1.5f;
    }
    if (p & PSP_CTRL_CROSS) {
        vplayer_toggle_pause();
        if (vplayer_paused()) vlib_set_resume(playing, vplayer_position());
    }
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
        // Top speed crosses the whole video in about 8 seconds (never under
        // 3 minutes a second), reached after holding for 1.5 seconds.
        float top = fmaxf(180000, dur / 8.0f);
        seek_speed = fminf(seek_speed + dt * top / 1.5f, top);
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
    if (fit_t > 0) fit_t -= dt;
    player_input(in, dt);
    if (playing < 0) return;
    // Remember the spot now and then, in case the PSP is switched off.
    save_t += dt;
    if (save_t > 10) { save_t = 0; vlib_set_resume(playing, vplayer_position()); }
    if (vplayer_ended()) {
        end_t += dt;
        if (end_t > 0.5f) stop(1);
    }
}

static int fit_of(void)
{
    int fit = videos[playing].fit;
    return fit < 0 ? settings.video_fit : fit;
}

static void player_draw(void)
{
    gfx_begin(HEX(0x000000, 255));
    const Texture *f = vplayer_frame();
    const Mp4 *m = vplayer_mp4();
    if (f && vplayer_video_ok()) {
        // Fit shows it all with bars, Zoom fills the screen and crops what
        // spills over, Stretch fills it out of shape.
        float sx = (float)SCREEN_W / f->w, sy = (float)SCREEN_H / f->h;
        float s = fit_of() == FIT_ZOOM ? fmaxf(sx, sy) : fminf(sx, sy);
        float w = f->w * s, h = f->h * s;
        if (fit_of() == FIT_STRETCH) { w = SCREEN_W; h = SCREEN_H; }
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
    if (fit_t > 0) {
        float fa = fminf(1.0f, fit_t / 0.3f);
        char label[32];
        snprintf(label, sizeof(label), "Picture: %s", fit_names[fit_of()]);
        float w = 44 + font_width(f_small, label, 0), x = (SCREEN_W - w) / 2, y = 12;
        ui_rrect(x, y, w, 26, 13, RGBA(20, 22, 30, (int)(225 * fa)));
        ui_ring(x, y, w, 26, 13, color_alpha(HEX(0x6C8CFF, 150), fa));
        ui_ring(x + 14, y + 7, 13, 12, 1, color_alpha(ACCENT, fa));
        font_draw(f_small, x + 34, y + 17, label, color_alpha(TEXT, fa), 0);
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
