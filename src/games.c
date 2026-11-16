#include "games.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "glib.h"
#include "gfx.h"
#include "stats.h"
#include "ui.h"
#include "video.h"

#define ACCENT HEX(0x5B8CFF, 255)
#define BG HEX(0x0E0F13, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define DIM HEX(0x8A8D99, 255)
#define CHIP HEX(0x3A3D48, 255)

#define COLS 4
#define TILE_W 104
#define TILE_H 58
#define GAP 10
#define ROW_H (TILE_H + 32)
#define TOP 52

enum { TAB_RECENT, TAB_ALL, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = { "Recent", "All Games" };
enum { SORT_AZ, SORT_MOST, SORT_COUNT };
static const char *const sort_names[SORT_COUNT] = { "A\xE2\x80\x93Z", "Most played" };

static Font *f_tab, *f_tab_b, *f_title, *f_small, *f_tiny, *f_chip, *f_badge, *f_hero, *f_stat, *f_button;

// The game page: open while page >= 0.
static int page = -1;
static Texture *pic1;
static GameDetail detail;
static float fail_t; // "Couldn't start" note
static int inited, tab, sort, sel, *list, list_count, built_for = -1;
static float scroll;

static int cmp_last(const void *a, const void *b)
{
    return strcmp(games[*(const int *)b].last, games[*(const int *)a].last);
}

static int cmp_most(const void *a, const void *b)
{
    const Game *x = &games[*(const int *)a], *y = &games[*(const int *)b];
    if (x->seconds != y->seconds) return x->seconds < y->seconds ? 1 : -1;
    return strcasecmp(x->title, y->title);
}

// Recent: played games, latest first. All Games: everything, A-Z (the
// library's own order) or most played.
static void build(void)
{
    free(list);
    list = malloc((game_count + 1) * sizeof(int));
    list_count = 0;
    if (!list) return;
    for (int i = 0; i < game_count; i++)
        if (tab == TAB_ALL || games[i].last[0]) list[list_count++] = i;
    if (tab == TAB_RECENT) qsort(list, list_count, sizeof(int), cmp_last);
    else if (sort == SORT_MOST) qsort(list, list_count, sizeof(int), cmp_most);
    if (sel >= list_count) sel = list_count ? list_count - 1 : 0;
    built_for = game_count;
}

void games_enter(void)
{
    if (!inited) {
        inited = 1;
        f_tab = ui_font(FAMILY_SORA, 400, 11);
        f_tab_b = ui_font(FAMILY_SORA, 700, 11);
        f_title = ui_font(FAMILY_SORA, 700, 16);
        f_small = ui_font(FAMILY_SORA, 400, 10);
        f_tiny = ui_font(FAMILY_SORA, 400, 9);
        f_chip = ui_font(FAMILY_SORA, 400, 9);
        f_badge = ui_font(FAMILY_SORA, 700, 8);
        f_hero = ui_font(FAMILY_SORA, 700, 20);
        f_stat = ui_font(FAMILY_SORA, 700, 13);
        f_button = ui_font(FAMILY_SORA, 600, 10);
    }
    glib_scan();
    built_for = -1;
    sel = 0;
    scroll = 0;
}

// PIC1 is read and decoded on its own thread so the page opens at once and
// the art fades in when it's ready.
static volatile int pic1_for = -1, pic1_w, pic1_h;
static u8 *volatile pic1_px;
static volatile int pic1_busy;
static int pic1_want;
static float pic1_k;

static int pic1_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int index = pic1_for, w = 0, h = 0;
    u8 *px = glib_pic1(index, &w, &h);
    if (px && (w <= 0 || h <= 0 || w > 512 || h > 512)) { free(px); px = NULL; }
    if (pic1_for == index && px) {
        pic1_w = w;
        pic1_h = h;
        pic1_px = px;
    } else {
        free(px); // the page was closed meanwhile
    }
    pic1_busy = 0;
    return sceKernelExitDeleteThread(0);
}

static void open_page(int index)
{
    page = index;
    pic1_k = 0;
    glib_detail(index, &detail);
    pic1_for = index;
    pic1_want = 1;
}

