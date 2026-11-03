#include "photos.h"

#include <pspctrl.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "font.h"
#include "gfx.h"
#include "plib.h"
#include "pviewer.h"
#include "ui.h"
#include "vlib.h"

#define BG HEX(0x0E0F13, 255)
#define ACCENT HEX(0x5B8CFF, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define DIM HEX(0x8A8D99, 255)
#define CHIP HEX(0x3A3D48, 255)

// Library: everything by the day it was taken, newest first, in a tight grid.
#define LIB_COLS 7
#define LIB_TILE 60
#define LIB_STEP 63
#define LIB_X 19
#define HEAD_H 20
#define CONTENT_Y 56
#define CONTENT_B (SCREEN_H - 20)

// Albums: cards three across; an open album is a grid of its photos.
#define CARD_W 144
#define CARD_H 66
#define CARD_STEP_X 152
#define CARD_STEP_Y 94
#define COLS 4
#define CELL_W (PTHUMB_W + 8)
#define CELL_H (PTHUMB_H + 24)

enum { VIEW_LIBRARY, VIEW_ALBUMS };

typedef struct { int photo, video; u32 day, time; } Item; // photo or video is -1
typedef struct { int head; int first, n; u32 day; float y; } Line;

static Font *f_head, *f_small, *f_label, *f_tiny;
static Texture *t_fill;
static int view, album = -1; // album: the open one, or -1
static int sel, album_sel;   // sel: in the Library or the open album; album_sel: on the Albums page
static float scroll;
static int viewing, view_from_library;
static int play = -1;

static Item *items;
static int item_count;
static Line *lines;
static int line_count;
static int *item_line;
static int *run; // photo indexes in Library order, for the viewer
static int run_count;
static u32 today;

// ---- Dates ----------------------------------------------------------------

static u32 ymd_of(const ScePspDateTime *t) { return t->year * 10000 + t->month * 100 + t->day; }

// File times are kept as seconds since the same point as vlib and plib.
static u32 day_of_seconds(u32 s)
{
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    u64 tick = ((u64)s + 63000000000ULL) * 1000000ULL;
    sceRtcSetTick(&t, &tick);
    return ymd_of(&t);
}

static void day_label(u32 day, char *out, int size)
{
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *const wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int y = day / 10000, m = day / 100 % 100, d = day % 100;
    if (day == today) { snprintf(out, size, "Today"); return; }
    // Yesterday: the day before, across a month's end too.
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    t.year = today / 10000; t.month = today / 100 % 100; t.day = today % 100;
    u64 tick;
    sceRtcGetTick(&t, &tick);
    sceRtcTickAddDays(&tick, &tick, -1);
    sceRtcSetTick(&t, &tick);
    if (day == ymd_of(&t)) { snprintf(out, size, "Yesterday"); return; }
    if (m < 1 || m > 12) { snprintf(out, size, "Undated"); return; }
    int wd = sceRtcGetDayOfWeek(y, m, d);
    if ((u32)y == today / 10000) snprintf(out, size, "%s, %s %d", wdays[wd % 7], months[m - 1], d);
    else snprintf(out, size, "%s %d, %d", months[m - 1], d, y);
}

// ---- Library ----------------------------------------------------------------

static int cmp_item(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    if (x->day != y->day) return x->day < y->day ? 1 : -1;
    if (x->time != y->time) return x->time < y->time ? 1 : -1;
    return 0;
}

