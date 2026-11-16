#include "games.h"

#include <math.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gcoll.h"
#include "glib.h"
#include "gorder.h"
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

enum { TAB_RECENT, TAB_ALL, TAB_COLLS, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = { "Recent", "All Games", "Collections" };

// Collections
#define CCOLS 3
#define CARD_W 140
#define CARD_H 80
#define CROW_H 112
#define HOLD_S 2.0f
static int open_coll = -1, coll_sel, picker_open, picker_sel, hold_done;
static float coll_scroll, hold_t, toast_t, t_now;
static char toast[64];

// On-screen keyboard for naming a collection, laid out like the playlist one.
enum { KEY_CHAR, KEY_BACK, KEY_SHIFT, KEY_SPACE };
typedef struct { int kind; char ch; float x, w; } Key;
static const char *const kb_rows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
static int kb_open, kb_row = 1, kb_col, kb_caps = 1, kb_game = -1;
static char kb_text[GCOLL_NAME_MAX + 1];
static void draw_hold(float x, float y, float w);
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
    if (open_coll >= 0) {
        list_count = gcoll_games(open_coll, list, game_count);
        if (sel >= list_count) sel = list_count ? list_count - 1 : 0;
        built_for = game_count;
        return;
    }
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

static void show_toast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_t = 2;
}

// ---- Keyboard -------------------------------------------------------------------

#define KB_KEY_W 36
#define KB_GAP 4

static int kb_layout(int row, Key *keys)
{
    int n = 0;
    if (row == 3) keys[n++] = (Key){ KEY_SHIFT, 0, 0, 44 };
    for (const char *c = kb_rows[row]; *c; c++) keys[n++] = (Key){ KEY_CHAR, *c, 0, KB_KEY_W };
    if (row == 2) keys[n++] = (Key){ KEY_BACK, 0, 0, 52 };
    if (row == 3) keys[n++] = (Key){ KEY_SPACE, ' ', 0, 92 };
    float total = -KB_GAP;
    for (int i = 0; i < n; i++) total += keys[i].w + KB_GAP;
    float x = (SCREEN_W - total) / 2;
    for (int i = 0; i < n; i++) { keys[i].x = x; x += keys[i].w + KB_GAP; }
    return n;
}

static void open_keyboard(int game)
{
    kb_open = 1;
    kb_text[0] = 0;
    kb_row = 1;
    kb_col = 0;
    kb_caps = 1;
    kb_game = game;
}

static void kb_save(void)
{
    char name[GCOLL_NAME_MAX + 1];
    const char *start = kb_text;
    while (*start == ' ') start++;
    snprintf(name, sizeof(name), "%s", start);
    int len = strlen(name);
    while (len && name[len - 1] == ' ') name[--len] = 0;
    if (!len) { show_toast("Type a name first"); return; }
    int c = gcoll_create(name);
    if (c < 0) { show_toast("There's already a collection with that name"); return; }
    char msg[64];
    if (kb_game >= 0) {
        gcoll_toggle(c, kb_game);
        snprintf(msg, sizeof(msg), "Added to %s", name);
    } else {
        snprintf(msg, sizeof(msg), "Created %s", name);
        coll_sel = c;
    }
    show_toast(msg);
    kb_open = 0;
}

static void kb_update(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    Key keys[16], other[16];
    int count = kb_layout(kb_row, keys);
    if (n & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        int row = kb_row + ((n & PSP_CTRL_UP) ? -1 : 1);
        if (row >= 0 && row < 4) {
            float cx = keys[kb_col].x + keys[kb_col].w / 2, best = 1e9f;
            int m = kb_layout(row, other);
            for (int i = 0; i < m; i++) {
                float d = fabsf(other[i].x + other[i].w / 2 - cx);
                if (d < best) { best = d; kb_col = i; }
            }
            kb_row = row;
            count = m;
            memcpy(keys, other, sizeof(keys));
        }
    }
    if (n & PSP_CTRL_LEFT) kb_col = (kb_col + count - 1) % count;
    if (n & PSP_CTRL_RIGHT) kb_col = (kb_col + 1) % count;
    int len = strlen(kb_text);
    if (p & PSP_CTRL_CROSS) {
        const Key *k = &keys[kb_col];
        if ((k->kind == KEY_CHAR || k->kind == KEY_SPACE) && len < GCOLL_NAME_MAX) {
            char c = k->ch >= 'A' && k->ch <= 'Z' && !kb_caps ? k->ch + 32 : k->ch;
            kb_text[len] = c;
            kb_text[len + 1] = 0;
            if (c != ' ' && len == 0) kb_caps = 0; // capital first letter, then lowercase
        } else if (k->kind == KEY_BACK && len) {
            kb_text[len - 1] = 0;
        } else if (k->kind == KEY_SHIFT) {
            kb_caps = !kb_caps;
        }
    }
    if (n & PSP_CTRL_SQUARE && len) kb_text[len - 1] = 0;
    if (p & PSP_CTRL_SELECT) kb_caps = !kb_caps;
    if (p & PSP_CTRL_START) kb_save();
    if (p & PSP_CTRL_CIRCLE) kb_open = 0;
}