// Starts the reader once the last one (maybe for another page) is done.
static void start_pic1(void)
{
    if (!pic1_want || pic1_busy || page < 0) return;
    pic1_want = 0;
    pic1_for = page;
    pic1_busy = 1;
    SceUID th = sceKernelCreateThread("boro_pic1", pic1_thread, 0x30, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) pic1_busy = 0;
}

static void take_pic1(void)
{
    u8 *px = pic1_px;
    if (!px) return;
    pic1_px = NULL;
    if (page >= 0 && !pic1) {
        pic1 = tex_create(pic1_w, pic1_h);
        if (pic1) {
            for (int y = 0; y < pic1_h; y++) memcpy(pic1->px + y * pic1->tw, px + y * pic1_w * 4, pic1_w * 4);
            tex_upload(pic1);
        }
    }
    free(px);
}

static void close_page(void)
{
    page = -1;
    pic1_for = -1;
    pic1_want = 0;
    if (pic1) tex_destroy(pic1);
    pic1 = NULL;
}

static void launch(int index)
{
    stats_flush();
    video_save();
    glib_launch(index);
    fail_t = 3; // still here: ARK said no
}

int games_update(const Input *in, float dt)
{
    if (fail_t > 0) fail_t -= dt;
    take_pic1();
    start_pic1();
    if (page >= 0) pic1_k = ui_approach(pic1_k, pic1 ? 1 : 0, dt, 6);
    if (page >= 0) {
        if (in->pressed & PSP_CTRL_CROSS) launch(page);
        if (in->pressed & PSP_CTRL_CIRCLE) close_page();
        return 0;
    }
    if (glib_ready() && built_for != game_count) build();
    u32 p = in->pressed, nav = in->nav;
    if (p & PSP_CTRL_CIRCLE) return 1;
    if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        tab = (tab + 1) % TAB_COUNT; // two tabs, so L and R both switch
        sel = 0;
        scroll = 0;
        build();
        return 0;
    }
    if (p & PSP_CTRL_SQUARE && tab == TAB_ALL) {
        sort = (sort + 1) % SORT_COUNT;
        build();
    }
    if (!list_count) return 0;
    if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
    if (nav & PSP_CTRL_RIGHT && sel < list_count - 1) sel++;
    if (nav & PSP_CTRL_UP && sel >= COLS) sel -= COLS;
    if (nav & PSP_CTRL_DOWN && sel + COLS < list_count) sel += COLS;
    else if (nav & PSP_CTRL_DOWN && sel / COLS < (list_count - 1) / COLS) sel = list_count - 1;
    if (p & PSP_CTRL_CROSS) open_page(list[sel]);
    else if (p & PSP_CTRL_TRIANGLE) launch(list[sel]);
    return 0;
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
    ui_battery(f_small, SCREEN_W - 14, base - 4, MUTED);
}

static void draw_hints(int glyphs[], const char *labels[], int n)
{
    float x = SCREEN_W - 14, base = SCREEN_H - 8;
    for (int i = n - 1; i >= 0; i--) {
        x -= ui_hint_width(f_tiny, glyphs[i], labels[i]);
        ui_hint(f_tiny, x, base, glyphs[i], labels[i], MUTED);
        x -= 12;
    }
}

// Icons fade in over a quiet placeholder as they arrive, instead of popping.
static float icon_k[1024];

static void draw_icon(int index, float x, float y, float dt)
{
    const Texture *t = glib_icon(index);
    float *k = index < 1024 ? &icon_k[index] : NULL;
    if (k) *k = t ? ui_approach(*k, 1, dt, 8) : 0;
    float a = t ? (k ? *k : 1) : 0;
    if (a < 1) gfx_rect(x, y, TILE_W, TILE_H, HEX(0x1E2026, 255));
    if (t) {
        gfx_filter(1);
        gfx_tex(t, x, y, TILE_W, TILE_H, color_alpha(0xFFFFFFFF, a));
    }
}

static void draw_badge(float x, float y, const char *label)
{
    float w = font_width(f_badge, label, 0) + 8;
    ui_rrect(x + 4, y + 4, w, 11, 3, RGBA(0, 0, 0, 200));
    font_draw(f_badge, x + 8, y + 12, label, TEXT, 0);
}