static void build_library(void)
{
    free(items); free(lines); free(item_line); free(run);
    int clips = 0;
    for (int i = 0; i < video_count; i++) if (videos[i].category == VCAT_CLIPS) clips++;
    int cap = photo_count + clips;
    items = malloc((cap ? cap : 1) * sizeof(Item));
    item_line = malloc((cap ? cap : 1) * sizeof(int));
    run = malloc((photo_count ? photo_count : 1) * sizeof(int));
    item_count = 0;
    for (int i = 0; i < photo_count; i++) {
        const Photo *p = &photos[i];
        items[item_count++] = (Item){ i, -1, p->taken ? p->taken : day_of_seconds(p->modified), p->modified };
    }
    for (int i = 0; i < video_count; i++)
        if (videos[i].category == VCAT_CLIPS)
            items[item_count++] = (Item){ -1, i, day_of_seconds(videos[i].modified), videos[i].modified };
    qsort(items, item_count, sizeof(Item), cmp_item);

    // A heading for each day, then its rows.
    lines = malloc((item_count * 2 + 1) * sizeof(Line));
    line_count = 0;
    run_count = 0;
    float y = 0;
    for (int i = 0; i < item_count; i++) {
        if (!i || items[i].day != items[i - 1].day) {
            lines[line_count++] = (Line){ 1, i, 0, items[i].day, y };
            y += HEAD_H;
        }
        Line *l = &lines[line_count - 1];
        if (l->head || l->n == LIB_COLS || l->day != items[i].day) {
            lines[line_count++] = (Line){ 0, i, 0, items[i].day, y };
            y += LIB_STEP;
            l = &lines[line_count - 1];
        }
        l->n++;
        item_line[i] = line_count - 1;
        if (items[i].photo >= 0) run[run_count++] = items[i].photo;
    }
}

static int col_of(int i) { return i - lines[item_line[i]].first; }

// The item in the row step rows away (skipping headings), same column.
static int row_step(int i, int step)
{
    int l = item_line[i];
    do l += step; while (l >= 0 && l < line_count && lines[l].head);
    if (l < 0 || l >= line_count) return i;
    int c = col_of(i);
    return lines[l].first + (c < lines[l].n ? c : lines[l].n - 1);
}

// The first item of the next older month, or the top after the oldest.
static int next_month(int i)
{
    u32 month = items[i].day / 100;
    for (int k = i + 1; k < item_count; k++) if (items[k].day / 100 != month) return k;
    return 0;
}

static int library_update(const Input *in)
{
    u32 p = in->pressed, nav = in->nav;
    if (!item_count) return 0;
    if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
    if (nav & PSP_CTRL_RIGHT && sel < item_count - 1) sel++;
    if (nav & PSP_CTRL_UP) sel = row_step(sel, -1);
    if (nav & PSP_CTRL_DOWN) sel = row_step(sel, 1);
    if (p & PSP_CTRL_TRIANGLE) sel = next_month(sel);
    if (p & PSP_CTRL_CROSS) {
        const Item *it = &items[sel];
        if (it->video >= 0) {
            play = it->video;
        } else {
            int at = 0;
            for (int k = 0; k < run_count; k++) if (run[k] == it->photo) at = k;
            plib_thumbs(0); // the viewer gets the CPU to itself
            pviewer_open_list(run, run_count, at, "Library");
            viewing = 1;
            view_from_library = 1;
        }
    }
    return 0;
}

// ---- Albums -----------------------------------------------------------------

static int albums_update(const Input *in)
{
    u32 p = in->pressed, nav = in->nav;
    if (album >= 0) {
        int n = palbums[album].count;
        if (p & PSP_CTRL_CIRCLE) { album = -1; return 0; }
        if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
        if (nav & PSP_CTRL_RIGHT && sel < n - 1) sel++;
        if (nav & PSP_CTRL_UP && sel >= COLS) sel -= COLS;
        if (nav & PSP_CTRL_DOWN && sel / COLS < (n - 1) / COLS) sel = sel + COLS < n ? sel + COLS : n - 1;
        if (p & PSP_CTRL_CROSS && n) {
            plib_thumbs(0);
            pviewer_open(palbums[album].first, n, sel, palbums[album].name);
            viewing = 1;
            view_from_library = 0;
        }
        return 0;
    }
    int n = palbum_count;
    if (!n) return 0;
    if (nav & PSP_CTRL_LEFT && album_sel > 0) album_sel--;
    if (nav & PSP_CTRL_RIGHT && album_sel < n - 1) album_sel++;
    if (nav & PSP_CTRL_UP && album_sel >= 3) album_sel -= 3;
    if (nav & PSP_CTRL_DOWN && album_sel / 3 < (n - 1) / 3) album_sel = album_sel + 3 < n ? album_sel + 3 : n - 1;
    if (p & PSP_CTRL_CROSS) { album = album_sel; sel = 0; scroll = 0; }
    return 0;
}