// ---- Input --------------------------------------------------------------------------

// Hold □ for 2 seconds: fires once per press, while something can be removed.
static int hold_square(const Input *in, float dt, int allowed)
{
    if (!(in->held & PSP_CTRL_SQUARE) || !allowed) {
        hold_t = 0;
        hold_done = 0;
        return 0;
    }
    if (hold_done) return 0;
    hold_t += dt;
    if (hold_t < HOLD_S) return 0;
    hold_done = 1;
    hold_t = 0;
    return 1;
}

static void picker_update(const Input *in)
{
    u32 p = in->pressed, nav = in->nav;
    int rows = coll_count + 1; // "New collection" first
    if (nav & PSP_CTRL_UP) picker_sel = (picker_sel + rows - 1) % rows;
    if (nav & PSP_CTRL_DOWN) picker_sel = (picker_sel + 1) % rows;
    if (p & PSP_CTRL_CROSS) {
        if (picker_sel == 0) {
            picker_open = 0;
            open_keyboard(page);
        } else {
            int c = picker_sel - 1;
            gcoll_toggle(c, page);
            char msg[64];
            snprintf(msg, sizeof(msg), gcoll_has(c, page) ? "Added to %s" : "Removed from %s", colls[c].name);
            show_toast(msg);
        }
    }
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) picker_open = 0;
}

static int grid_nav(u32 nav, int *at, int count, int cols)
{
    int was = *at;
    if (nav & PSP_CTRL_LEFT && *at > 0) (*at)--;
    if (nav & PSP_CTRL_RIGHT && *at < count - 1) (*at)++;
    if (nav & PSP_CTRL_UP && *at >= cols) *at -= cols;
    if (nav & PSP_CTRL_DOWN && *at + cols < count) *at += cols;
    else if (nav & PSP_CTRL_DOWN && *at / cols < (count - 1) / cols) *at = count - 1;
    return *at != was;
}

static int colls_update(const Input *in, float dt)
{
    u32 p = in->pressed;
    int cells = coll_count + 1; // the last one makes a new collection
    if (coll_sel >= cells) coll_sel = cells - 1;
    if (grid_nav(in->nav, &coll_sel, cells, CCOLS)) hold_t = 0;
    if (hold_square(in, dt, coll_sel < coll_count)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Deleted %s", colls[coll_sel].name);
        gcoll_delete(coll_sel);
        show_toast(msg);
    }
    if (p & PSP_CTRL_CROSS) {
        if (coll_sel == coll_count) {
            open_keyboard(-1);
        } else {
            open_coll = coll_sel;
            sel = 0;
            scroll = 0;
            build();
        }
    }
    return p & PSP_CTRL_CIRCLE ? 1 : 0;
}