static void draw_grid(float dt)
{
    int row = sel / COLS, rows_n = (list_count + COLS - 1) / COLS;
    float target = row > 0 ? row - 1 : 0;
    if (target > rows_n - 2) target = rows_n - 2 > 0 ? rows_n - 2 : 0;
    scroll = ui_approach(scroll, target, dt, 14);
    gfx_clip(0, 30, SCREEN_W, SCREEN_H - 30 - 20);
    char time[24], last[24], line[64];
    for (int i = 0; i < list_count; i++) {
        float x = 17 + (i % COLS) * (TILE_W + GAP), y = TOP + (i / COLS - scroll) * ROW_H;
        if (y > SCREEN_H || y + ROW_H < 30) continue;
        const Game *g = &games[list[i]];
        int on = i == sel;
        if (on) ui_rrect(x - 3, y - 3, TILE_W + 6, TILE_H + 6, 8, ACCENT);
        if (on) ui_rrect(x - 1, y - 1, TILE_W + 2, TILE_H + 2, 7, BG);
        draw_icon(list[i], x, y, dt);
        ui_round_corners(x, y, TILE_W, TILE_H, 6, BG);
        if (g->type == GTYPE_PS1) draw_badge(x, y, "PS1");
        else if (g->type == GTYPE_HOMEBREW) draw_badge(x, y, "HOMEBREW");
        font_draw_fit(f_tiny, x, y + TILE_H + 12, g->title, on ? TEXT : MUTED, TILE_W);
        glib_format_time(time, sizeof(time), g->seconds);
        if (g->last[0]) {
            glib_format_last(last, sizeof(last), g->last);
            snprintf(line, sizeof(line), "%s \xC2\xB7 %s", time, last);
        } else {
            snprintf(line, sizeof(line), "%s", time);
        }
        font_draw_fit(f_chip, x, y + TILE_H + 23, line, DIM, TILE_W);
    }
    gfx_unclip();
    gfx_rect(0, SCREEN_H - 20, SCREEN_W, 20, BG);
}

static void stat_card(float x, const char *label, const char *value)
{
    ui_rrect(x, 108, 108, 40, 8, HEX(0x1E2026, 255));
    font_draw(f_badge, x + 10, 121, label, MUTED, 1);
    font_draw_fit(f_stat, x + 10, 140, value, TEXT, 92);
}

static void draw_fail(void)
{
    if (fail_t <= 0) return;
    const char *s = "Couldn\xE2\x80\x99t start this game";
    float w = font_width(f_small, s, 0) + 28, x = (SCREEN_W - w) / 2;
    ui_rrect(x, 226, w, 22, 11, HEX(0x2A1A1E, 240));
    font_draw_center(f_small, SCREEN_W / 2, 241, s, HEX(0xFF8A8A, 255));
}

static int ext_is(const char *path, const char *ext)
{
    int a = strlen(path), b = strlen(ext);
    return a >= b && !strcasecmp(path + a - b, ext);
}