// ---- Entry points -------------------------------------------------------------

void photos_enter(void)
{
    if (!f_head) {
        f_head = ui_font(FAMILY_SORA, 700, 18);
        f_small = ui_font(FAMILY_SORA, 400, 10);
        f_label = ui_font(FAMILY_SORA, 400, 9);
        f_tiny = ui_font(FAMILY_SORA, 700, 11);
        t_fill = tex_round_fill(8);
    }
    ScePspDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    today = ymd_of(&now);
    plib_scan();
    build_library();
    album = -1;
    sel = album_sel = 0;
    scroll = 0;
    viewing = 0;
    play = -1;
    plib_thumbs(1);
}

void photos_leave(void) { plib_thumbs(0); }

int photos_viewing(void) { return viewing; }

int photos_take_play(void)
{
    int v = play;
    play = -1;
    return v;
}

int photos_update(const Input *in, float dt)
{
    (void)dt;
    if (viewing) {
        if (pviewer_update(in, dt)) {
            // Back on the photo that was showing.
            viewing = 0;
            int at = pviewer_current();
            if (view_from_library) {
                for (int k = 0; k < item_count; k++) if (items[k].photo == run[at]) sel = k;
            } else {
                sel = at;
            }
            plib_thumbs(1);
        }
        return 0;
    }
    u32 p = in->pressed;
    if (p & PSP_CTRL_CIRCLE && album < 0) return 1;
    if (p & PSP_CTRL_SQUARE && album < 0) {
        view = view == VIEW_LIBRARY ? VIEW_ALBUMS : VIEW_LIBRARY;
        sel = 0;
        scroll = 0;
        return 0;
    }
    return view == VIEW_LIBRARY ? library_update(in) : albums_update(in);
}

// ---- Drawing ----------------------------------------------------------------

// The middle of a thumbnail, cut to w x h.
static void draw_crop(const Texture *t, float x, float y, float w, float h)
{
    float sw = t->w, sh = t->h;
    if (sw / sh > w / h) sw = sh * w / h; else sh = sw * h / w;
    gfx_filter(1);
    gfx_tex_part(t, (t->w - sw) / 2, (t->h - sh) / 2, sw, sh, x, y, w, h, 0xFFFFFFFF);
}

static void draw_placeholder(int seed, float x, float y, float w, float h)
{
    u32 c0, c1;
    ui_placeholder_colors(seed * 7 + 3, &c0, &c1);
    gfx_rect(x, y, w, h, color_lerp(c0, c1, 0.5f));
}

static void draw_tile(const Item *it, float x, float y, float s)
{
    const Texture *t = NULL;
    if (it->photo >= 0) { plib_want(it->photo); t = plib_thumb(it->photo); }
    else t = vlib_thumb(it->video);
    if (t) draw_crop(t, x, y, s, s);
    else draw_placeholder(it->photo >= 0 ? it->photo : it->video + 500, x, y, s, s);
    if (it->video >= 0) {
        const Video *v = &videos[it->video];
        char d[16] = "";
        if (v->duration_ms) ui_format_time(d, sizeof(d), v->duration_ms);
        float w = 14 + (*d ? font_width(f_label, d, 0) + 4 : 0);
        ui_rrect(x + s - w - 3, y + s - 14, w, 11, 3, RGBA(0, 0, 0, 170));
        ui_icon(ICON_PLAY, x + s - w + 1, y + s - 12, 7, TEXT);
        if (*d) font_draw(f_label, x + s - w + 11, y + s - 5, d, TEXT, 0);
    }
}