int games_update(const Input *in, float dt)
{
    t_now += dt;
    if (fail_t > 0) fail_t -= dt;
    if (toast_t > 0) toast_t -= dt;
    take_pic1();
    start_pic1();
    if (gorder_active()) {
        if (gorder_update(in, dt) == 2) show_toast("Saved. Back in the PSP menu, it shows the new order.");
        return 0;
    }
    if (kb_open) {
        kb_update(in);
        if (!kb_open && open_coll < 0) build();
        return 0;
    }
    if (page >= 0) pic1_k = ui_approach(pic1_k, pic1 ? 1 : 0, dt, 6);
    if (page >= 0) {
        if (picker_open) {
            picker_update(in);
        } else {
            if (in->pressed & PSP_CTRL_CROSS) launch(page);
            if (in->pressed & PSP_CTRL_TRIANGLE) { picker_open = 1; picker_sel = 0; }
            if (in->pressed & PSP_CTRL_CIRCLE) {
                close_page();
                if (open_coll >= 0) build(); // it may have left this collection
            }
        }
        return 0;
    }
    if (glib_ready() && built_for != game_count) build();
    u32 p = in->pressed, nav = in->nav;
    if (open_coll >= 0) {
        if (p & PSP_CTRL_CIRCLE) {
            open_coll = -1;
            hold_t = 0;
            build();
            return 0;
        }
        if (grid_nav(nav, &sel, list_count, COLS)) hold_t = 0;
        if (hold_square(in, dt, list_count > 0)) {
            gcoll_toggle(open_coll, list[sel]);
            show_toast("Removed from this collection");
            build();
        }
        if (list_count && p & PSP_CTRL_CROSS) open_page(list[sel]);
        else if (list_count && p & PSP_CTRL_TRIANGLE) launch(list[sel]);
        return 0;
    }
    if (p & PSP_CTRL_START && glib_ready() && game_count) {
        gorder_open();
        return 0;
    }
    if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
        tab = (tab + (p & PSP_CTRL_LTRIGGER ? TAB_COUNT - 1 : 1)) % TAB_COUNT;
        sel = 0;
        scroll = 0;
        hold_t = 0;
        build();
        return 0;
    }
    if (tab == TAB_COLLS) return glib_ready() ? colls_update(in, dt) : (p & PSP_CTRL_CIRCLE) != 0;
    if (p & PSP_CTRL_CIRCLE) return 1;
    if (p & PSP_CTRL_SQUARE && tab == TAB_ALL) {
        sort = (sort + 1) % SORT_COUNT;
        build();
    }
    if (!list_count) return 0;
    grid_nav(nav, &sel, list_count, COLS);
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
        if (on && open_coll >= 0) draw_hold(x, y + TILE_H + 1, TILE_W);
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

static void draw_toast(void)
{
    if (toast_t <= 0) return;
    float a = toast_t < 0.3f ? toast_t / 0.3f : 1;
    float w = font_width(f_small, toast, 0) + 28, x = (SCREEN_W - w) / 2;
    ui_rrect(x, 222, w, 22, 11, color_alpha(HEX(0x23252D, 245), a));
    font_draw_center(f_small, SCREEN_W / 2, 237, toast, color_alpha(TEXT, a));
}

// A thin bar filling under whatever hold □ will remove.
static void draw_hold(float x, float y, float w)
{
    if (hold_t <= 0.05f) return;
    gfx_rect(x, y, w, 3, RGBA(255, 255, 255, 40));
    gfx_rect(x, y, w * fminf(1, hold_t / HOLD_S), 3, HEX(0xFF6B6B, 255));
}

// Add to collection: a card over the game page.
static void draw_picker(void)
{
    if (!picker_open) return;
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(5, 6, 9, 200));
    int rows = coll_count + 1, shown = rows < 6 ? rows : 6;
    float w = 260, h = 44 + shown * 26 + 10, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 - 8;
    ui_rrect(x, y, w, h, 12, HEX(0x1A1C24, 250));
    ui_ring(x, y, w, h, 12, HEX(0x2E3240, 255));
    font_draw(f_stat, x + 16, y + 26, "Add to collection", TEXT, 0);
    int first = picker_sel - shown + 1 > 0 ? picker_sel - shown + 1 : 0;
    for (int r = 0; r < shown; r++) {
        int i = first + r;
        float ry = y + 38 + r * 26;
        if (i == picker_sel) ui_rrect(x + 8, ry, w - 16, 24, 7, color_alpha(ACCENT, 0.25f));
        if (i == 0) {
            font_draw(f_small, x + 20, ry + 16, "+ New collection\xE2\x80\xA6", ACCENT, 0);
            continue;
        }
        int c = i - 1, has = gcoll_has(c, page);
        font_draw_fit(f_small, x + 20, ry + 16, colls[c].name, TEXT, w - 70);
        ui_ring(x + w - 36, ry + 5, 14, 14, 3, has ? ACCENT : DIM);
        if (has) ui_rrect(x + w - 33, ry + 8, 8, 8, 2, ACCENT);
    }
    int g[] = { ICON_CROSS, ICON_CIRCLE };
    const char *l[] = { "Add / remove", "Done" };
    draw_hints(g, l, 2);
}

