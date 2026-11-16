#include "games.h"

#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "glib.h"
#include "gfx.h"
#include "ui.h"

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

static Font *f_tab, *f_tab_b, *f_title, *f_small, *f_tiny, *f_chip, *f_badge;
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
    }
    glib_scan();
    built_for = -1;
    sel = 0;
    scroll = 0;
}

int games_update(const Input *in, float dt)
{
    (void)dt;
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

static void draw_icon(int index, float x, float y)
{
    const Texture *t = glib_icon(index);
    if (t) {
        gfx_filter(1);
        gfx_tex(t, x, y, TILE_W, TILE_H, 0xFFFFFFFF);
        return;
    }
    u32 c0, c1;
    ui_placeholder_colors(index * 7 + 3, &c0, &c1);
    u32 mid = color_lerp(c0, c1, 0.5f);
    gfx_gradient(x, y, TILE_W, TILE_H, c0, mid, mid, c1);
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
        draw_icon(list[i], x, y);
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

void games_draw(float dt)
{
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
            int g[] = { ICON_SQUARE, ICON_CIRCLE };
            const char *l[] = { "Sort", "Back" };
            draw_hints(g, l, 2);
        } else {
            draw_hints(back, back_l, 1);
        }
    }
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