static void page_draw(void)
{
    const Game *g = &games[page];
    gfx_begin(BG);
    if (pic1) {
        gfx_filter(1);
        gfx_tex_part(pic1, 0, 0, pic1->w, pic1->h * 140 / 272, 0, 0, SCREEN_W, 140, color_alpha(0xFFFFFFFF, pic1_k));
        u32 top = color_alpha(BG, 0.35f), bottom = BG;
        gfx_gradient(0, 0, SCREEN_W, 140, top, top, bottom, bottom);
    }
    font_draw(f_small, 14, 18, "Games", MUTED, 0);
    ui_battery(f_small, SCREEN_W - 14, 14, MUTED);
    const Texture *icon = glib_icon(page);
    if (icon) { gfx_filter(1); gfx_tex(icon, 14, 32, TILE_W, TILE_H, 0xFFFFFFFF); }
    else ui_placeholder_art(14, 32, TILE_H, 6, page);
    font_draw_fit(f_hero, 132, 52, g->title, TEXT, SCREEN_W - 146);
    char sub[96], size[24];
    const char *kind = g->type == GTYPE_PS1 ? "PS1" : g->type == GTYPE_HOMEBREW ? "Homebrew" :
                       ext_is(g->path, ".cso") ? "CSO" : ext_is(g->path, ".pbp") ? "EBOOT" : "ISO";
    if (detail.bytes >= 1000000000ull) snprintf(size, sizeof(size), "%.1f GB", detail.bytes / 1e9);
    else snprintf(size, sizeof(size), "%d MB", (int)(detail.bytes / 1000000));
    if (g->disc_id[0]) snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s \xC2\xB7 %s", g->disc_id, kind, size);
    else snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", kind, size);
    font_draw(f_small, 132, 68, sub, MUTED, 0);
    ui_rrect(132, 76, 60, 20, 10, ACCENT);
    ui_icon(ICON_PLAY, 141, 81, 10, TEXT);
    font_draw(f_button, 156, 90, "Play", TEXT, 0);

    char v[24];
    glib_format_time(v, sizeof(v), g->seconds);
    stat_card(14, "PLAYED", v);
    snprintf(v, sizeof(v), "%lu", (unsigned long)g->sessions);
    stat_card(130, "SESSIONS", v);
    glib_format_last(v, sizeof(v), g->last[0] ? g->last : NULL);
    stat_card(246, "LAST PLAYED", v);
    glib_format_last(v, sizeof(v), detail.first[0] ? detail.first : NULL);
    stat_card(362, "FIRST PLAYED", v);

    font_draw(f_stat, 14, 172, "Last 7 days", TEXT, 0);
    u32 most = 1;
    for (int d = 0; d < 7; d++) if (detail.days[d] > most) most = detail.days[d];
    ScePspDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    int weekday = sceRtcGetDayOfWeek(now.year, now.month, now.day); // 0 Sunday
    static const char names[] = "SMTWTFS";
    for (int d = 0; d < 7; d++) {
        float x = 24 + d * 64, h = detail.days[d] ? 2 + 48.0f * detail.days[d] / most : 2;
        ui_rrect(x, 234 - h, 36, h, h > 6 ? 3 : 1, d == 6 ? ACCENT : HEX(0x46506F, 255));
        char day[2] = { names[(weekday + d + 1) % 7], 0 };
        font_draw_center(f_chip, x + 18, 246, day, MUTED);
        if (detail.days[d]) {
            char t[16];
            glib_format_time(t, sizeof(t), detail.days[d]);
            font_draw_center(f_chip, x + 18, 230 - h, t, MUTED);
        }
    }
    int gl[] = { ICON_CROSS, ICON_CIRCLE };
    const char *l[] = { "Play", "Back" };
    draw_hints(gl, l, 2);
    draw_fail();
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}

void games_draw(float dt)
{
    if (page >= 0) {
        page_draw();
        return;
    }
    gfx_begin(BG);
    draw_top_bar();
    int back[] = { ICON_CIRCLE };
    const char *back_l[] = { "Back" };
    if (!glib_ready()) {
        font_draw_center(f_title, SCREEN_W / 2, 130, "Looking for games\xE2\x80\xA6", TEXT);
        draw_hints(back, back_l, 1);
    } else if (!game_count) {
        ui_empty_card(150, "No games yet", "ISO or CSO files into", "ms0:/ISO", "EBOOT games in PSP/GAME show up too.");
        draw_hints(back, back_l, 1);
    } else if (!list_count) {
        font_draw_center(f_title, SCREEN_W / 2, 120, "Nothing played yet", TEXT);
        font_draw_center(f_small, SCREEN_W / 2, 142, "Turn on Boro Playtime in Settings > Plugins, then play a game.", MUTED);
        draw_hints(back, back_l, 1);
    } else {
        u32 total = 0;
        for (int i = 0; i < game_count; i++) total += games[i].seconds;
        char t[24], head[64];
        glib_format_time(t, sizeof(t), total);
        snprintf(head, sizeof(head), "%d %s \xC2\xB7 %s played", list_count, list_count == 1 ? "game" : "games", total ? t : "nothing");
        font_draw(f_small, 17, 42, head, MUTED, 0);
        if (tab == TAB_ALL) font_draw_right(f_small, SCREEN_W - 17, 42, sort_names[sort], DIM);
        draw_grid(dt);
        if (tab == TAB_ALL) {
            int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_SQUARE, ICON_CIRCLE };
            const char *l[] = { "Details", "Play", "Sort", "Back" };
            draw_hints(g, l, 4);
        } else {
            int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE };
            const char *l[] = { "Details", "Play", "Back" };
            draw_hints(g, l, 3);
        }
        draw_fail();
    }
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