static void draw_keyboard(void)
{
    gfx_begin(BG);
    font_draw(f_stat, 14, 22, "New collection", TEXT, 0);
    ui_battery(f_small, SCREEN_W - 14, 16, MUTED);
    if (kb_game >= 0) {
        const Texture *t = glib_icon(kb_game);
        if (t) { gfx_filter(1); gfx_tex(t, 14, 38, 64, 36, 0xFFFFFFFF); }
    }
    float fx = kb_game >= 0 ? 90 : 14, fw = SCREEN_W - 14 - 40 - fx;
    float tw = font_width(f_hero, kb_text, 0), tx = fx + 2 - (tw > fw - 8 ? tw - (fw - 8) : 0);
    gfx_clip(fx, 36, fw, 40);
    float end = font_draw(f_hero, tx, 64, kb_text, TEXT, 0);
    gfx_unclip();
    if (fmodf(t_now, 1.0f) < 0.5f) gfx_rect(end + 2, 48, 2, 20, ACCENT);
    gfx_rect(fx, 74, fw, 2, ACCENT);
    char count[16];
    snprintf(count, sizeof(count), "%d/%d", (int)strlen(kb_text), GCOLL_NAME_MAX);
    font_draw_right(f_tiny, SCREEN_W - 14, 62, count, DIM);
    for (int r = 0; r < 4; r++) {
        Key keys[16];
        int n = kb_layout(r, keys);
        float y = 88 + r * 32;
        for (int i = 0; i < n; i++) {
            const Key *k = &keys[i];
            int on = r == kb_row && i == kb_col, special = k->kind != KEY_CHAR;
            u32 bg = on ? ACCENT : special ? HEX(0x17181E, 255) : HEX(0x23252D, 255);
            ui_rrect(k->x, y, k->w, 28, 5, bg);
            if (special && !on) ui_ring(k->x, y, k->w, 28, 5, CHIP);
            float cx = k->x + k->w / 2;
            if (k->kind == KEY_BACK) ui_icon(ICON_BACKSPACE, cx - 8, y + 6, 16, TEXT);
            else if (k->kind == KEY_SHIFT) ui_icon(ICON_SHIFT, cx - 8, y + 6, 16, kb_caps && !on ? ACCENT : TEXT);
            else if (k->kind == KEY_SPACE) font_draw_center(f_button, cx, y + 18, "space", TEXT);
            else {
                char c[2] = { k->ch >= 'A' && k->ch <= 'Z' && !kb_caps ? k->ch + 32 : k->ch, 0 };
                font_draw_center(f_stat, cx, y + 19, c, TEXT);
            }
        }
    }
    float x = SCREEN_W - 14 - ui_hint_width(f_tiny, ICON_CIRCLE, "Cancel"), base = SCREEN_H - 8;
    ui_hint(f_tiny, x, base, ICON_CIRCLE, "Cancel", MUTED);
    x -= 12 + font_width(f_tiny, "START Save", 0);
    font_draw(f_tiny, x, base, "START Save", ACCENT, 0);
    x -= 12 + font_width(f_tiny, "SELECT Caps", 0);
    font_draw(f_tiny, x, base, "SELECT Caps", MUTED, 0);
    x -= 12 + ui_hint_width(f_tiny, ICON_SQUARE, "Delete");
    ui_hint(f_tiny, x, base, ICON_SQUARE, "Delete", MUTED);
    x -= 12 + ui_hint_width(f_tiny, ICON_CROSS, "Type");
    ui_hint(f_tiny, x, base, ICON_CROSS, "Type", MUTED);
    draw_toast();
    gfx_end();
}