static void draw_pills(void)
{
    static const char *const names[] = { "Library", "Albums" };
    float x = 14;
    for (int i = 0; i < 2; i++) {
        float w = font_width(f_small, names[i], 0) + 18;
        if (i == view) gfx_nine(t_fill, 8, x, 32, w, 17, TEXT);
        else ui_ring(x, 32, w, 17, 8, CHIP);
        font_draw(f_small, x + 9, 44, names[i], i == view ? BG : MUTED, 0);
        x += w + 6;
    }
}

static void library_draw(float dt)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%d photo%s", photo_count, photo_count == 1 ? "" : "s");
    font_draw_right(f_small, SCREEN_W - 14, 44, buf, DIM);
    if (!item_count) return;
    // Keep the selected row whole on screen, its day's heading too when it fits.
    const Line *sl = &lines[item_line[sel]];
    float target = scroll, view_h = CONTENT_B - CONTENT_Y;
    if (sl->y - HEAD_H < target) target = sl->y - (sl > lines && sl[-1].head ? HEAD_H : 0);
    if (sl->y + LIB_STEP > target + view_h) target = sl->y + LIB_STEP - view_h;
    if (target < 0) target = 0;
    scroll = ui_approach(scroll, target, dt, 14);
    gfx_clip(0, CONTENT_Y - 2, SCREEN_W, CONTENT_B - CONTENT_Y + 2);
    char label[40];
    for (int l = 0; l < line_count; l++) {
        const Line *ln = &lines[l];
        float y = CONTENT_Y + ln->y - scroll;
        if (y > CONTENT_B || y + LIB_STEP < CONTENT_Y - 4) continue;
        if (ln->head) {
            day_label(ln->day, label, sizeof(label));
            font_draw(f_tiny, 14, y + 14, label, TEXT, 0);
            continue;
        }
        for (int k = 0; k < ln->n; k++) {
            int i = ln->first + k;
            float x = LIB_X + k * LIB_STEP;
            draw_tile(&items[i], x, y, LIB_TILE);
            if (i == sel) {
                gfx_rect(x - 1, y - 1, LIB_TILE + 2, 3, ACCENT);
                gfx_rect(x - 1, y + LIB_TILE - 2, LIB_TILE + 2, 3, ACCENT);
                gfx_rect(x - 1, y - 1, 3, LIB_TILE + 2, ACCENT);
                gfx_rect(x + LIB_TILE - 2, y - 1, 3, LIB_TILE + 2, ACCENT);
            }
        }
    }
    gfx_unclip();
    // A thin scroll bar for long libraries.
    float total = lines[line_count - 1].y + LIB_STEP;
    if (total > view_h) {
        float bh = view_h * view_h / total, by = CONTENT_Y + (view_h - bh) * scroll / (total - view_h);
        gfx_rect(SCREEN_W - 5, CONTENT_Y, 2, view_h, HEX(0x282A32, 255));
        gfx_rect(SCREEN_W - 5, by, 2, bh, MUTED);
    }
}

static void albums_draw(float dt)
{
    int n = palbum_count;
    char buf[48];
    snprintf(buf, sizeof(buf), "%d album%s", n, n == 1 ? "" : "s");
    font_draw_right(f_small, SCREEN_W - 14, 44, buf, DIM);
    int row = album_sel / 3;
    float target = row > 0 ? row - 1 : 0;
    scroll = ui_approach(scroll, target * CARD_STEP_Y, dt, 14);
    gfx_clip(0, CONTENT_Y - 4, SCREEN_W, CONTENT_B - CONTENT_Y + 4);
    for (int i = 0; i < n; i++) {
        float x = 14 + (i % 3) * CARD_STEP_X, y = CONTENT_Y + 4 + (i / 3) * CARD_STEP_Y - scroll;
        if (y > CONTENT_B || y + CARD_STEP_Y < CONTENT_Y) continue;
        const PhotoAlbum *a = &palbums[i];
        int on = i == album_sel;
        if (on) ui_rrect(x - 3, y - 3, CARD_W + 6, CARD_H + 6, 9, ACCENT);
        if (on) ui_rrect(x - 1, y - 1, CARD_W + 2, CARD_H + 2, 7, BG);
        plib_want(a->first);
        const Texture *t = plib_thumb(a->first);
        if (t) draw_crop(t, x, y, CARD_W, CARD_H);
        else draw_placeholder(a->first, x, y, CARD_W, CARD_H);
        ui_round_corners(x, y, CARD_W, CARD_H, 6, BG);
        char count[16];
        snprintf(count, sizeof(count), "%d", a->count);
        float cw = font_width(f_small, count, 0);
        font_draw_fit(f_small, x, y + CARD_H + 14, a->name, on ? TEXT : MUTED, CARD_W - cw - 8);
        font_draw_right(f_small, x + CARD_W, y + CARD_H + 14, count, DIM);
    }
    gfx_unclip();
}