// Collections tab: a 2x2 collage of each one's games, then "New collection".
static void draw_colls(float dt)
{
    int cells = coll_count + 1, row = coll_sel / CCOLS, rows_n = (cells + CCOLS - 1) / CCOLS;
    float target = row > 0 ? row - 1 : 0;
    if (target > rows_n - 2) target = rows_n - 2 > 0 ? rows_n - 2 : 0;
    coll_scroll = ui_approach(coll_scroll, target, dt, 14);
    gfx_clip(0, 30, SCREEN_W, SCREEN_H - 30 - 20);
    int idx[4];
    char t[24], sub[48];
    for (int i = 0; i < cells; i++) {
        float x = 17 + (i % CCOLS) * (CARD_W + 12), y = 38 + (i / CCOLS - coll_scroll) * CROW_H;
        if (y > SCREEN_H || y + CROW_H < 30) continue;
        int on = i == coll_sel;
        if (on) ui_rrect(x - 3, y - 3, CARD_W + 6, CARD_H + 6, 8, ACCENT);
        if (on) ui_rrect(x - 1, y - 1, CARD_W + 2, CARD_H + 2, 7, BG);
        if (i == coll_count) {
            ui_ring(x, y, CARD_W, CARD_H, 6, DIM);
            font_draw_center(f_hero, x + CARD_W / 2, y + CARD_H / 2 + 7, "+", MUTED);
            font_draw(f_tiny, x, y + CARD_H + 13, "New collection", on ? TEXT : MUTED, 0);
            continue;
        }
        int n = gcoll_games(i, idx, 4);
        for (int k = 0; k < 4; k++) {
            float cx = x + (k % 2) * 71, cy = y + (k / 2) * 41, cw = 69, ch = 39;
            const Texture *tex = k < n ? glib_icon(idx[k]) : NULL;
            if (tex) { gfx_filter(1); gfx_tex(tex, cx, cy, cw, ch, 0xFFFFFFFF); }
            else gfx_rect(cx, cy, cw, ch, HEX(0x1E2026, 255));
        }
        ui_round_corners(x, y, CARD_W, CARD_H, 6, BG);
        if (on) draw_hold(x, y + CARD_H + 2, CARD_W);
        font_draw_fit(f_tiny, x, y + CARD_H + 13, colls[i].name, on ? TEXT : MUTED, CARD_W);
        glib_format_time(t, sizeof(t), gcoll_seconds(i));
        static int all[256];
        int total = gcoll_games(i, all, 256);
        snprintf(sub, sizeof(sub), "%d %s \xC2\xB7 %s", total, total == 1 ? "game" : "games", t);
        font_draw_fit(f_chip, x, y + CARD_H + 25, sub, DIM, CARD_W);
    }
    gfx_unclip();
    gfx_rect(0, SCREEN_H - 20, SCREEN_W, 20, BG);
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
    int in_colls = 0;
    for (int c = 0; c < coll_count; c++) in_colls += gcoll_has(c, page);
    const char *cl = in_colls ? "In collections" : "+ Collection";
    float cw = font_width(f_button, cl, 0) + 20;
    ui_ring(198, 76, cw, 20, 10, DIM);
    font_draw(f_button, 208, 90, cl, MUTED, 0);

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
    int gl[] = { ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE };
    const char *l[] = { "Play", "Collection", "Back" };
    if (!picker_open) draw_hints(gl, l, 3);
    draw_fail();
    draw_picker();
    draw_toast();
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}

void games_draw(float dt)
{
    if (gorder_active()) {
        gorder_draw(dt);
        return;
    }
    if (kb_open) {
        draw_keyboard();
        return;
    }
    if (page >= 0) {
        page_draw();
        return;
    }
    gfx_begin(BG);
    int back[] = { ICON_CIRCLE };
    const char *back_l[] = { "Back" };
    if (open_coll >= 0) {
        font_draw(f_small, 14, 22, "Collections", DIM, 0);
        font_draw_fit(f_tab_b, 14 + font_width(f_small, "Collections", 0) + 8, 22, colls[open_coll].name, TEXT, 260);
        ui_battery(f_small, SCREEN_W - 14, 18, MUTED);
        if (!list_count) {
            font_draw_center(f_title, SCREEN_W / 2, 120, "Nothing in here yet", TEXT);
            font_draw_center(f_small, SCREEN_W / 2, 142, "Open a game and press \xE2\x96\xB3 to add it.", MUTED);
            draw_hints(back, back_l, 1);
        } else {
            char t[24], head[64];
            glib_format_time(t, sizeof(t), gcoll_seconds(open_coll));
            snprintf(head, sizeof(head), "%d %s \xC2\xB7 %s played", list_count, list_count == 1 ? "game" : "games", t);
            font_draw(f_small, 17, 42, head, MUTED, 0);
            draw_grid(dt);
            int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_SQUARE, ICON_CIRCLE };
            const char *l[] = { "Details", "Play", "Hold: remove", "Back" };
            draw_hints(g, l, 4);
        }
        draw_fail();
        draw_toast();
        ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
        gfx_end();
        return;
    }
    draw_top_bar();
    if (glib_ready() && tab == TAB_COLLS) {
        draw_colls(dt);
        if (coll_sel < coll_count) {
            int g[] = { ICON_CROSS, ICON_SQUARE, ICON_CIRCLE };
            const char *l[] = { "Open", "Hold: delete", "Back" };
            draw_hints(g, l, 3);
        } else {
            int g[] = { ICON_CROSS, ICON_CIRCLE };
            const char *l[] = { "New", "Back" };
            draw_hints(g, l, 2);
        }
        draw_toast();
    } else if (!glib_ready()) {
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
        float rx = SCREEN_W - 17;
        if (tab == TAB_ALL) {
            font_draw_right(f_small, rx, 42, sort_names[sort], DIM);
            rx -= font_width(f_small, sort_names[sort], 0) + 14;
        }
        font_draw_right(f_small, rx, 42, "START PSP menu order", DIM);
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
        draw_toast();
    }
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}

void games_play(int index)
{
    games_enter();
    if (index < 0 || index >= game_count) return;
    open_page(index);
    launch(index);
}