static void album_draw(float dt)
{
    const PhotoAlbum *a = &palbums[album];
    char sub[32];
    snprintf(sub, sizeof(sub), "%d photo%s", a->count, a->count == 1 ? "" : "s");
    font_draw_fit(f_head, 14, 50, a->name, TEXT, 300);
    float hw = font_width(f_head, a->name, 0);
    font_draw(f_small, 14 + (hw < 300 ? hw : 300) + 8, 50, sub, MUTED, 0);
    int row = sel / COLS;
    float target = scroll;
    if (row < target) target = row;
    if (row > target + 1) target = row - 1;
    scroll = ui_approach(scroll, target, dt, 14);
    gfx_clip(0, 54, SCREEN_W, SCREEN_H - 54 - 22);
    for (int i = 0; i < a->count; i++) {
        float x = 14 + (i % COLS) * CELL_W, y = 62 + (i / COLS - scroll) * CELL_H;
        if (y > SCREEN_H || y + CELL_H < 40) continue;
        int photo = a->first + i, on = i == sel;
        if (on) {
            ui_rrect(x - 3, y - 3, PTHUMB_W + 6, PTHUMB_H + 6, 8, ACCENT);
            ui_rrect(x - 1, y - 1, PTHUMB_W + 2, PTHUMB_H + 2, 6, BG);
        }
        plib_want(photo);
        const Texture *t = plib_thumb(photo);
        if (t) { gfx_filter(1); gfx_tex(t, x, y, PTHUMB_W, PTHUMB_H, 0xFFFFFFFF); }
        else draw_placeholder(photo, x, y, PTHUMB_W, PTHUMB_H);
        font_draw_fit(f_label, x, y + PTHUMB_H + 13, photos[photo].name, on ? TEXT : MUTED, PTHUMB_W);
    }
    gfx_unclip();
}

static float hint(float x, int glyph, const char *label)
{
    x -= ui_hint_width(f_label, glyph, label);
    ui_hint(f_label, x, SCREEN_H - 8, glyph, label, MUTED);
    return x - 12;
}

void photos_draw(float dt)
{
    if (viewing) {
        pviewer_draw();
        return;
    }
    if (!photo_count && !item_count) {
        ui_empty_card(150, "No photos yet", "JPEG or PNG photos into", plib_dir(), "Each folder there becomes an album.");
        hint(SCREEN_W - 14, ICON_CIRCLE, "Back");
        return;
    }
    float x = SCREEN_W - 14;
    if (album >= 0) {
        album_draw(dt);
        x = hint(x, ICON_CIRCLE, "Back");
        hint(x, ICON_CROSS, "View");
        return;
    }
    draw_pills();
    if (view == VIEW_LIBRARY) library_draw(dt);
    else albums_draw(dt);
    gfx_rect(0, SCREEN_H - 20, SCREEN_W, 20, BG);
    x = hint(x, ICON_CIRCLE, "Back");
    x = hint(x, ICON_SQUARE, view == VIEW_LIBRARY ? "Albums" : "Library");
    if (view == VIEW_LIBRARY && item_count) {
        x = hint(x, ICON_TRIANGLE, "Next month");
        hint(x, ICON_CROSS, items[sel].video >= 0 ? "Play" : "View");
    } else if (view == VIEW_ALBUMS && palbum_count) {
        hint(x, ICON_CROSS, "Open");
    }
}
